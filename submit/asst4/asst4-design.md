Assignment 4 Write Up
=====================
###Aidan & Willie: White-Faced Storm Petrel-OS###

Table of Contents
=================

1.  Topics
    1.  Transaction
    2.  The Journal (Log)
        1.  Records/Journal Buffer
        2.  Commits
        3.  Checkpoints
            1.  Synchronization
            2.  One More Issue
    3.  Recovery
    4.  Buffer Cache
    5.  Bootstrap
2.  Plan of Action
3.  Question Responses
4.  Peer Review Responses
    1.  Responses for Petrel OS
    2.  Responses for Otter OS

Topics
======
Transaction
-----------
The following calls initiate a transaction, so they are responsible for constructing the transaction struct and pass it to their helper functions:

    sts_creat()
    sfs_mkdir()
    sfs_write()
    sfs_remove()
    sfs_rename()
    sfs_truncate()

We will need to modify the helper functions to these function to take a pointer to the transaction that they are performing on. The transaction struct will be defined as:

    struct transaction {
        unsigned id;
        struct array *bufs /* dynamic array of buffers that the transaction has used */
    }

The logging for each of the metadata changes will happen in the following helper functions:

Changes to bitmap:

    sfs_balloc
    sfs_bfree
Changes to directory:

    sfs_dir_link (grab the return value of sfs_writedir)
    sfs_dir_unlink
Changes to inodes:

    sfs_io (size [line 719])
    sfs_truncate (size, direct block [line 1685], indirect [1935], dindirect, tindirect)
    sfs_loadvnode (type)
    sfs_bmap (direct blocks [294], indirect [364], dindirect, tindirect)
Changes to directory linkcount:

    sfs_creat
    sfs_link
    sfs_mkdir
    sfs_rmdir
    sfs_rename

The Journal (Log)
-----------------
We will implement the following functions to interace with our journal:

    create_transaction() // Returns the transaction struct to be passed around
    place_record() // In the journal buffer
    commit_transaction() // Flush the journal buffer
    initiate_checkpoint()
    recover()

We plan on implementing REDO journaling with no BEGIN or ABORT log entries. Our log records are designed to be idempotent to withstand crashes during crash recovery. The journal will be N blocks and live in a defiend location on disk so that we can located each time during boot. At the head of the journal will be a metadata block on the journal:

    struct journal_metadata {
        unsigned num_entries; // If zero, no recovery necessary
    }

We will define the following relevant constants:

    #define JOURNAL_METADATA_BLOCK
    #define JOURNAL_RECORD_START
    #define JOURNAL_RECORD_END

The log records will immediately follow the metadata block. Multiple records will live in a block, so we will pad records in such a way that their size is a divisor of block size. When writing records, we will access the correct index with: `JOURNAL_RECORD_START + sizeof(struct record) * num_entries`. A record will be structured as such:

    struct record {
        unsigned transaction_id;
        int type; /* commit, inode change, directory change, or bitmap change */
        union changed {
            struct inode {
                /* Question: should we log the physical or logical block number? */
                /* Does it matter? */
        uint32_t sfi_size;
        uint16_t sfi_type;
        uint16_t sfi_linkcount;
        uint32_t fileblock;
        uint32_t sfi_indirect;
        // Do we need to log sfi_waste?
            }
            struct directory {
                char * sfd_name[SFS_NAMELEN]
                unsigned ino; // of directory that owns this directory
                unsigned slot_num; // the slot in this directory
            }
            struct bitmap {
                int freemap_index;
                int setting; /* whether the above index should be 0 or 1 */ 
            }
        }
    }

Since we are doing REDO journaling, we don’t believe we actually need the initial values (reflected above), but we made record them for debugging purposes initially.

###Records/Journal Buffer###
The records first be written to a journal buffer (which will be allocated with a set size beforehand) whenever a relevant change is made in the relevant helper functions (see the Transaction section). Note that as long as a record is written to disk first before the file system change makes it to disk, we've maintained write ahead logging. In other words, it is inconsequential whether the records make it to the journal buffer first or if the file system change makes it to the buffer cache first. This journal buffer will be protected with a big lock, which will be used to ensure both mutual write access and synchronous writes to disk. If the journal buffer ever fills up, we simply flush it to the journal on disk.

###Commits###
A commit record will be made at the end of a transaction (when the function that created the transaction in the first place is about to terminate). We will make transaction commits synchronous with writes of everything in the journal buffer to disk so that we can be sure that we can recover said transaction once it is committed. Note that when we make a commit and write to disk, incomplete transactions may be written to disk as well. This is fine as the log buffer is simply an optimization. After all, we could have simply made every write to the log synchronously (but that would have been really slow). If we successfully flush the journal buffer on a commit, we can zero the buffer and reuse it. Note that we should never update the `num_entries` in the journal metadata block until the relevant record has been written.

###Checkpoints###
We will maintain a counter in memory of the total number of records that exist (both on disk and in memory) so that we can initiate a checkpoint when a certain percentage of the on disk journal is filled. (Practically speaking, we should also make sure that our in memory journal buffer doesn’t fill up, but that is not as important as we could alternatively allocate more memory for the journal buffer. The same is not true for the on-disk journal.) Note that this counter does not ever have to be written to disk. It should increment every time a record is placed in the journal buffer and reset every time a checkpoint is complete.

Using said counter, we initiate a checkpoint when the free space of our on disk journal space reaches a certain fraction of the total space (set large enough such that we are reasonably confident that we have enough space for the ongoing transactions to be logged to completion). When a checkpoint is initiated, no new transactions are allowed to start (thus all io syscalls will be blocked). The ongoing transactions will continue to be logged like they normally do. This will ensure that once all the ongoing transactions are complete, the entire log will be flushed to disk anyway. Once all ongoing transactions are complete (so we will need to maintain a counter of transactions in progress), a "checkpoint" is made by setting the `num_entries` from the journal metadata to 0 (effectively deleting the journal) and zeroing all the records. We can do this because once a checkpoint is made, we should no longer need to maintain any metadata changes in the log. By clearing the journal at each checkpoint, we also make it clear during mount whether we need to do any recovery. Note that we can also reset the transaction id at this point.

####Synchronization####
When there are active transactions, we do not want to begin transactions.  When we begin a checkpoint, we do not want any other transactions to begin.  In order to allow both of these exclusions, we plan to utilize two CVs - `no_active_transactions` and `checkpoint_cleared`.  We will maintain variables in addition to these CVs and associated locks - `num_active_transactions`, updates to which will be locked with `transaction_lock`, and `in_checkpoint`, updates to which will be locked with `checkpoint_lock`. 

When a checkpoint is triggered on a hardclock, it will acquire the `transaction_lock` and the `checkpoint_lock` in that order.  If `num_active_transactions` is 0, it will set `in_checkpoint` to true, release both locks and proceed.  At the end of the checkpoint, it will reacquire the `checkpoint_lock`, set `in_checkpoint` to false, and broadcast `checkpoint_cleared`, finally releasing the `checkpoint_lock`.  If there are active transactions, it will release the `checkpoint_lock` and sleep on `no_active_transactions`, reacquiring the `checkpoint_lock` upon waking.

When a transaction attempts to begin, it will acquire the `transaction_lock` and `checkpoint_lock` in that order.  If there is a checkpoint active, it will release the `checkpoint_lock` and sleep on `checkpoint_cleared`, reacquiring the lock upon waking.  If `in_checkpoint` is false, then increment `num_active_transactions`, release both locks, and proceed.  After a transaction is finished, acquire the `transaction_lock`, decrement the transaction count, and if it hits zero, signal on `no_active_transactions`, finally releasing the lock and finishing.

####One More Issue####
A potential issue is that it is possible to run into a scenario in which even if we start a checkpoint with enough journal space left, we still run out of space as the in progress journals could make too many metadata changes. For now, we don’t have a resolution for this scenario and it is unclear whether there is a resolution without sacrificing performance (ie doing more checkpointing/writes).

###Recovery###
We know that we need to recover on boot when `num_entries` in the journal metadata is set to any value other than 0. To ensure this invariant, the very last thing we need to do when shutting down is to checkpoint.

To recover, first make a pass to find all the transactions that actually committed--these are the only transactions that we need to REDO. Once the first pass is complete, make another pass through the journal, this time applying the changes for transactions that were committed. We can update all the metadata with the final values indicated in the journal. Once recovery is complete, we can update the journal offset to indicate that the journal is no longer needed and the file system is intact.

###Buffer Cache###
We need to modify the buffer cache to hold a transaction ref count of the transactions that are touching said buffer. The ref count is incremented when a the creation of a record in the journal buffer adds a new buffer cache to the transaction’s list of touched buffer cache. On a commit (after writing to disk), iterate through the transaction’s list of buffers and decrement the buffer’s transaction count. We enforce that a buffer can't be flushed unless the ref count is 0. Checkpointing would effectively bring all of the transaction ref counts in all the buffer to 0.

###Bootstrap###
The bootstrapping for journaling will be done when SFS is mounted through the user code defined in `mksfs.c`. Before we can bootstrap though, we need to first go through the recovery sequence. Once recovery is compelte, or if no recovery is necessary, we need to zero out the space on disk that we reserve for your journal and mark all those blocks as allocated.

Plan of Action
--------------
    4/21 (S) - Data structures
    4/22 - Modifying SFS calls for transactions
    4/23 - Buffering log entries
    4/24 - Checkpointing complete
    4/25 - Testing checkpointing with known programs
    4/26 (F) - Code redo logic
    4/27 (S) - Code/test redo logic
    4/28 (S) - Test scenarios for recovery
    4/29 - Aidan leaves for Oxford, test and collaborate remotely
    4/30 - Test and collaborate remotely
    5/1 - Aidan returns from Oxford, Test
    5/2 - Test
    5/3 (F) - Due

Code Reading
------------
1.Why does a system call such as `sys_remove` not require interfacing with your specific filetable implementation? In particular, at a high level, what happens if `sys_remove` is called on a file that is currently "open" by another    running thread? Will a read on the file by the second thread succeed? Why or why not?

`sys_remove` is concerned with removing the vnode for a file from the VFS, and does not concern itself with updating user processes as to this removal.  As such, it remains independent of our filetable implementation, though this could cause problems for other processes with the reference.  Processes calling this function should probably call `close()` before to remove the file table reference, but after `remove()` another process may retain the reference.  Since the vnode doesn’t exist, the call to `VOP_READ()` on the vnode pointer stored in the file table will fail.  

2.Describe the sequence of events that occur in both the VFS and SFS layers for each of the following system calls:

###`SYS_open`###
        
`vfs_open()` is called to handle most of this function    
    
If called with `O_CREAT`, calls `vfs_lookparent()` which in turn calls `getdevice()` after locking the VFS.  This will return the vnode for the device in the path, if it exists.  This is then passed to `VOP_LOOKPARENT()` and `VOP_DECREF()`.
Returning to `vfs_open()`, the name of the file to be created is passed to `VOP_CREAT()` and `VOP_DECREF` is called on its directory.

`VOP_CREAT()` calls `sfs_creat()` in `sfs_vnode.c`.  This in turn checks if the file already exists with `sfs_dir_findname()` and fails if it does and excl is not set.

If it exists, call `sfs_loadvnode()`, which loops through the vnodes until one is found with the inode number matching. If it is not loaded, space is allocated and it is read in with `sfs_readblock()`.  After this, set its ops to either `sfs_fileops` or `sfs_dirops` accordingly, call `VOP_INIT()` and add it to the table with `vnodearray_add()`.

If it doesn’t exist, `sfs_makeobj()` is called, which in turn calls `sfs_balloc()` to obtain a free space on disk through `bitmap_alloc()`.  The block is zeroed with `sfs_clearblock()` before being handed back to `sfs_makeobj()`.  The inode number from `sfs_balloc()` is then passed to `sfs_loadvnode()` which loads the corresponding vnode into memory.

If `O_CREAT` is not set, `vfs_open()` simply calls vfs_lookup(), which operates much as `vfs_lookparent()` but with the call to `VOP_LOOKPARENT()` replaced with `VOP_LOOKUP()` since the vnode should already exist.

Now that the vnode for the file is obtained (or created), it is passed to `VOP_OPEN()`, which in turn calls `sfs_open()`, which currently does nothing as we are not supporting `O_APPEND`.  `VOP_INCOPEN()` is called to increment the open count on success.  

If `O_TRUNC` is set, `VOP_TRUNCATE()` is called if writing is allowed, which in turn calls `sfs_truncate()`.  This first iterates through the direct blocks and calls `sfs_bfree()` which frees their disk indices with `bitmap_unmark()`.  If the indirect block contains references to blocks past the new EOF, we read the indirect block then iterate through the blocks it points to and free them with `sfs_bfree()`.  After this the indirect block is either written out with sfs_wblock() if it is dirty or immediately freed with `sfs_bfree()`.  Finally the vnode length is updated.

###`SYS_write`###

This call enters the VFS system with `VOP_WRITE()`, which in turn calls
`sfs_write()`, which wraps a call to `sfs_io()` with acquisition/release of the VFS 
biglock.

`sfs_io()` for `UIO_WRITE` will first check if the offset falls in the middle of a block.  If it does, it will call `sfs_partialio()` to write the data starting from the index into the block to either the end of the block or the end of the bytes to be written.  After this, it iterates through the blocks and calls `sfs_blockio()` to perform as many whole-block writes as possible.  If there remains a non-block-aligned portion at the end `sfs_patialio()` will be invoked again.  

`sfs_blockio()` uses `sfs_bmap()` to find the disk block number.  Since we are writing, `sfs_bmap()` will call `sfs_balloc()` to obtain a block number to write to (and in the case of an indirect block, will allocate both the indirect block and the data block to be written as needed).  After obtaining the block, writes will update the uio struct such it specifies only the region of the block, then call `sfs_rwblock()` to complete the write.

`sfs_partialio()` operates very similarly to the above but accounts for the skipped region at the beginning of the block.

Finally, returning to `sfs_io()`, the vnode length is updated.

###`SYS_mkdir`###

The entry point to the VFS framework for this system call is `vfs_mkdir()`.  This will first call `vfs_lookparent()` to find the parent directory.

After a call to `getdevice()`, the returned vnode is passed to `VOP_LOOKPARENT()` and thus in turn to `sfs_lookparent()`.  This function locks the VFS, then simply increments the vnode argument’s reference count with `VOP_INCREF` and returns it.  This is because we do not support subdirectories so the root directory will always be passed to and returned from this function.

After the parent (root) directory is obtained, `vfs_mkdir()` then passes it to `VOP_MKDIR()` along with the directory name, which in turn wraps `sfs_mkdir()`.

`sfs_mkdir()` will call `sfs_load_inode()`, which calls `buffer_read()` to load the inode of the vnode into the sfs_vnode buffer.  `buffer_map()` is then called to get a pointer to the buffer data.  We then check if the directory already exists with `sfs_dir_findname()`, which iterates through the directory entries, reading them with `sfs_readdir()` and comparing their names to the passed argument. 

If the directory doesn’t already exist, then we go to `sfs_makeobj()`.  This calls `sfs_balloc()` to get a data block number, then calls `sfs_loadvnode()` with it.  This function iterates through the vnodes of the file system looking for one with a matching number.  If a match is found, `VOP_INCREF()` is called on it and the inode is loaded with `sfs_load_inode()`.  If the vnode is not found, then `sfs_create_vnode()` is called to malloc it and `buffer_read()` is used to read the inode into it.  The `sfs_vnode` will be initialized with `VOP_INIT()` to have directory operations, and finally the vnode is added to the table with `vnodearray_add()`.

`sfs_dir_link()` is then called three times, on ".", "..", and the name of the new directory.  In this function an `sfs_dir` struct is created and initialized with the name and inode, then passed along to `sfs_writedir()`.

Back in `sfs_mkdir()`, the reference counts for the inodes of the created directory and its parent are updated.  Then, for both vnodes the function has been operating on, the buffers are marked dirty with `buffer_mark_dirty()` then the inodes in the vnode for said directories are released with `sfs_release_inode()` and cleanup is performed.

3.Describe 4 different types of inconsistent states the filesystem could end up in after a crash, and  give a sequence of events that could lead to each. You will probably want to explain in your design doc how you handle these problems. 

i) User has access to an inode for a removed file.  This could happen if another user attempts to delete a file but the system crashes before the file system on disk can be updated, leaving other users with access to a file that should have been removed.

ii) User has a file whose inodes point to blocks past EOF.  This could happen if another user truncates the file but a crash occurs before the changes are logged to disk, leaving other users the ability to access data they should not.

iii) A file exists in no directory without having been removed.  This could happen if a user attempts to move a file but the system crashes in between updating the inode of the directory from which it is removed and the update of the one it is to be added to.  An alternative issue could arise if the order of these updates is swapped and the file exists in two directories simultaneously.

iv) A directory has an incorrect link count.  This could happen if a user creates a file/directory but the system crashes before the `link_count` field of the directory can be updated.  In our system, this is only an issue for creating a file as we do not support the creation of sub-directories.

4.Explain what the current implementation of sfsck can and cannot do.

`check_sb` (Superblock):

-   Checks if it is an SFS file system
-   Checks for valid volume name, but does not add null terminator or remove invalid character if they exist.

`check_root_dir`:

-   Checks if root directory is actually a file, and if so updates its type to directory and updates the data on disk to match
-   After this check, will call `check_dir()`, which should be the only such call as we do not support sub-directories

`check_dir`:

-   If the directory struct is of incorrect size, it rounds it up to the correct size
-   Then calls `check_inode_blocks()` on directory inode, which checks if the directory inode points to any blocks past the end of the file.  If any such blocks exist, the inodes pointers to them are invalidated and the blocks are zeroed.
-   If the directory has a name but no file, the name is set to null
-   If the directory has an inode but no name, it is assigned a unique name based on its inode number
-   If the directory has an invalid name, it will not be fixed
-   If a directory has duplicate entries with the same inode, all but one are invalidated
-   If a directory has duplicate entries for different inodes, all but one are renamed with a unique name based on their inode number
-   If the "." entry of the directory does not have the inode of the current directory, it is updated to point to it correctly
-   If the ".." entry of the directory does not have the inode of the current directory’s parent (null, in our case), it is updated to point to it correctly
-   If no "." and/or ".." entry is seen, adding it/them is attempted
-   If the directory contains a subdirectory or an entry of invalid type, the entry is invalidated (no name, inode = NOINO)
-   Directory `link_count` is set to number of entries+2 ("." and "..") if it is not already

`check_bitmap()`:

-   Iterates through blocks and fixes any incorrectly marked bits in the bitmap (ie., free when marked used or visa versa) then writes changes to disk.

`adjust_filelinks()`:

-   Reads each sfs inode from disk and checks to see if its link count matches that stored in the inode array.  If not, the count is corrected and the updated sfs inode is written to disk.

Peer Review Responses
=====================

###Responses for Petrel OS:###

    1. The Log/Journal
    What is the interface to your log (journal)?

    The following functions will be implemented to interface with the journal:
    create_transaction() // Returns the transaction struct to be passed around
    place_record() // In the journal buffer
    commit_transaction() // Flush the journal buffer
    initiate_checkpoint()
    recover()

    How will you implement your log?

    There will be a list of `record` structs buffered in physical memory (at a fixed location) that will be written out to disk on commits or when the buffer is nearly full.

    The journal buffer will be zeroed after writing out to disk and the journal on disk will be zeroed out on checkpoints.

    Will it be an UNDO log, a REDO log, or both?

    REDO only

    How will you enforce write-ahead-logging?

    Each buffer will maintain a transaction counter to track all those that are operating on it.  Once a transaction completes (and is written out), it decrements the counter(s) on any buffers it operates on.  A buffer will only be written out when its transaction counter hits 0.

    How will you know where the beginning and end of the log are after a failure?

    The journal will always begin at the first location after a failure, since a checkpoint zeroes the entire journal.  The end of the journal is tracked by the num_entries field of the journal metadata struct.

    What will checkpoints look like? How often will you take them?  (A checkpoint is way of ensuring that all the updates corresponding to some set of log records have been propagated to disk; checkpoints let you bound how much of the log you need to use at recovery time.)

    Checkpointing will be handled with two condition variables to prevent new transactions starting during a checkpoint and to prevent checkpoints starting when there are active transactions (described in synchronization section).  Checkpoint will effect all changes, zero the journal and set number of entries to 0.  Will be triggered on hardclock.
    How/when will you reclaim log space?

    After every checkpoint the entire journal will be reclaimed.

    2. The Recovery Process

    What is your algorithm that must be executed after a failure to restore the file system to a consistent state?

    Fetch the journal and iterate through each entry, effecting the changes described in order.

    Are there any steps you need to take prior to running recovery?

    Check if there are any entries in the journal - if not, do not need to recover.

    What assumptions can your recovery process make?

    We assume that recovery can be attempted any number of times as long as the logs are read in the correct order.

    What guarantees will your recovery process make? (That is, what can you say about the state of the system after recovery runs.)

    The file system will be in a consistent state and all logged changes have been effected (empty log).

    3. Specific Log Records

    For each system call operation, what information must be logged?

    Any changes to inodes, directories (name, link count, or inode), or free block bitmap must be logged.

    Where can you make calls to logging routines?

    The functions that will call logging methods are detailed in the transaction section, but all functions below entry-level SFS calls must be modified to take a transaction struct argument.

    How will you propagate information about the log to the locations at which you need to write log records?

    As mentioned above, all SFS functions below the entry level must be modified to take a transaction object which will be passed down the chain until one of the critical logging functions is called.

    What recovery actions are needed for each log record? Have you specified enough information in your log records to facilitate what you need during recovery?

    The type of change (directory, inode, bitmap), the identifier (inode number, bit index, etc), and the change itself (free/alloc, linkcount etc.) should be sufficient to effect the change.

    Is there a set of micro-operations that comprise all the system call APIs? That is, rather than thinking about a log record per system call, is there a set of records that can be mixed and matched to construct what's necessary for all the APIs? It may be helpful to think about what operations can be performed on what data structures and how those can be combined?
    The systems calls are effectively combinations of the functions that are listed as necessary callers of log operations in the Transaction section.

    4. Testing

    How will you test your system?
    Can you outline a testing philosophy or strategy that will let you approach testing in a methodical fashion?
    Can you perform unit testing?  How?

    The first testing phase will consist of executing set operations and examining the print of the buffered log to determine that all entries are buffered correctly and in the right order with respect to individual transactions.

    The next phase will consist of testing the writing out of the log to disk, ensuring that the log is written exactly as it is buffered without new transactions entering.

    The final phase of testing will print the log after a crash for set points in a known program and ascertaining if the operations for recovery are correctly read back and are sufficient to restore the state.  The end goal will be to then execute these changes successfully.

    It seems to make sense to test in this order as creating logs and replaying them seems troublesome - testing the replaying should come after logging.

    5. Miscellaneous

    Are there any tools/utilities you might build to make it easy to read/write log records?

    A "map" function that takes a function pointer to operate on all logs could be useful, especially when combined with a print function for log entries.

    What features can you design into your system to facilitate debugging?

    See above

    While you may assume there is no file system activity while you run recovery, there will almost certainly be activity while you write log records, take checkpoint, and reclaim log space.  What kind of synchronization will you need for that?

    A single mutex will enforce writer separation for logging and checkpointing will be handled by a pair of CVs and their associated locks, as described above.

###Responses for Otter OS:###

    1. The Log/Journal



    What is the interface to your log (journal)?

    There are functions to create journal entries, write journal entries to log, and commit transaction (write multiple journal entries to disk).



    How will you implement your log?

    Log will be a series of log entries that is buffered in memory and lives on disk.

    Will it be an UNDO log, a REDO log, or both?

    REDO log only



    How will you enforce write-ahead-logging?

    Keep a transaction count in each buffer and only allow buffer to be written to disk if it has no outstanding transactions (whose journal entries need to be written to disk).



    How will you know where the beginning and end of the log are after a failure?

    This information is stored in the journal summary block which has a fixed location on disk.

    What will checkpoints look like? How often will you take them?  (A checkpoint is way of ensuring that all the updates corresponding to some set of log records have been propagated to disk; checkpoints let you bound how much of the log you need to use at recovery time.)

    Checkpointing will be done by using CVs to block incoming transactions while allowing active transactions to complete. To mark the journal as checkpointed, simply change the size of the log to 0. Checkpoints are taken when the on disk journal becomes 50% full.


    How/when will you reclaim log space?

    Reclaim log space after every checkpoint, when we know all changes have made their way to disk.



    2. The Recovery Process

    What is your algorithm that must be executed after a failure to restore the file system to a consistent state?

    Iterate through the entire journal and replay all changes.

    Are there any steps you need to take prior to running recovery?

    Identify the start/end of journal and read the journal into memory.



    What assumptions can your recovery process make?

    (1) Actions are idempotent so we can do them an arbitrary number of times (as long as we do them in order). (2) Committed journal entries are correct, that is we can replay their changes without having to worry about correctness.



    What guarantees will your recovery process make? (That is, what can you say about the state of the system after recovery runs.)

    After recovery, the meta data in the filesystem will be in a self consistent state.



    3. Specific Log Records

    For each system call operation, what information must be logged?
    There are three main types of information that must be logged:
    (1) changes to inode fields (including indirect pointers which may not live in the inode itself)
    (2) changes to the free disk block bitmap
    (3) new directory entries



    Where can you make calls to logging routines?

    When possible, in the lower level sfs function calls that are shared between higher level sfs functions. Otherwise inside the sfs function where the change we want to log is actually done.



    How will you propagate information about the log to the locations at which you need to write log records?

    We pass down a transaction struct through all function calls that are part of the transaction.



    What recovery actions are needed for each log record? Have you specified enough information in your log records to facilitate what you need during recovery?
    In general we need the (1) location of change (2) change to be made. We believe we have specified enough information.



    Is there a set of micro-operations that comprise all the system call APIs? That is, rather than thinking about a log record per system call, is there a set of records that can be mixed and matched to construct what's necessary for all the APIs? It may be helpful to think about what operations can be performed on what data structures and how those can be combined?


    Yes there is a set of micro-operations and those are the operations we plan to log.



    4. Testing

    How will you test your system?


    Can you outline a testing philosophy or strategy that will let you approach testing in a methodical fashion?
    Can you perform unit testing?  How?
    We address these questions simultaneously since they seem to be asking the same thing. We unit test by testing journal logging and recovery code separately. For journal logging, we can do toy system calls and print out the journal to manually check if the log entries are correct. For recovery, we can create fake logs and check if they are replayed correctly.



    5. Miscellaneous

    Are there any tools/utilities you might build to make it easy to read/write log records?
    Yes, we will write a print log function that will print the journal in a human friendly way.



    What features can you design into your system to facilitate debugging?
    One example is storing the number of journal entries in every transaction commit message and validating this during recovery. We can also consider storing the old value as well as the new value in our journal entries.



    While you may assume there is no file system activity while you run recovery, there will almost certainly be activity while you write log records, take checkpoint, and reclaim log space.  What kind of synchronization will you need for that?

    Writing logs will require a single big lock. Checkpointing requires 2 big locks and 2 CVs.