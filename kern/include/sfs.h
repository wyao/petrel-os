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

#ifndef _SFS_H_
#define _SFS_H_


/*
 * Header for SFS, the Simple File System.
 */


/*
 * Get abstract structure definitions
 */
#include <buf.h>
#include <fs.h>
#include <vnode.h>

/*
 * Get on-disk structures and constants that are made available to
 * userland for the benefit of mksfs, dumpsfs, etc.
 */
#include <kern/sfs.h>

struct sfs_vnode {
	struct vnode sv_v;              /* abstract vnode structure */
	uint32_t sv_ino;                /* inode number */
	unsigned sv_type;		/* cache of sfi_type */
	struct buf *sv_buf;     /* buffer holding inode info */
	uint32_t sv_bufdepth;   /* how many currently interested in sv_buf */
	struct lock *sv_lock;		/* lock for vnode */
};

struct sfs_fs {
	struct fs sfs_absfs;            /* abstract filesystem structure */
	struct sfs_super sfs_super;	/* on-disk superblock */
	bool sfs_superdirty;            /* true if superblock modified */
	struct device *sfs_device;      /* device mounted on */
	struct vnodearray *sfs_vnodes;  /* vnodes loaded into memory */
	struct bitmap *sfs_freemap;     /* blocks in use are marked 1 */
	bool sfs_freemapdirty;          /* true if freemap modified */
	struct lock *sfs_vnlock;	/* lock for vnode table */
	struct lock *sfs_bitlock;	/* lock for bitmap/superblock */
	struct lock *sfs_renamelock;	/* lock for sfs_rename() */
};

/*
 * Function for mounting a sfs (calls vfs_mount)
 */
int sfs_mount(const char *device);


/*
 * Internal functions
 */

/* Initialize uio structure */
#define SFSUIO(iov, uio, ptr, block, rw) \
    uio_kinit(iov, uio, ptr, SFS_BLOCKSIZE, ((off_t)(block))*SFS_BLOCKSIZE, rw)

/* Block I/O ops */
int sfs_readblock(struct fs *fs, daddr_t block, void *data, size_t len);
int sfs_writeblock(struct fs *fs, daddr_t block, void *data, size_t len);

/* Get root vnode */
struct vnode *sfs_getroot(struct fs *fs);

/* Convenience functions */
int sfs_load_inode(struct sfs_vnode *sv);
void sfs_release_inode(struct sfs_vnode *sv);

#endif /* _SFS_H_ */
