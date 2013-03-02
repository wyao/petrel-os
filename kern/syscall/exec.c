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

//TODO: turn interrupts off
//TODO: Stack pointer == argv pointer

int sys_execv(userptr_t progname, userptr_t args){
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

    // Copy args to kernel
    (void)args;

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
