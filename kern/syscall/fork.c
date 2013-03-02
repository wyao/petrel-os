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
#include <mips/trapframe.h>

struct init_data{
  struct semaphore *wait_on_child;
  struct semaphore *wait_on_parent;
  struct trapframe *child_tf;
  struct addrspace *child_as;
};

static 
pid_t
getpid(){
  KASSERT(process_table != NULL);
  int i;
  for (i=0; i<MAX_PROCESSES; i++){
    if (process_table[i] == NULL)
      return (pid_t)i;
  }
  return -1; // No pids available
}

static
void
child_init(void *p, unsigned long n){
  (void)n;
  struct init_data *s = (struct init_data *)p;
  P(s->wait_on_parent);

  struct trapframe tf = *(s->child_tf);
  as_activate(s->child_as);

  V(s->wait_on_child);

  mips_usermode(&tf);
}


pid_t sys_fork(struct trapframe *tf, int *err){
  int i;

  // Acquire and reserve child pid
  lock_acquire(getpid_lock);
  pid_t childpid = getpid();
  if (childpid == -1) {
    lock_release(getpid_lock);
    *err = ENPROC;
    goto err1;
  }
  process_table[childpid] = curthread; // NOTE: This reserves the pid until child process is initiated
  lock_release(getpid_lock);

  // Synchronization primitives to be passed to childe intialization routine
  struct init_data *s = kmalloc(sizeof(struct init_data));
  if (s == NULL){
    *err = ENOMEM;
    goto err2;
  }
  s->wait_on_child = sem_create("wait on child",0);
  if (s->wait_on_child == NULL){
    *err = ENOMEM;
    goto err3;
  }
  s->wait_on_parent = sem_create("wait on parent",0);
  if (s->wait_on_parent == NULL){
    *err = ENOMEM;
    goto err4;
  }

  // Copy trapframe from parent's stack
  struct trapframe child_tf = *tf;
  s->child_tf = &child_tf;

  // Copy the parent file descriptors
  struct file_table **child_fd = kmalloc(MAX_FILE_DESCRIPTOR*sizeof(struct file_table *));
  if (child_fd == NULL){
    *err = ENOMEM;
    goto err5;
  }
  for (i=0; i<MAX_FILE_DESCRIPTOR; i++)
    child_fd[i] = curthread->fd[i];
  // TODO: after child is initialized, we need to increment all the reference counts for the file_table structs

  // Copy the parent address space
  struct addrspace *child_as;
  *err = as_copy(curthread->t_addrspace, &child_as);
  if (*err){
    *err = ENOMEM;
    goto err6;
  }
  s->child_as = child_as;

  // Create lock and cv for the child process
  struct lock *child_cv_lock = lock_create("cv_lock");
  if (child_cv_lock == NULL){
    *err = ENOMEM;
    goto err7;
  }
  struct cv *child_waiting_on = cv_create("waiting_on");
  if (child_waiting_on == NULL){
    *err = ENOMEM;
    goto err8;
  }

  // Create a pid_list entry for the parent
  struct pid_list *new_child_pidlist = kmalloc(sizeof(struct pid_list));
  if (new_child_pidlist == NULL){
    *err = ENOMEM;
    goto err9;
  }

  //TODO: Give child name?

  struct thread *child_thread;

  *err = thread_fork("child", child_init, s, 0, &child_thread);
  if (*err){

  }

  // Copy process field from parent to child
  child_thread->parent_pid = curthread->pid;
  child_thread->fd = child_fd;
  for (i=0; i<MAX_FILE_DESCRIPTOR; i++){
    if (child_thread->fd[i] != NULL){
      child_thread->fd[i]->refcnt++;
    }
  }

  // Free init_data

  // Error cleanup
  err9:
    cv_destroy(child_waiting_on);
  err8:
    lock_destroy(child_cv_lock);
  err7:
    as_destroy(child_as);
  err6:
    kfree(child_fd);
  err5:
    sem_destroy(s->wait_on_parent);
  err4:
    sem_destroy(s->wait_on_child);
  err3:
    kfree(s);
  err2:
    lock_acquire(getpid_lock);
    process_table[childpid] = NULL;
    lock_release(getpid_lock);
  err1:
    return -1;
}
