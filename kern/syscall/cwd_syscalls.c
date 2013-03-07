#include <types.h> 
#include <lib.h>
#include <thread.h>
#include <copyinout.h>
#include <syscall.h>
#include <vfs.h>
#include <vnode.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <current.h>
#include <kern/iovec.h>
#include <uio.h>
#include <kern/limits.h>

int sys_chdir(const_userptr_t pathname){
	char path[PATH_MAX];
	size_t got;

	if (pathname == NULL)
		goto err1;

	int ret = copyinstr(pathname,path,PATH_MAX,&got);

	err1:
	if (ret)
		return EFAULT;

	return vfs_chdir(path);
}

int sys___getcwd(userptr_t buf, size_t buf_len, int *err){
	char path[PATH_MAX];
	struct uio uio;
	struct iovec iov;
	uio_kinit(&iov,&uio,path,PATH_MAX,0,UIO_READ);

	*err = vfs_getcwd(&uio);
	if (*err)
		return -1;

	*err = copyout((const void*)path,buf,buf_len);
	if (*err)
		return -1;
	return uio.uio_offset;
}
