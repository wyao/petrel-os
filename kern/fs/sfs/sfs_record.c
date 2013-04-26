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
struct record *makerec_dir(uint32_t parent_inode, uint32_t slot, uint32_t inode, char *sfd_name){
	struct record *r = kmalloc(sizeof(struct record));
	int i;
	if (r != NULL){
		r->transaction_type = REC_DIR;
		r->changed.r_directory.parent_inode = parent_inode;
		r->changed.r_directory.slot = slot;
		r->changed.r_directory.inode = inode;

		for (i=0; i<SFS_NAMELEN; i++){
			r->changed.r_directory.sfd_name[i] = sfd_name[i];
			if (sfd_name[i] == 0)
				break;
		}
		if (i >= SFS_NAMELEN) // Name was too long
			kfree(r);
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
