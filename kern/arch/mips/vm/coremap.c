#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/coremap.h>
#include <synch.h>

// Local shared variables
static struct cm_entry *coremap;
static struct spinlock *busy_lock;

static uint32_t base; // Number of pages taken up by coremap

static uint32_t num_cm_entries;
static uint32_t num_cm_free;
static uint32_t num_cm_kernel;
static uint32_t num_cm_user;

#define COREMAP_TO_PADDR(i) (paddr_t)PAGE_SIZE * (i + base)
#define PADDR_TO_COREMAP(paddr)  (paddr / PAGE_SIZE) - base

/* coremap_bootstrap
 *
 * ram_stealmem() cannot be called after ram_getsize(), so
 * we cannot use kmalloc to allocate the coremap. We must
 * steal the memory for the coremap and fix it indefinitely.
 *
 * Synchronization: None
 */

void coremap_bootstrap(void){
    paddr_t lo, hi;
    uint32_t npages, size;

    ram_getsize(&lo, &hi);

    // Must be page aligned
    KASSERT((lo & PAGE_FRAME) == lo);
    KASSERT((hi & PAGE_FRAME) == hi);

    /* Determine coremape size. Technically don't need space for
     * the coremap pages themself, but for simplicity's sake...
     */
    npages = (hi - lo) / PAGE_SIZE;
    size = npages * sizeof(struct cm_entry);
    size = ROUNDUP(size, PAGE_SIZE);
    KASSERT((size & PAGE_FRAME) == size);

    // STEAL!
    coremap = (struct cm_entry *) PADDR_TO_KVADDR(lo);

    // Bookkeeping
    lo += size;
    base = lo / PAGE_SIZE;
    num_cm_entries = (hi / PAGE_SIZE) - base;
    num_cm_free = num_cm_entries;

    num_cm_kernel = 0;
    num_cm_user = 0;

    // Initialize coremap entries; basically zero everything


    // Initialize synchronization primitives
    spinlock_init(busy_lock);
    written_to_disk = cv_create("written to disk");
    cv_lock = lock_create("cv lock");
}

/*
 * Finds a non-busy page that is marked CME_FREE and returns its coremap index
 * or -1 on failure.  Returned page is marked as busy.
 */
int find_free_page(void){
    int i;
    for (i=0; i<(int)num_cm_entries; i++){
        if (cme_try_pin(&coremap[i])){
            if (cme_get_state(&coremap[i]) == CME_FREE)
                return i;
            else
                cme_set_busy(&coremap[i],0);
        }
    }
    return -1;
}

/* 
 * Coremap accessor/setter methods 
 */

int cme_get_vaddr(struct cm_entry *cme){
    return (int)(cme->vaddr_base << 4);
}
void cme_set_vaddr(struct cm_entry *cme, int vaddr){
    cme->vaddr_base = vaddr;
}

int cme_get_state(struct cm_entry *cme){
    return (int)(cme->state);
}
void cme_set_state(struct cm_entry *cme, int state){
    cme->state = state;
}

/* core map entry pinning */
int cme_get_busy(struct cm_entry *cme){
    return (int)cme->busy_bit;
}
void cme_set_busy(struct cm_entry *cme, int busy){
    cme->busy_bit = (busy > 0);
}
// Returns 1 on success (cme was not busy) and 0 on failure
int cme_try_pin(struct cm_entry *cme){
    spinlock_acquire(busy_lock);

    int ret = cme_get_busy(cme);
    if (!ret)
        cme_set_busy(cme,1);

    spinlock_release(busy_lock);
    return ~ret;
}

int cme_get_use(struct cm_entry *cme){
    return (int)cme->use_bit;
}
void cme_set_use(struct cm_entry *cme, int use){
    cme->use_bit = (use > 0);
}
