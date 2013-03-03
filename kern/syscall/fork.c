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
#include <limits.h>

struct init_data{
  struct semaphore *wait_on_child;
  struct semaphore *wait_on_parent;
  struct trapframe *child_tf;
};

static 
pid_t
getpid(){
  KASSERT(process_table != NULL);
  int i;
  for (i=PID_MIN; i<MAX_PROCESSES; i++){
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
  tf.tf_v0 = 0;
  tf.tf_v1 = 0;
  tf.tf_epc += 4; // Advance program counter
  as_activate(curthread->t_addrspace);

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

  // Copy the parent address space
  struct addrspace *child_as;
  *err = as_copy(curthread->t_addrspace, &child_as);
  if (*err){
    *err = ENOMEM;
    goto err5;
  }

  // Create a pid_list entry for the parent
  struct pid_list *new_child_pidlist = kmalloc(sizeof(struct pid_list));
  if (new_child_pidlist == NULL){
    *err = ENOMEM;
    goto err9;
  }
  //TODO: upon success of thread_fork, add this to parent's child_list (when?)

  struct thread *child_thread;

  *err = thread_fork("child", child_init, s, 0, &child_thread);
  if (*err){
    goto err10;
  }

  // Populate child thread with allocated fields and those copied from parent
  child_thread->parent_pid = curthread->pid;
  for (i=0; i<MAX_FILE_DESCRIPTOR; i++){
    if (curthread->fd[i] != NULL){
      child_thread->fd[i] = curthread->fd[i];
      child_thread->fd[i]->refcnt++;
    }
  }
  child_thread->t_addrspace = child_as;
  child_thread->t_cwd = curthread->t_cwd;
  VOP_INCREF(child_thread->t_cwd); // TODO: do we need to do this?
  child_thread->pid = childpid;
  child_thread->parent_pid = curthread->pid;
  // TODO: other setup?
  
  V(s->wait_on_parent);
  P(s->wait_on_child);

  // TODO: Check if child was successful?
  // Insert child's PID into head of parents list
  new_child_pidlist->next = curthread->children;
  new_child_pidlist->pid = childpid;
  curthread->children = new_child_pidlist;
  process_table[childpid] = child_thread;

  // Free init_data (child is done)
  sem_destroy(s->wait_on_parent);
  sem_destroy(s->wait_on_child);
  kfree(s);

  return childpid;

  // Error cleanup
  err10:
    kfree(new_child_pidlist);
  err9:
    as_destroy(child_as);
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
