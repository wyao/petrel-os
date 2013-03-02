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
	struct pid_list *tmp;

	lock_acquire(curthread->cv_lock);  // Released in thread_exit

	while(curthread->children != NULL) {
		tmp = curthread->children;
		process_table[tmp->pid]->parent_pid = -1; // Mark children as orphans
		curthread->children = curthread->children->next;
		kfree(tmp);
	}
	curthread->exit_status = exitcode;

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
	int contains = 0;
	struct pid_list *tmp = curthread->children;
	while (tmp != NULL){
		tmp = curthread->children;
		if (tmp->pid == pid){
			contains = 1;
			break;
		}
		tmp = tmp->next;
	}
	if (!contains){
		*err = ECHILD;
		return -1;
	}
	// Then check if the child is already a zombie - if so, store its exit status in status, destroy it, and return
	// If the parent acquires this lock first, child will wait to exit.  Otherwise, by the time the child releases
	// the lock it will have completely exited.
	lock_acquire(process_table[pid]->cv_lock);
	if (process_table[pid]->t_state != S_ZOMBIE) {
		cv_wait(process_table[pid]->waiting_on,process_table[pid]->cv_lock);
	}
	lock_release(process_table[pid]->cv_lock);
	// Remove pid from list of children
	struct pid_list *curr = curthread->children;
	while (curr != NULL){
		if (curr->next == tmp){
			curr->next = tmp->next;
			kfree(tmp);
			break;
		}
	}

	lock_destroy(process_table[pid]->cv_lock);
	cv_destroy(process_table[pid]->waiting_on);	
	*status = process_table[pid]->exit_status;
	process_table[pid]->parent_pid = -1; // Mark for reaping by exorcise
	process_table[pid] = NULL;

	return pid;
}

pid_t
sys_getpid(void){
	return curthread->pid;
}
