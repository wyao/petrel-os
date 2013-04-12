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

#include <kern/fcntl.h>
#include <vnode.h>
#include <vfs.h>
#include <bitmap.h>
#include <cpu.h>
#include <uio.h>

#define MIN_USER_CM_PAGES 10

// Local shared variables
static struct cm_entry *coremap;
static struct spinlock busy_lock = SPINLOCK_INITIALIZER;
static struct spinlock stat_lock = SPINLOCK_INITIALIZER;
static int clock_hand;

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
    KASSERT(coremap[ix].as == NULL);
    KASSERT(coremap[ix].disk_offset == -1);
    KASSERT(coremap[ix].vaddr_base == 0);
    KASSERT(coremap[ix].state == CME_FREE);
    KASSERT(coremap[ix].busy_bit == 1);
    KASSERT(coremap[ix].use_bit == 0);

    spinlock_acquire(&stat_lock);
    num_cm_free -= 1;
    KASSERT(num_cm_free >= 0);
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
 * not NULL, else allocates user page.
 *
 * Synchronization: note that by not locking stat_lock the entire time
 * it is possible for num_cm_user to be less than MIN_USER_CM_PAGES, but
 * this should be fine if MIN_USER_CM_PAGES is set large enough. This
 * decision was made to keep stat_lock as granular as possible.
 *
 * Leaves it up to vm_fault() (for user) and alloc_kpages() (for kernel)
 * to unpin page.
 */
paddr_t alloc_one_page(struct addrspace *as, vaddr_t va){
    int ix = -1;
    int iskern;

    KASSERT(num_cm_entries != 0);

    iskern = (as == NULL);

    // check there we leave enough pages for user
    if (iskern && reached_kpage_limit()) {
        kprintf("alloc_one_page: kernel heap full\n");
        return INVALID_PADDR;
    }

    if (num_cm_free > 0)
        ix = find_free_page();
    if (ix < 0) {
        // Find a page to swap
        ix = choose_evict_page();
        KASSERT(ix != -1); // Shouldn't happen...ever...
        if (cme_get_state(ix) != CME_FREE){
            // To avoid deadlock, acquire AS locks in order of raw pointer value
            // If we are evicting our own page, do nothing extra.
            if ((int)coremap[ix].as < (int)as){
                lock_release(as->pt_lock);
                lock_acquire(coremap[ix].as->pt_lock);
                lock_acquire(as->pt_lock);
            }
            if ((int)coremap[ix].as > (int)as)
                lock_acquire(coremap[ix].as->pt_lock);

            // SHOOT DOWN ADDRESS ON ALL CPUS

            if (cme_get_state(ix) == CME_DIRTY)
                swapout(COREMAP_TO_PADDR(ix));
            evict_page(COREMAP_TO_PADDR(ix));

            if (coremap[ix].as != as)
                lock_release(coremap[ix].as->pt_lock);
        }
    }

    // ix should be a valid page index at this point
    KASSERT(coremap[ix].state == CME_FREE);
    KASSERT(coremap[ix].busy_bit == 1);
    mark_allocated(ix, iskern);

    // If not kernel, update as and vaddr_base
    // Also assign a disk offset for swapping
    if (!iskern) {
        KASSERT(va != 0);
        coremap[ix].as = as;
        coremap[ix].vaddr_base = va >> 12;
        // We only want a enw offset if we are not swapping in
        if (coremap[ix].disk_offset == -1)
            coremap[ix].disk_offset = swapfile_reserve_index();
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
    cme_set_busy(PADDR_TO_COREMAP(pa), 0);
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
        KASSERT(coremap[ix].as == NULL);
        KASSERT(coremap[ix].disk_offset == -1);
        KASSERT(coremap[ix].vaddr_base == 0);
        KASSERT(coremap[ix].state == CME_FIXED);
        KASSERT(coremap[ix].use_bit == 0);
        spinlock_acquire(&stat_lock);
        num_cm_kernel--;
    }
    else {
        KASSERT(coremap[ix].as != NULL);
        coremap[ix].as = NULL;

        // Free swap space
        KASSERT(coremap[ix].disk_offset != -1);
        swapfile_free_index(coremap[ix].disk_offset);
        coremap[ix].disk_offset = -1;

        KASSERT(coremap[ix].vaddr_base != 0);
        coremap[ix].vaddr_base = 0;
        coremap[ix].use_bit = 0;
        spinlock_acquire(&stat_lock);
        num_cm_user--;
    }
    // Have to zero this page for some reason
    bzero((void *)PADDR_TO_KVADDR(pa), PAGE_SIZE);

    cme_set_state(ix, CME_FREE);
    num_cm_free++;
    spinlock_release(&stat_lock);

    cme_set_busy(ix, 0); // Make available
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
        if (cme_get_state(i) == CME_FREE) {
            if (cme_try_pin(i)) {
                if (cme_get_state(i) == CME_FREE)
                    return i;
                cme_set_busy(i, 0);
            }
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

int cme_get_offset(int ix){
    return coremap[ix].disk_offset;
}

void cme_set_offset(int ix, int offset){
    coremap[ix].disk_offset = offset;
}

unsigned cme_get_state(int ix){
    return (unsigned)(coremap[ix].state);
}
void cme_set_state(int ix, unsigned state){
    coremap[ix].state = state;
}

/* core map entry pinning */
unsigned cme_get_busy(int ix){
    if (coremap[ix].busy_bit == 0)
        return 0;
    else
        return 1;
}
void cme_set_busy(int ix, unsigned busy){
    coremap[ix].busy_bit = (busy > 0);
}
// Returns 1 on success (cme[ix] was not busy) and 0 on failure
unsigned cme_try_pin(int ix){
    spinlock_acquire(&busy_lock);
    // If busy
    if(cme_get_busy(ix)) {
        spinlock_release(&busy_lock);
        return 0;

    }
    // If not busy, pin it!
    cme_set_busy(ix,1);
    spinlock_release(&busy_lock);
    return 1;
}

unsigned cme_get_use(int ix){
    return (unsigned)coremap[ix].use_bit;
}
void cme_set_use(int ix, unsigned use){
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
        coremap[i].as = NULL;
        coremap[i].disk_offset = -1;
        coremap[i].vaddr_base = 0;
        coremap[i].state = CME_FREE;
        coremap[i].busy_bit = 0;
        coremap[i].use_bit = 0;
    }

    /* Initialize synchronization primitives
     */
    written_to_disk = cv_create("written to disk");
    cv_lock = lock_create("cv lock");
}


/*
 * Swap space helper functions
 */

/*
 * Called at the end of boot()
 * TODO: Should we call when first used?
 */
void swapfile_init(void){
    // Should not yet be initialized
    KASSERT(swapfile == NULL);
    KASSERT(disk_map == NULL);
    KASSERT(disk_map_lock == NULL);

    char *disk_path = NULL;
    disk_path = kstrdup("lhd0raw:");
    if (disk_path == NULL)
        panic("swapfile_init: could not open disk");

    int err = vfs_open(disk_path,O_RDWR,0,&swapfile);
    if (err)
        panic("swapfile_init: could not open disk");

    // TODO: 1200 is an approximation of 5MB/4KB, which is max number of pages we can have in swap
    disk_map = bitmap_create(1200);
    if (disk_map == NULL)
        panic("swapfile_init: could not create disk map");

    disk_map_lock = lock_create("disk map lock");
    if (disk_map_lock == NULL)
        panic("swapfile_init: could not create disk map lock");

    //dirty_pages = sem_create("dirty pages",0);

    //thread_fork("writer",writer_thread,NULL,0,NULL);
}

/*
 * Uses the bitmap disk_map to find and return an available index, marking it used
 * Panics in case of swap space being filled
 */
unsigned swapfile_reserve_index(void){
    KASSERT(swapfile != NULL);
    KASSERT(disk_map != NULL);
    KASSERT(disk_map_lock != NULL);

    lock_acquire(disk_map_lock);

    unsigned index;
    if (bitmap_alloc(disk_map,&index))
        panic("swapfile_reserve_index: disk out of space");

    lock_release(disk_map_lock);
    return index;
}

/*
 * Marks the given index of the disk map freed - index must have been previously obtained
 * through the use of swapfile_reserve_index
 */
void swapfile_free_index(unsigned index){
    KASSERT(swapfile != NULL);
    KASSERT(disk_map != NULL);
    KASSERT(disk_map_lock != NULL);

    lock_acquire(disk_map_lock);

    KASSERT(bitmap_isset(disk_map,index));
    bitmap_unmark(disk_map,index);

    lock_release(disk_map_lock);
}

/*
 * Writes a page of physical memory to disk offset specified in coremap
 * evict_page should only be called after successful return of this function
 */
int swapout(paddr_t ppn){
    KASSERT(PADDR_IS_VALID(ppn));

    int i = PADDR_TO_COREMAP(ppn);
    KASSERT(coremap[i].disk_offset != -1);
    KASSERT(coremap[i].state == CME_DIRTY);
    
    int ret = write_page(ppn,coremap[i].disk_offset);
    if (!ret)
        cme_set_state(i,CME_CLEAN);
    return ret;
}

/*
 * Reads a page of physical memory from the disk offset specified in the 
 * thread's page table to the given physical address. 
 * Upon success of read, updates the core map and page table.
 * Should be called after successful completion of evict_page
 */
int swapin(struct addrspace *as, vaddr_t vpn, paddr_t dest){
    int idx, ret;
    unsigned offset;

    KASSERT(as != NULL);

    struct pt_ent *pte = get_pt_entry(as,vpn);
    KASSERT(pte != NULL && pte_get_exists(pte));
    KASSERT(!pte_get_present(pte));

    offset = pte_get_location(pte);
    ret = read_page(dest,offset);

    if (!ret){
        idx = PADDR_TO_COREMAP(dest);
        coremap[idx].disk_offset = offset;
        coremap[idx].vaddr_base = vpn;
        coremap[idx].as = as;

        pte_set_present(pte,1);
        pte_set_location(pte,dest);
    }

    return ret;
}

/*
 * Updates page table to have entry marked as absent and its location
 * set to the offset on disk where it can be found.
 *
 * SHOULD ONLY BE CALLED WHEN THE LOCKS FOR BOTH ADDRSPACES ARE HELD
 */
void evict_page(paddr_t ppn){
    KASSERT(PADDR_IS_VALID(ppn));

    int i = PADDR_TO_COREMAP(ppn);
    KASSERT(coremap[i].state == CME_CLEAN);
    KASSERT(coremap[i].disk_offset != -1);
    KASSERT(coremap[i].as != NULL);

    struct pt_ent *pte = get_pt_entry(coremap[i].as,coremap[i].vaddr_base<<12);
    KASSERT(pte != NULL);

    pte_set_present(pte,0);
    pte_set_location(pte,coremap[i].disk_offset);
    cme_set_state(i,CME_FREE);
}

int write_page(paddr_t ppn, unsigned offset){
    KASSERT(PADDR_IS_VALID(ppn));
    // TODO: Assert offset is not off disk

    void *src = (void *)PADDR_TO_KVADDR(ppn);
    struct iovec iov;
    struct uio u;
    uio_kinit(&iov, &u, src, PAGE_SIZE, offset*PAGE_SIZE, UIO_WRITE);

    return VOP_WRITE(swapfile,&u);
}

int read_page(paddr_t ppn, unsigned offset){
   KASSERT(PADDR_IS_VALID(ppn));
    // TODO: Assert offset is not off disk

    void *src = (void *)PADDR_TO_KVADDR(ppn);
    struct iovec iov;
    struct uio u;
    uio_kinit(&iov, &u, src, PAGE_SIZE, offset*PAGE_SIZE, UIO_READ);

    return VOP_READ(swapfile,&u);
}

// NOTE: As of now, ignoring cleaner thread
void writer_thread(void *junk, unsigned long num){
    (void)junk;
    (void)num;

    int i;
    while(1){
        P(dirty_pages);
        for (i=0; i<(int)num_cm_entries; i++){
            if (cme_get_state(i) == CME_DIRTY){
                // Write dirty page to memory
                cme_set_state(i,CME_CLEAN);
                // Signal CV
                break;
            }
        }
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

    uint32_t vpn = (uint32_t)cme_get_vaddr(ix) & TLBHI_VPAGE;
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
void ipi_tlbshootdown_wait(struct cpu *target, uint32_t ppn){
    struct tlbshootdown ts;
    struct semaphore *s = sem_create("wait on",0);

    ts.done_handling = s;
    ts.ppn = ppn;

    ipi_tlbshootdown(target,&ts);
    P(s); // Only V-ed upon completion of shootdown

    sem_destroy(s);
}

