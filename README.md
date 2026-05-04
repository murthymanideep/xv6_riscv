# This repository extends the xv6 (RISC-V) operating system with custom system calls and kernel-level statistics.

---

## 1)Implemented System Calls

### hello()
Prints a fixed message from inside the kernel.  
Takes no arguments and always returns 0.

### getpid2()
Returns the PID of the calling process.  
Implemented using `myproc()` without relying on the existing `getpid()`.

### getppid()
Returns the PID of the parent process.  
Returns `-1` if the process has no parent.  
Accesses parent information using `wait_lock` and the parent’s process lock.

### getnumchild()
Returns the number of live (non-zombie) child processes.  
Scans the global process table and checks parent relationships.  
Uses `wait_lock` and per-process locks for safe access.

### getsyscount()
Returns the total number of system calls made by the process.  
Reads a counter stored in the process structure.

### getchildsyscount(pid)
Returns the system call count of a given child process.  
Returns `-1` if the PID is not a child.  
Uses `wait_lock` and child process locks for consistency.

---

## Design Decisions and Assumptions

- Each process maintains its own system call counter.
- The counter is incremented on every system call.
- Parent–child relationships are accessed only while holding `wait_lock`.
- Process state and counters are accessed using per-process locks.
- Child processes are identified by scanning the global process table.
- Zombie processes are not counted as active children.
- System call counters are not preserved after process termination.

---

---

## 2)Scheduler (SC-MLFQ)

### Overview
The default xv6 round-robin scheduler was replaced with a System-Call-Aware Multi-Level Feedback Queue (SC-MLFQ) scheduler.  
The scheduler maintains four priority queues. Processes are scheduled from the highest priority queue first. CPU-bound processes are gradually demoted to lower queues while interactive processes remain at higher queues. Starvation is prevented using periodic global priority boosting.

---

### Scheduler Design
Four queues are implemented.

| Level | Time Quantum |
|------|------------|
| 0 | 2 ticks |
| 1 | 4 ticks |
| 2 | 8 ticks |
| 3 | 16 ticks |

Level 0 has the highest priority.

- The scheduler always selects a RUNNABLE process from the highest non-empty queue.  
- Within each queue, round-robin scheduling is used.

---

### Process Accounting
Each process maintains the following fields:

- `level` – current queue level  
- `ticks_used` – ticks consumed in current time slice  
- `ticks_total[4]` – total ticks consumed at each level  
- `times_scheduled` – number of times scheduled  
- `slice_start_syscalls` – syscall counter value at slice start  

---

### System-Call-Aware Rule
At the end of each time slice:

- ΔS = system calls during slice  
- ΔT = ticks consumed during slice  

Rule:
- If ΔS ≥ ΔT → process is interactive → do not demote  
- If ΔS < ΔT → process is CPU-bound → demote  

A process is demoted by one level if it consumes its entire time slice and does not satisfy the interactive condition.  
Level 3 is the lowest queue and cannot be demoted further.

---

### Global Priority Boost
To prevent starvation, a global boost occurs every `128 timer ticks`.

During the boost:
- All RUNNABLE processes move to Level 0  
- `ticks_used` is reset  

---

### Kernel Changes
The following components were modified:

- **proc.h**
  - Added scheduling fields to `struct proc`

- **proc.c**
  - Reimplemented `scheduler()` with:
    - multi-queue scheduling  
    - round-robin within each queue  
    - syscall-aware demotion  
    - global priority boost  

- **trap.c**
  - Timer interrupt updated to:
    - increment tick usage  
    - enforce time quantum  
    - trigger yield  

---

### New System Calls

#### getlevel()
Returns the current MLFQ level of the calling process.  
Return value: `0–3`

#### getmlfqinfo()
Returns scheduling statistics of a process.  
Return value:
- `0` → success  
- `-1` → invalid PID  

---

### Experimental Evaluation

#### CPU-bound processes
- Perform long computations with minimal system calls  
- Gradually move from Level 0 → Level 3  
- Remain in lowest queue  

#### Syscall-heavy processes
- Perform frequent system calls  
- Satisfy ΔS ≥ ΔT  
- Stay in higher priority queues  

#### Mixed workloads
- CPU-bound processes move down  
- Interactive processes stay higher  
- Scheduler prioritizes interactive tasks  

---

### Starvation Prevention
- Global boost ensures all processes eventually regain high priority  
- Prevents indefinite waiting  

---

### Conclusion
The scheduler correctly implements SC-MLFQ behavior:

- CPU-bound processes move to lower queues  
- Interactive processes remain in higher queues  
- Mixed workloads are handled correctly  
- Starvation is prevented  








---
## Note
This repository is based on the original xv6 (RISC-V) codebase from MIT PDOS.

---

# Original xv6 README (Unmodified)

xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  xv6 loosely follows the structure and style of v6,
but is implemented for a modern RISC-V multiprocessor using ANSI C.

ACKNOWLEDGMENTS

xv6 is inspired by John Lions's Commentary on UNIX 6th Edition (Peer
to Peer Communications; ISBN: 1-57398-013-7; 1st edition (June 14,
2000)).  See also https://pdos.csail.mit.edu/6.1810/, which provides
pointers to on-line resources for v6.

The following people have made contributions: Russ Cox (context switching,
locking), Cliff Frey (MP), Xiao Yu (MP), Nickolai Zeldovich, and Austin
Clements.

We are also grateful for the bug reports and patches contributed by
Abhinavpatel00, Takahiro Aoyagi, Marcelo Arroyo, Hirbod Behnam, Silas
Boyd-Wickizer, Anton Burtsev, carlclone, Ian Chen, clivezeng, Dan
Cross, Cody Cutler, Mike CAT, Tej Chajed, Asami Doi,Wenyang Duan,
echtwerner, eyalz800, Nelson Elhage, Saar Ettinger, Alice Ferrazzi,
Nathaniel Filardo, flespark, Peter Froehlich, Yakir Goaron, Shivam
Handa, Matt Harvey, Bryan Henry, jaichenhengjie, Jim Huang, Matúš
Jókay, John Jolly, Alexander Kapshuk, Anders Kaseorg, kehao95,
Wolfgang Keller, Jungwoo Kim, Jonathan Kimmitt, Eddie Kohler, Vadim
Kolontsov, Austin Liew, l0stman, Pavan Maddamsetti, Imbar Marinescu,
Yandong Mao, Matan Shabtay, Hitoshi Mitake, Carmi Merimovich,
mes900903, Mark Morrissey, mtasm, Joel Nider, Hayato Ohhashi,
OptimisticSide, papparapa, phosphagos, Harry Porter, Greg Price, Zheng
qhuo, Quancheng, RayAndrew, Jude Rich, segfault, Ayan Shafqat, Eldar
Sehayek, Yongming Shen, Fumiya Shigemitsu, snoire, Taojie, Cam Tenny,
tyfkda, Warren Toomey, Stephen Tu, Alissa Tung, Rafael Ubal, unicornx,
Amane Uehara, Pablo Ventura, Luc Videau, Xi Wang, WaheedHafez, Keiichi
Watanabe, Lucas Wolf, Nicolas Wolovick, wxdao, Grant Wu, x653, Andy
Zhang, Jindong Zhang, Icenowy Zheng, ZhUyU1997, and Zou Chang Wei.

ERROR REPORTS

Please send errors and suggestions to Frans Kaashoek and Robert Morris
(kaashoek,rtm@mit.edu).  The main purpose of xv6 is as a teaching
operating system for MIT's 6.1810, so we are more interested in
simplifications and clarifications than new features.

BUILDING AND RUNNING XV6

You will need a RISC-V "newlib" tool chain from
https://github.com/riscv/riscv-gnu-toolchain, and qemu compiled for
riscv64-softmmu.  Once they are installed, and in your shell
search path, you can run "make qemu".
