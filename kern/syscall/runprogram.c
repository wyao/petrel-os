/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than this function does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <synch.h>
#include <kern/unistd.h>

static void
stdio_init(){
		char *consoleR = NULL;
	char *consoleW = NULL;
	char *consoleE = NULL;
	consoleR = kstrdup("con:");
	consoleW = kstrdup("con:");
	consoleE = kstrdup("con:");

	if (consoleR == NULL || consoleW == NULL || consoleE == NULL)
		panic("thread_bootstrap: could not connect to console\n");

	struct vnode *out;
	struct vnode *in;
	struct vnode *err;
	int r0 = vfs_open(consoleR,O_RDONLY,0664,&in);
	int r1 = vfs_open(consoleW,O_WRONLY,0664,&out);
	int r2 = vfs_open(consoleE,O_WRONLY,0664,&err);
	if (r0 | r1 | r2)
	 	panic("thread_bootstrap: could not connect to console\n");

	struct file_table *stdin = kmalloc(sizeof(struct file_table));
	struct file_table *stdout = kmalloc(sizeof(struct file_table));
	struct file_table *stderr = kmalloc(sizeof(struct file_table));
	if (stdin == NULL || stdout == NULL || stderr == NULL)
		panic("thread_bootstrap: out of memory\n");

	stdin->status = O_RDONLY;
	stdin->refcnt = 1;
	stdin->offset = 0;
	stdin->file = in;
	stdin->update_pos = 0;
	stdin->mutex = lock_create("stdin");

	stdout->status = O_WRONLY;
	stdout->refcnt = 1;
	stdout->offset = 0;
	stdout->file = out;
	stdout->update_pos = 0;
	stdout->mutex = lock_create("stdout");

	stderr->status = O_WRONLY;
	stderr->refcnt = 1;
	stderr->offset = 0;
	stderr->file = err;
	stderr->update_pos = 0;
	stderr->mutex = lock_create("stderr");

	if (stdin->mutex == NULL || stdout->mutex == NULL || stderr->mutex == NULL)
		panic("thread_bootstrap: stdin, stdout, or stderr lock couldn't be initialized\n");

	curthread->fd[STDIN_FILENO] = stdin;
	curthread->fd[STDOUT_FILENO] = stdout;
	curthread->fd[STDERR_FILENO] = stderr;

	kfree(consoleR);
	kfree(consoleW);
	kfree(consoleE);

	/*
	 * Set current working directory to root
	 */
	char *root = NULL;
	struct vnode *rootdir;
	root = kstrdup("emu0:/");
	if (root == NULL)
		panic("stdio_init: couldnt get root directory\n");
	int result = vfs_open(root,O_RDONLY,0664,&rootdir);
	if (result)
		panic("stdio_init: couldnt get root directory\n");
	curthread->t_cwd = rootdir;
	kfree(root);
}

/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int
runprogram(char *progname)
{
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	stdio_init();

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new thread. */
	KASSERT(curthread->t_addrspace == NULL);

	/* Create a new address space. */
	curthread->t_addrspace = as_create();
	if (curthread->t_addrspace==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Activate it. */
	as_activate(curthread->t_addrspace);

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* thread_exit destroys curthread->t_addrspace */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(curthread->t_addrspace, &stackptr);
	if (result) {
		/* thread_exit destroys curthread->t_addrspace */
		return result;
	}

	/* Warp to user mode. */
	enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

