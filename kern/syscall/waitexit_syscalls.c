#include <types.h>
#include <lib.h>
#include <thread.h>
#include <syscall.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <current.h>

void
sys__exit(int exitcode){
	(void)exitcode;
	thread_exit();
}

pid_t
sys_waitpid(pid_t pid, int *status, int options, int *err){
	if (options != 0){
		*err = EINVAL;
		return -1;		
	}
	if (status == NULL){
		*err = EFAULT;
		return -1;
	}
	if (process_table[pid] == NULL) {
		*err = ESRCH;
		return -1;
	}
	// Need to check if the process is a child of the caller
	// Then check if the child is already a zombie - if so, store its exit status in status, destroy it, and return
	// TODO: MUST LOCK STATUS CHANGES, OTHERWISE CHILD MIGHT EXIT DURING THIS CHECK

	lock_acquire(process_table[pid]->cv_lock);
	cv_wait(process_table[pid]->waiting_on,process_table[pid]->cv_lock);

	*status = process_table[pid]->exit_status;
	lock_release(process_table[pid]->cv_lock);
	// TODO: Remove pid from list of children
	//thread_destroy(process_table[pid]);
	process_table[pid] = NULL;

	return pid;
}

pid_t
sys_getpid(void){
	return curthread->pid;
}
