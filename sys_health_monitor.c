/*───────────────────────────────────────────────────────────────────────────
 * Group Name:    Group 6
 * Group Members: Kamden Morgan, Alicia Mansaray, Alex Rodriguez
 * Course:        SCIA 360 – Operating System Security
 * Project:       Linux Kernel Module for Real‑Time Health Monitoring
 * Version:       1.5  (2025‑05‑02)
 *   • Fixes build errors on ≥6.11 kernels (implicit global_page_state/NR_PGPGIN
 *     symbols and macro clash with current task pointer).
 *   • Fallback path now uses the generic vm‑event API (all_vm_events()) and the
 *     PGPGIN/PGPGOUT counters, which remain stable across kernel releases.
 *   • Renames internal variable “current” → “io_total” to avoid conflict with
 *     the kernel’s “current” task macro.
 *───────────────────────────────────────────────────────────────────────────*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched/loadavg.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/version.h>
#include <linux/cpumask.h>

/* ---------- Block‑layer headers present from 5.4 upward ---------------- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
#  include <linux/blkdev.h>
#  include <linux/part_stat.h>

/* Iterator‑macro probe: kernels differ in what they export --------------- */
#  if defined(for_each_disk)                     /* <= 6.10 */
#    define HAVE_DISK_ITER 1
#  elif defined(for_each_gendisk)                /* some 6.11‑rc trees */
#    define HAVE_DISK_ITER 1
#    define for_each_disk(gd)  for_each_gendisk(gd)
#  else                                          /* iterator removed */
#    define HAVE_DISK_ITER 0
#  endif
#else  /* < 5.4: no block‑layer headers for modules */
#  define HAVE_DISK_ITER 0
#endif

/* Sector‑stats were exported to modules in 5.18 ------------------------- */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
#  define HAVE_DISK_STATS 1
#else
#  define HAVE_DISK_STATS 0
#endif

/* ─── Configurable parameters ──────────────────────────────────────────── */
static int mem_threshold = 100;     /* MiB free memory floor           */
module_param(mem_threshold, int, 0644);
MODULE_PARM_DESC(mem_threshold, "Free‑memory threshold in MiB");

static int cpu_threshold = 80;      /* % of total CPU capacity         */
module_param(cpu_threshold, int, 0644);
MODULE_PARM_DESC(cpu_threshold, "CPU threshold as %% of all cores");

static int io_threshold = 5000;     /* sectors per second              */
module_param(io_threshold, int, 0644);
MODULE_PARM_DESC(io_threshold, "Disk‑I/O threshold (sectors/s)");

/* ─── Module state ─────────────────────────────────────────────────────── */
#define TAG "[Group6] "

static struct timer_list poll_timer;
static u64 last_io_ticks;           /* tracks cumulative sectors so far */
static bool io_fallback_logged;
static struct proc_dir_entry *proc_entry;
static spinlock_t snap_lock;

struct sys_snapshot {
    u64 ts_ms;
    u32 free_mem_mib;
    u32 total_mem_mib;
    u32 load_pct;        /* % of aggregate core capacity */
    u32 io_rate_sps;     /* disk sectors / second        */
} snapshot;

/* ─── Helpers ──────────────────────────────────────────────────────────── */
static void collect_memory(u32 *free_mib, u32 *total_mib)
{
    struct sysinfo si;
    si_meminfo(&si);
    *total_mib = (u32)(si.totalram >> 10);  /* pages → MiB */
    *free_mib  = (u32)(si.freeram  >> 10);
}

static u32 collect_load_percent(void)
{
    unsigned long avg_fp = avenrun[1];            /* 1‑minute */
    u64 load_x100 = (avg_fp * 100ULL) >> FSHIFT;  /* fixed‑pt → % */
    unsigned int cores = max_t(unsigned int, num_online_cpus(), 1);
    return div_u64(load_x100, cores);
}

/* ─── Disk‑I/O collection ─────────────────────────────────────────────── */
static u32 collect_disk_ios(void)
{
#if HAVE_DISK_STATS && HAVE_DISK_ITER
    /* Preferred path: block‑layer sector counters */
    u64 io_total = 0;
    struct gendisk *gd;

    rcu_read_lock();
    for_each_disk(gd) {
        io_total += part_stat_read(gd->part0, sectors[STAT_READ])  +
                     part_stat_read(gd->part0, sectors[STAT_WRITE]);
    }
    rcu_read_unlock();
#else
    /* Fallback path: use PGPGIN/PGPGOUT vm‑event counters which
     * approximate overall I/O traffic in pages. These counters exist in
     * all supported kernels even after the NR_PGPG* symbols were dropped.
     */
    if (!io_fallback_logged) {
        printk(KERN_INFO TAG
               "Disk‑stats interface missing; falling back to PGPGIN/PGPGOUT vm‑events.\n");
        io_fallback_logged = true;
    }

    unsigned long events[NR_VM_EVENT_ITEMS];
    all_vm_events(events);
    u64 pages_io = (u64)events[PGPGIN] + (u64)events[PGPGOUT];
    u64 io_total = pages_io * (PAGE_SIZE >> 9);   /* pages → 512‑byte sectors */
#endif

    /* Delta against previous sample to obtain rate (per‑5‑second window). */
    if (!last_io_ticks) {
        last_io_ticks = io_total;
        return 0;
    }

    u64 delta = io_total - last_io_ticks;
    last_io_ticks = io_total;
    return div_u64(delta, 5);
}

/* ─── Timer callback (5 s) ─────────────────────────────────────────────── */
static void poll_metrics(struct timer_list *t)
{
    struct sys_snapshot tmp;

    collect_memory(&tmp.free_mem_mib, &tmp.total_mem_mib);
    tmp.load_pct     = collect_load_percent();
    tmp.io_rate_sps  = collect_disk_ios();
    tmp.ts_ms        = jiffies_to_msecs(jiffies);

    spin_lock(&snap_lock);
    snapshot = tmp;
    spin_unlock(&snap_lock);

    if (tmp.free_mem_mib < mem_threshold)
        printk(KERN_WARNING TAG "Alert: free memory %u MiB below %d\n",
               tmp.free_mem_mib, mem_threshold);

    if (tmp.load_pct > cpu_threshold)
        printk(KERN_WARNING TAG
               "Alert: 1‑min CPU load %u %% above %d %%\n",
               tmp.load_pct, cpu_threshold);

    if (tmp.io_rate_sps > io_threshold)
        printk(KERN_WARNING TAG
               "Alert: disk I/O %u sps above %d\n",
               tmp.io_rate_sps, io_threshold);

    mod_timer(&poll_timer, jiffies + msecs_to_jiffies(5000));
}

/* ─── /proc reader ─────────────────────────────────────────────────────── */
static int proc_show(struct seq_file *m, void *v)
{
    struct sys_snapshot s;
    spin_lock(&snap_lock);
    s = snapshot;
    spin_unlock(&snap_lock);

    seq_printf(m,
           "Timestamp_ms : %llu\n"
           "Memory_free  : %u MiB\n"
           "Memory_total : %u MiB\n"
           "CPU_load_1m  : %u %%\n"
           "Disk_io_rate : %u sectors/s\n",
           s.ts_ms, s.free_mem_mib, s.total_mem_mib,
           s.load_pct, s.io_rate_sps);
    return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, NULL);
}

static const struct proc_ops proc_file_ops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ─── Lifecycle ────────────────────────────────────────────────────────── */
static int __init sys_health_init(void)
{
    spin_lock_init(&snap_lock);

    printk(KERN_INFO TAG
           "SCIA 360: Module v1.5 loaded successfully. "
           "Team Members: Kamden Morgan, Alicia Mansaray, Alex Rodriguez\n");

    proc_entry = proc_create("sys_health", 0444, NULL, &proc_file_ops);
    if (!proc_entry)
        return -ENOMEM;

    timer_setup(&poll_timer, poll_metrics, 0);
    mod_timer(&poll_timer, jiffies + msecs_to_jiffies(5000));
    return 0;
}

static void __exit sys_health_exit(void)
{
    del_timer_sync(&poll_timer);
    if (proc_entry)
        proc_remove(proc_entry);
    printk(KERN_INFO TAG "SCIA 360: Module unloaded. Goodbye!\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group 6");
MODULE_DESCRIPTION("Real‑Time Health Monitoring Module for SCIA 360 – v1.5");
MODULE_VERSION("1.5");

module_init(sys_health_init);
module_exit(sys_health_exit);
