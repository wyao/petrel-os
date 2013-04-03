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
#include <copyinout.h>

//TODO: Use global lock instead

int sys_execv(userptr_t progname, userptr_t args){
    int i, pad, argc, result, part;
    char *kbuf;
    size_t get, offset;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    userptr_t userdest;

    struct addrspace *old_addr = curthread->t_addrspace;
    char **usr_args = (char**)args;

    kbuf = (void *) kmalloc(sizeof(void *));

    // Check user pointer (reusing kbuf)
    /*
     * This only checks four bytes of the user-supplied argv array.  You can
     * thus safely access argv[0], but further accesses to argv[i] are still
     * unsafe and unchecked.
     */
    result = copyin((const_userptr_t)args,kbuf,4);
    if (result){
        kfree(kbuf);
        return result;
    }
    kfree(kbuf);

    // Ensure only 1 exec at a time
    lock_acquire(global_exec_lock);

    // Count args
    argc = 0;
    /*
     * These accesses are unsafe.
     */
    while(usr_args[argc] != NULL){
        argc++;
    }

    /*
     * With an ARG_MAX of 65536, it is definitely unsafe to be allocating these
     * arrays on the stack; they could easily overflow the 4k kernel stack
     * page.
     */
    size_t got[argc];
    userptr_t user_argv[argc];

    /*
     * Allocating such a large amount of contiguous memory may become very
     * difficult or impossible due to external fragmentation in the kernel heap
     * or in physical memory.  You may want to consider allocating smaller
     * buffers on demand, as you need them.
     */
    char *args_buf = kmalloc(ARG_MAX*sizeof(char));
    if (args_buf == NULL)
        goto err_;

    // Check user pointer
    kbuf = (char *)kmalloc(PATH_MAX*sizeof(char));
    if (kbuf == NULL){
        result = ENOMEM;
        goto err0;
    }
    result = copyinstr((const_userptr_t)progname,kbuf,PATH_MAX,&get);
    if (result){
        goto err1;
    }

    /* Open the file. */
    result = vfs_open((char *)progname, O_RDONLY, 0, &v);
    if (result) {
        goto err1;
    }

    // Keep old addrspace in case of failure
    struct addrspace *new_addr = as_create();
    if (new_addr == NULL){
        result = ENOMEM;
        goto err2;
    }

    // Copy args to kernel with copyinstr; The array is terminated by a NULL
    // The args argument is an array of 0-terminated strings.
    i = 0;
    part = 0;
    /*
     * These accesses are (still) unsafe.
     */
    while (usr_args[i] != NULL){
      /*
       * If the user has two arguments of size ARG_MAX - 1, this will easily
       * overflow args_buf.  In particular, you never actually enforce that the
       * user's arguments are bound by ARG_MAX; you just assume the user will
       * oblige.
       */
        result = copyinstr((const_userptr_t)usr_args[i], &args_buf[part], ARG_MAX, &got[i]);
        if (result){
            goto err3;
        }
        part += got[i];
        i++;
    }

    // Swap addrspace
    curthread->t_addrspace = new_addr;
    as_activate(curthread->t_addrspace);

    /* Load the executable. */
    result = load_elf(v, &entrypoint);
    if (result) {
        goto err4;
    }

    /* Define the user stack in the address space */
    result = as_define_stack(curthread->t_addrspace, &stackptr);
    if (result) {
        goto err4;
    }

    // Copy args to new addrspace
    offset = 0;
    for (i=argc-1; i>-1; i--){
        part -= got[i]; // readjust inherited part index
        /*
         * Assuming that pointers are 4-byte aligned breaks the machine
         * dependency abstraction barrier.
         */
        pad = (4 - (got[i]%4) ) % 4; // Word align
        offset += pad;
        offset += got[i];

        user_argv[i] = (userptr_t)(stackptr - offset);

        result = copyoutstr((const char*)&args_buf[part], user_argv[i], got[i], &get);
        if (result){
            goto err4;
        }
    }

    // Copy pointers to argv
    userdest = user_argv[0] - 4 * (argc+1);
    stackptr = (vaddr_t)userdest; // Set stack pointer
    for (i=0; i<argc; i++){
        result = copyout((const void *)&user_argv[i], userdest, 4);
        if (result)
            goto err4;
        userdest += 4;
    }

    // Wrap up
    kfree(args_buf);
    vfs_close(v);
    lock_release(global_exec_lock);

    /* Warp to user mode. */
    enter_new_process(argc, (userptr_t)stackptr, stackptr, entrypoint);

    /* enter_new_process does not return. */
    /*
     * You should panic if this happens; in no correct execution of the kernel
     * should this ever occur.
     */
    return EINVAL;

    err4:
        curthread->t_addrspace = old_addr;
        as_activate(curthread->t_addrspace);
    err3:
        as_destroy(new_addr);
    err2:
        vfs_close(v);
    err1:
        kfree(kbuf);
    err0:
        kfree(args_buf);
    err_:
        lock_release(global_exec_lock);
        return result;
}
