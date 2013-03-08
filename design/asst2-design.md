Assignment 2 Write Up
=====================
###Aidan & Willie: White-Faced Storm Petrel-OS###

New Section
===========
This is a new section that we are adding now that we've completed A2 with the goals of:

1.  Documenting important design decisions/constraints/assumptions of our implementation. Some might be new, others might be from our original design doc.
2.  Give our perspective on our implementation in hopes that it may help the grading of this assignment/improve the feedback we recieve.

Processes
---------
We make the assumption that only 1 user program can be launched using `runprogram()` so that we can always assign that initial program with a pid of `PID_MIN` (currently 2). All additional user programs must be forked. The viable pid for user programs are `PID_MIN` to `PID_MAX` inclusive.

Exec
----
We use a `global_exec_lock` to ensure that only 1 exec syscall can be performed at one time due to memory considerations. We following the file name, path name, and total argument length max limited defined in limit.h:

    /* Longest filename (without directory) not including null terminator */
    #define __NAME_MAX      255

    /* Longest full path name */
    #define __PATH_MAX      1024

    /* Max bytes for an exec function */
    #define __ARG_MAX       (64 * 1024)

Fild Descriptors
----------------
Each process is limited to 16 file descriptors (defined in `__FD_MAX`) with 0-2 assigned to stdio by default. The 16 availalbe file descriptors must be between 0-15 inclusive.

Scheduling
----------
We ultimately decided to implement a multi-level feedback queue styled after the Solaris scheduler for our operating system.  This means each processor, rather than having a single queue for threads, would have NUM_PRIORITIES queues that were declared as follows:

    struct mlf_queue {
        struct threadlist runqueue[NUM_PRIORITIES];
    };

Operations on these arrays of queues were locked with the CPUs spinlock, just like the operations on the round-robin scheduler's single queues.  We implemented the following helper functions making use of the threadlist helper functions declared in cpu.h:

    void mlf_add_thread(struct mlf_queue *m, struct thread *t);
    struct thread *mlf_rem_head(struct mlf_queue *m);
    struct thread *mlf_rem_tail(struct mlf_queue *m);
    bool mlf_isempty(struct mlf_queue *m);
    unsigned mlf_count(struct mlf_queue *m);

Our scheduler operated as follows:
Priorities ranged from 0 (highest) to NUM_PRIORITIES-1 (lowest).  Threads entered by default at the highest priority.  Any time a thread was caused to yield by using its entire timeslice, it would be demoted one priority level.  This required modifying `thread_yield()`.  When a thread blocked by going to sleep, it would be incremented one priority level.  This required modifying `thread_switch()`.  

The rationale for this was that threads that frequently block on IO should be allowed to run often to input their requests, while computational jobs which will run for long periods of time will have to wait.  

We modified `thread_make_runnable()` to insert into the correct priority queue level, and we additionally modified `thread_consider_migration()` to move threads from the tail of the lowest occupied priority queue into another CPU at the same priority level. 

Reaping Policy
--------------
For our reaping policy, we ended up opting out of a dedicated reaping thread and instead extending the reaping framework existing between the `thread_exit()` and `exorcise()` functions.  We modified `exorcise()` so that only threads with valid `parent_pid` (in our implementation, pid > 0) would be reaped.  

While `parent_pid` is initialized to 0, and as such is invalid, we chose to set a `parent_pid` of -1 for orphaned user processes, although the exorcise code makes no distinction between kernel threads that never acquired a parent and child threads that have been marked as orphans.  When a parent waits for a child, it will set said child's `parent_pid` to -1 since it is effectively orphaned as it has exited and no processes will wait on it.  Similarly, when a parent process exits, it will mark all its children as orphans by setting their `parent_pid` to -1.  

Known Issues
------------
###badcall###
All badcall tests relevant to asst2 pass individually. However we require at least 4BM of RAM to run all the asst2 tests (by pressing `2`) without running out of memory. Once we do run out memory, we get the following memory related failures in various badcalls: 

    testbin/badcall: UH-OH: creating badcallfile: failed: Out of memory
    testbin/badcall: FAILURE: write with NULL buffer: Bad file number

We don't believe the above issue to be a bug as we suspect this has primarily to do with the large memory leak of dumbvm. However we do have a bug (which we believe to be the same bug) in the following forms:

    testbin/badcall: UH-OH: opening null: failed: No such file or directory
    testbin/badcall: FAILURE: open null: with bad flags: No such file or directory

When badcall tests are run individually, this never happens. However we can reliably reproduce this by running all the asst2 badcall tests, and it will manifest itself during lseek during the first iteration, and in multiple locations during subsequent locations.

###bigexec###
We adjusted our DUMBVM_STACKPAGES to 18 pages to run this test. Our kernel panics on the largest input (1000 8-letter words):

    0xffffffff8002bf04 in copystr (dest=0x80283800 "\200(4", src=0x40280c <Address 0x40280c out of bounds>,
        maxlen=1024, stoplen=1024, gotlen=0x80040eac) at ../../vm/copyinout.c:241

Table of Contents
=================
1.    Overview
    2.    Process
        2.    Data Structures
        2.    Reaping Policy
        2.    Helper Functions
    2.    File Descriptors
        2.    Data Structures
2.    Topics
    3.    File Descriptors
        3.    open
        3.    read
        3.    write
        3.    lseek
        3.    close
        3.    dup2
    3.    fork
    3.    execv
    3.    waitpid/exit
        3.    waitpid
        3.    exit
    3.    Other System Calls
        3.    chdir
        3.    getcwd
        3.    getpid
        3.    kill_curthread
    3.    Scheduling
    3.    Synchonization Issues
3.    Plan of Action & Credit
4.    Code Reading Repsonses

1. Overview
===========
1.1 Processes
-------------

For the sake of designing for correctness first, we have decided to enforce one thread per process with no plan of ever implementing multiple threads per process. This design decision allows us to add new process fields directly on the existing thread struct. By trading away extensibility, we avoid having to move existing thread struct fields that traditionally belong to a process, which we expect to break portions of the existing code. Thus, we will modify the existing thread struct as follows:

    struct thread{
        // Old thread properties here
    
        // CV that other processes will wait on for "wait_pid"
        // and will be broadcasted over in "exit"
        struct cv *waiting_on;
        struct lock *cv_lock;
        
        // Fields that belong to a process
        int pid;
        int priority;
        int parent_pid;
        int owner;
        int exit_status;

        // Linked list of the child PIDs
        struct pid_list *children;
        
        // Scheduling variables
        struct times *recent_runtime;
        
        //File descriptor indexed array of file table addresses
        // This will be statically allocated
        struct ft_ent *fd_table[MAX_FILE_DESCRIPTOR]; // Max of 16 was mentioned
    }

The above modified thread struct now also serves to represent our process control block (PCB) and is allocated in the kernel heap (where thread structs are already allocated now). To allow for constant time access into each PCB, we will also allocate in the kernel heap an array of pointers to each PCB that is indexed by the `pid`. We will call this array the process table. Since we want to avoid having to reallocate memory while preserving constant time access to our PCB, we have decided to set a hard limit `MAX_PROCESS` (which can be modified later) of 128 processes and allocate the process table array on startup, using 518 bytes of memory. We will modify `thread_create()`, and potentially `thread_boostrap()` and `cpu_create()`, to properly initalize the new process fields and process table. The process table will be declared as a global variable in thread.c as `struct thread process_table[MAX_PROCESS]`.

PIDs are recycled by setting the address in the process table index by that `pid` to `NULL` when the process exits. When `thread_create()` is called, a `pid` is chosen randomly by the output of a random number mod `MAX_PROCESS`. If that `pid` is unavailable, we iterate the process table until we find an available `pid`, else we return with an error. The motivation for this design is to:  

1.  Choose the `pid` uniformly (more or less)
2.  Ensure that a `pid` is guaranteed to be assigned if one is available
3.  Use a simple approach that avoids additional data structures

In our process struct, we need to maintain a list of all its children for reaping and waiting processes.  We also need to maintain a global list of orphaned processes in the kernel heap.  As such, we defined two pid list structs - synchronized and unsynchronized.  Since processes in our system are single-threaded, there is no need for synchronization and including a mutex primitive would merely waste space.  We chose to implement these as doubly linked lists, as each process is unlikely to have many children and an effectively reaping system should not allow many zombies to stay around, so a linear data structure should suffice.

    struct synchronized_pid_list{
        struct process_list *list;
        struct lock *mutex;
    }
    
    struct pid_list{
        struct process_list_ent *head;
        struct process_list_ent *tail;
    }

    struct pid_list_ent{
        int pid;
        struct process_list_ent *next;
    }

We also maintain a linked list that will be used to track the times at which the process started running and ceased running over a recent interval.  We will use this data for our scheduling algorithm (discussed in 2.7) which uses the data of the process' recent runtime.
    
    struct times{
        struct t_interval *head;
        struct t_interval *tail;
    }
    
    struct t_interval{
        time_t start;
        time_t end;
    }

###1.1.2 Reaping Policy###
*   When a parent calls `waitpid()` on a child process (even after the child has already exited), the parent will reap the child before returning.
*   When a parent process exits, it will reap all its zombie children and add the rest of its children to the global `orphan_list`, an instance of the `synchronized_pid_list` struct that lives in kernel memory, after acquiring its lock.
*   A dedicated reaping thread will periodically acquire the lock of the orphan list and reap any children that have become zombies.  This thread will be spawned alongside the bootup thread in `thread_bootstrap`.

A dedicated thread is maintained to 
Orphan list: (Global)
Will live in kernel memory
Linked list with lock for synchronization
When processes exit, non-zombie children will have their pids added to the list

###1.1.3 Helper Functions###
    int add_pid(struct pid_list *list, int pid)
    int remove_pid(struct pid_list *list, int pid)
    int contains_pid(struct pid_list *list, int pid)


1.2 File Descriptors
--------------------
File descriptors are integers that index into the array of file table entry addresses (`struct ft_ent **fd_table`) maintained by the PCB struct. File table entries (`ft_ent`) are allocated on the kernel heap. Each process maintains its own file table (although different processes may reference to the same `ft_ent` due to `fork()`), which is a loose term (since there is no actual implementation of it) that we use to refer to of all the file table entries (`ft_ent`) of that process. The file table entry maintains the reference to the vnode of the file that it associates with, the status (eg read or write), and the offset. The file table entry struct is defined as follows:

    struct ft_ent{
        struct vnode *file;
        int status;
        int offset;
        int refcnt;
    
        struct lock *mutex;
    }

The lock in the `ft_ent` struct exists because of the `fork()` system call.  Through `fork()`, multiple processes might be pointing to the same `ft_ent` from their file descriptor tables, and we must ensure that updates to the file permissions or position in the file must happen atomically (changes to the file itself should be handled by synchronization on the `vnode` struct itself).

2. Topics
=========

2.1 File descriptors
--------------------

###2.1.1 `open(const char *filename, int flags, int mode)`###
Mode will be assumed to be `O_RDONLY` if not provided.  Look through the current PCB `fd_table` for an available index.  If no non-null indices exist, set errno to EMFILE and return -1.  Otherwise, allocate and initialize a `fd_ent` with the `refcnt` set to 1 and the `status` set to `mode`, and set the chosen index of the `fd_table` to point to it.  Call `vfs_open()` with the vnode pointer in the fd_ent struct and the user-provided `flags` argument.  If the VFS call succeeds, return the file descriptor, otherwise set the appropriate errno and return -1 after freeing the allocated struct.
         
###2.1.2 `read(int fd, void *buf, size_t buflen`###
If `fd` is not in the range [0,MAXFDS), or the PCB `fd_table[fd]` is null, then set the errno to EBADF and return -1.  Obtain the lock on the `ft_ent`.  If the the mode of the `ft_ent` is WRONLY, release the lock, set the errno to EBADF and return -1.  Otherwise, build a `struct iovec` that contains the user supplied buffer and buffer length.  Create a `struct uio` and populate it with the `iovec`.  Set the `uio_segflg` to `UIO_USRSPACE`, the `uio_rw` to `UIO_READ`, the `uio_offset` to `ft_ent.offset`, and the address space to that of the current process.  Call `VOP_READ()` on the vnode and the iovec.  If the VFS call fails, set the appropriate error message and return -1.  Finally, set the `ft_ent.offset` to the `uio.resid` which was updated by the call to `VOP_READ()`, then release the lock and return the difference between the old and new offset.

###2.1.3 `write(int fd, const void *buf, size_t nbytes)`###
If `fd` is not in the range [0,MAXFDS), or the PCB `fd_table[fd]` is null, then set the errno to EBADF and return -1.  Obtain the lock on the `ft_ent`.  If the the mode of the `ft_ent` is RDONLY, release the lock, set the errno to EBADF and return -1.  Otherwise, build a `struct iovec` that contains the user supplied buffer and buffer length.  Create a `struct uio` and populate it with the `iovec`.  Set the `uio_segflg` to `UIO_USERSPACE`, the `uio_rw` to `UIO_WRITE`, the `uio_offset` to `ft_ent.offset`, and the address space to that of the current process.  Call `VOP_WRITE()` on the vnode and the iovec.  If the VFS call fails, set the appropriate error message and return -1.  Finally, set the `ft_ent.offset` to the `uio.resid` which was updated by the call to `VOP_WRITE()`, then release the lock and return the difference between the old and new offset.

###2.1.4 `lseek(int fd, off_t pos, int whence)`###
If `fd` is not in the range [0,MAXFDS), or the PCB `fd_table[fd]` is null, then set the errno to EBADF and return -1.  If `whence` is not `SEEK_SET`, `SEEK_CUR`, or `SEEK_END`, set the errno to EINVAL and return -1.  Obtain the lock on the `ft_ent`.  Call `VOP_TRYSEEK()` on the vnode with the position as `pos+ft_ent.offset` - if the seek is not legal, set the appropriate errno and return -1.  Otherwise, update the offset in the `ft_ent`, release the lock, and return the new offset.

###2.1.5 `close(int fd)`###
If entry `fd` of the current PCB `fd_table` is NULL, set errno to EBADF and return -1.  Otherwise, obtain the lock on the `ft_ent`.  Decrement the `fd_table[fd]->refcnt` and call `VOP_DECREF()` on the vnode of the `ft_ent`.  If the vnode's `vn_refcnt` is 0, call `VOP_CLOSE()` on it.  If this VFS call fails, set errno to EIO and return -1.  If the refcnt of the `ft_ent` is 0, release the lock, free the `ft_ent` struct, set `fd_table[fd]` to NULL, and return 0.  If the refcnt of the `ft_ent` is positive, simply release the lock and return 0.

###2.1.6 `dup2(int oldfd, int newfd)`###
If `oldfd` or `newfd` are outside the range of [0,MAXFDS), or if the PCB's `fd_table[oldfd]` is NULL, set errno to EBADF and return -1.  If none of the entried of the PCB's `fd_table` are non-NULL, set errno to EMFILE and return -1.  Otherwise, if `fd_table[newfd]` is non-NULL, call `close(newfd)`.  Then set `fd_table[newfd] = fd_table[oldfd]` and return `newfd`.

2.2 fork
--------
The `syscall()` function will pass the trapframe to `fork()` so that we have access to it. Always fail as early as possible and in the parent process, so call `thread_fork()` as late as possible. `fork()` begins by making a shallow copy of the parent's trapframe for the child process named `child_tf`. As described in the overview, assign the `pid` for the child process by randomly selecting an index of `process_table`. If `process_table[pid]` is not `NULL`, then iterate down the array until one is found that is, looping round to the index 1 (omitting 0) if necessary. Hold the assigned `pid` as a variable in the parent process. Make a copy of all the other PCB fields introduced in the overview from the parent PCB and hold them as variables in the parent process, with the exception of cvs and locks which should simply be initalized later. Note that we have `as_copy()` available to use for copying the address space.  

Once the above initialization is done, create a semaphore initalized at 0 that will be used to signal to the child after the proper initalization on the child PCB is done. (Note that initializing as much of the child's PCB in the parent is preferable, but that can't be completed until after `thread_fork()` exits.) Also initalize a variable `child` that will reference the child's PCB after `thread_fork()` returns. Pass this variable, the semaphore, and `child_tf` together as a struct to the existing `thread_fork()` function along with a helper function `new_process_setup()`. This function will `P()` the semaphore to wait for the initalization of the PCB by the parent to complete. Once completed, the function will create a local stackframe variable `tf` (so that it is on the child's stack) that copies `child_tf`, destroy the semaphore, modify the return value `child_tf->tf_v0 = 0;` and call `mips_usermode()` with `tf`.  

After `thread_fork()` returns, the new PCB will have been created. In the parent thread, place all the PCB fields that have been initiated above into the child's PCB. Set the address of the PCB in the address table. Modify the parent's trapframe so that the return value is the parent's `pid`. Return with 0, or an error code.  

Note that `thread_create()` may need to be modified to initalize the default values of the file descriptor array (eg with standard in/out) in order to accomodate the very first process. Similar logic may apply to other fields that have been added to the PCB.

2.3 execv
---------
The implementation of `execv()` will following the existing `runprogram()` implementation closely, but modified to pass in the arguments to the program being run. Before we activate the new address space do the following. Create a buffer in kernel memory to hold all the arguments. Copy each argument to said kernel buffer with `copyin()` while being mindful that each argument string is terminated by the null character and that `args[argc]` should be `NULL`. Use `strlen()` to ensure that the arguments are not too long such that they collectively exceed `ARG_MAX`. Once the buffer is loaded, activate the new address space. After leaving enough room at the base of the user stack for `**argv`, use `copyout()` to copy each of the arguments in order from the kernel buffer while being mindful of word alignment. Place the addresses of each of these arguments that are now in the user address space in `**argv` . The rest of the execution will follow the remaining code in `runprogram()`, but using the correct stackpointer when calling `as_define_stack()` and the correct argument count when entering the new user process.

Because the kernel stack size is limited to 1 page (4096 bytes) and because the buffer for the arguments could potentially take up a lot of kernel space (potentially much more than other system calls), not placing a limit on the number of `execv()` that can be run at the same time could lead us to run out of kernel memory. The simple solution is to have a global kernel lock initialized during `thread_bootstrap()` that needs to be acquired in order for `execv()` to continue, thereby limiting the kernel to 1 `execv()` call at any time. Alternatively, turn on the interrupt and thereby limitting the number of `execv()` calls to the number of processor. Alternatively still, use a global kernel `semaphore()` to limit the number of `execv()` calls made at the same time. For all these approaches, more math will need to be done to determine how much memory we will likely have in the kernel. We currently lean towards the simplest approach, which is the first one. Other size limits may have to be placed on the arguments passed in on top of the default `ARG_MAX` set in `limits.h`.

2.4 waitpid/exit
----------------

###2.4.1 `waitpid(pid_t pid, int *status, int options)`###
A process can only call `waitpid` on a child. We enforce this by first checking that the child `pid` is in the `struct pid_list *children` of the PCB.
If `options` is not 0, set errno to EINVAL and return -1.  If `status` is NULL, set errno to EFAULT and return -1.  If `pid` does not exist, set errno to ESRCH and return -1. If `pid` does not exist in the calling process' `child_list`, set errno to ECHILD and return -1.  If the status of the child with pid `pid` is S_ZOMBIE, then store its `exit_status` in the `status` pointer, then call `thread_destroy()` on the child and remove its pid from your `child_list` with `remove_pid()`.  Otherwise, call `cv_wait()` on the `wait_on` condition variable of the child's process control block.  Upon waking, store the child PCB's `exit_status` in the `status` pointer, release the `cv_lock`, then call `thread_destroy()` on the child and remove its pid from your `child_list` with `remove_pid()`.  Finally, return `pid`.


###2.4.2 `__exit(int exitcode)`###
Iterate through the process control block's `pid_list children`.  For each child, look at the status of the thread of execution.  If it is `S_ZOMBIE`, then call `thread_destroy()` and remove its pid from your child pid list with `remove_pid()`.  If the child is not a zombie, obtain the `lock` of the `orphan_list` in the kernel heap and add the pid of the child to it with `add_pid()`.  Set the `exit_status` field of the current process control block to `_MKWAIT_EXIT(exitstatus)`.  Call `thread_exit()` to do cleanup and set a state of S_ZOMBIE.  Call `cv_broadcast()` on the `waiting_on` cv of the calling process' control block to wake up sleeping parent.

2.5 Other System Calls
----------------------
###2.5.1 `chdir(const char *pathname)`###
use vfs_chdir()
If pathname is NULL, set errno to EFAULT and return 0.  Otherwise, call `vfs_chdir()` on the supplied pathname.  If the VFS call fails, rset the appropriate errno and return -1.  Otherwise, return 0.

###2.5.2 `__getcwd(char *buf, size_t buflen)`###
If `buf` is NULL, set the errno to EFAULT and return NULL.  Call `vfs_getcwd()` on the user supplied buffer.  If the VFS call fails, rset the appropriate errno and return -1.  Otherwise, return the length of the pathname.

###2.5.3 getpid##
Obtain the `pid` of the current process by returning `curthread->pid`. Note that `getpid()` does not fail.

###2.5.4 `kill_curthread()`###
Simply calls `__exit(__WSIGNALED)`

2.6 Scheduling
--------------
Priority-based; aiming to emulate shortest-time-to-completion first

N wait channels of increasing priority - jobs scheduled to run are inserted into the appropriate channel for their priority
Scheduler picks a queue in a weighted random manner and runs the first job in it (ie, the first job in a high priority queue is twice as likely to run as the first job in the next lowest priority queue)
Job priority is recalculated each time it is reinserted to the ready queue:
Priority of jobs that have run for a long time over a bounded time interval are likely to continue to take a long time to run, and thus will receive lower priority
This will attempt to create a shortest time to completion scheduler based on the job's recent run history
This method will require us to track the amount of time the job has been in the 'RUN' state - we can do this by keeping tuples of (time scheduled, time descheduled) in the process struct, treadmilled, and recalculating the time run over the set interval

###Parameters:###
*   Random queue selection
*   Number of priority levels
*   Time threshold for each priority level
*   Length of bounded time interval

We will set the time slice window based on empirical performance on a variety of test cases.

###Performances and Caveats###
Obviously, this has the danger of starvation.  We hope that the appropriate bound on the time interval will ensure threads that have been downgraded in priority several times will be returned to a higher priority eventually.  The weighted random selection of priority queues should also grant less starvation than a deterministic method.

2.7 Synchronization issues
--------------------------

Most of the synchronizaiton issues of our implementation were discussed in detail in our overview, but we will present an overview of the problems anticipated and the synchronizaiton primitives employed to solve them.

Our simple zombie reaping scheme avoids many of the synchronization issues that could occur in wait/exit.  If the parent exits before the child, any non-zombie children will be added to the global orphan list.  Thus, even if a child and parent are racing to execute and the parent wins, the child will be placed in the orphan list before it becomes a zombie, but not long after it status is changed it will be reaped by the dedicated reaping thread.  If the child exits before the parent, it will save it exit status and do some cleanup (and marking itself as a zombie) before signalling its own CV on which the parent is waiting.  This means that as the parent wakes up, the child is guaranteed to have exited and marked its appropriate status, and the parent can do final cleanup.  The downside of this system in which zombies are only reaped by the parent at exit is that a malicious user might spawn many processes and waste the systems pids on zombies.  To combat this an upper limit might be set on the number of children a parent can spawn.

Because processes are single-threaded in our system, we also avoid many IO synchronization issues.  Each process will only have a single thread accessing its `fd_table` at a time, so synchronizaiton is only required at the level of the `ft_ent` structs, which may have several forked processes attempting to operate on them.  Since all operations effectively are changing the state of this file metadata (as well as the vnode itself), a simple mutex lock is employed to ensure updates happen atomically.

The global orphan list is also synchronized with a mutex to ensure that additions of zombies (by exiting parents) and their removal by the reaping thread are atomic operations.

`fork()` employs semaphores for synchronization between the newly spawning child process and the caller.  A semaphore is initialized to 0 and passed to the child through `thread_fork()` called on a helper function.  This semaphore will only be V-ed by the parent on successful initiation of the PCB, thus ensuring that the child, who must V the semaphore from its helper function, can return to usermode and continue executing beyond the system call.

3. Plan of Action & Credit
==========================
![schedule](http://i45.tinypic.com/2eyf9sh.png)

We kept our data structures mostly in place after the peer reviews. We originally had our PCBs as a doubly linked list that lives in kernel heap, but our partnering group pointed out to us that the linking isn't necessary. We learned a lot about how fork/exec is done through our partnering group, although section and OH was even more helpful. We discussed synchronization issues in great depth during the peer review, and those discussions are now reflected in our design. We didn't make other changes to our design due to the peer review, at least not directly.

4. Code Reading Responses
======================
ELF Questions

1. What are the ELF magic numbers?

    The first 4 bytes of e_indent, the array of bytes that specifies how the file should be interpreted, are 3x7f, 'E', 'L', 'F', respectively. The subsequent fields indicate the file class, data encoding, ELF version, OS/syscall ABI identification, and syscall ABI version.

2. What is the difference between `UIO_USERISPACE` and `UIO_USERSPACE`? When should one use `UIO_SYSSPACE` instead?

    `UIO_USERISPACE` and `UIO_USERSPACE` stand for user process code and user process data respectively. One should use UIO_SYSSPACE when writing data to a kernel buffer.

3. Why can the struct uio that is used to read in a segment be allocated on the stack in load_segment() (i.e., where does the memory read actually go)?

    The uio struct contains a iovec, which wraps a buffer that is the destination of the memory read.  The uio, however, also specifies the address space as that of the current thread, so the read happen into the user address space. 

4. In runprogram(), why is it important to call vfs_close() before going to usermode?

    Once we've loaded the executable, we no longer need a reference to the file. If we don't close the vnode before warping to user mode in another process, we will never close the file and have a memory leak.

5. What function forces the processor to switch into usermode? Is this function machine dependent?

    `enter_new_process()`, located in trap.c, forces the processor to switch into usermode. It is machine dependent--Passing argc/argv may use additional stack space on some other platforms, but not on mips.

6. In what file are copyin and copyout defined? memmove? Why can't copyin and copyout be implemented as simply as memmove?

    copyin() and copyout() are defined in copyinout.c and memmove() is defined in memmove.c. copyin()/copyout() copies block of memory across user/kernel addresses ensuring that user pointers are not accessing offlimit addresses, something that memmove() is not capable of doing.

7. What (briefly) is the purpose of userptr_t?

    It is used for noting that the provided address needs to be within the proper userspace region.


Trap/Syscall Questions

1. What is the numerical value of the exception code for a MIPS system call?

    `#define EX_SYS 8`

2. How many bytes is an instruction in MIPS? (Answer this by reading syscall() carefully, not by looking somewhere else.)

    4 bytes, the amount the program counter is advanced before syscall returns.

3. Why do you "probably want to change" the implementation of kill_curthread()?

    We don't want the kernel to panic when a user-level code hits a fatal fault.

4. What would be required to implement a system call that took more than 4 arguments?

    Additional arguments would need to be fetched from the user-level stack starting at sp+16.


MIPS Questions

1. What is the purpose of the SYSCALL macro?

    The SYSCALL() macro allows for a single shared system call dispatcher by loading a number into the v0 register (where the dispatcher expects to find it) and jumping to the shared code.
    
2. What is the MIPS instruction that actually triggers a system call? (Answer this by reading the source in this directory, not looking somewhere else.)

    Line 85 of syscalls-mips.S, which executes the instruction "syscall"
    
3. After reading syscalls-mips.S and syscall.c, you should be prepared to answer the following question: OS/161 supports 64-bit values; lseek() takes and returns a 64-bit offset value. Thus, lseek() takes a 32-bit file handle (arg0), a 64-bit offset (arg1), a 32-bit whence (arg3), and needs to return a 64-bit offset value. In void syscall(struct trapframe *tf) where will you find each of the three arguments (in which registers) and how will you return the 64-bit offset? 

    The first four arguments must be passed in register a0-3, and 64-bit arguments must be in aligned registers.  This means arg0 of lseek() will be in a0, arg1 in registers a2 and a2 with register a1 unused.  The final argument will be found in the user level stack at sp+16.  The 64-bit return value will be stored across registers v0 and v1.