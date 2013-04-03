#include <lib.h>
#include <vm.h>
#include <machine/coremap.h>


// Local shared variables
static struct cm_entry *coremap;

static uint32_t base; // Number of pages taken up by coremap

static uint32_t num_cm_entries;
static uint32_t num_cm_free;
static uint32_t num_cm_kernel = 0;
static uint32_t num_cm_user = 0;

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
    uint32_t npages;

    ram_getsize(&lo, &hi);

    // Must be page aligned
    KASSERT((lo & PAGE_FRAME) == lo);
    KASSERT((hi & PAGE_FRAME) == hi);

    /* Determine coremape size. Technically don't need space for
     * the coremap pages themself, but for simplicity's sake...
     */
    npages = (hi - lo) / PAGE_SIZE
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

    // Initialize coremap entries; basically zero everything
    
}