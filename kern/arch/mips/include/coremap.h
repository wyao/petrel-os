#ifndef _COREMAP_H_
#define _COREMAP_H_

/*
 * Coremap definitions
 */

#include <machine/vm.h>

#define CME_FREE 0
#define CME_FIXED 1
#define CME_CLEAN 2
#define CME_DIRTY 3

/* Core map structures and functions */
struct cm_entry{
    struct thread *thread;
    int disk_offset;  // Stores the disk offset when the page is in memory
    int vaddr_base:28;
    int state:2;
    int busy_bit:1;
    int use_bit:1;
};

struct cv *written_to_disk;
struct lock *cv_lock;


/*
 * Page selection helpers
 */
int find_free_page(void);

/*
 * Acessor/setter methods
 */
int cme_get_vaddr(struct cm_entry *cme);
void cme_set_vaddr(struct cm_entry *cme, int vaddr);

int cme_get_state(struct cm_entry *cme);
void cme_set_state(struct cm_entry *cme, int state);

/* core map entry pinning */
int cme_get_busy(struct cm_entry *cme);
void cme_set_busy(struct cm_entry *cme, int busy);

// Attempts to (synchronously) acquire busy bit on given CME and returns success or failure
int cme_try_pin(struct cm_entry *cme);

int cme_get_use(struct cm_entry *cme);
void cme_set_use(struct cm_entry *cme, int use);

/*
 * Machine-dependent functions
 */

void coremap_bootstrap(void);

#endif /* _COREMAP_H_ */
