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
#include <addrspace.h>

struct init_data{
  struct sempahore *wait_on_child;
  struct sempahore *wait_on_parent;
  struct trapframe *child_tf;
};


static
void
child_init(void *s, unsigned long n){
  (void)n;
  P(s->wait_on_parent);

  struct trapframe tf = s->child_tf;

  V(s->wait_on_child); //TODO: Is this right?
}


pid_t sys_fork(upserptr_t tf, int *err){

  struct trapframe child_tf = tf;
  if (child_tf = NULL){
    *err = ENOMEM; //TODO: ????
    goto err1;
  }


  // TODO: strip pid assignment out of thread_create???

  struct addrspace *child_as;
  *err = as_copy(curthread->t_addrspace, *child_as);
  if (*err){

  }

  // TODO: copy curthread->fd

  // TODO: need to get_pid if thread_create gets one?

  struct init_data *s = kmalloc(sizeof(struct init_data));
  if (s == NULL){

  }
  s->child_tf = child_tf;
  s->wait_on_child = sem_create("wait on child",0);
  if (s->wait_on_child == NULL){

  }
  s->wait_on_parent = sem_create("wait on parent",0);
  if (s->wait_on_parent == NULL){

  }

  struct *child_thread;

  *err = thread_fork("child", child_init, s, 0, &child_thread);
  if (*err){

  }

  // Copy process field from parent to child


  // Error cleanup
  err1:
    return -1;
}