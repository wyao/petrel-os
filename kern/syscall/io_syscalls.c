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

int
sys_open(userptr_t filename, int flags, int *err) {
  int i;
  // Look for an available file descriptor
  for (i=0; i<MAX_FILE_DESCRIPTOR; i++) {
    if (curthread->fd[i] == NULL) {
      // Initialize the file table struct and populate it
      // Note: it seems that vfs_open() will malloc and initialize the vnode
      curthread->fd[i] = kmalloc(sizeof(struct file_table));
      curthread->fd[i]->refcnt = 1;
      curthread->fd[i]->status = flags & O_ACCMODE;
      curthread->fd[i]->offset = 0;
      curthread->fd[i]->mutex = lock_create("mutex");

      // Return value is 0 for success
      *err = vfs_open((char *)filename,flags,0664,&(curthread->fd[i]->file));
      if (*err != 0){
	return -1;
      }
      return i;
    }
  }
  
  // If control reaches here, no FDs were available and we return an error
  *err = EMFILE;
  return -1;
}

int 
sys_close(int fd) {
  // TODO: should we check if the fd table is non-null or can we assume?
  if (curthread->fd[fd] == NULL)
    return EBADF;
  lock_acquire(curthread->fd[fd]->mutex);
  
  curthread->fd[fd]->refcnt--;
  // Returns void; prints for hard I/O errors so no way to return them
  vfs_close(curthread->fd[fd]->file);
  
  // Free contents of struct (vnode should be freed by vfs_close)
  if (curthread->fd[fd]->refcnt == 0){
    lock_release(curthread->fd[fd]->mutex);
    lock_destroy(curthread->fd[fd]->mutex);
    kfree(curthread->fd[fd]);
    curthread->fd[fd] = NULL;
  }
  else
    lock_release(curthread->fd[fd]->mutex);
  return 0;
}

int 
sys_read(int fd, userptr_t buf, size_t buf_len, int *err){
  if (fd < 0 || fd >= MAX_FILE_DESCRIPTOR || curthread->fd[fd] == NULL){
    *err = EBADF;
    return -1;
  }
  // TODO: Check lock initialized?
  lock_acquire(curthread->fd[fd]->mutex);
  
  if (curthread->fd[fd]->status == O_WRONLY) {
    *err = EBADF;
    lock_release(curthread->fd[fd]->mutex);
    return -1;
  }
  struct iovec iov;
  iov.iov_ubase = buf;
  iov.iov_len = buf_len;

  struct uio uio;
  uio.uio_iov = &iov;
  uio.uio_segflg = UIO_USERSPACE;
  uio.uio_rw = UIO_READ;
  uio.uio_offset = curthread->fd[fd]->offset;

  // TODO: find out what vop_read actually returns...
  *err = VOP_READ(curthread->fd[fd]->file,&uio);
  int read = uio.uio_offset - curthread->fd[fd]->offset;
  curthread->fd[fd]->offset = uio.uio_offset;  //TODO: double check this - should be new offset after read

  lock_release(curthread->fd[fd]->mutex);
  return read;
}

int 
sys_write(int fd, userptr_t buf, size_t buf_len, int *err){
  if (fd < 0 || fd >= MAX_FILE_DESCRIPTOR || curthread->fd[fd] == NULL){
    *err = EBADF;
    return -1;
  }
  // TODO: Check lock initialized?
  lock_acquire(curthread->fd[fd]->mutex);
  
  if (curthread->fd[fd]->status == O_RDONLY) {
    *err = EBADF;
    lock_release(curthread->fd[fd]->mutex);
    return -1;
  }
  struct iovec iov;
  iov.iov_ubase = buf;
  iov.iov_len = buf_len;

  struct uio uio;
  uio.uio_iov = &iov;
  uio.uio_segflg = UIO_USERSPACE;
  uio.uio_rw = UIO_READ;
  uio.uio_offset = curthread->fd[fd]->offset;

  // TODO: find out what vop_write actually returns...
  *err = VOP_WRITE(curthread->fd[fd]->file,&uio);
  int read = uio.uio_offset - curthread->fd[fd]->offset;
  curthread->fd[fd]->offset = uio.uio_offset; // TODO: double check this - should be new offset after write

  lock_release(curthread->fd[fd]->mutex);
  return read;
}
