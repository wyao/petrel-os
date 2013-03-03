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
//TODO: memory limits; see error msgs

int sys_execv(userptr_t progname, userptr_t args){
    char **usr_args = (char**)args;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    int result;

    /* Open the file. */
    result = vfs_open((char *)progname, O_RDONLY, 0, &v);
    if (result) {
        return result;
    }

    // Make new addr
    struct addrspace *new_addr = as_create();
    if (new_addr == NULL){
        vfs_close(v);
        return ENOMEM;
    }

    // Create in kernel buffer
    int i = 0;
    while(usr_args[i] != NULL){
        i++;
    }/*
    char *args_buf[i+1];

    // Copy args to kernel with copyinstr
    // The args argument is an array of 0-terminated strings.
    // The array itself should be terminated by a NULL pointer.

    int len = 0;
    i = 0;
    size_t got;
    while (usr_args[i] != NULL){
        len = strlen(usr_args[i]) + 1;
        result = copyinstr(args[i], args_buf[i], len, &got);
        if (result){
            //TODO: Use got? and cleanup
            return result;
        }
        i++;
    }*/

    // Swap addrspace
    as_activate(new_addr);

    // Copy args to new addrspace

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
