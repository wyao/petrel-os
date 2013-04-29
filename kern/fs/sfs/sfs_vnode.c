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
 * File-level (vnode) interface routines.
 */
#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <limits.h>
#include <stat.h>
#include <lib.h>
#include <array.h>
#include <bitmap.h>
#include <uio.h>
#include <synch.h>
#include <vfs.h>
#include <buf.h>
#include <device.h>
#include <sfs.h>
#include <current.h>
#include <vm.h>

/*
 * Locking protocol for sfs:
 *    The following locks exist:
 *       vnode locks (sv_lock)
 *       vnode table lock (sfs_vnlock)
 *       bitmap lock (sfs_bitlock)
 *       rename lock (sfs_renamelock)
 *       buffer lock
 *
 *    Ordering constraints:
 *       rename lock       before  vnode locks
 *       vnode locks       before  vnode table lock
 *       vnode locks       before  buffer locks
 *       vnode table lock  before  bitmap lock
 *       buffer lock      before  bitmap lock
 *
 *    I believe the vnode table lock and the buffer locks are
 *    independent.
 *
 *    Ordering among vnode locks:
 *       directory lock    before  lock of a file within the directory
 *
 *    Ordering among directory locks:
 *       Parent first, then child.
 */

/* Slot in a directory that ".." is expected to appear in */
#define DOTDOTSLOT  1

/* At bottom of file */
static
int sfs_loadvnode(struct sfs_fs *sfs, uint32_t ino, int forcetype,
		 struct sfs_vnode **ret, bool load_inode, struct transaction *t);

/* needed by reclaim */
static
int sfs_dotruncate(struct vnode *v, off_t len, struct transaction *t);

/* Journaling functions -- bottom of file */
int next_transaction_id = 0;
int log_buf_offset = 0;

#define REC_PER_BLK (int) (SFS_BLOCKSIZE / (RECORD_SIZE))
#define BITBLOCKS(fs) SFS_BITBLOCKS(((struct sfs_fs*)fs->fs_data)->sfs_super.sp_nblocks)
#define JN_SUMMARY_LOCATION(fs) SFS_MAP_LOCATION + BITBLOCKS(fs) + 1
#define JN_LOCATION(fs) SFS_MAP_LOCATION + BITBLOCKS(fs) + 1 + 1
#define MAX_JN_ENTRIES (SFS_JN_SIZE-1)/REC_PER_BLK

static
struct transaction *
create_transaction(void);

static
int hold_buffer_cache(struct transaction *t, struct buf *buf);

static
int record(struct record *r);

static
int commit(struct transaction *t, struct fs *fs);

/*static
void abort(struct transaction *t);*/

static
int check_and_record(struct record *r, struct transaction *t);

static
int checkpoint(void);


////////////////////////////////////////////////////////////
//
// Simple stuff

static
struct sfs_vnode *
sfs_create_vnode(void)
{
	struct sfs_vnode *new_vn;

	new_vn = kmalloc(sizeof(struct sfs_vnode));
	if (new_vn == NULL) {
		return NULL;
	}
	new_vn->sv_buf = NULL;
	new_vn->sv_bufdepth = 0;
	new_vn->sv_ino = -1;
	new_vn->sv_type = SFS_TYPE_INVAL;
	new_vn->sv_lock = lock_create("sfs vnode lock");
	if (new_vn->sv_lock == NULL) {
		kfree(new_vn);
		return NULL;
	} else {
		return new_vn;
	}
}

static
void
sfs_destroy_vnode(struct sfs_vnode *victim)
{
	lock_destroy(victim->sv_lock);
	kfree(victim);
}

/* Zero out a disk block.
 *
 * Allocates 1 buffer if bufret != NULL.  Uses 1 regardless.
 */
static
int
sfs_clearblock(struct sfs_fs *sfs, uint32_t block, struct buf **bufret)
{
	struct buf *buf;
	void *ptr;
	int result;

	result = buffer_get(&sfs->sfs_absfs, block, SFS_BLOCKSIZE, &buf);
	if (result) {
		return result;
	}

	ptr = buffer_map(buf);
	bzero(ptr, SFS_BLOCKSIZE);
	buffer_mark_valid(buf);
	buffer_mark_dirty(buf);

	if (bufret != NULL) {
		*bufret = buf;
	}
	else {
		buffer_release(buf);
	}

	return 0;
}

////////////////////////////////////////////////////////////
//
// Space allocation

/*
 * Allocate a block.
 *
 * Returns the block number, plus the buffer if BUFRET isn't null.
 * The buffer, if any, is marked valid and dirty, and zeroed out.
 *
 * Allocates 1 buffer (via sfs_clearblock) if bufret != NULL.
 * Uses 1 regardless.
 */
static
int
sfs_balloc(struct sfs_fs *sfs, uint32_t *diskblock, struct buf **bufret, struct transaction *t)
{
	int result;

	lock_acquire(sfs->sfs_bitlock);

	result = bitmap_alloc(sfs->sfs_freemap, diskblock);
	if (result) {
		struct record *r = makerec_bitmap((uint32_t)result,1);

		int log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;

		return result;
	}
	sfs->sfs_freemapdirty = true;

	lock_release(sfs->sfs_bitlock);
	
	if (*diskblock >= sfs->sfs_super.sp_nblocks) {
		panic("sfs: balloc: invalid block %u\n", *diskblock);
	}

	/* Clear block before returning it */
	return sfs_clearblock(sfs, *diskblock, bufret);
}

/*
 * Free a block.
 */
static
void
sfs_bfree(struct sfs_fs *sfs, uint32_t diskblock, struct transaction *t)
{
	lock_acquire(sfs->sfs_bitlock);
	bitmap_unmark(sfs->sfs_freemap, diskblock);

	struct record *r = makerec_bitmap(diskblock,0);
	check_and_record(r,t);

	sfs->sfs_freemapdirty = true;
	lock_release(sfs->sfs_bitlock);
}

/*
 * Check if a block is in use.
 */
static
int
sfs_bused(struct sfs_fs *sfs, uint32_t diskblock)
{
	int result;

	if (diskblock >= sfs->sfs_super.sp_nblocks) {
		panic("sfs: sfs_bused called on out of range block %u\n",
		      diskblock);
	}

	lock_acquire(sfs->sfs_bitlock);
	result = bitmap_isset(sfs->sfs_freemap, diskblock);
	lock_release(sfs->sfs_bitlock);

	return result;
}

////////////////////////////////////////////////////////////
//
// Block mapping/inode maintenance

/*
 * Look up the disk block number (from 0 up to the number of blocks on
 * the disk) given a file and the logical block number within that
 * file. If DOALLOC is set, and no such block exists, one will be
 * allocated.
 *
 * Locking: must hold vnode lock. May get/release buffer cache locks and (via
 *    sfs_balloc) sfs_bitlock.
 *    
 * Requires up to 2 buffers.
 */
static
int
sfs_bmap(struct sfs_vnode *sv, uint32_t fileblock,
		int doalloc, uint32_t *diskblock, struct transaction *t)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	struct sfs_inode *inodeptr;
	struct buf *kbuf, *kbuf2;
	uint32_t *iddata;
	uint32_t block, cur_block, next_block;
	uint32_t idoff;
	int result, indir, i;
	struct record *r;

	KASSERT(lock_do_i_hold(sv->sv_lock));
	KASSERT(SFS_DBPERIDB * sizeof(*iddata) == SFS_BLOCKSIZE);

	/*
	 * Check that we are not being asked for a block beyond 
	 * the maximum allowed file size
	 */
	if(fileblock >= SFS_NDIRECT + SFS_DBPERIDB + SFS_DBPERIDB*SFS_DBPERIDB +
			SFS_DBPERIDB*SFS_DBPERIDB*SFS_DBPERIDB)
	{
		return EINVAL;
	}

	result = sfs_load_inode(sv);
	if (result) {
		return result;
	}
	inodeptr = buffer_map(sv->sv_buf);
	hold_buffer_cache(t,sv->sv_buf);

	/*
	 * If the block we want is one of the direct blocks...
	 */
	if (fileblock < SFS_NDIRECT) {
		/*
		 * Get the block number
		 */
		block = inodeptr->sfi_direct[fileblock];

		/*
		 * Do we need to allocate?
		 */
		if (block==0 && doalloc) {
			result = sfs_balloc(sfs, &block, NULL, t);
			if (result) {
				sfs_release_inode(sv);
				return result;
			}

			/* Remember what we allocated; mark inode dirty */
			inodeptr->sfi_direct[fileblock] = block;

			// Record for adding direct block
			r = makerec_inode(sv->sv_ino,0,0,fileblock,block);
			int log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			buffer_mark_dirty(sv->sv_buf);
		}

		/*
		 * Hand back the block
		 */
		if (block != 0 && !sfs_bused(sfs, block)) {
			panic("sfs: Data block %u (block %u of file %u) "
					"marked free\n", block, fileblock, sv->sv_ino);
		}
		*diskblock = block;
		sfs_release_inode(sv);
		return 0;
	}

	/* It's not a direct block. Figure out what level of indirection we are in. */

	fileblock -= SFS_NDIRECT;

	if(fileblock >= SFS_DBPERIDB + SFS_DBPERIDB*SFS_DBPERIDB)
	{
		/* It's reachable through the triple indirect block */
		indir = 3;
		fileblock -= SFS_DBPERIDB + SFS_DBPERIDB*SFS_DBPERIDB;

		/* Set the next_block to triple indirect */
		next_block = inodeptr->sfi_tindirect;
	}
	else if(fileblock >= SFS_DBPERIDB)
	{
		/* It's reachable through the double indirect block */
		indir = 2;
		fileblock -= SFS_DBPERIDB;

		/* Set the next block to double indirect */
		next_block = inodeptr->sfi_dindirect;
	}
	else
	{
		indir = 1;
		next_block = inodeptr->sfi_indirect;
	}

	/* Allocate the block if necessary */
	if(next_block == 0 && !doalloc)
	{
		*diskblock = 0;
		sfs_release_inode(sv);
		return 0;
	}
	else if(next_block == 0)
	{
		result = sfs_balloc(sfs, &next_block, &kbuf, t);
		if(result)
		{
			sfs_release_inode(sv);
			return result;
		}
		/* Remember the block we have allocated */
		if(indir == 3)
		{
			inodeptr->sfi_tindirect = next_block;
			r = makerec_inode(sv->sv_ino,3,1,0,block);
		}
		else if(indir == 2)
		{
			inodeptr->sfi_dindirect = next_block;
			r = makerec_inode(sv->sv_ino,2,1,0,block);
		}
		else if(indir == 1)
		{
			inodeptr->sfi_indirect = next_block;
			r = makerec_inode(sv->sv_ino,1,1,0,block);
		}
		int log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;

		buffer_mark_dirty(sv->sv_buf);
	} else {
		result = buffer_read(sv->sv_v.vn_fs, next_block,
				SFS_BLOCKSIZE, &kbuf);
		if (result) {
			sfs_release_inode(sv);
			return result;
		}
	}



	/* Now loop through the levels of indirection until we get to the
	 * direct block we need.
	 */
	for(i = indir; i>0; i--)
	{
		iddata = buffer_map(kbuf);
		hold_buffer_cache(t,kbuf);

		/* Now adjust the file block so that it would look as if
		 * we only have one branch of indirections (i.e. only a triple indirect block).
		 * Calculate idoff - this is the offset into the current block, which gives the
		 * number of the next block we have to read.
		 */
		if(i == 3)
		{
			idoff = fileblock/(SFS_DBPERIDB*SFS_DBPERIDB);
			fileblock -= idoff * (SFS_DBPERIDB*SFS_DBPERIDB);
		}
		if(i == 2)
		{
			idoff = fileblock/SFS_DBPERIDB;
			fileblock -= idoff * SFS_DBPERIDB;
		}
		if(i == 1)
		{
			idoff = fileblock;
		}

		cur_block = next_block;
		next_block = iddata[idoff];

		if(next_block == 0 && !doalloc)
		{
			/* No block at the next level, and we weren't asked to allocate one,
			 * so return */
			*diskblock = 0;
			buffer_release(kbuf);
			sfs_release_inode(sv);
			return 0;
		}
		else if(next_block == 0)
		{
			result = sfs_balloc(sfs, &next_block, &kbuf2, t);
			if(result)
			{
				buffer_release(kbuf);
				sfs_release_inode(sv);
				return result;
			}

			iddata[idoff] = next_block;

			r = makerec_inode(sv->sv_ino,i,0,idoff,next_block);
			int log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			buffer_mark_dirty(kbuf);
			buffer_release(kbuf);
			kbuf = kbuf2;
			iddata = buffer_map(kbuf);
		} else {
			buffer_release(kbuf);
			result = buffer_read(sv->sv_v.vn_fs, next_block,
						SFS_BLOCKSIZE, &kbuf);
			if (result) {
				sfs_release_inode(sv);
				return result;
			}
		}
	}
	buffer_release(kbuf);


	/* Hand back the result and return. */
	if (next_block != 0 && !sfs_bused(sfs, next_block)) {
		panic("sfs: Data block %u (block %u of file %u) marked free\n",
				next_block, fileblock, sv->sv_ino);
	}
	*diskblock = next_block;
	sfs_release_inode(sv);
	return 0;
}

////////////////////////////////////////////////////////////
//
// File-level I/O

/*
 * Do I/O to a block of a file that doesn't cover the whole block.  We
 * need to read in the original block first, even if we're writing, so
 * we don't clobber the portion of the block we're not intending to
 * write over.
 *
 * skipstart is the number of bytes to skip past at the beginning of
 * the sector; len is the number of bytes to actually read or write.
 * uio is the area to do the I/O into.
 *
 * Requires up to 2 buffers.
 */
static
int
sfs_partialio(struct sfs_vnode *sv, struct uio *uio,
	      uint32_t skipstart, uint32_t len, struct transaction *t)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	struct buf *iobuffer;
	char *ioptr;
	uint32_t diskblock;
	uint32_t fileblock;
	int result;

	/* Allocate missing blocks if and only if we're writing */
	int doalloc = (uio->uio_rw==UIO_WRITE);

	KASSERT(lock_do_i_hold(sv->sv_lock));
	KASSERT(skipstart + len <= SFS_BLOCKSIZE);

	/* Compute the block offset of this block in the file */
	fileblock = uio->uio_offset / SFS_BLOCKSIZE;

	/* Get the disk block number */
	result = sfs_bmap(sv, fileblock, doalloc, &diskblock, t);
	if (result) {
		return result;
	}

	if (diskblock == 0) {
		/*
		 * There was no block mapped at this point in the file.
		 *
		 * We must be reading, or sfs_bmap would have
		 * allocated a block for us.
		 */
		KASSERT(uio->uio_rw == UIO_READ);
		return uiomovezeros(len, uio);
	}
	else {
		/*
		 * Read the block.
		 */
		result = buffer_read(&sfs->sfs_absfs, diskblock, SFS_BLOCKSIZE,
				     &iobuffer);
		if (result) {
			return result;
		}
	}

	/*
	 * Now perform the requested operation into/out of the buffer.
	 */
	ioptr = buffer_map(iobuffer);
	result = uiomove(ioptr+skipstart, len, uio);
	if (result) {
		buffer_release(iobuffer);
		return result;
	}

	/*
	 * If it was a write, mark the modified block dirty.
	 */
	if (uio->uio_rw == UIO_WRITE) {
		buffer_mark_dirty(iobuffer);
	}

	buffer_release(iobuffer);
	return 0;
}

/*
 * Do I/O (either read or write) of a single whole block.
 *
 * Locking: must hold vnode lock. May get/release sfs_bitlock.
 * 
 * Requires up to 2 buffers.
 */
static
int
sfs_blockio(struct sfs_vnode *sv, struct uio *uio, struct transaction *t)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	struct buf *iobuf;
	void *ioptr;
	uint32_t diskblock;
	uint32_t fileblock;
	int result;
	int doalloc = (uio->uio_rw==UIO_WRITE);

	KASSERT(lock_do_i_hold(sv->sv_lock));
	
	/* Get the block number within the file */
	fileblock = uio->uio_offset / SFS_BLOCKSIZE;

	/* Look up the disk block number */
	result = sfs_bmap(sv, fileblock, doalloc, &diskblock, t);
	if (result) {
		return result;
	}

	if (diskblock == 0) {
		/*
		 * No block - fill with zeros.
		 *
		 * We must be reading, or sfs_bmap would have
		 * allocated a block for us.
		 */
		KASSERT(uio->uio_rw == UIO_READ);
		return uiomovezeros(SFS_BLOCKSIZE, uio);
	}

	if (uio->uio_rw == UIO_READ) {
		result = buffer_read(&sfs->sfs_absfs, diskblock, SFS_BLOCKSIZE,
				     &iobuf);
	}
	else {
		result = buffer_get(&sfs->sfs_absfs, diskblock, SFS_BLOCKSIZE,
				    &iobuf);
	}
	if (result) {
		return result;
	}

	/*
	 * Do the I/O into the buffer.
	 */
	ioptr = buffer_map(iobuf);
	result = uiomove(ioptr, SFS_BLOCKSIZE, uio);
	if (result) {
		buffer_release(iobuf);
		return result;
	}

	if (uio->uio_rw == UIO_WRITE) {
		buffer_mark_valid(iobuf);
		buffer_mark_dirty(iobuf);
	}

	buffer_release(iobuf);
	return 0;
}

/*
 * Do I/O of a whole region of data, whether or not it's block-aligned.
 *
 * Locking: must hold vnode lock. May get/release sfs_bitlock.
 * 
 * Requires up to 3 buffers.
 */
static
int
sfs_io(struct sfs_vnode *sv, struct uio *uio, struct transaction *t)
{
	uint32_t blkoff;
	uint32_t nblocks, i;
	int result = 0;
	uint32_t extraresid = 0;
	struct sfs_inode *inodeptr;


	KASSERT(lock_do_i_hold(sv->sv_lock));
	
	result = sfs_load_inode(sv);
	if (result) {
		return result;
	}
	inodeptr = buffer_map(sv->sv_buf);

	if (uio->uio_rw == UIO_WRITE){
		hold_buffer_cache(t,sv->sv_buf);
	}

	/*
	 * If reading, check for EOF. If we can read a partial area,
	 * remember how much extra there was in EXTRARESID so we can
	 * add it back to uio_resid at the end.
	 */
	if (uio->uio_rw == UIO_READ) {
		off_t size, endpos;

		size = inodeptr->sfi_size;
		endpos = uio->uio_offset + uio->uio_resid;

		if (uio->uio_offset >= size) {
			/* At or past EOF - just return */
			sfs_release_inode(sv);
			return 0;
		}

		if (endpos > size) {
			extraresid = endpos - size;
			KASSERT(uio->uio_resid > extraresid);
			uio->uio_resid -= extraresid;
		}
	}

	/*
	 * First, do any leading partial block.
	 */
	blkoff = uio->uio_offset % SFS_BLOCKSIZE;
	if (blkoff != 0) {
		/* Number of bytes at beginning of block to skip */
		uint32_t skip = blkoff;

		/* Number of bytes to read/write after that point */
		uint32_t len = SFS_BLOCKSIZE - blkoff;

		/* ...which might be less than the rest of the block */
		if (len > uio->uio_resid) {
			len = uio->uio_resid;
		}

		/* Call sfs_partialio() to do it. */
		result = sfs_partialio(sv, uio, skip, len, t);
		if (result) {
			goto out;
		}
	}

	/* If we're done, quit. */
	if (uio->uio_resid==0) {
		goto out;
	}

	/*
	 * Now we should be block-aligned. Do the remaining whole blocks.
	 */
	KASSERT(uio->uio_offset % SFS_BLOCKSIZE == 0);
	nblocks = uio->uio_resid / SFS_BLOCKSIZE;
	for (i=0; i<nblocks; i++) {
		result = sfs_blockio(sv, uio, t);
		if (result) {
			goto out;
		}
	}

	/*
	 * Now do any remaining partial block at the end.
	 */
	KASSERT(uio->uio_resid < SFS_BLOCKSIZE);

	if (uio->uio_resid > 0) {
		result = sfs_partialio(sv, uio, 0, uio->uio_resid, t);
		if (result) {
			goto out;
		}
	}

 out:

	/* If writing, adjust file length */
	if (uio->uio_rw == UIO_WRITE &&
	    uio->uio_offset > (off_t)inodeptr->sfi_size) {
		inodeptr->sfi_size = uio->uio_offset;

		struct record *r = makerec_isize(sv->sv_ino,uio->uio_offset);
		int log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;

		buffer_mark_dirty(sv->sv_buf);
	}
	sfs_release_inode(sv);

	/* Add in any extra amount we couldn't read because of EOF */
	uio->uio_resid += extraresid;

	/* Done */
	return result;
}

////////////////////////////////////////////////////////////
//
// Directory I/O

/*
 * Read the directory entry out of slot SLOT of a directory vnode.
 * The "slot" is the index of the directory entry, starting at 0.
 *
 * Locking: Must hold the vnode lock. May get/release sfs_bitlock.
 * 
 * Requires up to 3 buffers.
 */
static
int
sfs_readdir(struct sfs_vnode *sv, struct sfs_dir *sd, int slot)
{
	struct iovec iov;
	struct uio ku;
	off_t actualpos;
	int result;

	KASSERT(lock_do_i_hold(sv->sv_lock));
	
	/* Compute the actual position in the directory to read. */
	actualpos = slot * sizeof(struct sfs_dir);

	/* Set up a uio to do the read */
	uio_kinit(&iov, &ku, sd, sizeof(struct sfs_dir), actualpos, UIO_READ);

	/* do it */
	result = sfs_io(sv, &ku, NULL);
	if (result) {
		return result;
	}

	/* We should not hit EOF in the middle of a directory entry */
	if (ku.uio_resid > 0) {
		panic("sfs: readdir: Short entry (inode %u)\n", sv->sv_ino);
	}

	/* Done */
	return 0;
}

/*
 * Write (overwrite) the directory entry in slot SLOT of a directory
 * vnode.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_writedir(struct sfs_vnode *sv, struct sfs_dir *sd, int slot, struct transaction *t)
{
	struct iovec iov;
	struct uio ku;
	off_t actualpos;
	int result;

	/* Compute the actual position in the directory. */
	
	KASSERT(lock_do_i_hold(sv->sv_lock));
	KASSERT(slot>=0);
	actualpos = slot * sizeof(struct sfs_dir);

	/* Set up a uio to do the write */
	uio_kinit(&iov, &ku, sd, sizeof(struct sfs_dir), actualpos, UIO_WRITE);

	/* do it */
	result = sfs_io(sv, &ku, t);
	if (result) {
		return result;
	}

	/* Should not end up with a partial entry! */
	if (ku.uio_resid > 0) {
		panic("sfs: writedir: Short write (ino %u)\n", sv->sv_ino);
	}

	/* Done */
	return 0;
}

/*
 * Compute the number of entries in a directory.
 * This actually computes the number of existing slots, and does not
 * account for empty slots.
 *
 * Locking: must hold vnode lock.
 * 
 * Requires 1 buffer.
 */
static
int
sfs_dir_nentries(struct sfs_vnode *sv, int *ret)
{
	off_t size;
	struct sfs_inode *inodeptr;
	int result;

	KASSERT(lock_do_i_hold(sv->sv_lock));
	KASSERT(sv->sv_type == SFS_TYPE_DIR);

	result = sfs_load_inode(sv);
	if (result) {
		return result;
	}
	inodeptr = buffer_map(sv->sv_buf);

	size = inodeptr->sfi_size;
	if (size % sizeof(struct sfs_dir) != 0) {
		panic("sfs: directory %u: Invalid size %llu\n",
		      sv->sv_ino, size);
	}

	sfs_release_inode(sv);

	*ret = size / sizeof(struct sfs_dir);
	return 0;
}

/*
 * Search a directory for a particular filename in a directory, and
 * return its inode number, its slot, and/or the slot number of an
 * empty directory slot if one is found.
 *
 * Locking: must hold vnode lock. May get/release sfs_bitlock.
 * 
 * Requires up to 3 buffers.
 */

static
int
sfs_dir_findname(struct sfs_vnode *sv, const char *name,
		    uint32_t *ino, int *slot, int *emptyslot)
{
	struct sfs_dir tsd;
	int found = 0;
	int nentries;
	int i, result;

	KASSERT(lock_do_i_hold(sv->sv_lock));

	result = sfs_dir_nentries(sv, &nentries);
	if (result) {
		return result;
	}

	/* For each slot... */
	for (i=0; i<nentries; i++) {

		/* Read the entry from that slot */
		result = sfs_readdir(sv, &tsd, i);
		if (result) {
			return result;
		}
		if (tsd.sfd_ino == SFS_NOINO) {
			/* Free slot - report it back if one was requested */
			if (emptyslot != NULL) {
				*emptyslot = i;
			}
		}
		else {
			/* Ensure null termination, just in case */
			tsd.sfd_name[sizeof(tsd.sfd_name)-1] = 0;
			if (!strcmp(tsd.sfd_name, name)) {

				/* Each name may legally appear only once... */
				KASSERT(found==0);

				found = 1;
				if (slot != NULL) {
					*slot = i;
				}
				if (ino != NULL) {
					*ino = tsd.sfd_ino;
				}
			}
		}
	}

	return found ? 0 : ENOENT;
}

/*
 * Search a directory for a particular inode number in a directory, and
 * return the directory entry and/or its slot.
 *
 * Locking: requires vnode lock
 *
 * Requires up to 3 buffers
 */

static
int
sfs_dir_findino(struct sfs_vnode *sv, uint32_t ino,
		struct sfs_dir *retsd, int *slot)
{
	struct sfs_dir tsd;
	int found = 0;
	int nentries;
	int i, result;

	KASSERT(lock_do_i_hold(sv->sv_lock));

	result = sfs_dir_nentries(sv, &nentries);
	if (result) {
		return result;
	}

	/* For each slot... */
	for (i=0; i<nentries && !found; i++) {

		/* Read the entry from that slot */
		result = sfs_readdir(sv, &tsd, i);
		if (result) {
			return result;
		}
		if (tsd.sfd_ino == ino) {
			found = 1;
			if (slot != NULL) {
				*slot = i;
			}
			if (retsd != NULL) {
				/* Ensure null termination, just in case */
				tsd.sfd_name[sizeof(tsd.sfd_name)-1] = 0;
				*retsd = tsd;
			}
		}
	}

	return found ? 0 : ENOENT;
}

/*
 * Create a link in a directory to the specified inode by number, with
 * the specified name, and optionally hand back the slot.
 *
 * Locking: must hold vnode lock. May get/release sfs_bitlock.
 * 
 * Requires up to 3 buffers.
 */
static
int
sfs_dir_link(struct sfs_vnode *sv, const char *name, uint32_t ino, int *slot, struct transaction *t)
{
	int emptyslot = -1;
	int result;
	struct sfs_dir sd;
	struct record *r;

	KASSERT(lock_do_i_hold(sv->sv_lock));
	
	/* Look up the name. We want to make sure it *doesn't* exist. */
	result = sfs_dir_findname(sv, name, NULL, NULL, &emptyslot);
	if (result!=0 && result!=ENOENT) {
		return result;
	}
	if (result==0) {
		return EEXIST;
	}

	if (strlen(name)+1 > sizeof(sd.sfd_name)) {
		return ENAMETOOLONG;
	}

	/* If we didn't get an empty slot, add the entry at the end. */
	if (emptyslot < 0) {
		result = sfs_dir_nentries(sv, &emptyslot);
		if (result) {
			return result;
		}
	}

	/* Set up the entry. */
	bzero(&sd, sizeof(sd));
	sd.sfd_ino = ino;
	strcpy(sd.sfd_name, name);

	r = makerec_dir(sv->sv_ino,emptyslot,ino,name);
	int log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	/* Hand back the slot, if so requested. */
	if (slot) {
		*slot = emptyslot;
	}

	/* Write the entry. */
	return sfs_writedir(sv, &sd, emptyslot, t);

}

/*
 * Unlink a name in a directory, by slot number.
 *
 * Locking: must hold vnode lock. May get/release sfs_bitlock.
 * 
 * Requires up to 3 buffers.
 */
static
int
sfs_dir_unlink(struct sfs_vnode *sv, int slot, struct transaction *t)
{
	struct sfs_dir sd;
	struct record *r;

	// synchronous write with lock held...bleh
	KASSERT(lock_do_i_hold(sv->sv_lock));
	
	/* Initialize a suitable directory entry... */
	bzero(&sd, sizeof(sd));
	sd.sfd_ino = SFS_NOINO;

	r = makerec_dir(sv->sv_ino,slot,0,NULL);
	int log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	/* ... and write it */
	return sfs_writedir(sv, &sd, slot, t);
}

/*
 * Check if a directory is empty.
 *
 * Locking: must hold vnode lock.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_dir_checkempty(struct sfs_vnode *sv)
{
	struct sfs_dir sd;
	int nentries;
	int i, result;

	KASSERT(lock_do_i_hold(sv->sv_lock));

	result = sfs_dir_nentries(sv, &nentries);
	if (result) {
		return result;
	}
	
	for (i=0; i<nentries; i++) {
		result = sfs_readdir(sv, &sd, i);
		if (result) {
			return result;
		}
		if (sd.sfd_ino == SFS_NOINO) {
			/* empty slot */
			continue;
		}

		/* Ensure null termination, just in case */
		sd.sfd_name[sizeof(sd.sfd_name)-1] = 0;

		if (!strcmp(sd.sfd_name, ".") || !strcmp(sd.sfd_name, "..")) {
			continue;
		}

		/* Non-empty slot containing other than . or .. -> not empty */
		return ENOTEMPTY;
	}

	return 0;
}

/*
 * Look for a name in a directory and hand back a vnode for the
 * file, if there is one.
 *
 * load_inode has same semantics as sfs_loadvnode
 *
 * Locking: must hold vnode lock. May get/release sfs_bitlock.
 *    Also gets/releases sfs_vnlock.
 *    
 * Requires up to 3 buffers.
 */
static
int
sfs_lookonce(struct sfs_vnode *sv, const char *name, struct sfs_vnode **ret,
		bool load_inode, int *slot)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	uint32_t ino;
	int result, result2;
	int emptyslot = -1;

	KASSERT(lock_do_i_hold(sv->sv_lock));
	
	result = sfs_dir_findname(sv, name, &ino, slot, &emptyslot);
	if (result==ENOENT) {
		*ret = NULL;
		if (slot != NULL) {
			if (emptyslot < 0) {
				result2 = sfs_dir_nentries(sv, &emptyslot);
				if (result2) {
					return result2;
				}
			}
			*slot = emptyslot;
		}
		return result;
	}
	else if (result) {
		return result;
	}

	return sfs_loadvnode(sfs, ino, SFS_TYPE_INVAL, ret, load_inode, NULL);
}

////////////////////////////////////////////////////////////
//
// Object creation

/*
 * Create a new filesystem object and hand back its vnode.
 * Always hands back vnode "locked and loaded"
 *
 * Locking: Gets/release sfs_bitlock.
 *    Also gets/releases sfs_vnlock, but does not hold them together.
 *    
 * May require 3 buffers if sfs_loadvnode triggers reclaim.
 */
static
int
sfs_makeobj(struct sfs_fs *sfs, int type, struct sfs_vnode **ret, struct transaction *t)
{
	uint32_t ino;
	int result;

	/*
	 * First, get an inode. (Each inode is a block, and the inode
	 * number is the block number, so just get a block.)
	 */

	result = sfs_balloc(sfs, &ino, NULL, t);
	if (result) {
		return result;
	}

	/*
	 * Now load a vnode for it.
	 */

	result = sfs_loadvnode(sfs, ino, type, ret, true, t);
	if (result) {
		sfs_bfree(sfs, ino, t);
	}
	return result;
}

////////////////////////////////////////////////////////////
//
// Vnode ops

/*
 * This is called on *each* open().
 *
 * Locking: not needed
 */
static
int
sfs_open(struct vnode *v, int openflags)
{
	/*
	 * At this level we do not need to handle O_CREAT, O_EXCL, or O_TRUNC.
	 * We *would* need to handle O_APPEND, but we don't support it.
	 *
	 * Any of O_RDONLY, O_WRONLY, and O_RDWR are valid, so we don't need
	 * to check that either.
	 */

	if (openflags & O_APPEND) {
		return EUNIMP;
	}

	(void)v;

	return 0;
}

/*
 * This is called on *each* open() of a directory.
 * Directories may only be open for read.
 *
 * Locking: not needed
 */
static
int
sfs_opendir(struct vnode *v, int openflags)
{
	switch (openflags & O_ACCMODE) {
	    case O_RDONLY:
		break;
	    case O_WRONLY:
	    case O_RDWR:
	    default:
		return EISDIR;
	}
	if (openflags & O_APPEND) {
		return EISDIR;
	}

	(void)v;
	return 0;
}

/*
 * Called on the *last* close().
 *
 * This function should attempt to avoid returning errors, as handling
 * them usefully is often not possible.
 *
 * Locking: not needed
 */
static
int
sfs_close(struct vnode *v)
{
	/* Nothing. */
	(void)v;
	return 0;
}

/*
 * Called when the vnode refcount (in-memory usage count) hits zero.
 *
 * This function should try to avoid returning errors other than EBUSY.
 *
 * Locking: gets/releases vnode lock. Gets/releases sfs_vnlock, and 
 *    possibly also sfs_bitlock, while holding the vnode lock
 *    
 * Requires 1 buffer, and then independently calls VOP_TRUNCATE, which
 * takes 4.
 */
static
int
sfs_reclaim(struct vnode *v)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct sfs_inode *iptr;
	unsigned ix, i, num;
	bool buffers_needed;
	int result;

	//kprintf("SFS_RECLAIM\n");
	// ENTRYPOINT
	struct transaction *t = create_transaction();

	lock_acquire(sv->sv_lock);
	lock_acquire(sfs->sfs_vnlock);

	/*
	 * Make sure someone else hasn't picked up the vnode since the
	 * decision was made to reclaim it. (You must also synchronize
	 * this with sfs_loadvnode.)
	 */
	lock_acquire(v->vn_countlock);
	if (v->vn_refcount != 1) {

		/* consume the reference VOP_DECREF gave us */
		KASSERT(v->vn_refcount>1);
		v->vn_refcount--;

		lock_release(v->vn_countlock);
		lock_release(sfs->sfs_vnlock);
		lock_release(sv->sv_lock);
		return EBUSY;
	}
	lock_release(v->vn_countlock);

	/* this grossness is because reclaim can either be called
	 * directly from the VFS layer or as a side effect of a
	 * VOP_DECREF called somewhere in the SFS layer
	 */
	buffers_needed = curthread->t_reserved_buffers == 0;
	if (buffers_needed) {
		reserve_buffers(4, SFS_BLOCKSIZE);
	}

	/* Get the on-disk inode. */
	result = sfs_load_inode(sv);
	if (result) {
		/*
		 * This case is likely to lead to problems, but
		 * there's essentially no helping it...
		 */
		lock_release(sfs->sfs_vnlock);
		lock_release(sv->sv_lock);
		if (buffers_needed) {
			unreserve_buffers(4, SFS_BLOCKSIZE);
		}
		return result;
	}
	iptr = buffer_map(sv->sv_buf);

	/* If there are no on-disk references to the file either, erase it. */
	if (iptr->sfi_linkcount==0) {
		result = sfs_dotruncate(&sv->sv_v, 0, t);
		if (result) {
			sfs_release_inode(sv);
			lock_release(sfs->sfs_vnlock);
			lock_release(sv->sv_lock);
			if (buffers_needed) {
				unreserve_buffers(4, SFS_BLOCKSIZE);
			}
			return result;
		}
		sfs_release_inode(sv);
		/* Discard the inode */
		buffer_drop(&sfs->sfs_absfs, sv->sv_ino, SFS_BLOCKSIZE);
		sfs_bfree(sfs, sv->sv_ino, t);
	} else {
		sfs_release_inode(sv);
	}

	if (buffers_needed) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
	}

	/* Remove the vnode structure from the table in the struct sfs_fs. */
	num = vnodearray_num(sfs->sfs_vnodes);
	ix = num;
	for (i=0; i<num; i++) {
		struct vnode *v2 = vnodearray_get(sfs->sfs_vnodes, i);
		struct sfs_vnode *sv2 = v2->vn_data;
		if (sv2 == sv) {
			ix = i;
			break;
		}
	}
	if (ix == num) {
		panic("sfs: reclaim vnode %u not in vnode pool\n",
		      sv->sv_ino);
	}
	vnodearray_remove(sfs->sfs_vnodes, ix);

	result = commit(t, v->vn_fs);
	if (result) {
		panic("panic for now");
	}

	VOP_CLEANUP(&sv->sv_v);

	lock_release(sfs->sfs_vnlock);
	lock_release(sv->sv_lock);

	sfs_destroy_vnode(sv);

	/* Done */
	return 0;
}

/*
 * Called for read(). sfs_io() does the work.
 *
 * Locking: gets/releases vnode lock.
 * 
 * Requires up to 3 buffers.
 */
static
int
sfs_read(struct vnode *v, struct uio *uio)
{
	struct sfs_vnode *sv = v->vn_data;
	int result;

	KASSERT(uio->uio_rw==UIO_READ);

	lock_acquire(sv->sv_lock);
	reserve_buffers(3, SFS_BLOCKSIZE);

	result = sfs_io(sv, uio, NULL);

	unreserve_buffers(3, SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);

	return result;
}

/*
 * Called for write(). sfs_io() does the work.
 *
 * Locking: gets/releases vnode lock.
 * 
 * Requires up to 3 buffers.
 */
static
int
sfs_write(struct vnode *v, struct uio *uio)
{
	struct sfs_vnode *sv = v->vn_data;
	int result;

	KASSERT(uio->uio_rw==UIO_WRITE);

	//kprintf("SFS_WRITE\n");
	// ENTRYPOINT: Create transaction
	struct transaction *t = create_transaction();

	lock_acquire(sv->sv_lock);
	reserve_buffers(3, SFS_BLOCKSIZE);

	result = sfs_io(sv, uio, t);

	result = commit(t, v->vn_fs);
	if (result) { // TODO: abort?
		panic("panic for now");
	}

	unreserve_buffers(3, SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);

	return result;
}

/*
 * Called for getdirentry()
 *
 * Locking: gets/releases vnode lock.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_getdirentry(struct vnode *v, struct uio *uio)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_dir tsd;
	off_t pos;
	int nentries;
	int result;

	KASSERT(uio->uio_offset >= 0);
	KASSERT(uio->uio_rw==UIO_READ);
	lock_acquire(sv->sv_lock);
	reserve_buffers(4, SFS_BLOCKSIZE);

	result = sfs_load_inode(sv);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	result = sfs_dir_nentries(sv, &nentries);
	if (result) {
		sfs_release_inode(sv);
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	/* Use uio_offset as the slot index. */
	pos = uio->uio_offset;

	while (1) {
		if (pos >= nentries) {
			/* EOF */
			result = 0;
			break;
		}

		result = sfs_readdir(sv, &tsd, pos);
		if (result) {
			break;
		}

		pos++;

		if (tsd.sfd_ino == SFS_NOINO) {
			/* Blank entry */
			continue;
		}

		/* Ensure null termination, just in case */
		tsd.sfd_name[sizeof(tsd.sfd_name)-1] = 0;

		result = uiomove(tsd.sfd_name, strlen(tsd.sfd_name), uio);
		break;
	}

	sfs_release_inode(sv);

	unreserve_buffers(4, SFS_BLOCKSIZE);

	lock_release(sv->sv_lock);

	/* Update the offset the way we want it */
	uio->uio_offset = pos;

	return result;
}

/*
 * Called for ioctl()
 * Locking: not needed.
 */
static
int
sfs_ioctl(struct vnode *v, int op, userptr_t data)
{
	/*
	 * No ioctls.
	 */

	(void)v;
	(void)op;
	(void)data;

	return EINVAL;
}

/*
 * Called for stat/fstat/lstat.
 *
 * Locking: gets/releases vnode lock.
 * 
 * Requires 1 buffer.
 */
static
int
sfs_stat(struct vnode *v, struct stat *statbuf)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_inode *inodeptr;
	int result;

	/* Fill in the stat structure */
	bzero(statbuf, sizeof(struct stat));

	result = VOP_GETTYPE(v, &statbuf->st_mode);
	if (result) {
		return result;
	}

	lock_acquire(sv->sv_lock);
	
	reserve_buffers(1, SFS_BLOCKSIZE);

	result = sfs_load_inode(sv);
	if (result) {
		unreserve_buffers(1, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	inodeptr = buffer_map(sv->sv_buf);

	statbuf->st_size = inodeptr->sfi_size;

	/* We don't support these yet; you get to implement them */
	statbuf->st_nlink = 0;
	statbuf->st_blocks = 0;

	/* Fill in other fields as desired/possible... */

	sfs_release_inode(sv);
	unreserve_buffers(1, SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);

	return 0;
}

/*
 * Return the type of the file (types as per kern/stat.h)
 * Locking: not needed (the type of the vnode is fixed once it's created)
 */
static
int
sfs_gettype(struct vnode *v, uint32_t *ret)
{
	struct sfs_vnode *sv = v->vn_data;

	switch (sv->sv_type) {
	case SFS_TYPE_FILE:
		*ret = S_IFREG;
		return 0;
	case SFS_TYPE_DIR:
		*ret = S_IFDIR;
		return 0;
	}
	panic("sfs: gettype: Invalid inode type (inode %u, type %u)\n",
	      sv->sv_ino, sv->sv_type);
	return EINVAL;
}

/*
 * Check for legal seeks on files. Allow anything non-negative.
 * We could conceivably, here, prohibit seeking past the maximum
 * file size our inode structure can support, but we don't - few
 * people ever bother to check lseek() for failure and having
 * read() or write() fail is sufficient.
 * 
 * Locking: not needed
 */
static
int
sfs_tryseek(struct vnode *v, off_t pos)
{
	if (pos<0) {
		return EINVAL;
	}

	/* Allow anything else */
	(void)v;

	return 0;
}

/*
 * Called for fsync().
 *
 * Since for now the buffer cache can't sync just one file, sync the
 * whole fs.
 * 
 * Locking: gets/releases vnode lock.
 */
static
int
sfs_fsync(struct vnode *v)
{
	struct sfs_vnode *sv = v->vn_data;

	return FSOP_SYNC(sv->sv_v.vn_fs);
}

/*
 * Called for mmap().
 */
static
int
sfs_mmap(struct vnode *v   /* add stuff as needed */)
{
	(void)v;
	return EUNIMP;
}

/*
 * Do the work of truncating a file (or directory)
 *
 * Locking: must hold vnode lock. Acquires/releases buffer locks.
 * 
 * Requires up to 4 buffers.
 */
static
int
sfs_dotruncate(struct vnode *v, off_t len, struct transaction *t)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;

	/* Length in blocks (divide rounding up) */
	uint32_t blocklen = DIVROUNDUP(len, SFS_BLOCKSIZE);

	struct sfs_inode *inodeptr;
	struct buf *idbuf, *didbuf, *tidbuf;
	uint32_t *iddata = NULL, *diddata = NULL, *tiddata = NULL ;
	uint32_t block, i;
	uint32_t idblock, baseblock, highblock, didblock, tidblock;
	int result = 0, final_result = 0;
	int id_hasnonzero = 0, did_hasnonzero = 0, tid_hasnonzero = 0;
	struct record *r;

	KASSERT(lock_do_i_hold(sv->sv_lock));

	result = sfs_load_inode(sv);
	if (result) {
		return result;
	}
	inodeptr = buffer_map(sv->sv_buf);

	hold_buffer_cache(t,sv->sv_buf);

	/*
	 * Go through the direct blocks. Discard any that are
	 * past the limit we're truncating to.
	 */
	for (i=0; i<SFS_NDIRECT; i++) {
		block = inodeptr->sfi_direct[i];
		if (i >= blocklen && block != 0) {
			sfs_bfree(sfs, block, t);
			inodeptr->sfi_direct[i] = 0;

			r = makerec_inode(sv->sv_ino,0,0,i,0);
			int log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;
		}
	}

	/* Indirect block number */
	idblock = inodeptr->sfi_indirect;

	/* Double indirect block number */
	didblock = inodeptr->sfi_dindirect;

	/* Triple indirect block number */
	tidblock = inodeptr->sfi_tindirect;

	/* The lowest block in the indirect block */
	baseblock = SFS_NDIRECT;

	/* The highest block in the file */
	highblock = baseblock + SFS_DBPERIDB + SFS_DBPERIDB* SFS_DBPERIDB +
			SFS_DBPERIDB* SFS_DBPERIDB* SFS_DBPERIDB - 1;


	/* We are going to cycle through all the blocks, changing levels
	 * of indirection. And free the ones that are past the new end
	 * of file.
	 */
	if (blocklen < highblock)
	{
		int indir = 1, level3 = 0, level2 = 0, level1 = 0;
		int id_modified = 0, did_modified = 0, tid_modified = 0;

		while(indir <= 3)
		{

			if(indir == 1)
			{
				baseblock = SFS_NDIRECT;
				if(idblock == 0)
				{
					indir++;
					continue;
				}
			}
			if(indir == 2)
			{
				baseblock = SFS_NDIRECT + SFS_DBPERIDB;
				if(didblock == 0)
				{
					indir++;
					continue;
				}
			}
			if(indir == 3 )
			{
				baseblock = SFS_NDIRECT + SFS_DBPERIDB + SFS_DBPERIDB * SFS_DBPERIDB;
				if(tidblock == 0)
				{
					indir++;
					continue;
				}
			}

			if(indir == 1)
			{
				/* If the level of indirection is 1, we are
				 * cycling through the blocks reachable from
				 * our indirect blocks. Read the indirect block
				 */
				KASSERT(idblock != 0);  // otherwise we would not be here

				/* Read the indirect block */
				result = buffer_read(sv->sv_v.vn_fs, idblock,
						SFS_BLOCKSIZE, &idbuf);
				/* if there's an error, guess we just lose
				 * all the blocks referenced by this indirect
				 * block!
				 */
				if (result) {
					kprintf("sfs_dotruncate: error reading indirect block %u: %s\n",
							idblock, strerror(result));
					final_result = result;
					indir++;
					continue;
				}

				/* We do not need to execute the parts for double and triple
				 * levels of indirection.
				 */
				goto ilevel1;
			}
			if(indir == 2)
			{
				KASSERT(didblock != 0);  // otherwise we would not be here

				/* Read the double indirect block */
				result = buffer_read(sv->sv_v.vn_fs, didblock,
						SFS_BLOCKSIZE, &didbuf);
				/* if there's an error, guess we just lose
				 * all the blocks referenced by this double-indirect
				 * block!
				 */
				if (result) {
					kprintf("sfs_dotruncate: error reading double indirect block %u: %s\n",
							didblock, strerror(result));
					final_result = result;
					indir++;
					continue;
				}

				/* We do not need to execute the parts for the triple
				 * level of indirection.
				 */
				goto ilevel2;
			}
			if(indir == 3)
			{

				KASSERT(tidblock != 0);  // otherwise we would not be here

				/* Read the triple indirect block */
				result = buffer_read(sv->sv_v.vn_fs, tidblock,
						SFS_BLOCKSIZE, &tidbuf);
				/* if there's an error, guess we just lose
				 * all the blocks referenced by this triple-indirect
				 * block!
				 */
				if (result) {
					kprintf("sfs_dotruncate: error reading triple indirect block %u: %s\n",
							tidblock, strerror(result));
					final_result = result;
					indir++;
					continue;
				}

				goto ilevel3;
			}

			/* This is the loop for level of indirection 3
			 * Go through all double indirect blocks pointed to from this triple
			 * indirect block, discard the ones that are past the new end of file.
			 */

			ilevel3:
			tiddata = buffer_map(tidbuf);
			hold_buffer_cache(t,tidbuf);

			for(level3 = 0; level3 < SFS_DBPERIDB; level3++)
			{
				if(blocklen >= baseblock +
						SFS_DBPERIDB * SFS_DBPERIDB * (level3)
						|| tiddata[level3] == 0)
				{
					if(tiddata[level3] != 0)
					{
						tid_hasnonzero = 1;
					}
					continue;
				}

				/* Read the double indirect block, hand it to the next
				 * inner loop.
				 */

				didblock = tiddata[level3];
				result = buffer_read(sv->sv_v.vn_fs, didblock,
						SFS_BLOCKSIZE, &didbuf);

				/* if there's an error, guess we just lose
				 * all the blocks referenced by this double-indirect
				 * block!
				 */
				if (result) {
					kprintf("sfs_dotruncate: error reading double indirect block %u: %s\n",
							didblock, strerror(result));
					final_result = result;
					continue;
				}

				/* This is the loop for level of indirection 2
				 * Go through all indirect blocks pointed to from this double
				 * indirect block, discard the ones that are past the new end of file.
				 */
				ilevel2:
				diddata = buffer_map(didbuf);
				hold_buffer_cache(t,didbuf);

				for(level2 = 0; level2 < SFS_DBPERIDB; level2++)
				{
					/* Discard any blocks that are past the new EOF */
					if (blocklen >= baseblock +
							(level3) * SFS_DBPERIDB * SFS_DBPERIDB +
							(level2) * SFS_DBPERIDB
							|| diddata[level2] == 0)
					{
						if(diddata[level2] != 0)
						{
							did_hasnonzero = 1;
						}
						continue;
					}

					/* Read the indirect block, hand it to the next
					 * inner loop.
					 */

					idblock = diddata[level2];
					result = buffer_read(sv->sv_v.vn_fs, idblock,
							SFS_BLOCKSIZE, &idbuf);
					/* if there's an error, guess we just lose
					 * all the blocks referenced by this indirect
					 * block!
					 */
					if (result) {
						kprintf("sfs_dotruncate: error reading indirect block %u: %s\n",
								idblock, strerror(result));
						final_result = result;
						continue;
					}


					/* This is the loop for level of indirection 1
					 * Go through all direct blocks pointed to from this indirect
					 * block, discard the ones that are past the new end of file.
					 */
					ilevel1:
					iddata = buffer_map(idbuf);
					hold_buffer_cache(t,idbuf);

					for (level1 = 0; level1<SFS_DBPERIDB; level1++)
					{
						/* Discard any blocks that are past the new EOF */
						if (blocklen < baseblock +
								(level3) * SFS_DBPERIDB * SFS_DBPERIDB +
								(level2) * SFS_DBPERIDB + level1
								&& iddata[level1] != 0)
						{

							int block = iddata[level1];
							iddata[level1] = 0;

							r = makerec_inode(sv->sv_ino,1,0,level1,0);

							id_modified = 1;

							sfs_bfree(sfs, block, t);
						}

						/* Remember if we see any nonzero blocks in here */
						if (iddata[level1]!=0)
						{
							id_hasnonzero=1;
						}
					}/* end for level 1*/

					if (!id_hasnonzero)
					{
						/* The whole indirect block is empty now; free it */
						sfs_bfree(sfs, idblock, t);
						if(indir == 1)
						{
							inodeptr->sfi_indirect = 0;
							r = makerec_inode(sv->sv_ino,1,1,0,0);
							int log_ret = check_and_record(r,t);
							if (log_ret)
								return log_ret;
						}
						if(indir != 1)
						{
							did_modified = 1;
							diddata[level2] = 0;
						}
					}
					else if(id_modified)
					{
						/* The indirect block has been modified */
						buffer_mark_dirty(idbuf);
						if(indir != 1)
						{
							did_hasnonzero = 1;
						}
					}

					buffer_release(idbuf);

					/* If we are just doing 1 level of indirection,
					 * break out of the loop
					 */
					if(indir == 1)
					{
						break;
					}
				} /* end for level2 */

				/* If we are just doing 1 level of indirection,
				 * break out of the loop
				 */
				if(indir == 1)
				{
					break;
				}

				if (!did_hasnonzero)
				{
					/* The whole double indirect block is empty now; free it */
					sfs_bfree(sfs, didblock, t);
					if(indir == 2)
					{
						inodeptr->sfi_dindirect = 0;

						r = makerec_inode(sv->sv_ino,2,1,0,0);
						int log_ret = check_and_record(r,t);
						if (log_ret)
							return log_ret;

						buffer_mark_dirty(sv->sv_buf);
					}
					if(indir == 3)
					{
						tid_modified = 1;
						tiddata[level3] = 0;
					}
				}
				else if(did_modified)
				{
					/* The double indirect block has been modified */
					buffer_mark_dirty(didbuf);
					if(indir == 3)
					{
						tid_hasnonzero = 1;
					}
				}

				buffer_release(didbuf);
				if(indir < 3)
				{
					break;
				}
			} /* end for level 3 */
			if(indir < 3)
			{
				indir++;
				continue;  /* while */
			}
			if (!tid_hasnonzero)
			{
				/* The whole triple indirect block is empty now; free it */
				sfs_bfree(sfs, tidblock, t);
				inodeptr->sfi_tindirect = 0;

				r = makerec_inode(sv->sv_ino,3,1,0,0);
				int log_ret = check_and_record(r,t);
				if (log_ret)
					return log_ret;
			}
			else if(tid_modified)
			{
				/* The triple indirect block has been modified */
				buffer_mark_dirty(tidbuf);
			}
			buffer_release(tidbuf);
			indir++;
		}
	}


	/* Set the file size */
	inodeptr->sfi_size = len;

	r = makerec_isize(sv->sv_ino,len);
	int log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	/* Mark the inode dirty */
	buffer_mark_dirty(sv->sv_buf);

	/* release the inode buffer */
	sfs_release_inode(sv);

	return final_result;
}

/*
 * Truncate a file (or directory)
 *
 * Locking: gets/releases vnode lock.
 * 
 * Requires up to 4 buffers.
 */
static
int
sfs_truncate(struct vnode *v, off_t len)
{
	struct sfs_vnode *sv = v->vn_data;
	int result;

	//kprintf("SFS_TRUNCATE\n");
	//ENTRYPOINT
	struct transaction *t = create_transaction();

	lock_acquire(sv->sv_lock);
	reserve_buffers(4, SFS_BLOCKSIZE);

	result = sfs_dotruncate(v, len, t);

	result = commit(t, v->vn_fs);
	if (result) {
		panic("panic for now");
	}

	unreserve_buffers(4, SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);

	return result;
}

/*
 * Helper function for sfs_namefile.
 * 
 * Locking: must hold vnode lock on parent.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_getonename(struct sfs_vnode *parent, uint32_t targetino,
	       char *buf, size_t *bufpos)
{
	size_t bp = *bufpos;

	struct sfs_dir sd;
	size_t namelen;
	int result;

	KASSERT(lock_do_i_hold(parent->sv_lock));
	KASSERT(targetino != SFS_NOINO);

	result = sfs_dir_findino(parent, targetino, &sd, NULL);
	if (result) {
		return result;
	}

	/* include a trailing slash in the length */
	namelen = strlen(sd.sfd_name)+1;
	if (namelen > bp) {
		/* 
		 * Doesn't fit. ERANGE is the error from the BSD man page,
		 * even though ENAMETOOLONG would make more sense...
		 */
		return ERANGE;
	}
	buf[bp-1] = '/';
	memmove(buf+bp-namelen, sd.sfd_name, namelen-1);
	*bufpos = bp-namelen;
	return 0;
}

/*
 * Get the full pathname for a file. This only needs to work on directories.
 * 
 * Locking: Gets/releases vnode locks, but only one at a time.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_namefile(struct vnode *vv, struct uio *uio)
{
	struct sfs_vnode *sv = vv->vn_data;
	struct sfs_vnode *parent = NULL;
	int result;
	char *buf;
	size_t bufpos, bufmax, len;

	KASSERT(uio->uio_rw == UIO_READ);
	
	bufmax = uio->uio_resid+1;
	if (bufmax > PATH_MAX) {
		return EINVAL;
	}

	buf = kmalloc(bufmax);
	if (buf == NULL) {
		return ENOMEM;
	}

	reserve_buffers(3, SFS_BLOCKSIZE);

	bufpos = bufmax;

	VOP_INCREF(&sv->sv_v);

	while (1) {
		lock_acquire(sv->sv_lock);
		/* not allowed to lock child since we're going up the tree */
		result = sfs_lookonce(sv, "..", &parent, NULL, false);
		lock_release(sv->sv_lock);

		if (result) {
			VOP_DECREF(&sv->sv_v);
			kfree(buf);
			unreserve_buffers(3, SFS_BLOCKSIZE);
			return result;
		}

		if (parent == sv) {
			/* .. was equal to . - must be root, so we're done */
			VOP_DECREF(&parent->sv_v);
			VOP_DECREF(&sv->sv_v);
			break;
		}

		lock_acquire(parent->sv_lock);
		result = sfs_getonename(parent, sv->sv_ino, buf, &bufpos);
		lock_release(parent->sv_lock);

		if (result) {
			VOP_DECREF(&parent->sv_v);
			VOP_DECREF(&sv->sv_v);
			kfree(buf);
			unreserve_buffers(3, SFS_BLOCKSIZE);
			return result;
		}

		VOP_DECREF(&sv->sv_v);
		sv = parent;
		parent = NULL;
	}

	/* Done looking, now send back the string */

	if (bufmax == bufpos) {
		/* root directory; do nothing (send back empty string) */
		result = 0;
	}
	else {
		len = bufmax - bufpos;
		len--;  /* skip the trailing slash */
		KASSERT(len <= uio->uio_resid);
		result = uiomove(buf+bufpos, len, uio);
	}

	kfree(buf);
	unreserve_buffers(3, SFS_BLOCKSIZE);
	return result;
}
/*
 * Create a file. If EXCL is set, insist that the filename not already
 * exist; otherwise, if it already exists, just open it.
 *
 * Locking: Gets/releases the vnode lock for v. Does not lock the new vnode,
 * as nobody else can get to it except by searching the directory it's in,
 * which is locked.
 * 
 * Requires up to 4 buffers (VOP_DECREF may require 3 if it triggers reclaim).
 */
static
int
sfs_creat(struct vnode *v, const char *name, bool excl, mode_t mode,
	  struct vnode **ret)
{
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_vnode *newguy;
	struct sfs_inode *sv_inodebuf;
	struct sfs_inode *new_inodeptr;
	uint32_t ino;
	int result;
	struct record *r;
	//kprintf("SFS_CREAT\n");

	// ENTRYPOINT: Begin transaction
	struct transaction *t = create_transaction();

	lock_acquire(sv->sv_lock);
	
	reserve_buffers(4, SFS_BLOCKSIZE);

	result = sfs_load_inode(sv);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}
	sv_inodebuf = buffer_map(sv->sv_buf);
	
	if (sv_inodebuf->sfi_linkcount == 0) {
		sfs_release_inode(sv);
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return ENOENT;
	}
	
	sfs_release_inode(sv);
	
	/* Look up the name */
	result = sfs_dir_findname(sv, name, &ino, NULL, NULL);
	if (result!=0 && result!=ENOENT) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	/* If it exists and we didn't want it to, fail */
	if (result==0 && excl) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return EEXIST;
	}

	if (result==0) {
		/* We got a file; load its vnode and return */
		result = sfs_loadvnode(sfs, ino, SFS_TYPE_INVAL, &newguy,false, t);
		if (result) {
			unreserve_buffers(4, SFS_BLOCKSIZE);
			lock_release(sv->sv_lock);
			return result;
		}
		*ret = &newguy->sv_v;
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return 0;
	}

	/* Didn't exist - create it */
	result = sfs_makeobj(sfs, SFS_TYPE_FILE, &newguy, t);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}
	new_inodeptr = buffer_map(newguy->sv_buf);
	hold_buffer_cache(t,newguy->sv_buf);

	/* We don't currently support file permissions; ignore MODE */
	(void)mode;

	/* Link it into the directory */
	result = sfs_dir_link(sv, name, newguy->sv_ino, NULL, t);
	if (result) {
		sfs_release_inode(newguy);
		lock_release(newguy->sv_lock);
		VOP_DECREF(&newguy->sv_v);
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	/* Update the linkcount of the new file */
	new_inodeptr->sfi_linkcount++;

	r = makerec_ilink(sv->sv_ino,new_inodeptr->sfi_linkcount);
	int log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	/* and consequently mark it dirty. */
	buffer_mark_dirty(newguy->sv_buf);
	sfs_release_inode(newguy);

	*ret = &newguy->sv_v;

	result = commit(t, v->vn_fs);
	if (result) {
		panic("panic for now");
	}

	unreserve_buffers(4, SFS_BLOCKSIZE);
	lock_release(newguy->sv_lock);
	lock_release(sv->sv_lock);
	return 0;
}

/*
 * Make a hard link to a file.
 * The VFS layer should prevent this being called unless both
 * vnodes are ours.
 *
 * Locking: locks both vnodes, but not at once. (Because the target
 * file cannot be reclaimed/erased until we drop our reference to it,
 * there's no need to hold its lock across the directory operation.)
 * 
 * Requires up to 4 buffers.
 */
static
int
sfs_link(struct vnode *dir, const char *name, struct vnode *file)
{
	struct sfs_vnode *sv = dir->vn_data;
	struct sfs_vnode *f = file->vn_data;
	struct sfs_inode *inodeptr;
	int slot;
	int result, result2;
	struct record *r;

	//kprintf("SFS_LINK\n");
	// ENTRYPOINT
	struct transaction *t = create_transaction();

	KASSERT(file->vn_fs == dir->vn_fs);

	reserve_buffers(4, SFS_BLOCKSIZE);

	/* directory must be locked first */
	lock_acquire(sv->sv_lock);

	/* Just create a link */
	result = sfs_dir_link(sv, name, f->sv_ino, &slot, t);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}

	lock_acquire(f->sv_lock);
	result = sfs_load_inode(f);
	if (result) {
		result2 = sfs_dir_unlink(sv, slot, t);
		if (result2) { /* eep? */
			panic("sfs_link: could not unwind link in inode %u, slot %d!\n",
					sv->sv_ino, slot);
		}
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(f->sv_lock);
		lock_release(sv->sv_lock);
		return result;
	}

	/* and update the link count, marking the inode dirty */
	inodeptr = buffer_map(f->sv_buf);
	hold_buffer_cache(t,f->sv_buf);

	inodeptr->sfi_linkcount++;

	r = makerec_ilink(f->sv_ino,inodeptr->sfi_linkcount);
	int log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	buffer_mark_dirty(f->sv_buf);

	result = commit(t, dir->vn_fs);
	if (result) {
		panic("panic for now");
	}

	sfs_release_inode(f);
	unreserve_buffers(4, SFS_BLOCKSIZE);
	lock_release(f->sv_lock);
	lock_release(sv->sv_lock);

	return 0;
}

/*
 * Create a directory.
 *
 * Locking: Acquires vnode lock on both parent and new directory.
 * Note that the ordering is not significant - nobody can hold the
 * lock on the new directory, because we just created it and nobody
 * else can get to it until we unlock the parent at the end.
 * 
 * Requires up to 4 buffers;
 */

static
int
sfs_mkdir(struct vnode *v, const char *name, mode_t mode)
{
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct sfs_vnode *sv = v->vn_data;
	int result;
	uint32_t ino;
	struct sfs_inode *dir_inodeptr;
	struct sfs_inode *new_inodeptr;
	struct sfs_vnode *newguy;
	struct record *r;
	int log_ret;

	//kprintf("SFS_MKDIR\n");
	// ENTRYPOINT
	struct transaction *t = create_transaction();

	(void)mode;

	lock_acquire(sv->sv_lock);
	reserve_buffers(4, SFS_BLOCKSIZE);
	
	result = sfs_load_inode(sv);
	if (result) {
		goto die_early;
	}
	dir_inodeptr = buffer_map(sv->sv_buf);
	hold_buffer_cache(t,sv->sv_buf);
	
	if (dir_inodeptr->sfi_linkcount == 0) {
		result = ENOENT;
		goto die_simple;
	}

	/* Look up the name */
	result = sfs_dir_findname(sv, name, &ino, NULL, NULL);
	if (result!=0 && result!=ENOENT) {
		goto die_simple;
	}

	/* If it exists, fail */
	if (result==0) {
		result = EEXIST;
		goto die_simple;
	}

	result = sfs_makeobj(sfs, SFS_TYPE_DIR, &newguy, t);
	if (result) {
		goto die_simple;
	}
	new_inodeptr = buffer_map(newguy->sv_buf);
	hold_buffer_cache(t,newguy->sv_buf);

	result = sfs_dir_link(newguy, ".", newguy->sv_ino, NULL, t);
	if (result) {
		goto die_uncreate;
	}

	result = sfs_dir_link(newguy, "..", sv->sv_ino, NULL, t);
	if (result) {
		goto die_uncreate;
	}

	result = sfs_dir_link(sv, name, newguy->sv_ino, NULL, t);
	if (result) {
		goto die_uncreate;
	}

        /*
         * Increment link counts (Note: not until after the names are
         * added - that way if one fails, the link count will be zero,
         * and reclaim will dispose of the new directory.
         *
         * Note also that the name in the parent directory gets added
         * last, so there's no case in which we have to go back and
         * remove it.
         */

	new_inodeptr->sfi_linkcount += 2;

	r = makerec_ilink(newguy->sv_ino, new_inodeptr->sfi_linkcount);
	log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	dir_inodeptr->sfi_linkcount++;

	r = makerec_ilink(sv->sv_ino, dir_inodeptr->sfi_linkcount);
	log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	buffer_mark_dirty(newguy->sv_buf);
	sfs_release_inode(newguy);
	buffer_mark_dirty(sv->sv_buf);
	sfs_release_inode(sv);

	lock_release(newguy->sv_lock);
	lock_release(sv->sv_lock);
	VOP_DECREF(&newguy->sv_v);

	result = commit(t, v->vn_fs);
	if (result) {
		panic("panic for now");
	}

	unreserve_buffers(4, SFS_BLOCKSIZE);

	KASSERT(result==0);
	return result;

die_uncreate:
	sfs_release_inode(newguy);
	lock_release(newguy->sv_lock);
	VOP_DECREF(&newguy->sv_v);

die_simple:
	sfs_release_inode(sv);

die_early:
	unreserve_buffers(4, SFS_BLOCKSIZE);
	lock_release(sv->sv_lock);
	return result;
}

/*
 * Delete a directory.
 *
 * Locking: Acquires vnode lock for parent dir and then vnode lock for 
 * victim dir. Releases both.
 * 
 * Requires 4 buffers.
 */
static
int
sfs_rmdir(struct vnode *v, const char *name)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_vnode *victim;
	struct sfs_inode *dir_inodeptr;
	struct sfs_inode *victim_inodeptr;
	int result;
	int slot;
	int log_ret;
	struct record *r;

	//kprintf("SFS_RMDIR\n");
	// ENTRYPOINT
	struct transaction *t = create_transaction();

	/* Cannot remove the . or .. entries from a directory! */
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		return EINVAL;
	}

	lock_acquire(sv->sv_lock);
	reserve_buffers(4, SFS_BLOCKSIZE);

	result = sfs_load_inode(sv);
	if (result) {
		goto die_early;
	}
	dir_inodeptr = buffer_map(sv->sv_buf);
	hold_buffer_cache(t,sv->sv_buf);

	if (dir_inodeptr->sfi_linkcount == 0) {
		result = ENOENT;
		goto die_simple;
	}

	result = sfs_lookonce(sv, name, &victim, true, &slot);
	if (result) {
		goto die_simple;
	}
	victim_inodeptr = buffer_map(victim->sv_buf);
	hold_buffer_cache(t,victim->sv_buf);

	if (victim->sv_ino == SFS_ROOT_LOCATION) {
		result = EPERM;
		goto die_total;
	}

	/* Only allowed on directories */
	if (victim_inodeptr->sfi_type != SFS_TYPE_DIR) {
		result = ENOTDIR;
		goto die_total;
	}

	result = sfs_dir_checkempty(victim);
	if (result) {
		goto die_total;
	}

	result = sfs_dir_unlink(sv, slot, t);
	if (result) {
		goto die_total;
	}

	KASSERT(dir_inodeptr->sfi_linkcount > 1);
	KASSERT(victim_inodeptr->sfi_linkcount==2);

	dir_inodeptr->sfi_linkcount--;

	r = makerec_ilink(sv->sv_ino,dir_inodeptr->sfi_linkcount);
	log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	buffer_mark_dirty(sv->sv_buf);

	victim_inodeptr->sfi_linkcount -= 2;

	r = makerec_ilink(victim->sv_ino,victim_inodeptr->sfi_linkcount);
	log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	buffer_mark_dirty(victim->sv_buf);
	/* buffer released below */

	result = sfs_dotruncate(&victim->sv_v, 0, t);

die_total:
	sfs_release_inode(victim);
	lock_release(victim->sv_lock);
 	VOP_DECREF(&victim->sv_v);
die_simple:
	sfs_release_inode(sv);
die_early:
 	unreserve_buffers(4, SFS_BLOCKSIZE);
 	lock_release(sv->sv_lock);

	return result;
}

/*
 * Delete a file.
 *
 * Locking: locks the directory, then the file. Unlocks both.
 *   This follows the hierarchical locking order imposed by the directory tree.
 *   
 * Requires up to 4 buffers.
 */
static
int
sfs_remove(struct vnode *dir, const char *name)
{
	struct sfs_vnode *sv = dir->vn_data;
	struct sfs_vnode *victim;
	struct sfs_inode *victim_inodeptr;
	struct sfs_inode *dir_inodeptr;
	int slot;
	int result;

	kprintf("SFS_REMOVE\n");
	// ENTRYPOINT: transaction is started after error checks pass

	/* need to check this to avoid deadlock even in error condition */
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		return EISDIR;
	}

	lock_acquire(sv->sv_lock);
	reserve_buffers(4, SFS_BLOCKSIZE);

	result = sfs_load_inode(sv);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}
	dir_inodeptr = buffer_map(sv->sv_buf);

	if (dir_inodeptr->sfi_linkcount == 0) {
		sfs_release_inode(sv);
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return ENOENT;
	}

	/* Look for the file and fetch a vnode for it. */
	result = sfs_lookonce(sv, name, &victim, true, &slot);
	if (result) {
		sfs_release_inode(sv);
		unreserve_buffers(4, SFS_BLOCKSIZE);
		lock_release(sv->sv_lock);
		return result;
	}
	victim_inodeptr = buffer_map(victim->sv_buf);

	/* Not allowed on directories */
	if (victim_inodeptr->sfi_type == SFS_TYPE_DIR) {
		sfs_release_inode(sv);
		sfs_release_inode(victim);
		lock_release(victim->sv_lock);
		lock_release(sv->sv_lock);
		VOP_DECREF(&victim->sv_v);
		unreserve_buffers(4, SFS_BLOCKSIZE);
		return EISDIR;
	}

	// ENTRYPOINT
	struct transaction *t = create_transaction();

	/* Erase its directory entry. */
	result = sfs_dir_unlink(sv, slot, t);
	if (result==0) {
		/* If we succeeded, decrement the link count. */
		KASSERT(victim_inodeptr->sfi_linkcount > 0);
		victim_inodeptr->sfi_linkcount--;
		buffer_mark_dirty(victim->sv_buf);

		struct record *r = makerec_ilink(victim->sv_ino,victim_inodeptr->sfi_linkcount);
		int log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;
		hold_buffer_cache(t,victim->sv_buf);
	}

	sfs_release_inode(sv);
	sfs_release_inode(victim);
	lock_release(victim->sv_lock);

	/* Discard the reference that sfs_lookonce got us */
	VOP_DECREF(&victim->sv_v);

	result = commit(t, dir->vn_fs);
	if (result) {
		panic("panic for now");
	}
	unreserve_buffers(4, SFS_BLOCKSIZE);

	lock_release(sv->sv_lock);
	return result;
}

/*
 * Simple helper function for rename.
 */
static
void
recovermsg(int result, int result2)
{
	kprintf("sfs_rename: %s: while recovering: %s\n", strerror(result),
		strerror(result2));
}

/*
 * Helper function for rename. Make sure COMPARE is not a direct
 * ancestor of (or the same as) CHILD.
 *
 * Note: acquires locks as it goes up.
 */
static
int
check_parent(struct sfs_vnode *lookfor, struct sfs_vnode *failon, 
	     struct sfs_vnode *child, int *found)
{
	struct sfs_vnode *up;
	int result;

	*found = 0;

	VOP_INCREF(&child->sv_v);
	while (1) {
		if (failon == child) {
			/* Bad */
			VOP_DECREF(&child->sv_v);
			return EINVAL;
		}

		if (lookfor == child) {
			*found = 1;
		}

		lock_acquire(child->sv_lock);
		result = sfs_lookonce(child, "..", &up, false, NULL);

		lock_release(child->sv_lock);

		if (result) {
			VOP_DECREF(&child->sv_v);
			return result;
		}
		if (child == up) {
			/* Hit root, done */
			VOP_DECREF(&up->sv_v);
			break;
		}
		VOP_DECREF(&child->sv_v);
		child = up;
	}

	VOP_DECREF(&child->sv_v);
	return 0;
}


/*
 * Rename a file.
 *
 * Locking:
 *    Locks sfs_renamelock.
 *    Calls check_parent, which locks various directories one at a
 *       time.
 *    Locks the target vnodes and their parents in a complex fashion
 *       (described in detail below) which is carefully arranged so
 *       it won't deadlock with rmdir. Or at least I hope so.
 *    Then unlocks everything.
 *
 *    The rationale for all this is complex. See the comments below.
 *
 * Requires up to 7 buffers.
 */
static
int
sfs_rename(struct vnode *absdir1, const char *name1, 
	   struct vnode *absdir2, const char *name2)
{
	struct sfs_fs *sfs = absdir1->vn_fs->fs_data;
	struct sfs_vnode *dir1 = absdir1->vn_data;
	struct sfs_vnode *dir2 = absdir2->vn_data;
	struct sfs_vnode *obj1=NULL, *obj2=NULL;
	struct sfs_inode *dir1_inodeptr, *dir2_inodeptr;
	struct sfs_inode *obj1_inodeptr, *obj2_inodeptr;
	int slot1=-1, slot2=-1;
	int result, result2;
	struct sfs_dir sd;
	int found_dir1;
	int log_ret;
	struct record *r;

	//kprintf("SFS_RENAME\n");
	// ENTRYPOINT
	struct transaction *t = create_transaction();

	/* The VFS layer is supposed to enforce this */
	KASSERT(absdir1->vn_fs == absdir2->vn_fs);

	if (!strcmp(name1, ".") || !strcmp(name2, ".") ||
	    !strcmp(name1, "..") || !strcmp(name2, "..")) {
		return EINVAL;
	}

	if (strlen(name2)+1 > sizeof(sd.sfd_name)) {
		return ENAMETOOLONG;
	}

	/*
	 * We only allow one rename to occur at a time. This appears
	 * to be necessary to preserve the consistency of the
	 * filesystem: once you do the parent check (that n1 is not an
	 * ancestor of d2/n2) nothing may be allowed to happen that
	 * might invalidate that result until all of the
	 * rearrangements are complete. If other renames are allowed
	 * to proceed, we'd need to lock every descendent of n1 to
	 * make sure that some ancestor of d2/n2 doesn't get inserted
	 * at some point deep down. This is impractical, so we use one
	 * global lock.
	 *
	 * To prevent certain deadlocks while locking the vnodes we
	 * need, the rename lock goes outside all the vnode locks.
	 */

	reserve_buffers(7, SFS_BLOCKSIZE);

	lock_acquire(sfs->sfs_renamelock);

	/*
	 * Get the objects we're moving.
	 *
	 * Lock each directory temporarily. We'll check again later to
	 * make sure they haven't disappeared and to find slots.
	 */
	lock_acquire(dir1->sv_lock);
	result = sfs_lookonce(dir1, name1, &obj1, false, NULL);
	lock_release(dir1->sv_lock);

	if (result) {
		goto out0;
	}

	lock_acquire(dir2->sv_lock);
	result = sfs_lookonce(dir2, name2, &obj2, false, NULL);
	lock_release(dir2->sv_lock);

	if (result && result != ENOENT) {
		goto out0;
	}

	if (result==ENOENT) {
		/*
		 * sfs_lookonce returns a null vnode with ENOENT in
		 * order to make our life easier.
		 */
		KASSERT(obj2==NULL);
	}
	
	/*
	 * Prohibit the case where obj1 is a directory and it's a direct
	 * ancestor in the tree of dir2 (or is the same as dir2). If
	 * that were to be permitted, it'd create a detached chunk of
	 * the directory tree, and we don't like that.
	 *
	 * If we see dir1 while checking up the tree, found_dir1 is
	 * set to true. We use this info to choose the correct ordering
	 * for locking dir1 and dir2.
	 *
	 * To prevent deadlocks, the parent check must be done without
	 * holding locks on any other directories.
	 */
	result = check_parent(dir1, obj1, dir2, &found_dir1);
	if (result) {
		goto out0;
	}

	/*
	 * Now check for cases where some of the four vnodes we have
	 * are the same.
	 *
	 * These cases are, in the order they are handled below:
	 *
	 *    dir1 == obj1		Already checked.
	 *    dir2 == obj2		Already checked.
	 *    dir2 == obj1		Already checked.
	 *    dir1 == obj2		Checked below.
	 *    dir1 == dir2		Legal.
	 *    obj1 == obj2		Legal, special handling.
	 */

	/*
	 * A directory should have no entries for itself other than '.'.  
	 * Thus, since we explicitly reject '.' above, the names
	 * within the directories should not refer to the directories
	 * themselves.
	 */
	KASSERT(dir1 != obj1);
	KASSERT(dir2 != obj2);

	/*
	 * The parent check should have caught this case.
	 */
	KASSERT(dir2 != obj1);

	/*
	 * Check for dir1 == obj2.
	 *
	 * This is not necessarily wrong if obj1 is the last entry in
	 * dir1 (this is essentially "mv ./foo/bar ./foo") but our
	 * implementation doesn't tolerate it. Because we need to
	 * unlink g2 before linking g1 in the new place, it will
	 * always fail complaining that g2 (sv1) isn't empty. We could
	 * just charge ahead and let this happen, but we'll get into
	 * trouble with our locks if we do, so detect this as a
	 * special case and return ENOTEMPTY.
	 */

	if (obj2==dir1) {
		result = ENOTEMPTY;
		goto out0;
	}


	/*
	 * Now we can begin acquiring locks for real.
	 *
	 * If we saw dir1 while doing the parent check, it means
	 * dir1 is higher in the tree than dir2. Thus, we should
	 * lock dir1 before dir2.
	 *
	 * If on the other hand we didn't see dir1, either dir2 is
	 * higher in the tree than dir1, in which case we should lock
	 * dir2 first, or dir1 and dir2 are on disjoint branches of
	 * the tree, in which case (because there's a single rename
	 * lock for the whole fs) it doesn't matter what order we lock
	 * in.
	 *
	 * If we lock dir1 first, we don't need to lock obj1 before
	 * dir2, since (due to the parent check) obj1 cannot be an
	 * ancestor of dir2.
	 *
	 * However, if we lock dir2 first, obj2 must be locked before
	 * dir1, in case obj2 is an ancestor of dir1. (In this case we
	 * will find that obj2 is not empty and cannot be removed, but
	 * we must lock it before we can check that.)
	 *
	 * Thus we lock in this order:
	 *
	 * dir1   (if found_dir1)
	 * dir2
	 * obj2   (if non-NULL)
	 * dir1   (if !found_dir1)
	 * obj1
	 *
	 * Also, look out for the case where both dirs are the same.
	 * (If this is true, found_dir1 will be set.)
	 */

	if (dir1==dir2) {
		/* This locks "both" dirs */
		lock_acquire(dir1->sv_lock);
		KASSERT(found_dir1);
	}
	else {
		if (found_dir1) {
			lock_acquire(dir1->sv_lock);
		}
		lock_acquire(dir2->sv_lock);
	}

	/*
	 * Now lock obj2.
	 *
	 * Note that we must redo the lookup and get a new obj2, as it
	 * may have changed under us. Since we hold the rename lock
	 * for the whole fs, the fs structure cannot have changed, so
	 * we don't need to redo the parent check or any of the checks
	 * for vnode aliasing with dir1 or dir2 above. Note however
	 * that obj1 and obj2 may now be the same even if they weren't
	 * before.
	 */
	KASSERT(lock_do_i_hold(dir2->sv_lock));
	if (obj2) {
		VOP_DECREF(&obj2->sv_v);
		obj2 = NULL;
	}
	result = sfs_lookonce(dir2, name2, &obj2, true, &slot2);
	if (result==0) {
		KASSERT(obj2 != NULL);
		obj2_inodeptr = buffer_map(obj2->sv_buf);
		hold_buffer_cache(t,obj2->sv_buf);
	} else if (result==ENOENT) {
		/*
		 * sfs_lookonce returns a null vnode and an empty slot
		 * with ENOENT in order to make our life easier.
		 */
		KASSERT(obj2==NULL);
		KASSERT(slot2>=0);
	}

	if (!found_dir1) {
		lock_acquire(dir1->sv_lock);
	}

	/* Postpone this check to simplify the error cleanup. */
	if (result != 0 && result != ENOENT) {
		goto out1;
	}

	/*
	 * Now reload obj1.
	 */
	KASSERT(lock_do_i_hold(dir1->sv_lock));
	VOP_DECREF(&obj1->sv_v);
	obj1 = NULL;
	result = sfs_lookonce(dir1, name1, &obj1, false, &slot1);
	if (result) {
		goto out1;
	}
	/*
	 * POSIX mandates that if obj1==obj2, we succeed and nothing
	 * happens.  This is somewhat stupid if obj1==obj2 and dir1 != dir2,
	 * but we'll go with POSIX anyway.
	 */
	if (obj1==obj2) {
		result = 0;
		VOP_DECREF(&obj1->sv_v);
		obj1 = NULL;
		goto out1;
	}
	lock_acquire(obj1->sv_lock);
	result = sfs_load_inode(obj1);
	if (result) {
		lock_release(obj1->sv_lock);
		VOP_DECREF(&obj1->sv_v);
		obj1 = NULL;
		goto out1;
	}
	obj1_inodeptr = buffer_map(obj1->sv_buf);
	hold_buffer_cache(t,obj1->sv_buf);

	result = sfs_load_inode(dir2);
	if (result) {
		goto out2;
	}
	dir2_inodeptr = buffer_map(dir2->sv_buf);
	hold_buffer_cache(t,dir2->sv_buf);

	result = sfs_load_inode(dir1);
	if (result) {
		goto out3;
	}
	dir1_inodeptr = buffer_map(dir1->sv_buf);
	hold_buffer_cache(t,dir1->sv_buf);

	/*
	 * One final piece of paranoia: make sure dir2 hasn't been rmdir'd.
	 * (If dir1 was, the obj1 lookup above would have failed.)
	 */
	if (dir2_inodeptr->sfi_linkcount==0) {
		result = ENOENT;
		goto out4;
	}

	/*
	 * Now we have all the locks we need and we can proceed with
	 * the operation.
	 */

	/* At this point we should have valid slots in both dirs. */
	KASSERT(slot1>=0);
	KASSERT(slot2>=0);

	if (obj2 != NULL) {
		/*
		 * Target already exists.
		 * Must be the same type (file or directory) as the source,
		 * and if a directory, must be empty. Then unlink it.
		 */

		if (obj1_inodeptr->sfi_type == SFS_TYPE_DIR) {
			if (obj2_inodeptr->sfi_type != SFS_TYPE_DIR) {
				result = ENOTDIR;
				goto out4;
			}
			result = sfs_dir_checkempty(obj2);
			if (result) {
				goto out4;
			}

			/* Remove the name */
			result = sfs_dir_unlink(dir2, slot2, t);
			if (result) {
				goto out4;
			}

			/* Dispose of the directory */
			KASSERT(dir2_inodeptr->sfi_linkcount > 1);
			KASSERT(obj2_inodeptr->sfi_linkcount == 2);
			dir2_inodeptr->sfi_linkcount--;

			r = makerec_ilink(dir2->sv_ino,dir2_inodeptr->sfi_linkcount);
			log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			buffer_mark_dirty(dir2->sv_buf);
			obj2_inodeptr->sfi_linkcount -= 2;

			r = makerec_ilink(obj2->sv_ino,obj2_inodeptr->sfi_linkcount);
			log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			buffer_mark_dirty(obj2->sv_buf);

			/* ignore errors on this */
			sfs_dotruncate(&obj2->sv_v, 0, t);
		}
		else {
			KASSERT(obj1->sv_type == SFS_TYPE_FILE);
			if (obj2->sv_type != SFS_TYPE_FILE) {
				result = EISDIR;
				goto out4;
			}

			/* Remove the name */
			result = sfs_dir_unlink(dir2, slot2, t);
			if (result) {
				goto out4;
			}

			/* Dispose of the file */
			KASSERT(obj2_inodeptr->sfi_linkcount > 0);
			obj2_inodeptr->sfi_linkcount--;

			r = makerec_ilink(obj2->sv_ino,obj2_inodeptr->sfi_linkcount);
			log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			buffer_mark_dirty(obj2->sv_buf);
		}

		sfs_release_inode(obj2);

		lock_release(obj2->sv_lock);
		VOP_DECREF(&obj2->sv_v);
		obj2 = NULL;
	}

	/*
	 * At this point the target should be nonexistent and we have
	 * a slot in the target directory we can use. Create a link
	 * there. Do it by hand instead of using sfs_dir_link to avoid
	 * duplication of effort.
	 */
	KASSERT(obj2==NULL);

	bzero(&sd, sizeof(sd));
	sd.sfd_ino = obj1->sv_ino;
	strcpy(sd.sfd_name, name2);

	r = makerec_dir(dir2->sv_ino,slot2,obj1->sv_ino,name2);
	log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	result = sfs_writedir(dir2, &sd, slot2, t);
	if (result) {
		goto out4;
	}

	obj1_inodeptr->sfi_linkcount++;

	r = makerec_ilink(obj1->sv_ino,obj1_inodeptr->sfi_linkcount);
	log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	buffer_mark_dirty(obj1->sv_buf);

	if (obj1->sv_type == SFS_TYPE_DIR) {
		/* Directory: reparent it */
		result = sfs_readdir(obj1, &sd, DOTDOTSLOT);
		if (result) {
			goto recover1;
		}
		if (strcmp(sd.sfd_name, "..")) {
			panic("sfs_rename: moving dir: .. not in slot %d\n",
			      DOTDOTSLOT);
		}
		if (sd.sfd_ino != dir1->sv_ino) {
			panic("sfs_rename: moving dir: .. is i%u not i%u\n",
			      sd.sfd_ino, dir1->sv_ino);
		}
		sd.sfd_ino = dir2->sv_ino;
		result = sfs_writedir(obj1, &sd, DOTDOTSLOT, t);
		if (result) {
			goto recover1;
		}
		dir1_inodeptr->sfi_linkcount--;

		r = makerec_ilink(dir1->sv_ino,dir1_inodeptr->sfi_linkcount);
		log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;

		buffer_mark_dirty(dir1->sv_buf);
		dir2_inodeptr->sfi_linkcount++;

		r = makerec_ilink(dir2->sv_ino,dir2_inodeptr->sfi_linkcount);
		log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;

		buffer_mark_dirty(dir2->sv_buf);
	}

	result = sfs_dir_unlink(dir1, slot1, t);
	if (result) {
		goto recover2;
	}
	obj1_inodeptr->sfi_linkcount--;

	r = makerec_ilink(obj1->sv_ino,obj1_inodeptr->sfi_linkcount);
	log_ret = check_and_record(r,t);
	if (log_ret)
		return log_ret;

	buffer_mark_dirty(obj1->sv_buf);

	KASSERT(result==0);

	if (0) {
		/* Only reached on error */
    recover2:
		if (obj1->sv_type == SFS_TYPE_DIR) {
			sd.sfd_ino = dir1->sv_ino;

			r = makerec_dir(obj1->sv_ino,DOTDOTSLOT,sd.sfd_ino,sd.sfd_name);
			log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			result2 = sfs_writedir(obj1, &sd, DOTDOTSLOT, t);
			if (result2) {
				recovermsg(result, result2);
			}
			dir1_inodeptr->sfi_linkcount++;

			r = makerec_ilink(dir1->sv_ino,dir1_inodeptr->sfi_linkcount);
			log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			buffer_mark_dirty(dir1->sv_buf);
			dir2_inodeptr->sfi_linkcount--;

			r = makerec_ilink(dir2->sv_ino,dir2_inodeptr->sfi_linkcount);
			log_ret = check_and_record(r,t);
			if (log_ret)
				return log_ret;

			buffer_mark_dirty(dir2->sv_buf);
		}
    recover1:
		result2 = sfs_dir_unlink(dir2, slot2, t);
		if (result2) {
			recovermsg(result, result2);
		}
		obj1_inodeptr->sfi_linkcount--;

		r = makerec_ilink(obj1->sv_ino,obj1_inodeptr->sfi_linkcount);
		log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;

		buffer_mark_dirty(obj1->sv_buf);
	}

 out4:
 	sfs_release_inode(dir1);
 out3:
 	sfs_release_inode(dir2);
 out2:
 	sfs_release_inode(obj1);
	lock_release(obj1->sv_lock);
 out1:
	if (obj2) {
		sfs_release_inode(obj2);
		lock_release(obj2->sv_lock);
	}
	lock_release(dir1->sv_lock);
	if (dir1 != dir2) {
		lock_release(dir2->sv_lock);
	}
 out0:
	if (obj2 != NULL) {
		VOP_DECREF(&obj2->sv_v);
	}
	if (obj1 != NULL) {
		VOP_DECREF(&obj1->sv_v);
	}

	unreserve_buffers(7, SFS_BLOCKSIZE);

	lock_release(sfs->sfs_renamelock);

	return result;
}

static
int
sfs_lookparent_internal(struct vnode *v, char *path, struct vnode **ret,
		  char *buf, size_t buflen)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_vnode *next;
	char *s;
	int result;

	VOP_INCREF(&sv->sv_v);

	while (1) {
		/* Don't need lock to check vnode type; it's constant */
		if (sv->sv_type != SFS_TYPE_DIR) {
			VOP_DECREF(&sv->sv_v);
			return ENOTDIR;
		}

		s = strchr(path, '/');
		if (!s) {
			/* Last component. */
			break;
		}
		*s = 0;
		s++;

		lock_acquire(sv->sv_lock);
		result = sfs_lookonce(sv, path, &next, false, NULL);
		lock_release(sv->sv_lock);
		
		if (result) {
			VOP_DECREF(&sv->sv_v);
			return result;
		}

		VOP_DECREF(&sv->sv_v);
		sv = next;
		path = s;
	}

	if (strlen(path)+1 > buflen) {
		VOP_DECREF(&sv->sv_v);
		return ENAMETOOLONG;
	}
	strcpy(buf, path);
	
	*ret = &sv->sv_v;

	return 0;
}

/*
 * lookparent returns the last path component as a string and the
 * directory it's in as a vnode.
 *
 * Locking: gets the vnode lock while calling sfs_lookonce. Doesn't
 *   lock the new vnode, but does hand back a reference to it (so it
 *   won't evaporate).
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_lookparent(struct vnode *v, char *path, struct vnode **ret,
		  char *buf, size_t buflen)
{
	int result;

	reserve_buffers(3, SFS_BLOCKSIZE);
	result = sfs_lookparent_internal(v, path, ret, buf, buflen);
	unreserve_buffers(3, SFS_BLOCKSIZE);
	return result;
}

/*
 * Lookup gets a vnode for a pathname.
 *
 * Locking: gets the vnode lock while calling sfs_lookonce. Doesn't
 *   lock the new vnode, but does hand back a reference to it (so it
 *   won't evaporate).
 *   
 * Requires up to 3 buffers.
 */
static
int
sfs_lookup(struct vnode *v, char *path, struct vnode **ret)
{
	struct sfs_vnode *sv = v->vn_data;
	struct vnode *dirv;
	struct sfs_vnode *dir;
	struct sfs_vnode *final;
	int result;
	char name[SFS_NAMELEN];

	reserve_buffers(3, SFS_BLOCKSIZE);

	result = sfs_lookparent_internal(&sv->sv_v, path, &dirv, name, sizeof(name));
	if (result) {
		unreserve_buffers(3, SFS_BLOCKSIZE);
		return result;
	}
	
	dir = dirv->vn_data;

	lock_acquire(dir->sv_lock);

	result = sfs_lookonce(dir, name, &final, false, NULL);
	lock_release(dir->sv_lock);
	VOP_DECREF(dirv);

	if (result) {
		unreserve_buffers(3, SFS_BLOCKSIZE);
		return result;
	}

	*ret = &final->sv_v;

	unreserve_buffers(3, SFS_BLOCKSIZE);
	return 0;
}

//////////////////////////////////////////////////

static
int
sfs_notdir(void)
{
	return ENOTDIR;
}

static
int
sfs_isdir(void)
{
	return EISDIR;
}

static
int
sfs_unimp(void)
{
	return EUNIMP;
}

/*
 * Casting through void * prevents warnings.
 * All of the vnode ops return int, and it's ok to cast functions that
 * take args to functions that take no args.
 */

#define ISDIR ((void *)sfs_isdir)
#define NOTDIR ((void *)sfs_notdir)
#define UNIMP ((void *)sfs_unimp)

/*
 * Function table for sfs files.
 */
static const struct vnode_ops sfs_fileops = {
	VOP_MAGIC,	/* mark this a valid vnode ops table */

	sfs_open,
	sfs_close,
	sfs_reclaim,

	sfs_read,
	NOTDIR,  /* readlink */
	NOTDIR,  /* getdirentry */
	sfs_write,
	sfs_ioctl,
	sfs_stat,
	sfs_gettype,
	sfs_tryseek,
	sfs_fsync,
	sfs_mmap,
	sfs_truncate,
	NOTDIR,  /* namefile */

	NOTDIR,  /* creat */
	NOTDIR,  /* symlink */
	NOTDIR,  /* mkdir */
	NOTDIR,  /* link */
	NOTDIR,  /* remove */
	NOTDIR,  /* rmdir */
	NOTDIR,  /* rename */

	NOTDIR,  /* lookup */
	NOTDIR,  /* lookparent */
};

/*
 * Function table for the sfs directory.
 */
static const struct vnode_ops sfs_dirops = {
	VOP_MAGIC,	/* mark this a valid vnode ops table */

	sfs_opendir,
	sfs_close,
	sfs_reclaim,

	ISDIR,   /* read */
	ISDIR,   /* readlink */
	sfs_getdirentry,
	ISDIR,   /* write */
	sfs_ioctl,
	sfs_stat,
	sfs_gettype,
	sfs_tryseek,
	sfs_fsync,
	ISDIR,   /* mmap */
	ISDIR,   /* truncate */
	sfs_namefile,

	sfs_creat,
	UNIMP,   /* symlink */
	sfs_mkdir,
	sfs_link,
	sfs_remove,
	sfs_rmdir,
	sfs_rename,

	sfs_lookup,
	sfs_lookparent,
};

/*
 * Function to load a inode into memory as a vnode, or dig up one
 * that's already resident.
 *
 * If load_inode is set, vnode is returned locked, and inode
 * buffer is retained (not released).
 *
 * Locking: gets/releases sfs_vnlock.
 * 
 * May require 3 buffers if VOP_DECREF triggers reclaim.
 */
static
int
sfs_loadvnode(struct sfs_fs *sfs, uint32_t ino, int forcetype,
		 struct sfs_vnode **ret, bool load_inode, struct transaction *t)
{
	struct vnode *v;
	struct sfs_vnode *sv;
	const struct vnode_ops *ops = NULL;
	struct sfs_inode *inodeptr;
	unsigned i, num;
	int result;
	struct record *r;

	/* sfs_vnlock protects the vnodes table */
	lock_acquire(sfs->sfs_vnlock);

	/* Look in the vnodes table */
	num = vnodearray_num(sfs->sfs_vnodes);

	/* Linear search. Is this too slow? You decide. */
	for (i=0; i<num; i++) {
		v = vnodearray_get(sfs->sfs_vnodes, i);
		sv = v->vn_data;

		/* Every inode in memory must be in an allocated block */
		if (!sfs_bused(sfs, sv->sv_ino)) {
			panic("sfs: Found inode %u in unallocated block\n",
			      sv->sv_ino);
		}

		if (sv->sv_ino==ino) {
			/* Found */

			/* May only be set when creating new objects */
			KASSERT(forcetype==SFS_TYPE_INVAL);

			VOP_INCREF(&sv->sv_v);

			/* obey lock ordering */
			lock_release(sfs->sfs_vnlock);

			if (load_inode) {
				lock_acquire(sv->sv_lock);
				/* find the inode for the caller */
				result = sfs_load_inode(sv);
				if (result) {
					lock_release(sv->sv_lock);
					VOP_DECREF(&sv->sv_v);
					return result;
				}
			}

			*ret = sv;
			return 0;
		}
	}

	/* Didn't have it loaded; load it */

	sv = sfs_create_vnode();
	if (sv==NULL) {
		lock_release(sfs->sfs_vnlock);
		return ENOMEM;
	}

	/* Must be in an allocated block */
	if (!sfs_bused(sfs, ino)) {
		panic("sfs: Tried to load inode %u from unallocated block\n",
		      ino);
	}

	/* Read the block the inode is in
	 * We can do this without the vnode lock since we're the ones
	 * creating the new vnode
	 */
	result = buffer_read(&sfs->sfs_absfs, ino, SFS_BLOCKSIZE, &sv->sv_buf);
	if (result) {
		lock_release(sfs->sfs_vnlock);
		sfs_destroy_vnode(sv);
		return result;
	}
	/* we'll release this by hand */
	inodeptr = buffer_map(sv->sv_buf);
	hold_buffer_cache(t,sv->sv_buf);

	/*
	 * FORCETYPE is set if we're creating a new file, because the
	 * buffer will have been zeroed out already and thus the type
	 * recorded there will be SFS_TYPE_INVAL.
	 */
	if (forcetype != SFS_TYPE_INVAL) {
		KASSERT(inodeptr->sfi_type == SFS_TYPE_INVAL);
		inodeptr->sfi_type = forcetype;

		r = makerec_itype(ino,forcetype);
		int log_ret = check_and_record(r,t);
		if (log_ret)
			return log_ret;

		buffer_mark_dirty(sv->sv_buf);
	}

	/*
	 * Choose the function table based on the object type,
	 * and cache the type in the vnode.
	 */
	switch (inodeptr->sfi_type) {
	    case SFS_TYPE_FILE:
		ops = &sfs_fileops;
		break;
	    case SFS_TYPE_DIR:
		ops = &sfs_dirops;
		break;
	    default:
		panic("sfs: loadvnode: Invalid inode type "
		      "(inode %u, type %u)\n",
		      ino, inodeptr->sfi_type);
	}
	sv->sv_type = inodeptr->sfi_type;

	/* Call the common vnode initializer */
	result = VOP_INIT(&sv->sv_v, ops, &sfs->sfs_absfs, sv);
	if (result) {
		lock_release(sfs->sfs_vnlock);
		buffer_release(sv->sv_buf);
		sfs_destroy_vnode(sv);
		return result;
	}

	/* Set the other fields in our vnode structure */
	sv->sv_ino = ino;

	/* this violates ordering constraints (since we hold the vnode table
	 * lock), but ok since this is the first reference to the vnode
	 */
	if (load_inode) {
		lock_acquire(sv->sv_lock);
		sv->sv_bufdepth++;
	} else {
		buffer_release(sv->sv_buf);
		sv->sv_buf = NULL;
	}

	/* Add it to our table */
	result = vnodearray_add(sfs->sfs_vnodes, &sv->sv_v, NULL);
	if (result) {
		VOP_CLEANUP(&sv->sv_v);
		lock_release(sfs->sfs_vnlock);
		if (load_inode) {
			sfs_release_inode(sv);
			lock_release(sv->sv_lock);
		}
		sfs_destroy_vnode(sv);
		return result;
	}

	lock_release(sfs->sfs_vnlock);
	*ret = sv;
	return 0;
}

/*
 * Get vnode for the root of the filesystem.
 * The root vnode is always found in block 1 (SFS_ROOT_LOCATION).
 *
 * Locking: not needed.
 */
struct vnode *
sfs_getroot(struct fs *fs)
{
	struct sfs_fs *sfs = fs->fs_data;
	struct sfs_vnode *sv;
	int result;

	reserve_buffers(1, SFS_BLOCKSIZE);

	result = sfs_loadvnode(sfs, SFS_ROOT_LOCATION, SFS_TYPE_INVAL,
			       &sv, false, NULL);
	if (result) {
		panic("sfs: getroot: Cannot load root vnode\n");
	}

	unreserve_buffers(1, SFS_BLOCKSIZE);

	return &sv->sv_v;
}

/*
 * Journaling functions
 */

static
struct transaction *
create_transaction(void) {
	struct transaction *t = kmalloc(sizeof(struct transaction));
	if (t == NULL) {
		return NULL;
	}

	t->bufs = array_create();
	if (t->bufs == NULL) {
		kfree(t);
		return NULL;
	}

	// Synchronization for checkpointing
	lock_acquire(checkpoint_lock);
	while (in_checkpoint)
		cv_wait(checkpoint_cleared,checkpoint_lock);
	lock_release(checkpoint_lock);

	lock_acquire(transaction_lock);	
	num_active_transactions++;
	lock_release(transaction_lock);
	kprintf("transaction created (%d total)\n",num_active_transactions);

	lock_acquire(transaction_id_lock);
	t->id = next_transaction_id;
	next_transaction_id++;
	lock_release(transaction_id_lock);
	return t;
}

static 
int checkpoint(){
	lock_acquire(transaction_lock);
	while (num_active_transactions > 0)
		cv_wait(no_active_transactions,transaction_lock);
	lock_release(transaction_lock);

	lock_acquire(checkpoint_lock);
	in_checkpoint = 1;
	lock_release(checkpoint_lock);
	kprintf("In a checkpoint\n");

	// Checkpoint stuff

	lock_acquire(checkpoint_lock);
	in_checkpoint = 0;
	cv_broadcast(checkpoint_cleared,checkpoint_lock);
	lock_release(checkpoint_lock);
	return 0;
}

static
int hold_buffer_cache(struct transaction *t, struct buf *buf) {
	unsigned i, num;
	int result;

	// Make it compatible with read only operations
	if (t != NULL) {
		// Check if buf exists in t->bufs
		num = array_num(t->bufs);
		for(i=0; i<num; i++) {
			if (array_get(t->bufs, i) == buf) {
				return 0;
			}
		}
		// Add buf to transaction
		result = array_add(t->bufs, buf, NULL);
		if (result) {
			return result;
		}
		// Increase reference count
		buf_incref(buf);
	}
	return 0;
}

static
int record(struct record *r) {
	KASSERT(sizeof(struct record) == RECORD_SIZE);

	lock_acquire(log_buf_lock);
	if (log_buf_offset == BUF_RECORDS) {
		// TODO: flush, could fail here?
		panic("Log buffer filed");
	}
	memcpy(&log_buf[log_buf_offset], (const void *)r, sizeof(struct record));
	log_buf_offset++;
	lock_release(log_buf_lock);
	return 0;
}

// TODO: track active transactions for checkpointing
/*
 * Synch note: log_buf_lock doubles as mutex lock for on disk journal
 */
static
int commit(struct transaction *t, struct fs *fs) {
	int i, j, result, part;
	unsigned ix, max;
	daddr_t block = JN_LOCATION(fs);
	struct record *tmp = kmalloc(SFS_BLOCKSIZE);
	if (tmp == NULL) {
		return ENOMEM;
	}
	max = 0;
	lock_acquire(log_buf_lock);
	if (log_buf_offset > 0) {
		// Partial write
		i = 0;
		if (journal_offset % REC_PER_BLK != 0) {
			part = journal_offset % REC_PER_BLK;
			// Read
			result = sfs_readblock(fs, block + journal_offset / REC_PER_BLK,
				tmp, SFS_BLOCKSIZE);
			if (result) {
				lock_release(log_buf_lock);
				goto err;
			}
			// Construct
			memcpy(&tmp[part], (const void *)log_buf,
				sizeof(struct record) * (REC_PER_BLK - part));
			// Write
			result = sfs_writeblock(fs, block + journal_offset / REC_PER_BLK,
				tmp, SFS_BLOCKSIZE);

			if (log_buf_offset > REC_PER_BLK - part) {
				i += REC_PER_BLK - part;
				journal_offset += REC_PER_BLK - part;
			}
			else {
				i += log_buf_offset;
				journal_offset += log_buf_offset;

			}
			// Record max
			for (j=0; j<i; j++) {
				if (log_buf[j].transaction_id > max) {
					max = log_buf[j].transaction_id;
				}
			}
		}
		// Write full blocks
		while (i<log_buf_offset-(log_buf_offset % REC_PER_BLK)) {
			result = sfs_writeblock(fs, block + journal_offset / REC_PER_BLK,
				&log_buf[i], SFS_BLOCKSIZE);
			if (result) {
				lock_release(log_buf_lock);
				goto err;
			}
			i += REC_PER_BLK;
			journal_offset += REC_PER_BLK;
			// Record max
			for (j=0; j<REC_PER_BLK; j++) {
				if (log_buf[j].transaction_id > max) {
					max = log_buf[j].transaction_id;
				}
			}
		}
		// Partial write TODO: make sure not at end of log_buf
		if (log_buf_offset != i) {
			result = sfs_writeblock(fs, block + journal_offset / REC_PER_BLK,
				&log_buf[i], SFS_BLOCKSIZE);
			if (result) {
				lock_release(log_buf_lock);
				goto err;
			}
			journal_offset += log_buf_offset - i;
			// Record max
			for (j=0; j< log_buf_offset - i; j++) {
				if (log_buf[j].transaction_id > max) {
					max = log_buf[j].transaction_id;
				}
			}
		}
		// Clear log buf
		log_buf_offset = 0;
	}
	// Update journal summary block
	struct sfs_jn_summary *s = kmalloc(SFS_BLOCKSIZE);
	if (s == NULL) {
		lock_release(log_buf_lock);
		result = ENOMEM;
		goto err;
	}
	sfs_readblock(fs, JN_SUMMARY_LOCATION(fs), s, SFS_BLOCKSIZE);
	s->num_entries = journal_offset;
	if (max > s->max_id)
		s->max_id = max;
	sfs_writeblock(fs, JN_SUMMARY_LOCATION(fs), s, SFS_BLOCKSIZE);
	kfree(s);

	lock_release(log_buf_lock); // This also act as on disk journal log here

	// Checkpoint signaling
	lock_acquire(transaction_lock);
	KASSERT(num_active_transactions > 0);
	num_active_transactions--;
	if (num_active_transactions == 0)
		cv_signal(no_active_transactions,transaction_lock);
	lock_release(transaction_lock);
	kprintf("transaction completed (%d left)\n",num_active_transactions);

	if (journal_offset + log_buf_offset > (int)(0.25 * MAX_JN_ENTRIES)){
		lock_acquire(checkpoint_lock);  // TODO: not sure if we need to do this
		if (!in_checkpoint){
			lock_release(checkpoint_lock);
			checkpoint();
		}
	}

	// cleanup
	result = 0;

	err:
		for (ix = array_num((const struct array*)t->bufs); ix>0; ix--) {
			buf_decref((struct buf *)array_get(t->bufs, ix-1));
			array_remove(t->bufs, ix-1);
		}
		array_destroy(t->bufs);
		kfree(t);
		kfree(tmp);
		return result;
}

/* To be called in case of error so system doesnt think transaction exists
static
void abort(struct transaction *t){
	int ix;

	for (ix = array_num((const struct array*)t->bufs); ix>0; ix--) {
		buf_decref((struct buf *)array_get(t->bufs, ix-1));
		array_remove(t->bufs, ix-1);
	}
	array_destroy(t->bufs);
	kfree(t);

	// Synchro for checkpointing
	lock_acquire(transaction_lock);
	KASSERT(num_active_transactions > 0);
	num_active_transactions--;
	if (num_active_transactions == 0)
		cv_signal(no_active_transactions,transaction_lock);
	lock_release(transaction_lock);
}
*/

static
int check_and_record(struct record *r, struct transaction *t) {
	int ret;
	if (r == NULL)
		return ENOMEM;
	r->transaction_id = t->id;
	ret = record(r);
	kfree(r);
	if (ret)
		return ret;
	return 0;
}

void journal_iterator(struct fs *fs, void (*f)(struct record *)) {
	int i, j, entries;
	struct record *r = kmalloc(SFS_BLOCKSIZE);
	daddr_t block = JN_LOCATION(fs);

	// Get number of entries in journal
	struct sfs_jn_summary *s = kmalloc(SFS_BLOCKSIZE);
	if (s == NULL)
		panic("Cannot allocate memory for journal summary");
	if (sfs_readblock(fs, JN_SUMMARY_LOCATION(fs), s, SFS_BLOCKSIZE))
		panic("Cannot from journal summary");
	entries = s->num_entries;
	kfree(s);
	kprintf("Num entries in journal: %d\n", entries);
	// Pass it to function
	for(i=0; i<(ROUNDUP(entries, REC_PER_BLK)/REC_PER_BLK); i++) {
		if (sfs_readblock(fs, block + i, r, SFS_BLOCKSIZE))
			panic("Just panic");
		for (j=0; j<REC_PER_BLK; j++) {
			(*f)(&r[j]);
		}
	}
}
