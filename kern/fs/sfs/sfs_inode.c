#include <types.h>
#include <kern/errno.h>
#include <buf.h>
#include <lib.h>
#include <sfs.h>
#include <synch.h>

int
sfs_load_inode(struct sfs_vnode *sv)
{
	struct sfs_fs *sfs = sv->sv_v.vn_fs->fs_data;
	int result;

	KASSERT(lock_do_i_hold(sv->sv_lock));
	if (sv->sv_bufdepth == 0) {
		KASSERT(sv->sv_buf == NULL);
		result = buffer_read(&sfs->sfs_absfs, sv->sv_ino, SFS_BLOCKSIZE, &sv->sv_buf);
		if (result) {
			return result;
		}
	}
	sv->sv_bufdepth++;

	return 0;
}

void
sfs_release_inode(struct sfs_vnode *sv) {
	KASSERT(lock_do_i_hold(sv->sv_lock));
	KASSERT(sv->sv_buf != NULL);
	sv->sv_bufdepth--;
	if (sv->sv_bufdepth == 0) {
		buffer_release(sv->sv_buf);
		sv->sv_buf = NULL;
	}
}
