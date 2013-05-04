/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * SFS filesystem
 *
 * Filesystem-level interface routines.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <array.h>
#include <bitmap.h>
#include <uio.h>
#include <vfs.h>
#include <buf.h>
#include <device.h>
#include <sfs.h>
#include <synch.h>
#include <vm.h>
#include <bitmap.h>
#include <thread.h>

/* Shortcuts for the size macros in kern/sfs.h */
#define SFS_FS_BITMAPSIZE(sfs)  SFS_BITMAPSIZE((sfs)->sfs_super.sp_nblocks)
#define SFS_FS_BITBLOCKS(sfs)   SFS_BITBLOCKS((sfs)->sfs_super.sp_nblocks)
#define JN_SUMMARY_LOCATION(sfs)    SFS_MAP_LOCATION + SFS_FS_BITBLOCKS(sfs) + 1

struct bitmap *b;

/* Journal prototype functions*/
static
void recover(struct sfs_fs *sfs);

static
void print_transaction(struct record *r);

/*
 * Routine for doing I/O (reads or writes) on the free block bitmap.
 * We always do the whole bitmap at once; writing individual sectors
 * might or might not be a worthwhile optimization. Similarly, storing
 * the bitmap in the buffer cache might or might not be a worthwhile
 * optimization. (But that would require a total rewrite of the way
 * it's handled, so not now.)
 *
 * The free block bitmap consists of SFS_BITBLOCKS 512-byte sectors of
 * bits, one bit for each sector on the filesystem. The number of
 * blocks in the bitmap is thus rounded up to the nearest multiple of
 * 512*8 = 4096. (This rounded number is SFS_BITMAPSIZE.) This means
 * that the bitmap will (in general) contain space for some number of
 * invalid sectors that are actually beyond the end of the disk
 * device. This is ok. These sectors are supposed to be marked "in
 * use" by mksfs and never get marked "free".
 *
 * The sectors used by the superblock and the bitmap itself are
 * likewise marked in use by mksfs.
 */

static
int
sfs_mapio(struct sfs_fs *sfs, enum uio_rw rw)
{
	uint32_t j, mapsize;
	char *bitdata;
	int result;

	KASSERT(lock_do_i_hold(sfs->sfs_bitlock));

	/* Number of blocks in the bitmap. */
	mapsize = SFS_FS_BITBLOCKS(sfs);

	/* Pointer to our bitmap data in memory. */
	bitdata = bitmap_getdata(sfs->sfs_freemap);

	/* For each sector in the bitmap... */
	for (j=0; j<mapsize; j++) {

		/* Get a pointer to its data */
		void *ptr = bitdata + j*SFS_BLOCKSIZE;

		/* and read or write it. The bitmap starts at sector 2. */
		if (rw == UIO_READ) {
			result = sfs_readblock(&sfs->sfs_absfs,
					       SFS_MAP_LOCATION + j,
					       ptr, SFS_BLOCKSIZE);
		}
		else {
			result = sfs_writeblock(&sfs->sfs_absfs,
						SFS_MAP_LOCATION + j,
						ptr, SFS_BLOCKSIZE);
		}

		/* If we failed, stop. */
		if (result) {
			return result;
		}
	}
	return 0;
}

/*
 * Sync routine. This is what gets invoked if you do FS_SYNC on the
 * sfs filesystem structure.
 */

static
int
sfs_sync(struct fs *fs)
{
	struct sfs_fs *sfs;
	int result;


	/*
	 * Get the sfs_fs from the generic abstract fs.
	 *
	 * Note that the abstract struct fs, which is all the VFS
	 * layer knows about, is actually a member of struct sfs_fs.
	 * The pointer in the struct fs points back to the top of the
	 * struct sfs_fs - essentially the same object. This can be a
	 * little confusing at first.
	 *
	 * The following diagram may help:
	 *
	 *     struct sfs_fs        <-------------\
     *           :                            |
     *           :   sfs_absfs (struct fs)    |   <------\
     *           :      :                     |          |
     *           :      :  various members    |          |
     *           :      :                     |          |
     *           :      :  fs_data  ----------/          |
     *           :      :                             ...|...
     *           :                                   .  VFS  .
     *           :                                   . layer .
     *           :   other members                    .......
     *           :
     *           :
	 *
	 * This construct is repeated with vnodes and devices and other
	 * similar things all over the place in OS/161, so taking the
	 * time to straighten it out in your mind is worthwhile.
	 */

	sfs = fs->fs_data;

	/* Sync the buffer cache */
	result = sync_fs_buffers(fs);
	if (result) {
		return result;
	}

	lock_acquire(sfs->sfs_bitlock);

	/* If the free block map needs to be written, write it. */
	if (sfs->sfs_freemapdirty) {
		result = sfs_mapio(sfs, UIO_WRITE);
		if (result) {
			lock_release(sfs->sfs_bitlock);
			return result;
		}
		sfs->sfs_freemapdirty = false;
	}

	/* If the superblock needs to be written, write it. */
	if (sfs->sfs_superdirty) {
		result = sfs_writeblock(&sfs->sfs_absfs, SFS_SB_LOCATION,
					&sfs->sfs_super, SFS_BLOCKSIZE);
		if (result) {
			lock_release(sfs->sfs_bitlock);
			return result;
		}
		sfs->sfs_superdirty = false;
	}

	lock_release(sfs->sfs_bitlock);

	return 0;
}

/*
 * Routine to retrieve the volume name. Filesystems can be referred
 * to by their volume name followed by a colon as well as the name
 * of the device they're mounted on.
 */
static
const char *
sfs_getvolname(struct fs *fs)
{
	struct sfs_fs *sfs = fs->fs_data;

	/*
	 * VFS only uses the volume name transiently, and its
	 * synchronization guarantees that we will not disappear while
	 * it's using the name. Furthermore, we don't permit the volume
	 * name to change on the fly (this is also a restriction in VFS)
	 * so there's no need to synchronize.
	 */

	return sfs->sfs_super.sp_volname;
}

/*
 * Unmount code.
 *
 * VFS calls FS_SYNC on the filesystem prior to unmounting it.
 */
static
int
sfs_unmount(struct fs *fs)
{
	struct sfs_fs *sfs = fs->fs_data;


	lock_acquire(sfs->sfs_vnlock);
	lock_acquire(sfs->sfs_bitlock);

	/* Do we have any files open? If so, can't unmount. */
	if (vnodearray_num(sfs->sfs_vnodes) > 0) {
		lock_release(sfs->sfs_bitlock);
		lock_release(sfs->sfs_vnlock);
		return EBUSY;
	}

	/* We should have just had sfs_sync called. */
	KASSERT(!sfs->sfs_superdirty);
	KASSERT(!sfs->sfs_freemapdirty);

	/* Once we start nuking stuff we can't fail. */
	vnodearray_destroy(sfs->sfs_vnodes);
	bitmap_destroy(sfs->sfs_freemap);

	/* The vfs layer takes care of the device for us */
	(void)sfs->sfs_device;

	/* Free the lock. VFS guarantees we can do this safely */
	lock_release(sfs->sfs_vnlock);
	lock_release(sfs->sfs_bitlock);
	lock_destroy(sfs->sfs_vnlock);
	lock_destroy(sfs->sfs_bitlock);
	lock_destroy(sfs->sfs_renamelock);

	/* Destroy the fs object */
	kfree(sfs);

	/* Destory journal objects */
	lock_destroy(log_buf_lock);
	lock_destroy(transaction_id_lock);

	/* nothing else to do */
	return 0;
}

/*
 * Mount routine.
 *
 * The way mount works is that you call vfs_mount and pass it a
 * filesystem-specific mount routine. Said routine takes a device and
 * hands back a pointer to an abstract filesystem. You can also pass
 * a void pointer through.
 *
 * This organization makes cleanup on error easier. Hint: it may also
 * be easier to synchronize correctly; it is important not to get two
 * filesystem with the same name mounted at once, or two filesystems
 * mounted on the same device at once.
 */

static
int
sfs_domount(void *options, struct device *dev, struct fs **ret)
{
	int result;
	struct sfs_fs *sfs;

	/* We don't pass any options through mount */
	(void)options;

	/*
	 * Make sure our on-disk structures aren't messed up
	 */
	KASSERT(sizeof(struct sfs_super)==SFS_BLOCKSIZE);
	KASSERT(sizeof(struct sfs_inode)==SFS_BLOCKSIZE);
	KASSERT(SFS_BLOCKSIZE % sizeof(struct sfs_dir) == 0);

	/*
	 * We can't mount on devices with the wrong sector size.
	 *
	 * (Note: for all intents and purposes here, "sector" and
	 * "block" are interchangeable terms. Technically a filesystem
	 * block may be composed of several hardware sectors, but we
	 * don't do that in sfs.)
	 */
	if (dev->d_blocksize != SFS_BLOCKSIZE) {
		return ENXIO;
	}

	/* Allocate journal objects */
	transaction_id_lock = lock_create("transaction id lock");
	if (transaction_id_lock == NULL) {
		return ENOMEM;
	}
	log_buf_lock = lock_create("log buffer lock");
	if (log_buf_lock == NULL) {
		lock_destroy(transaction_id_lock);
		return ENOMEM;
	}

	/* Allocate object */
	sfs = kmalloc(sizeof(struct sfs_fs));
	if (sfs==NULL) {
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return ENOMEM;
	}

	/* Allocate array */
	sfs->sfs_vnodes = vnodearray_create();
	if (sfs->sfs_vnodes == NULL) {
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return ENOMEM;
	}

	/* Set the device so we can use sfs_readblock() */
	sfs->sfs_device = dev;

	/* Set up abstract fs calls */
	sfs->sfs_absfs.fs_sync = sfs_sync;
	sfs->sfs_absfs.fs_getvolname = sfs_getvolname;
	sfs->sfs_absfs.fs_getroot = sfs_getroot;
	sfs->sfs_absfs.fs_unmount = sfs_unmount;
	sfs->sfs_absfs.fs_readblock = sfs_readblock;
	sfs->sfs_absfs.fs_writeblock = sfs_writeblock;
	sfs->sfs_absfs.fs_data = sfs;
	
	/* Create and acquire the locks so various stuff works right */
	sfs->sfs_vnlock = lock_create("sfs_vnlock");
	if (sfs->sfs_vnlock == NULL) {
		vnodearray_destroy(sfs->sfs_vnodes);
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return ENOMEM;
	}

	sfs->sfs_bitlock = lock_create("sfs_bitlock");
	if (sfs->sfs_bitlock == NULL) {
		lock_destroy(sfs->sfs_vnlock);
		vnodearray_destroy(sfs->sfs_vnodes);
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return ENOMEM;
	}

	sfs->sfs_renamelock = lock_create("sfs_renamelock");
	if (sfs->sfs_renamelock == NULL) {
		lock_destroy(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_vnlock);
		vnodearray_destroy(sfs->sfs_vnodes);
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return ENOMEM;
	}

	lock_acquire(sfs->sfs_vnlock);
	lock_acquire(sfs->sfs_bitlock);

	/* Load superblock */
	result = sfs_readblock(&sfs->sfs_absfs, SFS_SB_LOCATION,
			       &sfs->sfs_super, SFS_BLOCKSIZE);
	if (result) {
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_vnlock);
		lock_destroy(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_renamelock);
		vnodearray_destroy(sfs->sfs_vnodes);
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return result;
	}

	/* Make some simple sanity checks */

	if (sfs->sfs_super.sp_magic != SFS_MAGIC) {
		kprintf("sfs: Wrong magic number in superblock "
			"(0x%x, should be 0x%x)\n",
			sfs->sfs_super.sp_magic,
			SFS_MAGIC);
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_vnlock);
		lock_destroy(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_renamelock);
		vnodearray_destroy(sfs->sfs_vnodes);
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return EINVAL;
	}

	if (sfs->sfs_super.sp_nblocks > dev->d_blocks) {
		kprintf("sfs: warning - fs has %u blocks, device has %u\n",
			sfs->sfs_super.sp_nblocks, dev->d_blocks);
	}

	/* Ensure null termination of the volume name */
	sfs->sfs_super.sp_volname[sizeof(sfs->sfs_super.sp_volname)-1] = 0;

	/* Load free space bitmap */
	sfs->sfs_freemap = bitmap_create(SFS_FS_BITMAPSIZE(sfs));
	if (sfs->sfs_freemap == NULL) {
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_vnlock);
		lock_destroy(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_renamelock);
		vnodearray_destroy(sfs->sfs_vnodes);
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return ENOMEM;
	}
	result = sfs_mapio(sfs, UIO_READ);
	if (result) {
		lock_release(sfs->sfs_vnlock);
		lock_release(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_vnlock);
		lock_destroy(sfs->sfs_bitlock);
		lock_destroy(sfs->sfs_renamelock);
		bitmap_destroy(sfs->sfs_freemap);
		vnodearray_destroy(sfs->sfs_vnodes);
		kfree(sfs);
		lock_destroy(log_buf_lock);
		lock_destroy(transaction_id_lock);
		return result;
	}

	no_active_transactions = cv_create("no active transactions");
	if (no_active_transactions == NULL)
		goto err1;
	checkpoint_cleared = cv_create("checkpoint cleared");
	if (checkpoint_cleared == NULL)
		goto err2;
	transaction_lock = lock_create("transaction lock");
	if (transaction_lock == NULL)
		goto err3;
	checkpoint_lock = lock_create("checkpoint lock");
	if (checkpoint_lock == NULL)
		goto err4;
	num_active_transactions = 0;
	in_checkpoint = 0;


	/* the other fields */
	sfs->sfs_superdirty = false;
	sfs->sfs_freemapdirty = false;

	// Recovery
	recover(sfs);

	/* Hand back the abstract fs */
	*ret = &sfs->sfs_absfs;

	lock_release(sfs->sfs_vnlock);
	lock_release(sfs->sfs_bitlock);

	// journal_iterator(*ret, print_transaction);
	(void)print_transaction;
	return 0;

	err4:
	lock_destroy(transaction_lock);
	err3:
	cv_destroy(checkpoint_cleared);
	err2:
	cv_destroy(no_active_transactions);
	err1:
	lock_release(sfs->sfs_vnlock);
	lock_release(sfs->sfs_bitlock);
	lock_destroy(sfs->sfs_vnlock);
	lock_destroy(sfs->sfs_bitlock);
	lock_destroy(sfs->sfs_renamelock);
	bitmap_destroy(sfs->sfs_freemap);
	vnodearray_destroy(sfs->sfs_vnodes);
	kfree(sfs);
	lock_destroy(log_buf_lock);
	lock_destroy(transaction_id_lock);
	kfree(log_buf);
	return ENOMEM;
}

/*
 * Actual function called from high-level code to mount an sfs.
 */

int
sfs_mount(const char *device)
{
	return vfs_mount(device, NULL, sfs_domount);
}

/*
 * Journal recovery routine
 */
static
void first_pass(struct record *r) {
	unsigned id = r->transaction_id;
	if (r->transaction_type == REC_COMMIT && !bitmap_isset(b, id)) {
		bitmap_mark(b, id);
	}
}

static
void recover(struct sfs_fs *sfs) {
	// Construct bitmap
	struct sfs_jn_summary *s = kmalloc(SFS_BLOCKSIZE);
	if (s == NULL)
		panic("Cannot allocate memory for journal summary");
	if (sfs_readblock(&sfs->sfs_absfs, JN_SUMMARY_LOCATION(sfs), s, SFS_BLOCKSIZE))
		panic("Cannot read from journal summary");
	b = bitmap_create((unsigned)s->max_id+1);
	if (b == NULL) {
		panic("Cannot create bitmap");
	}
	// journal_iterator(&sfs->sfs_absfs, print_transaction);
	// First pass
	journal_iterator(&sfs->sfs_absfs, first_pass);

	// Second pass
	fs_journal_iterator(&sfs->sfs_absfs, b, apply_record);

	// Explicitly synch bitmap
	sync_fs_buffers(&sfs->sfs_absfs);

	// TODO: set journal entries to 0

	bitmap_destroy(b);
	kfree(s);
}

static
void print_transaction(struct record *r) {
	kprintf("%d", r->transaction_id);
	if (r->transaction_type == REC_INODE) {
		kprintf("\tINODE");
		kprintf("\ti_num: %d", r->changed.r_inode.inode_num);
		kprintf("\tlvl: %d", r->changed.r_inode.id_lvl);
		kprintf("\tset: %d", r->changed.r_inode.set);
		kprintf("\toffset: %d", r->changed.r_inode.offset);
		kprintf("\tblockno: %d", r->changed.r_inode.blockno);
	}
	else if(r->transaction_type == REC_ITYPE){
		kprintf("\tTYPE");
		kprintf("\ti_num: %d", r->changed.r_itype.inode_num);
		kprintf("\ttype: %d", r->changed.r_itype.type);
	}
	else if(r->transaction_type == REC_ISIZE){
		kprintf("\tSIZE");
		kprintf("\ti_num: %d", r->changed.r_isize.inode_num);
		kprintf("\tsize: %d", r->changed.r_isize.size);
	}
	else if(r->transaction_type == REC_ILINK){
		kprintf("\tLINK");
		kprintf("\ti_num: %d", r->changed.r_ilink.inode_num);
		kprintf("\tlnk_cnt: %d", r->changed.r_ilink.linkcount);
	}
	else if(r->transaction_type == REC_DIR){
		kprintf("\tDIR");
		kprintf("\tprnt_inode: %d", r->changed.r_directory.parent_inode);
		kprintf("\tslot: %d", r->changed.r_directory.slot);
		kprintf("\tinode: %d", r->changed.r_directory.inode);
		kprintf("\tname: %s", r->changed.r_directory.sfd_name);
	}
	else if(r->transaction_type == REC_BITMAP){
		kprintf("\tBITMAP");
		kprintf("\tidx: %d", r->changed.r_bitmap.index);
		kprintf("\tsetting: %d", r->changed.r_bitmap.setting);
	}
	kprintf("\n");
}
