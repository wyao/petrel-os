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

//TODO: turn interrupts off
//TODO: Stack pointer == argv pointer
//TODO: memory limits; see error msgs; see limit.h
//TODO: set up stdio?

int sys_execv(userptr_t progname, userptr_t args){
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    userptr_t userdest;
    int result, i, len, argc, pad;
    size_t got;

    char **usr_args = (char**)args;
 
    /* Open the file. */
    result = vfs_open((char *)progname, O_RDONLY, 0, &v);
    if (result) {
        return result;
    }

    // Make new addr
    struct addrspace *new_addr = as_create();
    //TODO: check addrs != NULL

    // Create in kernel buffer
    while(usr_args[argc] != NULL){
        argc++;
    }
    char *args_buf[argc+1];

    // Copy args to kernel with copyinstr; The array is terminated by a NULL
    // The args argument is an array of 0-terminated strings.
    while (usr_args[i] != NULL){
        len = strlen(usr_args[i]) + 1;
        const_userptr_t usersrc = (const_userptr_t)&args[i];
        
        result = copyinstr(usersrc, args_buf[i], len, &got);
        if (result){
            vfs_close(v);
            return result;
        }
        if ((int)got != len){
            vfs_close(v);
            return EIO;
        }
        i++;
    }

    // Swap addrspace
    as_activate(new_addr);

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

    // Copy args to new addrspace
    userptr_t user_argv[argc+1];

    for (i=argc-1; i>-1; i--){
        len = strlen(args_buf[i]) + 1;
        const char *arg_out = (char *)&args_buf[i];
        pad = (4 - (len%4) ); // Word align

        if (i==argc-1){
            user_argv[i] = (userptr_t)(stackptr - len - pad);
        }
        else{
            user_argv[i] = (userptr_t)(usr_args[i+1] - len - pad);
        }

        copyoutstr(arg_out, user_argv[i], len, &got);
        // TODO: Err checking
    }

    // Copy pointers to argv
    userdest = user_argv[0] - 4 * (argc+1);
    stackptr = (vaddr_t)userdest; // Set stack pointer
    for (i=0; i<argc+1; i++){
        const void *src = (void *)&user_argv[i];
        copyout(src, userdest, 4);
        userdest += 4;
    }

    /* Warp to user mode. */
    enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
              stackptr, entrypoint);

    /* enter_new_process does not return. */
    vfs_close(v);
    return EINVAL;
}
