/*
 * Added for PetrelOS Assignment 4
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
#include <copyinout.h>


static 
struct sfs_vnode *get_vnode(struct fs *fs, unsigned inode_num);

/* Return a record struct populated except for transaction ID field */

struct record *makerec_inode(uint32_t inode_num, uint16_t id_lvl, uint16_t set, uint32_t offset, uint32_t blockno){
	struct record *r = kmalloc(sizeof(struct record));
	if (r != NULL){
		r->transaction_type = REC_INODE;
		struct r_inode s = {inode_num, id_lvl, set, offset, blockno};
		r->changed.r_inode = s;
	}
	return r;
}
struct record *makerec_itype(uint32_t inode_num, uint32_t type){
	struct record *r = kmalloc(sizeof(struct record));
	if (r != NULL){
		r->transaction_type = REC_ITYPE;
		struct r_itype s = {inode_num, type};
		r->changed.r_itype = s;
	}
	return r;
}
struct record *makerec_isize(uint32_t inode_num, uint32_t size){
	struct record *r = kmalloc(sizeof(struct record));
	if (r != NULL){
		r->transaction_type = REC_ISIZE;
		struct r_isize s = {inode_num, size};
		r->changed.r_isize = s;
	}
	return r;
}
struct record *makerec_ilink(uint32_t inode_num, uint32_t linkcount){
	struct record *r = kmalloc(sizeof(struct record));
	if (r != NULL){
		r->transaction_type = REC_ILINK;
		struct r_ilink s = {inode_num, linkcount};
		r->changed.r_ilink = s;
	}
	return r;
}
struct record *makerec_dir(uint32_t parent_inode, uint32_t slot, uint32_t inode, const char *sfd_name){
	struct record *r = kmalloc(sizeof(struct record));
	int i;
	if (r != NULL){
		r->transaction_type = REC_DIR;
		r->changed.r_directory.parent_inode = parent_inode;
		r->changed.r_directory.slot = slot;
		r->changed.r_directory.inode = inode;

		if (sfd_name != NULL) {
			for (i=0; i<SFS_NAMELEN; i++){
				r->changed.r_directory.sfd_name[i] = sfd_name[i];
				if (sfd_name[i] == 0)
					break;
			}
			if (i >= SFS_NAMELEN) // Name was too long
				kfree(r);
		}
	}
	return r;
}
struct record *makerec_bitmap(uint32_t index, uint32_t setting){
	struct record *r = kmalloc(sizeof(struct record));
	if (r != NULL){
		r->transaction_type = REC_BITMAP;
		struct r_bitmap s = {index, setting};
		r->changed.r_bitmap = s;
	}
	return r;
}

// Apply a recorded change
int apply_record(struct fs *fs, struct record *r){
	struct sfs_inode *inodeptr;
	struct sfs_vnode *vnodeptr;
	struct sfs_fs *sfs;
	struct buf *kbuf;
	struct sfs_dir sd;
	struct iovec iov;
	struct uio ku;
	uint32_t *indir, *iddata;
	off_t actual_pos;
	int result;

	switch(r->transaction_type){
		case REC_INODE:
		vnodeptr = get_vnode(fs,r->changed.r_inode.inode_num);
		inodeptr = buffer_map(vnodeptr->sv_buf);
		if (r->changed.r_inode.id_lvl == 0){
			inodeptr->sfi_direct[r->changed.r_inode.offset] = r->changed.r_inode.blockno;
			buffer_mark_dirty(vnodeptr->sv_buf);
		}
		else {
			// Find level of indirection
			if (r->changed.r_inode.id_lvl == 1)
				indir = &inodeptr->sfi_indirect;
			if (r->changed.r_inode.id_lvl == 2)
				indir = &inodeptr->sfi_dindirect;
			if (r->changed.r_inode.id_lvl == 3)
				indir = &inodeptr->sfi_tindirect;

			// Allocated an indirect block
			if (r->changed.r_inode.set){
				*indir = r->changed.r_inode.blockno;
				buffer_mark_dirty(vnodeptr->sv_buf);
			}
			// Changed contents of indirect block
			else {
				result = buffer_get(fs, *indir, SFS_BLOCKSIZE, &kbuf);
				KASSERT(result == 0);
				iddata = buffer_map(kbuf);
				iddata[r->changed.r_inode.offset] = r->changed.r_inode.blockno;
				buffer_mark_dirty(kbuf);
			}
		}
		break;
		case REC_ILINK:
		vnodeptr = get_vnode(fs,r->changed.r_ilink.inode_num);
		inodeptr = buffer_map(vnodeptr->sv_buf);
		inodeptr->sfi_linkcount = r->changed.r_ilink.linkcount;
		buffer_mark_dirty(vnodeptr->sv_buf);
		break;
		case REC_ISIZE:
		vnodeptr = get_vnode(fs,r->changed.r_isize.inode_num);
		inodeptr = buffer_map(vnodeptr->sv_buf);
		inodeptr->sfi_size = r->changed.r_isize.size;
		buffer_mark_dirty(vnodeptr->sv_buf);
		break;
		case REC_ITYPE:
		vnodeptr = get_vnode(fs,r->changed.r_itype.inode_num);
		inodeptr = buffer_map(vnodeptr->sv_buf);
		inodeptr->sfi_type = r->changed.r_itype.type;
		buffer_mark_dirty(vnodeptr->sv_buf);
		break;
		case REC_BITMAP:
		sfs = fs->fs_data;
		if (r->changed.r_bitmap.setting)
			bitmap_mark(sfs->sfs_freemap,r->changed.r_bitmap.index);
		else
			bitmap_unmark(sfs->sfs_freemap,r->changed.r_bitmap.index);
		sfs->sfs_freemapdirty = 1;
		break;
		case REC_DIR:
		// Set up directory entry
		sd.sfd_ino = r->changed.r_directory.inode;
		strcpy(sd.sfd_name, r->changed.r_directory.sfd_name);
		// Get parent inode
		vnodeptr = get_vnode(fs,r->changed.r_directory.parent_inode);
		actual_pos = sizeof(struct sfs_dir)*r->changed.r_directory.slot;
		uio_kinit(&iov, &ku, &sd, sizeof(struct sfs_dir), actual_pos, UIO_WRITE);

		result = lock_do_i_hold(vnodeptr->sv_lock);
		if (!result) // Not sure if we need to hold lock in recovery, but theres an assert at beginning of io calls
			lock_acquire(vnodeptr->sv_lock);
		KASSERT(sfs_io2(vnodeptr,&ku) == 0);
		if (!result)
			lock_release(vnodeptr->sv_lock);
		break;
		default:
		return EINVAL;
	}
	return 0;
}

static 
struct sfs_vnode *get_vnode(struct fs *fs, unsigned inode_num){
	struct sfs_fs *sfs = fs->fs_data;
	struct sfs_vnode *vnodeptr;
	int result;

	int i, nvns;
	nvns = vnodearray_num(sfs->sfs_vnodes);
	for (i=0; i<nvns; i++){
		vnodeptr = vnodearray_get(sfs->sfs_vnodes,i)->vn_data;
		if (vnodeptr->sv_ino == inode_num){
			result = sfs_load_inode(vnodeptr);
			KASSERT(result != 0); // inode should be able to load
			return vnodeptr;
		}
	}
	KASSERT(0); // Should never fail
	return NULL;
}
