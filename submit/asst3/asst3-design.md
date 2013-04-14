Assignment 2 Write Up
=====================
###Aidan & Willie: White-Faced Storm Petrel-OS###

Table of Contents
=================

1.  Topics
    1.  Data Structures
        1.  Page Table Entries
        2.  Core Map Entries
        3.  Drawbacks
    2.  TLB Design
    3.  Paging
        1.  Eviction (With LRU Clock)
        2.  Swap Space
        3.  Page Fault Handling & Synchronization
    4.  sbrk()
2.  Question Responses
    1.  TLB Questions
    2.  Malloc Questions
    3.   TLB and Paging Algorithms
3.  Analysis of Performance
4.  Integration Plan
5.  Plan of Action

Topics
======
Data Structures
---------------
We will implement both the global core map and the per-process page tables as chained hash tables.  We believe this effectively balances the desire to avoid linear access time with the desire to maintain a manageable size.  The extreme for size conservation would be a simple linked list, which would only be as large as the number of pages (virtual in the case of page tables and physical in the case of the core map) in use, but has linear access time.  The extreme for access time would be a one-to-one hash table or a linked list with an indexing array, which would both give constant access but demand much upfront allocation.  Our compromise would have a hash table of size approximating the number of pages in use on average whose entries look as follows:

    struct table_entry {
        int page_number;
        struct *table_entry next;
        void *data;
    }

Thus, page table entries and core map entries are only differentiated by 1) the type of page number stored as the identifier, and 2) the contents of the data block.

###Page Table Entries###
We will modify our addrspace structures to contain a `table_entry` struct with a `page_table_entry` as its data block.  This means each thread will still contain a single page table, but we can abstract away the initialization, destruction, and copying out to the addrspace methods.  We will additionally add a lock on the page table in the process struct, and require that any updates to the TLB or the process table be done while holding this lock:

    struct lock *pt_lock

The data block for the page table entry will contain the following fields:

    struct pt_entry {
        paddr_t phys_page;
        off_t disk_offset;
    }

Where the first 20 bits of the phys_page are the page number, the last two bits are the dirty and valid bit respectively, and the intervening bits are the protections.

The protections field tracks if the page is readable/writeable, which is important for throwing `VM_FAULT_READONLY`.  The dirty bit will need to be stored at the page table as well as the TLB since a dedicated pager thread will be reading over pages to write to disk even when they are not in the TLB.  The physical page number is the base physical page to which the offset from the virtual address will be added.  `disk_offset` is the offset onto disk at which the page resides in memory, so if there is a page fault the OS knows where to place/retrieve it.

###Core Map Entries###
The core map entry will contain the following fields:

    struct cm_entry {
        bool use_bit;
        struct process_list refs;
        pid_t pid;
        int vpn;
    }

The `use_bit` in the `core_map_entry` is for our eviction policy, which will be implemented as an LRU clock, requiring each allocated physical page to track an extra bit. 

The core map entries contain a pair of pid/vpn to keep track of which processes reference this page of memory.  This is useful for finding free blocks and updating the page table of the referencing process.  We have chosen not to allow multiple processes to share physical pages as a simplifying step, though this would simply require a linked list of pid/vpn pairs and more lock acquisitions during eviction.

###Drawbacks###
The chained hash has worst-case linear lookup time, so the size of the hash table and the choice of hash function will be very important for performance.

While the chained hash may save size in the number of entries allocated it does require two new fields, the identifier and the "next" pointer, one or both of which would not be required for an array or linked list implementation. 

TLB Design
----------
A random TLB replacement policy is chosen as most policies end up fairly close to random anyway and it is more important to have a robust implementation first before optimizing. To evict a random TLB entry, use a combination of `tlb_write()` and `random()` (since `tlb_random()` reserves 8 of the TLB entries). On every context switch, clear the entire TLB instead of making use of the address space ids to adhere to Margo's Mantra as is already done with `as_activate()`.

To implement TLB handling, modify `vm_fault()`, the function called on TLB exceptions (see trap.c). Note that the `TLBLO_DIRTY` bit in the TLB is actually a write privilege bit, not ever set by the processor. If set, writes are permitted, else `EX_MOD` exception is raised when a write is attempted. Load all entries into the TLB without setting the `TLBLO_DIRTY` bit. Two scenarios are possible if a write happens to a page referenced by a TLB entry:

1.  If the `TLBLO_DIRTY` is not set, then an `EX_MOD` exception is raised. Handle this `VM_FAULT_READONLY` exception in `vm_fault()` by setting `TLBLO_DIRTY` and marking the corresponding page dirty. 
2.  If the `TLBLO_DIRTY` is set, this means that the page has already been marked dirty and no exception is raised.

The `TLBLO_DIRTY` bit needs to be unset any time we wish to mark a dirty page clean (after writing it to disk for instance).

In general, `vm_fault()` will handle perform the same error checking that it already does from the distribution code when handling `VM_FAULT_READ` and `VM_FAULT_WRITE`, but it will also:
1.  Attempt to find the physical address via the page table (after first acquiring the page table lock)
2.  If unsuccessful, load the appropriate page into the page table via a "page fault". Note that this is in quotes since we are already in the kernel and don't need an actual fault to trap the code. The details of this is covered in the section on page faults.
3.  Create the TLB entry and load it into the TLB. Evict randomly if necessary.
4.  Restart at fault address and return to usermode

Note that because the TLB is simply a cache and `tlb_read()` waits 2 cycles to ensure that entryhi/lo are both set, turning interrupts off is a sufficient synchronization for accessing the TLB.

Paging
------
###Eviction (With LRU Clock)###
We employ a per-page "busy bit" to synchronize access.  The advantages are twofold: firstly, a per-page lock is very space intensive, and second, when choosing a page to evict we do not want to block waiting.  If we check business (i.e., being in the process of eviction) by acquiring a global lock, testing and setting if 0, then releasing the lock, we will never block while maintaining mutual exclusion. 

    struct lock *busy_lock;
    struct bitmap busy_bits[num_ppages];

We will be implementing an LRU clock to determine pages to evict.  When a page fault happens and there is no space in physical memory, a global "clock" will iterate cyclically through the rage of physical pages available to the user until one is found with either no `core_map_entry` (unused) or the "`busy_bit`" and "`use_bit`" of its `core_map_entry` set to 0, setting all intervening ones to 0 along the way, then return the physical page number to evict.  To do this, we will declare a global "hand" that indexes physical page numbers starting at 0:

    int clock_hand;
    static int choose_evict();

While this incurs a time complexity penalty over a random eviction policy (worst case linear in the number of physical pages, assuming constant lookup, which with a chained hash table implementation of the core map would make it worse-worse case quadratic...) we believe this would save us time in the long run as disk operations are extremely expensive and LRU has better locality guarantees.

The setting of the "`use_bit`" is a bit tricky. It is unreasonable to set the use bit on every memory access. In fact, the use bit can only be set during a TLB fault. We could just accept that use bits are only set during TLB faults. Another possible way to ensure that use bits that are cleared get set again is to forcibly probe and remove an address from the TLB whenever its use bit is cleared. However this seems expensive (and unnecessary?), so the compromise that we will pursue is to probe the TLB to see if the virtual address is there and only evict if both the use bit is not set and it is not in the TLB. Note that these are parameters (or more accurately modified algorithms) that we will be trying out. This is further discussed in the Analysis of Performance section.

###Swap Space###
A global bitmap will be employed to keep track of free disk space.   Operations on the bitmap (`bitmap_alloc`, `bitmap_mark`, `bitmap_unmark`) will be controlled by a spinlock.  These will be declared in vm.c.

    struct bitmap *disk_map;
    struct lock *disk_map_lock;

The bitmap will be of size at least kuseg/pagesize. When a page is created, it will be assigned a space on the disk by a call to `bitmap_alloc`, which will find an unused slot of the disk, mark the space occupied, then return the index.  This index will be saved in the `core_map_entry` corresponding to the physical page, and will be used as the offset for `VOP_READ` and `VOP_WRITE` operations to move the page into/out of swap space.

###Page Fault Handling & Synchronization###
Page faults will be handled in the following fashion:

Note: Because we can only trigger a page fault after a TLB miss, we will begin our fault handler holding the lock on our own page table.

####Page not present####
*   Pick a page by the LRU clock. Then acquire the global "`busy_lock`", and if the busy bit is not set, set it, release the lock and return the index.  This guarantees that we acquire a page that is not in the middle of being evicted by another process.
*   If the chosen page P is unmapped, simply update our own page table, and add an entry to the core map linking P to the faulting address in the current process.  Then acquire the `busy_lock`, free the busy bit, release the `busy_lock` and the current process' `pt_lock` and restart the instruction.
*   If the chosen page P is mapped, then we will have to evict it and update the referencing page table.  To avoid deadlock (where A tries to acquire B's lock while holding its own and visa versa) we will release our own `pt_lock` and reacquire it and the `pt_lock` of the process from P's core map entry in the order of pid (let's call this process B).
*   If P is not dirty, then there exists some copy of it in backing store and we don't have to write it to disk.  Thus, we simply find the `pt_ent` for P in B's page table and set the valid bit of the `ppage` field to 0.
*   If P is dirty, we must first write it to disk at the offset specified by the `disk_offset` field of B's page table entry for P, then set the valid bit of the `ppage` field to 0.
*   At this point, we have guaranteed that the page table entry in B is invalidated for P, but it might yet reside in the TLB.  Since we want to prevent all accesses to P while we evict it, we will execute a `tlb_shootdown(P)` to remove any entries for P from all TLBs.  Since this functionality for shootdown of a specific entry is not yet implemented, we will have to do so, employing an interprocessor interrupt.
*   This completes the eviction of P, so we can now read the page located by the `disk_offset` field of our page table entry into P.  We then update our `pt_ent` for the faulting address to be marked valid (by flipping the "valid" bit of `ppage`).
*   We now update the core map entry for P to contain the pid/vpn pair of the current process/faulting page.  Since we still hold the busy bit for this page, we do not need to synchronize this update further.
*   Finally, release the busy bit (wrapped by acquiring/releasing `busy_lock`), the two `pt_lock`s, and restart the instruction.

####Page DNE/First Attempt to Write####
If the page is being accessed for the first time (i.e., it does not have swap space), then we will have to modify the above handler in two ways:

*   Instead of reading a page from disk into the chosen location P, we simply zero the memory at P so we can't access someone else's page
*   Instead of updating the page table entry, we will first have to create one.  For this, we will have to acquire `disk_map_lock`, call `bitmap_alloc(disk_map)` to obtain an offset into swap space.

sbrk()
-------
    static size_t sbrk_region_size
    static vaddress_t current_break;

    sbrk(size){
        // Do initialization the first time if necessary?
        
        if (size <= 0)
            return(current_break);
        current_break += size;
        sbrk_region_size -= size;
        if (sbrk_region_size < 0)
            return -1;
        return(current_break - size);
    }

Question Responses
==================
TLB Questions
-------------
###Problem 1 (5 points)###
Assuming that a user program just accessed a piece of data at (virtual) address X, describe the conditions under which each of the following can arise. If the situation cannot happen, answer "impossible" and explain why it cannot occur.

*   TLB miss, page fault - the user references a page that is on disk or has not yet been created
*   TLB miss, no page fault - the user references a page that was evicted from the TLB but still resides (ie., has not been written to swap space)
*   TLB hit, page fault - impossible; pages with TLB entries are always in physical memory.  When a page is written to disk, the TLB should be updated to reflect this
*   TLB hit, no page fault - the user references a page that is still in the TLB, and, if their eviction policy is good, that they have recently referenced

###Problem 2 (2 points)###
A friend of yours who foolishly decided not to take 161, but who likes OS/161, implemented a TLB that has room for only one entry, and experienced a bug that caused a user-level instruction to generate a TLB fault infinitely: the instruction never completed executing! Explain how this could happen. (Note that after OS/161 handles an exception, it restarts the instruction that caused the exception.)

On the first access of some memory address by the user there will be a TLB miss as the user has not accessed it yet.  After this, the TLB will evict its only entry, which was presumably the code segment the user was executing.  When the user tries to resume execution of the code, they will get another TLB miss and evict the entry again.  This will repeat from the beginning infinitely.

###Problem 3 (3 points)###
How many memory-related exceptions (i.e., hardware exceptions and other software exceptional conditions) can the following MIPS-like instruction raise? Explain the cause of each exception.
    # load word from $0 (zero register), offset 0x120, into register $3
    lw  $3, 0x0120($0)

*   TLB miss - The address 0x0120 is not in the TLB
*   Page fault - The address 0x0120 is not in the page table
*   Addressing error (invalid address) - The address 0x0120 is invalid/not accessible

Malloc Questions
----------------
###Question 1.###
How many times does the system call `sbrk()` get called from within `malloc()`?

There are up to two `sbrk()` calls during the one time initialization (if `__heapbase` == 0), once to find the base of the heap and once to align the heap base if necessary. In subsequent calls to `malloc()`, `sbrk()` is only called If nothing was found to expand the heap inside call to `__malloc_sbrk()`.

On the i386 platform, what is the numeric value of (finish - start)?
    
The relevant piece of code is that the heap is expanded with:

    mh = __malloc_sbrk(size + MBLOCKSIZE)

Here the size is 16 (after alignment) and `MBLOCKSIZE` is defined to be 8, so the program break is incremented in increments of 24 bytes. Since there are 10 iterations, finish-start = 24*10 = 240


###Question 2.###
Again on the i386, would `malloc()` call `sbrk()` when doing that last allocation at the marked line above? What can you say about x?

No, `malloc()` would not need to call `sbrk()` to expand the heap even though `free()` only tries to merge adjacent blocks. `free()` will merge the 6 freed blocks into 2 sets of 3 * 24 = 72 byte blocks. The block size necessary for `malloc(60)` after alignment is 64 + 8 = 72, just the right size.

    x = start + 24 (the location of res[1])
    

###Question 3.###
It is conventional for libc internal functions and variables to be prefaced with "`__`". Why do you think this is so?

They are identifiers reserved to the implementation for future extensions to the C language or POSIX.
    
###Question 4.###
The man page for malloc requires that "the pointer returned must be suitably aligned for use with any data type." How does our implementation of malloc guarantee this?
    
    /* Round size up to an integral number of blocks. */
    size = ((size + MBLOCKSIZE - 1) & ~(size_t)(MBLOCKSIZE-1));

TLB and Paging Algorithms
-------------------------
Question: How will you structure your code (where will you place your TLB and paging algorithms) so that others can be added trivially, and switching between them only requires reconfiguring your kernel?

See Integration Plan section below.

Analysis of Performance
=======================
Our TLB eviction policy is random, which should perform fairly well given a large enough TLB without having any performance/memory overhead of having to maintain stats necessary for more complex eviction policy. Additionally, random should guard against antagonistic memory access patterns well as there is no particular access pattern that is guaranteed to cause thrashing.

Our page eviction policy attempts at using a LRU clock, but because we can't trap on every single memory access, the use bit is only marked when the access incurs a TLB fault, so it is unclear how good of an estimate for LRU this really is.

We will maintain the following useful statistics most of which (except the last 2) can be found by simply counting the entries in the coremap:

*   Total number of pages available
*   Total number of pages managed by you
*   Number of clean pages
*   Number of dirty pages
*   Number of kernel pages
*   Number of TLB evictions
*   Number of page evictions

Using these statistics we can tune the following parameters:

*   The size of our hash table (which will be the average number of virtual/physical pages)
*   Tune the policy for page eviction:
    1.  Accept that use bits of the LRU clock is updated only during TLB faults
    2.  Forcibly evict pages from the TLB when clearing the used bits to make the LRU clock more accurate
    3.  Probe the TLB to see if the virtual address is there and only evict if both the use bit is not set and it is not in the TLB.

Integration Plan
================
Since the coremap is a global data structure, it should be declared in vm.h and initialized in `vm_bootstrap()`

`vm_boostrap()` will be responsible for initializing the following global structures with `ram_stealmem()`, as `kmalloc()` is unavailable until the bootstrapping is complete:

    struct table_entry *core_map;
    struct bitmap *disk_map;
    struct bitmap *busy_bits;
    struct lock *disk_map_lock;
    struct lock *busy_lock;

We will additionally have to modify `as_create()` and `as_destroy()` to initialize/deconstruct the per-process page table and page table lock:

    struct table_entry *page_table;
    struct lock *pt_lock;


Because we are enforcing that a physical page can belong to only one process (and as such we only need one pid/vpn per `core_map`), we have to handle this in `as_copy()`.  `as_copy()` will have to go through each page in the page table and find a new place in physical memory for it.  Since much of this is already implemented in our page fault handler, we will abstract out the following function in vm.c:

    static void load_page(struct addrspace *as, int vpn)

That will find a location, perform eviction, and load the page from swap space or zero it out if it hasn't been referenced yet.  `as_copy` will additionally have to copy the added structures - the page table and the `pt_lock()` among others.

Plan of Action
==============
*   Sun: Data Structures
*   Mon (1st): Set up tools for debugging/performance analysis
*   Tues: TLB
*   Weds: TLB
*   Thurs: srbk & TLB testing
*   Fri: Start paging
*   Sat (6th): Paging focusing on bootstrapping 
*   Sun: Paging focusing on Swapping
*   Mon: Paging focusing on Swapping
*   Tues: Paging focusing on synchronization
*   Weds: Finish up paging
*   Thurs: Testing/performance tuning
*   Fri (12th): Due at 5 pm
