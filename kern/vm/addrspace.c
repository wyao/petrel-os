/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

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

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

#define PT_PRIMARY_INDEX(va) (int)(va >> 22)
#define PT_SECONDARY_INDEX(va) (int)((va >> 12) & 0x3FF)
#define ADDRESS_OFFSET(addr) (int)(addr & 0xFFF)

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    22//12

#if USE_DUMBVM
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;  // THIS IS IN BOTH VM.C AND HERE

/* DUMBVM HELPER METHODS */
static
paddr_t
getppages(unsigned long npages) //DUPLICATED IN VM.C FOR NOW
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
#endif

/* AS FUNCTIONS */

/* as_create
 *
 * Creates/initializes addrspace struct.
 *
 * Sychronization: none
 */
struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	#if USE_DUMBVM
	/*
	 * DUMBVM INITIALIZATION
	 */
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;

	return as;

	#else
	/*
	 * ASST3 Initialization
	 */
	as->pt_lock = lock_create("page table lock");
	if (as->pt_lock == NULL)
		goto err1;
	as->page_table = pt_create();
	if (as->page_table == NULL)
		goto err2;
	as->regions = array_create();
	if (as->regions == NULL)
		goto err2;
	as->heap_start = (vaddr_t)0;
	as->is_loading = false;

	return as;

	err2:
	lock_destroy(as->pt_lock);
	err1:
	kfree(as);
	return NULL;

	#endif
}

/* as_copy
 *
 * Duplicates addrspace and copies each physical page
 * to guarantee unique physical page access.
 *
 * Sychrnonization: Will be performed by helper functions
 */

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	#if USE_DUMBVM
	/*
	 * DUMBVM COPY
	 */
	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);

	*ret = new;
	return 0;

	#else
	(void)old;
	(void)ret;

	// Copy over old page table
	int i,j;
	struct pt_ent *curr_old, *curr_new;
	lock_acquire(old->pt_lock);  // Required to prevent eviction during copying

	for (i=0; i<PAGE_SIZE; i++){
		if (old->page_table[i] != NULL){
			new->page_table[i] = kmalloc(PAGE_SIZE*sizeof(struct pt_ent));
			// For each page table entry
			for (j=0; j<PAGE_SIZE; j++){
				curr_old = &old->page_table[i][j];
				curr_new = &new->page_table[i][j];
				// If the entry exists (ie, page is in memory or swap space)
				if (pte_get_exists(curr_old)){
					// Page is in memory
					if (pte_get_present(curr_old)){
						paddr_t base_new = alloc_one_page(curthread,PT_TO_VADDR(i,j));
						paddr_t base_old = (pte_get_location(curr_old) << 12);
						// CONVERT TO KERNEL PTR AND MEMCPY
						void *src = (void *)PADDR_TO_KVADDR(base_old);
						void *dest = (void *)PADDR_TO_KVADDR(base_new);
						memcpy(dest,src,PAGE_SIZE); // Returns dest - do we need to check?

						// Set new page table entry to a valid mapping to new location with same permissions
						pt_update(new,PT_TO_VADDR(i,j),base_new>>12,0,1);

						// Unbusy the coremap entry for the new page
						cme_set_busy(cm_get_index(base_new),0);
					}
					// Page is in swap space (TODO - for now treats it as if it didn't exist)
					else{
						// Find a disk space for the page
						// Copy the page to disk with VOP_WRITE
						// Write disk location to pte and mark not present
					}
				}
			}
		}
	}

	*ret = new;
	lock_release(old->pt_lock);

	return 0;
	#endif
}

void
as_destroy(struct addrspace *as)
{
	/*
	 * Clean up as needed.
	 */

	#if !USE_DUMBVM
	/*
	 * ASST3 Destruction
	 */
	unsigned num_regions, i;
	struct region *ptr;

	lock_acquire(as->pt_lock); // TODO: Do we need to synchronize this?
	pt_destroy(as->page_table);
	lock_release(as->pt_lock);
	lock_destroy(as->pt_lock);

	// Free the recorded regions
	num_regions = array_num(as->regions);
	for (i=0; i<num_regions; i++) {
		ptr = array_get(as->regions, i);
		kfree(ptr);
	}
	array_destroy(as->regions);

	#endif

	kfree(as);
}

void
as_activate(struct addrspace *as)
{
	/*
	 * DUMBVM ACTIVATION
	 */

	//int i, spl;

	(void)as;

	/* Disable interrupts on this CPU while frobbing the TLB. */
	/*spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);*/

	vm_tlbshootdown_all();

}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	/*
	 * DUMBVM DEFINE REGION
	 */
	#if USE_DUMBVM
	size_t npages;

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;

	#else

	int errno;
	struct region *region;

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME; //TODO WHAT IS THIS??
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	// Update heap_start
	if (as->heap_start < (vaddr + sz))
		as->heap_start = vaddr + sz;

	// Record region (to be used in vm_fault)
	region = kmalloc(sizeof(struct region));
	if (region == NULL)
		return 3 /* ENOMEM */;
	region->base = vaddr;
	region->sz = sz;
	region->readable = readable;
	region->writeable = writeable;
	region->executable = executable;

	errno = array_add(as->regions, region, NULL);
	if (errno)
		return errno;

	return 0;
	#endif
}

int
as_prepare_load(struct addrspace *as)
{
	/*
	 * DUMBVM PREPARE LOAD
	 */
	#if USE_DUMBVM
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}

	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);

	return 0;

	#else

	as->is_loading = true;
	return 0;

	#endif
}

int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */
	#if USE_DUMBVM

	(void)as;
	return 0;

	#else

	as->is_loading = false;
	return 0;

	#endif
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * WDUMBVM DEFINE STACK
	 */
	#if USE_DUMBVM
	KASSERT(as->as_stackpbase != 0);
	#endif

	*stackptr = USERSTACK;
	(void)as;
	return 0;
}

int as_get_permissions(struct addrspace *as, vaddr_t va){
	int i;
	int permissions = 0;
	struct region *r;
	int len = array_num(as->regions);

	for (i=0; i<len; i++){
		r = array_get(as->regions,i);
		if (va > r->base && va < (r->base + r->sz)){
			if (r->readable)
				permissions += VM_READ;
			if (r->writeable)
				permissions += VM_WRITE;
			if (r->executable)
				permissions += VM_EXEC;
			return permissions;
		}
	}
	// Control should never reach here
	return -1;
}


/*
 * Page table helper methods
 */

struct pt_ent **pt_create(void){
	int i;
	struct pt_ent **ret = (struct pt_ent **)kmalloc(PAGE_SIZE);

	if (ret == NULL)
		return NULL;

	for (i=0; i<PAGE_SIZE/4; i++){
		ret[i] = NULL;
	}
	return ret;
}

void pt_destroy(struct pt_ent **pt){
	int i;
	for (i=0; i<PAGE_SIZE; i++){
		if (pt[i] != NULL)
			kfree(pt[i]);
	}
	kfree(pt);
}

struct pt_ent *get_pt_entry(struct addrspace *as, vaddr_t va){
	struct pt_ent *pt_dir = as->page_table[PT_PRIMARY_INDEX(va)];
	if (pt_dir != NULL){
		return &pt_dir[PT_SECONDARY_INDEX(va)];
	}
	return NULL;
}

//TODO: CREATE IF DOESN'T EXIST?
paddr_t va_to_pa(struct addrspace *as, vaddr_t va){
	struct pt_ent *pte = get_pt_entry(as,va);
	if (pte == NULL)
		return INVALID_PADDR;
	if (!pte_get_present(pte) || !pte_get_exists(pte))
		return INVALID_PADDR;
	paddr_t pa = (paddr_t)(pte_get_location(pte) << 12) + ADDRESS_OFFSET(va);
	return pa;
}


int pt_insert(struct addrspace *as, vaddr_t va, int ppn, int permissions){
	// If a secondary page table does not exist, allocate one
	int i;
	if (as->page_table[PT_PRIMARY_INDEX(va)] == NULL) {
		as->page_table[PT_PRIMARY_INDEX(va)] = kmalloc(PAGE_SIZE);

		if (as->page_table[PT_PRIMARY_INDEX(va)] == NULL)
			return ENOMEM;

		for (i=0; i<PAGE_SIZE/4; i++){
			as->page_table[PT_PRIMARY_INDEX(va)][i].page_paddr_base = 0;
			as->page_table[PT_PRIMARY_INDEX(va)][i].permissions = 0;
			as->page_table[PT_PRIMARY_INDEX(va)][i].present = 0;
			as->page_table[PT_PRIMARY_INDEX(va)][i].exists = 0;
		}
	}

	struct pt_ent *pte = get_pt_entry(as,va);
	// Ensure that the VADDR doesn't already map to something or we messed up
	KASSERT(!pte_get_exists(pte));

	pte_set_location(pte,ppn);
	pte_set_permissions(pte,permissions);
	pte_set_present(pte,1);
	pte_set_exists(pte,1);

	return 0;
}

int pt_remove(struct addrspace *as, vaddr_t va){
	struct pt_ent *pte = get_pt_entry(as,va);
	if (pte == NULL)
		return -1;
	pte_set_exists(pte,0);
	return 0;
}

int pt_update(struct addrspace *as, vaddr_t va, int ppn, int permissions, int is_present){
	struct pt_ent *pte = get_pt_entry(as,va);
	if (pte == NULL)
		return -1;

	pte_set_location(pte,ppn);
	pte_set_present(pte,is_present);
	pte_set_exists(pte,1);
	int newperms = pte_get_permissions(pte)&(~permissions);
	pte_set_permissions(pte,newperms);

	return 0;
}

/*
 * Bit masking functions for Page Table
 */

int pte_get_location(struct pt_ent *pte){
	return (int)(pte->page_paddr_base);
}
void pte_set_location(struct pt_ent *pte, int location){
	pte->page_paddr_base = location;
}
int pte_get_permissions(struct pt_ent *pte){
	return (int)pte->permissions;
}
void pte_set_permissions(struct pt_ent *pte, int permissions){
	pte->permissions = permissions;
}
int pte_get_present(struct pt_ent *pte){
	return (int)pte->present;
}
void pte_set_present(struct pt_ent *pte, int present){
	pte->present = (present > 0);
}
int pte_get_exists(struct pt_ent *pte){
	return (int)pte->exists;
}
void pte_set_exists(struct pt_ent *pte, int exists){
	pte->exists = (exists > 0);
}
