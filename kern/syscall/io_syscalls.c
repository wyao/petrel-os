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
#include <kern/seek.h>
#include <kern/stat.h>

//TODO Use PATH_MAX?

int
sys_open(userptr_t filename, int flags, int *err) {
  int i,result;
  char *kbuf;
  size_t got;

  int f = flags & O_ACCMODE;
  if (f != O_RDONLY && f != O_WRONLY && f != O_RDWR){
    *err = EINVAL;
    return -1;
  }
  if (filename == NULL){
    *err = EFAULT;
    return -1;
  }
  // Look for an available file descriptor
  for (i=0; i<MAX_FILE_DESCRIPTOR; i++) {
    if (curthread->fd[i] == NULL) {
      // Initialize the file table struct and populate it
      // Note: it seems that vfs_open() will malloc and initialize the vnode
      curthread->fd[i] = kmalloc(sizeof(struct file_table));
      if (curthread->fd[i] == NULL){
        *err = ENOMEM;
        goto err1;
      }

      curthread->fd[i]->mutex = lock_create("mutex");
      if (curthread->fd[i]->mutex == NULL){
        *err = ENOMEM; //TODO unsure what errno to use
        goto err2;
      }

      curthread->fd[i]->refcnt = 1;
      curthread->fd[i]->status = flags & O_ACCMODE;
      curthread->fd[i]->offset = 0;
      curthread->fd[i]->update_pos = 1;

      kbuf = (char *)kmalloc(PATH_MAX*sizeof(char));
      if (kbuf == NULL){
        *err = ENOMEM; //TODO: Not sure what error to use
        goto err3;
      }
      result = copyinstr((const_userptr_t)filename,kbuf,PATH_MAX,&got);
      if (result){
        *err = EFAULT;
        goto err4;
      }
      // Return value is 0 for success
      // TODO: Should this be the same errno?
      *err = vfs_open((char *)filename,flags,0664,&(curthread->fd[i]->file));
      if (*err){
        goto err4;
      }

      // Success
      kfree(kbuf);
      return i;

      err4:
        kfree(kbuf);
      err3:
        lock_destroy(curthread->fd[i]->mutex);
      err2:
        kfree(curthread->fd[i]);
      err1:
        return -1;
    }
  }
  
  // If control reaches here, no FDs were available and we return an error
  *err = EMFILE;
  return -1;
}

int 
sys_close(int fd) {
  // TODO: should we check if the fd table is non-null or can we assume?
  if (fd < 0 || fd >= MAX_FILE_DESCRIPTOR)
    return EBADF;
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
sys_rw(int fd, userptr_t buf, size_t buf_len, int *err, int rw) {
  if (fd < 0 || fd >= MAX_FILE_DESCRIPTOR){
    *err = EBADF;
    return -1;
  }
  if (curthread->fd[fd] == NULL){
    *err = EBADF;
    return -1;
  }
  if (buf == NULL){
    *err = EFAULT;
    return -1;
  }

  lock_acquire(curthread->fd[fd]->mutex);
  
  if ((curthread->fd[fd]->status != rw) && (curthread->fd[fd]->status != O_RDWR)) { 
    *err = EBADF;
    lock_release(curthread->fd[fd]->mutex);
    return -1;
  }
  struct iovec iov;
  struct uio uio;

  iov.iov_ubase = buf;
  iov.iov_len = buf_len;
  uio.uio_iov = &iov;
  uio.uio_iovcnt = 1;
  uio.uio_offset = curthread->fd[fd]->offset;
  uio.uio_resid = buf_len;
  uio.uio_segflg = UIO_USERSPACE;
  uio.uio_space = curthread->t_addrspace;

  if (rw == O_RDONLY) {
    //uio_kinit(&iov, &uio, buf, buf_len, curthread->fd[fd]->offset, UIO_READ);
    uio.uio_rw = UIO_READ;
    *err = VOP_READ(curthread->fd[fd]->file,&uio);
  }
  else {
    uio.uio_rw = UIO_WRITE;
    *err = VOP_WRITE(curthread->fd[fd]->file,&uio);
  }
  int diff = uio.uio_offset - curthread->fd[fd]->offset;

  if (curthread->fd[fd]->update_pos)
    curthread->fd[fd]->offset = uio.uio_offset;  //TODO: double check this - should be new offset after read

  lock_release(curthread->fd[fd]->mutex);
  return diff;
}

int 
sys_read(int fd, userptr_t buf, size_t buf_len, int *err){
  return sys_rw(fd,buf,buf_len,err,O_RDONLY);
}

int 
sys_write(int fd, userptr_t buf, size_t buf_len, int *err){
  return sys_rw(fd,buf,buf_len,err,O_WRONLY);
}

int 
sys_dup2(int oldfd, int newfd, int *err){
  if (newfd < 0 || oldfd < 0 || newfd >= MAX_FILE_DESCRIPTOR || oldfd >= MAX_FILE_DESCRIPTOR) {
    *err = EBADF;
    return -1;
  }
  if (curthread->fd[oldfd] == NULL) {
    *err = EBADF;
    return -1;
  }
  if (oldfd == newfd || curthread->fd[oldfd] == curthread->fd[newfd]){
    return oldfd;
  }
  int i;
  int j=0;
  for (i=0; i<MAX_FILE_DESCRIPTOR; i++) {
    if (curthread->fd[i] != NULL)
      j++;
  }
  if (j==MAX_FILE_DESCRIPTOR){
    *err = EMFILE;
    return -1;
  }
  if (curthread->fd[newfd] != NULL){
    *err = sys_close(newfd);
  }
  curthread->fd[newfd] = curthread->fd[oldfd];
  curthread->fd[newfd]->refcnt++;
  return newfd;
}

off_t 
sys_lseek(int fd,off_t pos, int whence, int *err){
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END){
    *err = EINVAL;
    return -1;
  }
  if (fd < 0 || fd >= MAX_FILE_DESCRIPTOR){
    *err = EBADF;
    return -1;
  }
  if (curthread->fd[fd] == NULL){
    *err = EBADF;
    return -1;
  }
  lock_acquire(curthread->fd[fd]->mutex);
  if (curthread->fd[fd]->update_pos == 0){
    *err = ESPIPE;
    lock_release(curthread->fd[fd]->mutex);
    return -1;
  }

  off_t newpos;
  struct stat stat;
  VOP_STAT(curthread->fd[fd]->file,&stat);
  if (whence == SEEK_SET)
    newpos = pos;
  if (whence == SEEK_CUR)
    newpos = curthread->fd[fd]->offset+pos;
  if (whence == SEEK_END)
    newpos = stat.st_size+pos;

  if (newpos < 0){
    *err = EINVAL;
    lock_release(curthread->fd[fd]->mutex);
    return -1;
  }
  *err = VOP_TRYSEEK(curthread->fd[fd]->file,newpos);
  if (*err){
    lock_release(curthread->fd[fd]->mutex);
    return -1;
  }
  curthread->fd[fd]->offset = newpos;
  lock_release(curthread->fd[fd]->mutex);
  return curthread->fd[fd]->offset;
}
