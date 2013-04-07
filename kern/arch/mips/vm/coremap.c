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

#define MIN_USER_CM_PAGES 10

// Local shared variables
static struct cm_entry *coremap;
static struct spinlock busy_lock = SPINLOCK_INITIALIZER;
static struct spinlock stat_lock = SPINLOCK_INITIALIZER;
static int clock_hand;

static uint32_t base; // Number of pages taken up by coremap

static int num_cm_entries;
static int num_cm_free;
static int num_cm_kernel;
static int num_cm_user;

#define COREMAP_TO_PADDR(i) (paddr_t)PAGE_SIZE * (i + base)
#define PADDR_TO_COREMAP(paddr)  (paddr / PAGE_SIZE) - base

/*
 * Static page selection helpers
 */

// See alloc_one_page for synchonization note.
static int reached_kpage_limit(void){
    if (num_cm_kernel + 1 >= num_cm_entries - MIN_USER_CM_PAGES) {
        return 1;
    }
    return 0;
}

static void mark_allocated(int ix, int iskern) {
    // Sanity check
    KASSERT(coremap[ix].thread == NULL);
    KASSERT(coremap[ix].disk_offset == -1);
    KASSERT(coremap[ix].vaddr_base == 0);
    KASSERT(coremap[ix].state == CME_FREE);
    KASSERT(coremap[ix].busy_bit == 1);
    KASSERT(coremap[ix].use_bit == 0);

    spinlock_acquire(&stat_lock);
    if (iskern) {
        coremap[ix].state = CME_FIXED;
        num_cm_kernel += 1;
    }
    else {
        coremap[ix].state = CME_DIRTY;
        num_cm_user += 1;
    }
    KASSERT(num_cm_free+num_cm_user+num_cm_kernel == num_cm_entries);
    spinlock_release(&stat_lock);
}

/*
 * Page selection APIs
 */

/* alloc_one_page
 *
 * Allocate one page of kernel memory. Allocates kernel page if thread is
 * not NULL, eles allocates user page.
 *
 * Synchronization: note that by not locking stat_lock the entire time
 * it is possible for num_cm_user to be less than MIN_USER_CM_PAGES, but
 * this should be fine if MIN_USER_CM_PAGES is set large enough. This
 * decision was made to keep stat_lock as granular as possible.
 */
paddr_t alloc_one_page(struct thread *thread, vaddr_t va){
    int ix, iskern;

    iskern = (thread == NULL);

    // check there we leave enough pages for user
    if (iskern && reached_kpage_limit()) {
        kprintf("alloc_one_page: kernel heap full\n");
        return INVALID_PADDR;
    }

    if (num_cm_free > 0) {
        ix = find_free_page();

        if (ix < 0){ // This should not happen
            kprintf("alloc_one_page: inconsistent state\n");
            return INVALID_PADDR;
        }
    }
    else {
        kprintf("alloc_one_page: currently does not support swapping\n");
        return INVALID_PADDR;
    }

    // ix should be a valid page index at this point
    mark_allocated(ix, iskern);

    // If not kernel, update thread and vaddr_base
    if (!iskern) {
        KASSERT(va != 0);
        coremap[ix].thread = thread;
        coremap[ix].vaddr_base = va >> 12;
    }

    return COREMAP_TO_PADDR(ix);
}

vaddr_t alloc_kpages(int npages) {
    paddr_t pa;

    if (npages > 1) {
        kprintf("alloc_kpages: only support single page allocations\n");
        return (vaddr_t) NULL;
    }

    pa = alloc_one_page(NULL, (vaddr_t)0);
    if (pa == INVALID_PADDR) {
        kprintf("alloc_kpages: allocation failed\n");
        return (vaddr_t) NULL;
    }
    return PADDR_TO_KVADDR(pa);
}

/*
 * free_coremap_page
 *
 * Synchronization: if not freeing kernel page, the physical page needs to be
 * pinned prior to calling this function. This is to ensure that we are able to
 * grab the page table lock and pin the page at the same time. Just for
 * consistency's sake, we also leave it to the caller to unpin the physical
 * page.
 */

void free_coremap_page(paddr_t pa, bool iskern) {
    int ix = PADDR_TO_COREMAP(pa);

    KASSERT(ix < num_cm_entries);

    if (coremap[ix].state == CME_FREE) {
        panic("free_coremap_page: freeing already free page\n");
    }

    KASSERT(iskern || cme_get_busy(ix));

    // TODO: Flush TLB

    if (iskern) {
        KASSERT(coremap[ix].thread == NULL);
        KASSERT(coremap[ix].disk_offset == -1);
        KASSERT(coremap[ix].vaddr_base == 0);
        KASSERT(coremap[ix].state == CME_FIXED);
        KASSERT(coremap[ix].use_bit == 0);
        spinlock_acquire(&stat_lock);
        num_cm_kernel--;
    }
    else {
        KASSERT(coremap[ix].thread != NULL);
        coremap[ix].thread = NULL;
        // TODO: clear swap space
        KASSERT(coremap[ix].vaddr_base != 0);
        KASSERT(coremap[ix].state != CME_DIRTY);
        coremap[ix].use_bit = 0;
        spinlock_acquire(&stat_lock);
        num_cm_user--;
    }
    cme_set_state(ix, CME_FREE);
    num_cm_free++;
    spinlock_release(&stat_lock);
}

void free_kpages(vaddr_t va) {
    free_coremap_page(KVADDR_TO_PADDR(va), true /* iskern */);
}

/*
 * Finds a non-busy page that is marked CME_FREE and returns its coremap index
 * or -1 on failure.  Returned page is marked as busy.
 */
int find_free_page(void){
    int i;
    for (i=0; i<(int)num_cm_entries; i++){
        if (cme_try_pin(i)){
            if (cme_get_state(i) == CME_FREE) {
                spinlock_acquire(&stat_lock);
                num_cm_free -= 1;
                KASSERT(num_cm_free >= 0);
                spinlock_release(&stat_lock);
                return i;
            }
            else
                cme_set_busy(i,0);
        }
    }
    return -1;
}

/*
 * Finds a non-busy page that is marked NRU and returns it. 
 * Intervening pages are marked unused.
 */
int choose_evict_page(void){
    while(1){
        if (cme_get_state(clock_hand) != CME_FIXED){
            if (cme_try_pin(clock_hand)){
                if (cme_get_use(clock_hand)){
                    cme_set_use(clock_hand,0);
                    cme_set_busy(clock_hand,0);
                }
                else //if tlb_probe_all(page i) TODO
                    return clock_hand;
            }
            clock_hand++;
            if (clock_hand >= (int)num_cm_entries)
                clock_hand = 0;
        }
    }
    return -1; //Control should never reach here...
}

/* 
 * Coremap accessor/setter methods 
 */

int cm_get_index(paddr_t pa){
    return PADDR_TO_COREMAP(pa);
}

int cme_get_vaddr(int ix){
    return (coremap[ix].vaddr_base << 12);
}
void cme_set_vaddr(int ix, int vaddr){
    coremap[ix].vaddr_base = vaddr >> 12;
}

int cme_get_state(int ix){
    return (int)(coremap[ix].state);
}
void cme_set_state(int ix, int state){
    coremap[ix].state = state;
}

/* core map entry pinning */
int cme_get_busy(int ix){
    return (int)coremap[ix].busy_bit;
}
void cme_set_busy(int ix, int busy){
    coremap[ix].busy_bit = (busy > 0);
}
// Returns 1 on success (cme[ix] was not busy) and 0 on failure
int cme_try_pin(int ix){
    spinlock_acquire(&busy_lock);

    int ret = cme_get_busy(ix);
    if (!ret)
        cme_set_busy(ix,1);

    spinlock_release(&busy_lock);
    return ~ret;
}

int cme_get_use(int ix){
    return (int)coremap[ix].use_bit;
}
void cme_set_use(int ix, int use){
    coremap[ix].use_bit = (use > 0);
}

/*
 * Machine-dependent functions
 */

/* coremap_bootstrap
 *
 * ram_stealmem() cannot be called after ram_getsize(), so
 * we cannot use kmalloc to allocate the coremap. We must
 * steal the memory for the coremap and fix it indefinitely.
 *
 * Synchronization: None
 */

void coremap_bootstrap(void){
    int i;
    paddr_t lo, hi;
    uint32_t npages, size;

    /* Initialize synchronization primitives before we lose access
     * to ram_stealmem()
     */
    written_to_disk = cv_create("written to disk");
    cv_lock = lock_create("cv lock");

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

    // NRU Clock
    clock_hand = 0;

    // Initialize coremap entries; basically zero everything
    for (i=0; i<(int)num_cm_entries; i++) {
        coremap[i].thread = NULL;
        coremap[i].disk_offset = -1;
        coremap[i].vaddr_base = 0;
        coremap[i].state = CME_FREE;
        coremap[i].busy_bit = 0;
        coremap[i].use_bit = 0;
    }
}


/*
 * TLB Shootdown handlers (MACHINE DEPENDENT)
 * Interrupts are disabled with spl to ensure that TLB wipes are atomic
 * (This may only be important for shootdown_all...)
 */

void vm_tlbshootdown_all(void){
    int i,spl;

    spl = splhigh();

    for (i=0; i<NUM_TLB; i++)
        tlb_write(TLBHI_INVALID(i),TLBLO_INVALID(),i);

    splx(spl);
}

void vm_tlbshootdown(const struct tlbshootdown *ts){
    struct semaphore *sem = ts->done_handling;
    uint32_t ppn = ts->ppn;
    int spl;

    spl = splhigh();

    int ix = PADDR_TO_COREMAP(ppn);

    if (cme_get_state(ix) == CME_FREE)
        goto done;

    uint32_t vpn = (uint32_t)cme_get_vaddr(ix);
    int ret = tlb_probe(vpn,0); // ppn is not used by function

    if (ret >= 0) // tlb_probe returns negative value on failure or index on success
        tlb_write(TLBHI_INVALID(ret),TLBLO_INVALID(),ret);

    done:
    if (sem != NULL)
        V(sem);

    splx(spl);
}

/*
 * Sets up tlbshootdown struct and permforms a shootdown synchronized by the semaphore
 * Handles allocation and destruction of the semaphore
 */
void vm_tlbshootdown_wait(uint32_t ppn){
    struct tlbshootdown ts;
    struct semaphore *s = sem_create("wait on",0);

    ts.done_handling = s;
    ts.ppn = ppn;

    vm_tlbshootdown(&ts);
    P(s); // Only V-ed upon completion of shootdown

    sem_destroy(s);
}

