/*
 * Process test code.
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <current.h>

static
void
proc_init_test(void)
{
    kprintf("Testing process initalization...\n");

    KASSERT(curthread != NULL);
    KASSERT(curthread->pid == 0);
    KASSERT(curthread->parent_pid == -1);
    KASSERT(curthread->children == NULL);
    KASSERT(curthread->fd[0] == NULL);

    KASSERT(process_table[0] == curthread);

    kprintf("Success!\n");
}

int
proctest(int nargs, char **args){
    (void)nargs;
    (void)args;

    kprintf("Starting process test...\n");
    proc_init_test();
    kprintf("\nProcess test done.\n");

    return 0;
}
