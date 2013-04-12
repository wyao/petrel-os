#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <kern/errno.h>
#include <thread.h>
#include <addrspace.h>

#define HEAP_MAX    0x40000000 // Max amount of space given to user heap

int sys_sbrk(int amount, int *err) {
    struct addrspace *as = curthread->t_addrspace;
    vaddr_t old;

    if (amount == 0)
        return as->heap_end;
    if (amount < 0) {
        if ((long)as->heap_end + (long)amount >= (long)as->heap_start) {
            as->heap_end += amount;
            return as->heap_end;
        }
        *err = EINVAL;
        return -1;
    }
    if (as->heap_end + amount < USERSTACK - STACK_PAGES * PAGE_SIZE &&
        as->heap_end + amount < as->heap_start + HEAP_MAX) {
        old = as->heap_end;
        as->heap_end += amount;
        return old;
    }
    *err = ENOMEM;
    return -1;
}
