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

/* At bottom of file */
static int sfs_loadvnode(struct sfs_fs *sfs, uint32_t ino, int type,
			 struct sfs_vnode **ret, struct buf **ret_inodebuf);

////////////////////////////////////////////////////////////
//
// Simple stuff

/* Zero out a disk block. */
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
 * Requires 1 buffer.
 */
static
int
sfs_balloc(struct sfs_fs *sfs, uint32_t *diskblock, struct buf **bufret)
{
	int result;

	result = bitmap_alloc(sfs->sfs_freemap, diskblock);
	if (result) {
		return result;
	}
	sfs->sfs_freemapdirty = true;

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
sfs_bfree(struct sfs_fs *sfs, uint32_t diskblock)
{
	bitmap_unmark(sfs->sfs_freemap, diskblock);
	sfs->sfs_freemapdirty = true;
}

/*
 * Check if a block is in use.
 */
static
int
sfs_bused(struct sfs_fs *sfs, uint32_t diskblock)
{
	if (diskblock >= sfs->sfs_super.sp_nblocks) {
		panic("sfs: sfs_bused called on out of range block %u\n",
		      diskblock);
	}
	return bitmap_isset(sfs->sfs_freemap, diskblock);
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
 * Requires up to 2 buffers, not counting inodebuf.
 */
static
int
sfs_bmap(struct sfs_vnode *sv, struct buf *inodebuf, uint32_t fileblock,
	 int doalloc, uint32_t *diskblock)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	struct sfs_inode *inodeptr;
	struct buf *idbuffer;
	uint32_t *idbufdata;
	uint32_t block;
	uint32_t idblock;
	uint32_t idnum, idoff;
	int result;

	KASSERT(SFS_DBPERIDB * sizeof(*idbufdata) == SFS_BLOCKSIZE);

	inodeptr = buffer_map(inodebuf);

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
			result = sfs_balloc(sfs, &block, NULL);
			if (result) {
				return result;
			}

			/* Remember what we allocated; mark inode dirty */
			inodeptr->sfi_direct[fileblock] = block;
			buffer_mark_dirty(inodebuf);
		}

		/*
		 * Hand back the block
		 */
		if (block != 0 && !sfs_bused(sfs, block)) {
			panic("sfs: Data block %u (block %u of file %u) "
			      "marked free\n", block, fileblock, sv->sv_ino);
		}
		*diskblock = block;
		return 0;
	}

	/*
	 * It's not a direct block; it must be in the indirect block.
	 * Subtract off the number of direct blocks, so FILEBLOCK is
	 * now the offset into the indirect block space.
	 */

	fileblock -= SFS_NDIRECT;

	/* Get the indirect block number and offset w/i that indirect block */
	idnum = fileblock / SFS_DBPERIDB;
	idoff = fileblock % SFS_DBPERIDB;

	/*
	 * We only have one indirect block. If the offset we were asked for
	 * is too large, we can't handle it, so fail.
	 */
	if (idnum > 0) {
		return EFBIG;
	}

	/* Get the disk block number of the indirect block. */
	idblock = inodeptr->sfi_indirect;

	if (idblock==0 && !doalloc) {
		/*
		 * There's no indirect block allocated. We weren't
		 * asked to allocate anything, so pretend the indirect
		 * block was filled with all zeros.
		 */
		*diskblock = 0;
		return 0;
	}
	else if (idblock==0) {
		/*
		 * There's no indirect block allocated, but we need to
		 * allocate a block whose number needs to be stored in
		 * the indirect block. Thus, we need to allocate an
		 * indirect block.
		 */
		result = sfs_balloc(sfs, &idblock, &idbuffer);
		if (result) {
			return result;
		}

		/* Remember the block we just allocated */
		inodeptr->sfi_indirect = idblock;

		/* Mark the inode dirty */
		buffer_mark_dirty(inodebuf);

		/* Clear the indirect block buffer */

		idbufdata = buffer_map(idbuffer);

		/*
		 * sfs_balloc already does this...
		 *
		 * bzero(idbufdata, SFS_BLOCKSIZE);
		 * buffer_mark_dirty(idbuffer);
		 */
	}
	else {
		/*
		 * We already have an indirect block allocated; load it.
		 */
		result = buffer_read(&sfs->sfs_absfs, idblock, SFS_BLOCKSIZE,
				     &idbuffer);
		if (result) {
			return result;
		}
		idbufdata = buffer_map(idbuffer);
	}

	/* Get the block out of the indirect block buffer */
	block = idbufdata[idoff];

	/* If there's no block there, allocate one */
	if (block==0 && doalloc) {
		result = sfs_balloc(sfs, &block, NULL);
		if (result) {
			buffer_release(idbuffer);
			return result;
		}

		/* Remember the block we allocated */
		idbufdata[idoff] = block;

		/* The indirect block is now dirty; mark it so */
		buffer_mark_dirty(idbuffer);
	}

	/* Hand back the result and return. */
	if (block != 0 && !sfs_bused(sfs, block)) {
		panic("sfs: Data block %u (block %u of file %u) marked free\n",
		      block, fileblock, sv->sv_ino);
	}

	buffer_release(idbuffer);

	*diskblock = block;
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
 * Requires up to 2 buffers, not counting inodebuf.
 */
static
int
sfs_partialio(struct sfs_vnode *sv, struct buf *inodebuf, struct uio *uio,
	      uint32_t skipstart, uint32_t len)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	struct buf *iobuffer;
	char *ioptr;
	uint32_t diskblock;
	uint32_t fileblock;
	int result;

	/* Allocate missing blocks if and only if we're writing */
	int doalloc = (uio->uio_rw==UIO_WRITE);

	KASSERT(skipstart + len <= SFS_BLOCKSIZE);

	/* Compute the block offset of this block in the file */
	fileblock = uio->uio_offset / SFS_BLOCKSIZE;

	/* Get the disk block number */
	result = sfs_bmap(sv, inodebuf, fileblock, doalloc, &diskblock);
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
 * Requires up to 2 buffers, not counting inodebuf.
 */
static
int
sfs_blockio(struct sfs_vnode *sv, struct buf *inodebuf, struct uio *uio)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	struct buf *iobuf;
	void *ioptr;
	uint32_t diskblock;
	uint32_t fileblock;
	int result;
	int doalloc = (uio->uio_rw==UIO_WRITE);

	/* Get the block number within the file */
	fileblock = uio->uio_offset / SFS_BLOCKSIZE;

	/* Look up the disk block number */
	result = sfs_bmap(sv, inodebuf, fileblock, doalloc, &diskblock);
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
 * Requires up to 3 buffers.
 */
static
int
sfs_io(struct sfs_vnode *sv, struct uio *uio)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	uint32_t blkoff;
	uint32_t nblocks, i;
	int result = 0;
	uint32_t extraresid = 0;
	struct buf *inodebuf;
	struct sfs_inode *inodeptr;

	result = buffer_read(&sfs->sfs_absfs, sv->sv_ino,
			     SFS_BLOCKSIZE, &inodebuf);
	if (result) {
		return result;
	}
	inodeptr = buffer_map(inodebuf);

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
			buffer_release(inodebuf);
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
		result = sfs_partialio(sv, inodebuf, uio, skip, len);
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
		result = sfs_blockio(sv, inodebuf, uio);
		if (result) {
			goto out;
		}
	}

	/*
	 * Now do any remaining partial block at the end.
	 */
	KASSERT(uio->uio_resid < SFS_BLOCKSIZE);

	if (uio->uio_resid > 0) {
		result = sfs_partialio(sv, inodebuf, uio, 0, uio->uio_resid);
		if (result) {
			goto out;
		}
	}

 out:

	/* If writing, adjust file length */
	if (uio->uio_rw == UIO_WRITE &&
	    uio->uio_offset > (off_t)inodeptr->sfi_size) {
		inodeptr->sfi_size = uio->uio_offset;
		buffer_mark_dirty(inodebuf);
	}
	buffer_release(inodebuf);

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

	/* Compute the actual position in the directory to read. */
	actualpos = slot * sizeof(struct sfs_dir);

	/* Set up a uio to do the read */
	uio_kinit(&iov, &ku, sd, sizeof(struct sfs_dir), actualpos, UIO_READ);

	/* do it */
	result = sfs_io(sv, &ku);
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
sfs_writedir(struct sfs_vnode *sv, struct sfs_dir *sd, int slot)
{
	struct iovec iov;
	struct uio ku;
	off_t actualpos;
	int result;

	/* Compute the actual position in the directory. */
	KASSERT(slot>=0);
	actualpos = slot * sizeof(struct sfs_dir);

	/* Set up a uio to do the write */
	uio_kinit(&iov, &ku, sd, sizeof(struct sfs_dir), actualpos, UIO_WRITE);

	/* do it */
	result = sfs_io(sv, &ku);
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
 * Requires 1 buffer.
 */
static
int
sfs_dir_nentries(struct sfs_vnode *sv, int *ret)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	off_t size;
	struct buf *inodebuf;
	struct sfs_inode *inodeptr;
	int result;

	KASSERT(sv->sv_type == SFS_TYPE_DIR);

	result = buffer_read(&sfs->sfs_absfs, sv->sv_ino,
			     SFS_BLOCKSIZE, &inodebuf);
	if (result) {
		return result;
	}
	inodeptr = buffer_map(inodebuf);

	size = inodeptr->sfi_size;
	if (size % sizeof(struct sfs_dir) != 0) {
		panic("sfs: directory %u: Invalid size %llu\n",
		      sv->sv_ino, size);
	}

	buffer_release(inodebuf);

	*ret = size / sizeof(struct sfs_dir);
	return 0;
}

/*
 * Search a directory for a particular filename in a directory, and
 * return its inode number, its slot, and/or the slot number of an
 * empty directory slot if one is found.
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
 * Create a link in a directory to the specified inode by number, with
 * the specified name, and optionally hand back the slot.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_dir_link(struct sfs_vnode *sv, const char *name, uint32_t ino, int *slot)
{
	int emptyslot = -1;
	int result;
	struct sfs_dir sd;

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

	/* Hand back the slot, if so requested. */
	if (slot) {
		*slot = emptyslot;
	}

	/* Write the entry. */
	return sfs_writedir(sv, &sd, emptyslot);

}

/*
 * Unlink a name in a directory, by slot number.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_dir_unlink(struct sfs_vnode *sv, int slot)
{
	struct sfs_dir sd;

	/* Initialize a suitable directory entry... */
	bzero(&sd, sizeof(sd));
	sd.sfd_ino = SFS_NOINO;

	/* ... and write it */
	return sfs_writedir(sv, &sd, slot);
}

/*
 * Look for a name in a directory and hand back a vnode for the
 * file, if there is one.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_lookonce(struct sfs_vnode *sv, const char *name,
		struct sfs_vnode **ret,
		struct buf **ret_inodebuf,
		int *slot)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	uint32_t ino;
	struct buf *inodebuf;
	struct sfs_inode *inodeptr;
	int result;

	result = sfs_dir_findname(sv, name, &ino, slot, NULL);
	if (result) {
		return result;
	}

	result = sfs_loadvnode(sfs, ino, SFS_TYPE_INVAL, ret, &inodebuf);
	if (result) {
		return result;
	}

	inodeptr = buffer_map(inodebuf);

	if (inodeptr->sfi_linkcount == 0) {
		panic("sfs: Link count of file %u found in dir %u is 0\n",
		      (*ret)->sv_ino, sv->sv_ino);
	}

	if (ret_inodebuf != NULL) {
		*ret_inodebuf = inodebuf;
	}
	else {
		buffer_release(inodebuf);
	}

	return 0;
}

////////////////////////////////////////////////////////////
//
// Object creation

/*
 * Create a new filesystem object and hand back its vnode.
 *
 * Requires 1 buffer.
 */
static
int
sfs_makeobj(struct sfs_fs *sfs, int type, struct sfs_vnode **ret,
	    struct buf **ret_inodebuf)
{
	uint32_t ino;
	int result;

	/*
	 * First, get an inode. (Each inode is a block, and the inode
	 * number is the block number, so just get a block.)
	 */

	result = sfs_balloc(sfs, &ino, NULL);
	if (result) {
		return result;
	}

	/*
	 * Now load a vnode for it.
	 */

	return sfs_loadvnode(sfs, ino, type, ret, ret_inodebuf);
}

////////////////////////////////////////////////////////////
//
// Vnode ops

/*
 * This is called on *each* open().
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
 * Requires 1 buffer, and then independently calls VOP_TRUNCATE, which
 * takes 3.
 */
static
int
sfs_reclaim(struct vnode *v)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct buf *ibuf;
	struct sfs_inode *iptr;
	unsigned ix, i, num;
	int result;

	vfs_biglock_acquire();

	/*
	 * Make sure someone else hasn't picked up the vnode since the
	 * decision was made to reclaim it. (You must also synchronize
	 * this with sfs_loadvnode.)
	 */
	if (v->vn_refcount != 1) {

		/* consume the reference VOP_DECREF gave us */
		KASSERT(v->vn_refcount>1);
		v->vn_refcount--;

		vfs_biglock_release();
		return EBUSY;
	}

	reserve_buffers(1, SFS_BLOCKSIZE);

	/* Get the on-disk inode. */
	result = buffer_read(&sfs->sfs_absfs, sv->sv_ino, SFS_BLOCKSIZE,
			     &ibuf);
	if (result) {
		/*
		 * This case is likely to lead to problems, but
		 * there's essentially no helping it...
		 */
		vfs_biglock_release();
		return result;
	}
	iptr = buffer_map(ibuf);

	/* If there are no on-disk references to the file either, erase it. */
	if (iptr->sfi_linkcount==0) {
		buffer_release(ibuf);
		unreserve_buffers(1, SFS_BLOCKSIZE);
		result = VOP_TRUNCATE(&sv->sv_v, 0);
		if (result) {
			buffer_release(ibuf);
			vfs_biglock_release();
			return result;
		}
		/* Discard the inode */
		buffer_drop(&sfs->sfs_absfs, sv->sv_ino, SFS_BLOCKSIZE);
		sfs_bfree(sfs, sv->sv_ino);
	}
	else {
		buffer_release(ibuf);
		unreserve_buffers(1, SFS_BLOCKSIZE);
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

	VOP_CLEANUP(&sv->sv_v);

	vfs_biglock_release();

	/* Release the storage for the vnode structure itself. */
	kfree(sv);

	/* Done */
	return 0;
}

/*
 * Called for read(). sfs_io() does the work.
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

	vfs_biglock_acquire();
	reserve_buffers(3, SFS_BLOCKSIZE);

	result = sfs_io(sv, uio);

	unreserve_buffers(3, SFS_BLOCKSIZE);
	vfs_biglock_release();

	return result;
}

/*
 * Called for write(). sfs_io() does the work.
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

	vfs_biglock_acquire();
	reserve_buffers(3, SFS_BLOCKSIZE);

	result = sfs_io(sv, uio);

	unreserve_buffers(3, SFS_BLOCKSIZE);
	vfs_biglock_release();

	return result;
}

/*
 * Called for ioctl()
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
 * Requires 1 buffer.
 */
static
int
sfs_stat(struct vnode *v, struct stat *statbuf)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	struct buf *inodebuf;
	struct sfs_inode *inodeptr;
	int result;

	vfs_biglock_acquire();

	/* Fill in the stat structure */
	bzero(statbuf, sizeof(struct stat));

	result = VOP_GETTYPE(v, &statbuf->st_mode);
	if (result) {
		vfs_biglock_release();
		return result;
	}

	reserve_buffers(1, SFS_BLOCKSIZE);

	result = buffer_read(&sfs->sfs_absfs, sv->sv_ino,
			     SFS_BLOCKSIZE, &inodebuf);
	if (result) {
		unreserve_buffers(1, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}

	inodeptr = buffer_map(inodebuf);

	statbuf->st_size = inodeptr->sfi_size;

	/* We don't support these yet; you get to implement them */
	statbuf->st_nlink = 0;
	statbuf->st_blocks = 0;

	/* Fill in other fields as desired/possible... */

	buffer_release(inodebuf);
	unreserve_buffers(1, SFS_BLOCKSIZE);
	vfs_biglock_release();

	return 0;
}

/*
 * Return the type of the file (types as per kern/stat.h)
 */
static
int
sfs_gettype(struct vnode *v, uint32_t *ret)
{
	struct sfs_vnode *sv = v->vn_data;

	vfs_biglock_acquire();

	switch (sv->sv_type) {
	case SFS_TYPE_FILE:
		*ret = S_IFREG;
		vfs_biglock_release();
		return 0;
	case SFS_TYPE_DIR:
		*ret = S_IFDIR;
		vfs_biglock_release();
		return 0;
	}
	panic("sfs: gettype: Invalid inode type (inode %u, type %u)\n",
	      sv->sv_ino, sv->sv_type);
	vfs_biglock_release();
	return EINVAL;
}

/*
 * Check for legal seeks on files. Allow anything non-negative.
 * We could conceivably, here, prohibit seeking past the maximum
 * file size our inode structure can support, but we don't - few
 * people ever bother to check lseek() for failure and having
 * read() or write() fail is sufficient.
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
 * Called for ftruncate() and from sfs_reclaim.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_truncate(struct vnode *v, off_t len)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;

	struct buf *inodebuf;
	struct sfs_inode *inodeptr;

	struct buf *idbuffer;
	uint32_t *idptr;

	/* Length in blocks (divide rounding up) */
	uint32_t blocklen = DIVROUNDUP(len, SFS_BLOCKSIZE);

	uint32_t i, j, block;
	uint32_t idblock, baseblock, highblock;
	int result;
	int hasnonzero;

	KASSERT(SFS_DBPERIDB * sizeof(*idptr) == SFS_BLOCKSIZE);

	vfs_biglock_acquire();

	reserve_buffers(3, SFS_BLOCKSIZE);

	result = buffer_read(&sfs->sfs_absfs, sv->sv_ino, SFS_BLOCKSIZE,
			     &inodebuf);
	if (result) {
		unreserve_buffers(3, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}
	inodeptr = buffer_map(inodebuf);

	/*
	 * Go through the direct blocks. Discard any that are
	 * past the limit we're truncating to.
	 */
	for (i=0; i<SFS_NDIRECT; i++) {
		block = inodeptr->sfi_direct[i];
		if (i >= blocklen && block != 0) {
			buffer_drop(&sfs->sfs_absfs, block, SFS_BLOCKSIZE);
			sfs_bfree(sfs, block);
			inodeptr->sfi_direct[i] = 0;
			buffer_mark_dirty(inodebuf);
		}
	}

	/* Indirect block number */
	idblock = inodeptr->sfi_indirect;

	/* The lowest block in the indirect block */
	baseblock = SFS_NDIRECT;

	/* The highest block in the indirect block */
	highblock = baseblock + SFS_DBPERIDB - 1;

	if (blocklen < highblock && idblock != 0) {
		/* We're past the proposed EOF; may need to free stuff */

		/* Read the indirect block */
		result = buffer_read(&sfs->sfs_absfs, idblock, SFS_BLOCKSIZE,
				     &idbuffer);
		if (result) {
			buffer_release(inodebuf);
			unreserve_buffers(3, SFS_BLOCKSIZE);
			vfs_biglock_release();
			return result;
		}
		idptr = buffer_map(idbuffer);

		hasnonzero = 0;
		for (j=0; j<SFS_DBPERIDB; j++) {
			/* Discard any blocks that are past the new EOF */
			if (blocklen < baseblock+j && idptr[j] != 0) {
				buffer_drop(&sfs->sfs_absfs, idptr[j],
					    SFS_BLOCKSIZE);
				sfs_bfree(sfs, idptr[j]);
				idptr[j] = 0;
				buffer_mark_dirty(idbuffer);
			}
			/* Remember if we see any nonzero blocks in here */
			if (idptr[j] != 0) {
				hasnonzero=1;
			}
		}

		if (!hasnonzero) {
			/* The whole indirect block is empty now; free it */
			buffer_release_and_invalidate(idbuffer);
			sfs_bfree(sfs, idblock);
			inodeptr->sfi_indirect = 0;
			buffer_mark_dirty(inodebuf);
		}
		else {
			buffer_release(idbuffer);
		}
	}

	/* Set the file size */
	inodeptr->sfi_size = len;

	/* Mark the inode dirty */
	buffer_mark_dirty(inodebuf);

	buffer_release(inodebuf);

	unreserve_buffers(3, SFS_BLOCKSIZE);
	vfs_biglock_release();
	return 0;
}

/*
 * Get the full pathname for a file. This only needs to work on directories.
 * Since we don't support subdirectories, assume it's the root directory
 * and hand back the empty string. (The VFS layer takes care of the
 * device name, leading slash, etc.)
 */
static
int
sfs_namefile(struct vnode *vv, struct uio *uio)
{
	struct sfs_vnode *sv = vv->vn_data;
	KASSERT(sv->sv_ino == SFS_ROOT_LOCATION);

	/* send back the empty string - just return */

	(void)uio;

	return 0;
}

/*
 * Create a file. If EXCL is set, insist that the filename not already
 * exist; otherwise, if it already exists, just open it.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_creat(struct vnode *v, const char *name, bool excl, mode_t mode,
	  struct vnode **ret)
{
	struct sfs_fs *sfs = v->vn_fs->fs_data;
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_vnode *newguy;
	struct buf *new_inodebuf;
	struct sfs_inode *new_inodeptr;
	uint32_t ino;
	int result;

	vfs_biglock_acquire();
	reserve_buffers(3, SFS_BLOCKSIZE);

	/* Look up the name */
	result = sfs_dir_findname(sv, name, &ino, NULL, NULL);
	if (result!=0 && result!=ENOENT) {
		unreserve_buffers(3, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}

	/* If it exists and we didn't want it to, fail */
	if (result==0 && excl) {
		unreserve_buffers(3, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return EEXIST;
	}

	if (result==0) {
		/* We got a file; load its vnode and return */
		result = sfs_loadvnode(sfs, ino, SFS_TYPE_INVAL, &newguy,NULL);
		if (result) {
			unreserve_buffers(3, SFS_BLOCKSIZE);
			vfs_biglock_release();
			return result;
		}
		*ret = &newguy->sv_v;
		unreserve_buffers(3, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return 0;
	}

	/* Didn't exist - create it */
	result = sfs_makeobj(sfs, SFS_TYPE_FILE, &newguy, &new_inodebuf);
	if (result) {
		unreserve_buffers(3, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}
	new_inodeptr = buffer_map(new_inodebuf);

	/* We don't currently support file permissions; ignore MODE */
	(void)mode;

	/* Link it into the directory */
	result = sfs_dir_link(sv, name, newguy->sv_ino, NULL);
	if (result) {
		buffer_release(new_inodebuf);
		unreserve_buffers(3, SFS_BLOCKSIZE);
		VOP_DECREF(&newguy->sv_v);
		vfs_biglock_release();
		return result;
	}

	/* Update the linkcount of the new file */
	new_inodeptr->sfi_linkcount++;

	/* and consequently mark it dirty. */
	buffer_mark_dirty(new_inodebuf);
	buffer_release(new_inodebuf);

	*ret = &newguy->sv_v;

	unreserve_buffers(3, SFS_BLOCKSIZE);
	vfs_biglock_release();
	return 0;
}

/*
 * Make a hard link to a file.
 * The VFS layer should prevent this being called unless both
 * vnodes are ours.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_link(struct vnode *dir, const char *name, struct vnode *file)
{
	struct sfs_vnode *sv = dir->vn_data;
	struct sfs_vnode *f = file->vn_data;
	struct sfs_fs *sfs = file->vn_fs->fs_data;
	struct buf *inodebuf;
	struct sfs_inode *inodeptr;
	int result;

	KASSERT(file->vn_fs == dir->vn_fs);

	vfs_biglock_acquire();
	reserve_buffers(4, SFS_BLOCKSIZE);

	result = buffer_read(&sfs->sfs_absfs, f->sv_ino, SFS_BLOCKSIZE,
			     &inodebuf);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}

	/* Just create a link */
	result = sfs_dir_link(sv, name, f->sv_ino, NULL);
	if (result) {
		buffer_release(inodebuf);
		unreserve_buffers(4, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}

	/* and update the link count, marking the inode dirty */
	inodeptr = buffer_map(inodebuf);
	inodeptr->sfi_linkcount++;
	buffer_mark_dirty(inodebuf);

	buffer_release(inodebuf);
	unreserve_buffers(4, SFS_BLOCKSIZE);
	vfs_biglock_release();
	return 0;
}

/*
 * Delete a file.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_remove(struct vnode *dir, const char *name)
{
	struct sfs_vnode *sv = dir->vn_data;
	struct sfs_vnode *victim;
	struct buf *victim_inodebuf;
	struct sfs_inode *victim_inodeptr;
	int slot;
	int result;

	vfs_biglock_acquire();
	reserve_buffers(4, SFS_BLOCKSIZE);

	/* Look for the file and fetch a vnode for it. */
	result = sfs_lookonce(sv, name, &victim, &victim_inodebuf, &slot);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}

	/* Erase its directory entry. */
	result = sfs_dir_unlink(sv, slot);
	if (result==0) {
		/* If we succeeded, decrement the link count. */
		victim_inodeptr = buffer_map(victim_inodebuf);
		KASSERT(victim_inodeptr->sfi_linkcount > 0);
		victim_inodeptr->sfi_linkcount--;
		buffer_mark_dirty(victim_inodebuf);
	}

	buffer_release(victim_inodebuf);
	unreserve_buffers(4, SFS_BLOCKSIZE);

	/* Discard the reference that sfs_lookonce got us */
	VOP_DECREF(&victim->sv_v);

	vfs_biglock_release();
	return result;
}

/*
 * Rename a file.
 *
 * Since we don't support subdirectories, assumes that the two
 * directories passed are the same.
 *
 * Requires up to 4 buffers.
 */
static
int
sfs_rename(struct vnode *d1, const char *n1,
	   struct vnode *d2, const char *n2)
{
	struct sfs_vnode *sv = d1->vn_data;
	struct sfs_vnode *g1;
	struct buf *g1_inodebuf;
	struct sfs_inode *g1_inodeptr;
	int slot1, slot2;
	int result, result2;

	vfs_biglock_acquire();
	reserve_buffers(4, SFS_BLOCKSIZE);

	KASSERT(d1==d2);
	KASSERT(sv->sv_ino == SFS_ROOT_LOCATION);

	/* Look up the old name of the file and get its inode and slot number*/
	result = sfs_lookonce(sv, n1, &g1, &g1_inodebuf, &slot1);
	if (result) {
		unreserve_buffers(4, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}
	g1_inodeptr = buffer_map(g1_inodebuf);

	/* We don't support subdirectories */
	KASSERT(g1->sv_type == SFS_TYPE_FILE);

	/*
	 * Link it under the new name.
	 *
	 * We could theoretically just overwrite the original
	 * directory entry, except that we need to check to make sure
	 * the new name doesn't already exist; might as well use the
	 * existing link routine.
	 */
	result = sfs_dir_link(sv, n2, g1->sv_ino, &slot2);
	if (result) {
		goto puke;
	}

	/* Increment the link count, and mark inode dirty */
	g1_inodeptr->sfi_linkcount++;
	buffer_mark_dirty(g1_inodebuf);

	/* Unlink the old slot */
	result = sfs_dir_unlink(sv, slot1);
	if (result) {
		goto puke_harder;
	}

	/*
	 * Decrement the link count again, and mark the inode dirty again,
	 * in case it's been synced behind our back.
	 */
	KASSERT(g1_inodeptr->sfi_linkcount>0);
	g1_inodeptr->sfi_linkcount--;
	buffer_mark_dirty(g1_inodebuf);
	buffer_release(g1_inodebuf);
	unreserve_buffers(4, SFS_BLOCKSIZE);

	/* Let go of the reference to g1 */
	VOP_DECREF(&g1->sv_v);

	vfs_biglock_release();
	return 0;

 puke_harder:
	/*
	 * Error recovery: try to undo what we already did
	 */
	result2 = sfs_dir_unlink(sv, slot2);
	if (result2) {
		kprintf("sfs: rename: %s\n", strerror(result));
		kprintf("sfs: rename: while cleaning up: %s\n",
			strerror(result2));
		panic("sfs: rename: Cannot recover\n");
	}
	g1_inodeptr->sfi_linkcount--;
	buffer_mark_dirty(g1_inodebuf);
 puke:
	buffer_release(g1_inodebuf);
	unreserve_buffers(4, SFS_BLOCKSIZE);

	/* Let go of the reference to g1 */
	VOP_DECREF(&g1->sv_v);

	vfs_biglock_release();
	return result;
}

/*
 * lookparent returns the last path component as a string and the
 * directory it's in as a vnode.
 *
 * Since we don't support subdirectories, this is very easy -
 * return the root dir and copy the path.
 */
static
int
sfs_lookparent(struct vnode *v, char *path, struct vnode **ret,
		  char *buf, size_t buflen)
{
	struct sfs_vnode *sv = v->vn_data;

	vfs_biglock_acquire();

	if (sv->sv_type != SFS_TYPE_DIR) {
		vfs_biglock_release();
		return ENOTDIR;
	}

	if (strlen(path)+1 > buflen) {
		vfs_biglock_release();
		return ENAMETOOLONG;
	}
	strcpy(buf, path);

	VOP_INCREF(&sv->sv_v);
	*ret = &sv->sv_v;

	vfs_biglock_release();
	return 0;
}

/*
 * Lookup gets a vnode for a pathname.
 *
 * Since we don't support subdirectories, it's easy - just look up the
 * name.
 *
 * Requires up to 3 buffers.
 */
static
int
sfs_lookup(struct vnode *v, char *path, struct vnode **ret)
{
	struct sfs_vnode *sv = v->vn_data;
	struct sfs_vnode *final;
	int result;

	vfs_biglock_acquire();

	if (sv->sv_type != SFS_TYPE_DIR) {
		vfs_biglock_release();
		return ENOTDIR;
	}

	reserve_buffers(3, SFS_BLOCKSIZE);

	result = sfs_lookonce(sv, path, &final, NULL, NULL);
	if (result) {
		unreserve_buffers(3, SFS_BLOCKSIZE);
		vfs_biglock_release();
		return result;
	}

	*ret = &final->sv_v;

	unreserve_buffers(3, SFS_BLOCKSIZE);
	vfs_biglock_release();
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
	UNIMP,   /* getdirentry */
	ISDIR,   /* write */
	sfs_ioctl,
	sfs_stat,
	sfs_gettype,
	UNIMP,   /* tryseek */
	sfs_fsync,
	ISDIR,   /* mmap */
	ISDIR,   /* truncate */
	sfs_namefile,

	sfs_creat,
	UNIMP,   /* symlink */
	UNIMP,   /* mkdir */
	sfs_link,
	sfs_remove,
	UNIMP,   /* rmdir */
	sfs_rename,

	sfs_lookup,
	sfs_lookparent,
};

/*
 * Function to load a inode into memory as a vnode, or dig up one
 * that's already resident.
 *
 * Requires 1 buffer.
 */
static
int
sfs_loadvnode(struct sfs_fs *sfs, uint32_t ino, int forcetype,
		 struct sfs_vnode **ret, struct buf **ret_inodebuf)
{
	struct vnode *v;
	struct sfs_vnode *sv;
	const struct vnode_ops *ops = NULL;
	struct buf *inodebuf;
	struct sfs_inode *inodeptr;
	unsigned i, num;
	int result;

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

			if (ret_inodebuf != NULL) {
				/* find the inode for the caller */
				result = buffer_read(&sfs->sfs_absfs, ino,
						     SFS_BLOCKSIZE, &inodebuf);
				if (result) {
					return result;
				}
				*ret_inodebuf = inodebuf;
			}

			VOP_INCREF(&sv->sv_v);
			*ret = sv;
			return 0;
		}
	}

	/* Didn't have it loaded; load it */

	sv = kmalloc(sizeof(struct sfs_vnode));
	if (sv==NULL) {
		return ENOMEM;
	}

	/* Must be in an allocated block */
	if (!sfs_bused(sfs, ino)) {
		panic("sfs: Tried to load inode %u from unallocated block\n",
		      ino);
	}

	/* Read the block the inode is in */
	result = buffer_read(&sfs->sfs_absfs, ino, SFS_BLOCKSIZE, &inodebuf);
	if (result) {
		kfree(sv);
		return result;
	}
	inodeptr = buffer_map(inodebuf);

	/*
	 * FORCETYPE is set if we're creating a new file, because the
	 * buffer will have been zeroed out already and thus the type
	 * recorded there will be SFS_TYPE_INVAL.
	 */
	if (forcetype != SFS_TYPE_INVAL) {
		KASSERT(inodeptr->sfi_type == SFS_TYPE_INVAL);
		inodeptr->sfi_type = forcetype;
		buffer_mark_dirty(inodebuf);
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
		buffer_release(inodebuf);
		kfree(sv);
		return result;
	}

	/* Set the other fields in our vnode structure */
	sv->sv_ino = ino;

	/* Add it to our table */
	result = vnodearray_add(sfs->sfs_vnodes, &sv->sv_v, NULL);
	if (result) {
		VOP_CLEANUP(&sv->sv_v);
		buffer_release(inodebuf);
		kfree(sv);
		return result;
	}

	/* Hand it back */

	if (ret_inodebuf != NULL) {
		*ret_inodebuf = inodebuf;
	}
	else {
		buffer_release(inodebuf);
	}

	*ret = sv;
	return 0;
}

/*
 * Get vnode for the root of the filesystem.
 * The root vnode is always found in block 1 (SFS_ROOT_LOCATION).
 */
struct vnode *
sfs_getroot(struct fs *fs)
{
	struct sfs_fs *sfs = fs->fs_data;
	struct sfs_vnode *sv;
	int result;

	vfs_biglock_acquire();
	reserve_buffers(1, SFS_BLOCKSIZE);

	result = sfs_loadvnode(sfs, SFS_ROOT_LOCATION, SFS_TYPE_INVAL,
			       &sv, NULL);
	if (result) {
		panic("sfs: getroot: Cannot load root vnode\n");
	}

	unreserve_buffers(1, SFS_BLOCKSIZE);
	vfs_biglock_release();

	return &sv->sv_v;
}
