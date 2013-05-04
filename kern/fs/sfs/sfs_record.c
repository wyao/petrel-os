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
struct sfs_inode *get_inode(struct fs *fs, unsigned inode_num);

static
int
sfs_bmap_r(struct fs *fs, struct sfs_inode *inodeptr, uint32_t fileblock, uint32_t *diskblock);

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
	if (r != NULL){
		r->transaction_type = REC_DIR;
		r->changed.r_directory.parent_inode = parent_inode;
		r->changed.r_directory.slot = slot;
		r->changed.r_directory.inode = inode;

		bzero(&r->changed.r_directory.sfd_name,SFS_NAMELEN);

		if (sfd_name != NULL) {
			strcpy(r->changed.r_directory.sfd_name,sfd_name);
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
void apply_record(struct fs *fs, struct record *r){
	struct sfs_inode *inodeptr;
	struct sfs_fs *sfs;
	struct sfs_dir sd;
	uint32_t *indir, *iddata;
	uint32_t fileblock, fileoff, dirblock;
	off_t actual_pos;
	char data[SFS_BLOCKSIZE];

	inodeptr = kmalloc(sizeof(struct sfs_inode));

	switch(r->transaction_type){
		case REC_INODE:
		inodeptr = get_inode(fs,r->changed.r_inode.inode_num);
		if (r->changed.r_inode.id_lvl == 0){
			inodeptr->sfi_direct[r->changed.r_inode.offset] = r->changed.r_inode.blockno;
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
				sfs_writeblock(fs,r->changed.r_inode.inode_num,inodeptr,SFS_BLOCKSIZE);
			}
			// Changed contents of indirect block
			else {
				sfs_readblock(fs,*indir,data,SFS_BLOCKSIZE);
				iddata = (uint32_t *)data;
				iddata[r->changed.r_inode.offset] = r->changed.r_inode.blockno;
				sfs_writeblock(fs,*indir,iddata,SFS_BLOCKSIZE);
			}
		}
		break;

		case REC_ILINK:
		inodeptr = get_inode(fs,r->changed.r_ilink.inode_num);
		inodeptr->sfi_linkcount = r->changed.r_ilink.linkcount;
		sfs_writeblock(fs,r->changed.r_inode.inode_num,inodeptr,SFS_BLOCKSIZE);
		break;

		case REC_ISIZE:
		inodeptr = get_inode(fs,r->changed.r_isize.inode_num);
		inodeptr->sfi_size = r->changed.r_isize.size;
		sfs_writeblock(fs,r->changed.r_inode.inode_num,inodeptr,SFS_BLOCKSIZE);
		break;

		case REC_ITYPE:
		inodeptr = get_inode(fs,r->changed.r_itype.inode_num);
		inodeptr->sfi_type = r->changed.r_itype.type;
		sfs_writeblock(fs,r->changed.r_inode.inode_num,inodeptr,SFS_BLOCKSIZE);
		break;

		case REC_BITMAP:
		sfs = fs->fs_data;
		if (r->changed.r_bitmap.setting)
			bitmap_mark(sfs->sfs_freemap,r->changed.r_bitmap.index);
		else
			bitmap_unmark(sfs->sfs_freemap,r->changed.r_bitmap.index);
		sfs->sfs_freemapdirty = 1;
		// TODO: sync buffer explicitly
		break;

		case REC_DIR:
		sd.sfd_ino = r->changed.r_directory.inode;
		bzero(sd.sfd_name,SFS_NAMELEN);
		strcpy(sd.sfd_name,r->changed.r_directory.sfd_name);

		inodeptr = get_inode(fs,r->changed.r_directory.parent_inode);
		actual_pos = sizeof(struct sfs_dir)*r->changed.r_directory.slot;
		fileblock = actual_pos/SFS_BLOCKSIZE;
		fileoff = actual_pos % SFS_BLOCKSIZE;

		sfs_bmap_r(fs,inodeptr,fileblock,&dirblock);
		sfs_readblock(fs,dirblock,data,SFS_BLOCKSIZE);
		memcpy(&data[fileoff],&sd,sizeof(struct sfs_dir));
		sfs_writeblock(fs,dirblock,data,SFS_BLOCKSIZE);
		
		break;
		default:
		panic("Invalid record");
	}

	kfree(inodeptr);
}

static 
struct sfs_inode *get_inode(struct fs *fs, unsigned inode_num) {
	struct sfs_inode *inodeptr;
	if (sfs_readblock(fs,inode_num,inodeptr,SFS_BLOCKSIZE))
		panic("Couldnt get inode");
	return inodeptr;
}

static
int
sfs_bmap_r(struct fs *fs, struct sfs_inode *inodeptr, uint32_t fileblock, uint32_t *diskblock)
{
	uint32_t *iddata;
	uint32_t block, cur_block, next_block;
	uint32_t idoff;
	int indir, i;

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

	/*
	 * If the block we want is one of the direct blocks...
	 */
	if (fileblock < SFS_NDIRECT) {
		/*
		 * Get the block number
		 */
		block = inodeptr->sfi_direct[fileblock];
		*diskblock = block;
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

	KASSERT(next_block != 0);


	/* Now loop through the levels of indirection until we get to the
	 * direct block we need.
	 */
	for(i = indir; i>0; i--)
	{
		KASSERT(sfs_readblock(fs,next_block,iddata,SFS_BLOCKSIZE)==0);

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
	}

		
	*diskblock = next_block;
	return 0;
}


