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
#include <spl.h>

//TODO: memory limits; see error msgs; see limit.h
//TODO: use PATH_MAX, NAME_MAX, ARG_MAX

int sys_execv(userptr_t progname, userptr_t args){
    int i, pad, spl, argc, result;
    char *kbuf;
    size_t get, offset;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    userptr_t userdest;

    struct addrspace *old_addr = curthread->t_addrspace;
    char **usr_args = (char**)args;

    // Count argc
    argc = 0;
    while(usr_args[argc] != NULL){
        argc++;
    }
    size_t got[argc];
    char *args_buf[argc];
    userptr_t user_argv[argc];

    // Turn interrupts off to prevent multiple execs from executing to save space
    spl = splhigh();

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
    while (usr_args[i] != NULL){
        args_buf[i] = kmalloc(NAME_MAX*sizeof(char));
        if (args_buf[i] == NULL){
            result = ENOMEM;
            goto err3;
        }
        result = copyinstr((const_userptr_t)usr_args[i], args_buf[i], NAME_MAX, &got[i]);
        if (result){
            goto err3;
        }
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
        pad = (4 - (got[i]%4) ) % 4; // Word align
        offset += pad;
        offset += got[i];

        user_argv[i] = (userptr_t)(stackptr - offset);

        result = copyoutstr((const char*)args_buf[i], user_argv[i], got[i], &get);
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
    for (i=0; i<argc; i++)
        kfree(args_buf[i]);
    vfs_close(v);
    splx(spl);

    /* Warp to user mode. */
    enter_new_process(argc, (userptr_t)stackptr, stackptr, entrypoint);

    /* enter_new_process does not return. */
    return EINVAL;

    err4:
        curthread->t_addrspace = old_addr;
        as_activate(curthread->t_addrspace);
    err3:
        for (i=0; i<argc; i++){
            if (args_buf[i] != NULL)
                kfree(args_buf[i]);
        }
        as_destroy(new_addr);
    err2:
        vfs_close(v);
    err1:
        kfree(kbuf);
    err0:
        splx(spl);
        return result;
}
