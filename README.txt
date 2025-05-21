

sys_health_monitor – Real‑Time Linux Health Monitor (v1.5)
==========================================================

Overview
--------
`sys_health_monitor` is a loadable kernel module that samples four key metrics
every five seconds and exposes them via `/proc/sys_health`:

  • Free memory (MiB)  
  • Total memory (MiB)  
  • 1‑minute CPU load relative to all online cores (%)  
  • Aggregate disk‑I/O rate in 512‑byte sectors per second  

Any metric that crosses its threshold triggers a kernel‑log warning.  The
module supports kernels 5.4 and newer; when per‑disk sector stats are not
exported (e.g., 6.11‑rc), it falls back to VM‑event counters so disk I/O
monitoring still works.

Quick Build & Install
---------------------
A ready‑made `Makefile` resides in the **src** folder.


1.  Change into the directory that contains the source and run  
       `make`  
    (the Makefile invokes your current kernel’s build system).

2.  Insert the module with optional thresholds, e.g.  
       `sudo insmod sys_health_monitor.ko mem_threshold=256 cpu_threshold=15 io_threshold=500`

3.  View live stats with  
       `cat /proc/sys_health`

4.  Follow alerts in another terminal with  
       `sudo dmesg -w`

5.  Remove the module when finished:  
       `sudo rmmod sys_health_monitor`

Module Parameters
-----------------
`mem_threshold`   – free‑memory floor in MiB (default 100)  
`cpu_threshold`   – 1‑minute load percentage (default 80)  
`io_threshold`    – disk‑I/O rate in sectors/s (default 5000)

Simple Functional Test
----------------------
*CPU*: `stress-ng --cpu 4 --timeout 20s`  
*Memory*: `stress-ng --vm 1 --vm-bytes 75% --timeout 20s`  
*Disk I/O*: `dd if=/dev/zero of=~/io_test.bin bs=4M count=1024 oflag=direct && sync`  

Each command should generate an **Alert:** line in `dmesg`.

Compatibility Notes
-------------------
* Prefers block‑layer sector counters (`part_stat_read`) when available.  
* Automatically switches to `all_vm_events()` with `PGPGIN/PGPGOUT` when sector
  stats or iterator macros are unavailable.  
* Variable names avoid clashes with the kernel’s global `current` pointer on
  6.11‑series kernels.

Cleaning Up
-----------
Run `make clean` to remove generated objects.

License
-------
GPL‑2.0; see the source file header for author credits.
