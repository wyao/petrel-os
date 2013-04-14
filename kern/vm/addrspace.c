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
		goto err3;
	as->heap_start = (vaddr_t)0;
	as->heap_end = (vaddr_t)0;
	as->is_loading = false;

	return as;

	err3:
	pt_destroy(as->page_table);
	err2:
	lock_destroy(as->pt_lock);
	err1:
	kfree(as);
	return NULL;
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

	int i,j, errno;
	unsigned n, c;
	struct region *old_region, *new_region;
	void *src, *dest;
	struct pt_ent *curr_old, *curr_new;
	int retval, perm, offset;
	paddr_t base_old;

	new->heap_start = old->heap_start;
	new->heap_end = old->heap_end;

	// Copy region
	n = array_num(old->regions);
	for (c=0; c<n; c++) {
		old_region = array_get(old->regions, c);
		new_region = kmalloc(sizeof(struct region));
		if (old_region == NULL || new_region == NULL) {
			goto err1;
		}

		new_region->base = old_region->base;
		new_region->sz = old_region->sz;
		new_region->readable = old_region->readable;
		new_region->writeable = old_region->writeable;
		new_region->executable = old_region->executable;

		errno = array_add(new->regions, new_region, NULL);
		if (errno) {
			goto err1;
		}
	}

	// Copy page table

	// PIN ALL PAGES - ensure that no eviction happen during copy
	// MUST HAPPEN BEFORE LOCKING ADDRESS SPACE TO AVOID DEADLOCK!
	pin_all_pages(old);

	lock_acquire(old->pt_lock);
	for (i=0; i<PAGE_ENTRIES; i++){
		if (old->page_table[i] != NULL){
			new->page_table[i] = kmalloc(PAGE_SIZE);
			// For each page table entry
			for (j=0; j<PAGE_ENTRIES; j++){
				curr_old = &old->page_table[i][j];
				curr_new = &new->page_table[i][j];
				// If the entry exists (ie, page is in memory or swap space)
				if (pte_get_exists(curr_old)){
					offset = swapfile_reserve_index();  // Location where we will copy the page to
					// Page is in memory
					if (pte_get_present(curr_old)){
						// Write from pinned memory page to new disk offset
						base_old = (pte_get_location(curr_old) << 12);
						src = (void *)PADDR_TO_KVADDR(base_old);

						retval = write_page(src,offset);
						KASSERT(retval == 0);

						// If page was in memory, we pinned it at the start with pin_all_pages
						// so we must unpin it upon completion of copying
						cme_set_busy(cm_get_index(base_old),0);						
					}
					// Page is in swap space
					else{
						// Make a temporary buffer and read the page from disk
						dest = kmalloc(PAGE_SIZE);
						retval = read_page(dest,pte_get_location(curr_old));
						KASSERT(retval == 0);

						// Copy the page from the buffer to the new disk offset
						retval = write_page(dest,offset);
						KASSERT(retval == 0);
						kfree(dest);
					}
					// Update the page table for the new process
					perm = pte_get_permissions(&old->page_table[i][j]);
					pt_update(new,PT_TO_VADDR(i,j),offset,perm,0);
				}
			}
		}
	}

	*ret = new;
	lock_release(old->pt_lock);

	return 0;
	
	err1:
	for (i=0; i<(int)n; i++){
		new_region = (struct region *)array_get(new->regions,i);
		if (new_region != NULL)
			kfree(new_region);
	}
	return ENOMEM;
}

void
as_destroy(struct addrspace *as)
{
	/*
	 * ASST3 Destruction
	 */
	unsigned num_regions, i;
	struct region *ptr;

	// PIN ALL PAGES - makes sure no evictions during destruction
	// MUST HAPPEN BEFORE LOCKING ADDRESS SPACE TO AVOID DEADLOCK!
	pin_all_pages(as);

	// Free page table entries and associated core map entries
	lock_acquire(as->pt_lock);
	pt_destroy(as->page_table);
	lock_release(as->pt_lock);
	lock_destroy(as->pt_lock);

	// Free the recorded regions
	num_regions = array_num(as->regions);
	i = num_regions - 1;
	while (num_regions > 0){
		ptr = array_get(as->regions, i);
		kfree(ptr);
		array_remove(as->regions, i);

		if (i == 0)
			break;
		i--;
	}
	array_destroy(as->regions);

	kfree(as);
}

void
as_activate(struct addrspace *as)
{
	(void)as;

	// Writes over entire TLB with invalid entries
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
	int errno;
	struct region *region;
	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME; //TODO WHAT IS THIS??
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	// Update heap_start
	if (as->heap_start < (vaddr + sz)) {
		as->heap_start = vaddr + sz;
		as->heap_end = as->heap_start;
	}

	// Record region (to be used in vm_fault)
	region = kmalloc(sizeof(struct region));
	if (region == NULL)
		return ENOMEM;
	region->base = vaddr;
	region->sz = sz;
	region->readable = readable;
	region->writeable = writeable;
	region->executable = executable;
	errno = array_add(as->regions, region, NULL);
	if (errno)
		return errno;

	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	// VM_READONLY fault handling has special case allowing dirtying of page
	// if the is_loading variable of the address space is set
	as->is_loading = true;

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	as->is_loading = false;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	*stackptr = USERSTACK;
	(void)as;
	return 0;
}

/*
 * Searches the regions of an address space to find the one the given
 * virtual address falls in, then returns the permissions of the region.
 * Returns -1 if the given address does not fall in a region
 */
int as_get_permissions(struct addrspace *as, vaddr_t va){
	unsigned i;
	int permissions = 0;
	struct region *r;
	unsigned len = array_num(as->regions);

	for (i=0; i<len; i++){
		r = array_get(as->regions,i);
		if (va >= r->base && va < (r->base + r->sz)){
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

// Allocates and returns a pointer to a page table directory
struct pt_ent **pt_create(void){
	int i;
	struct pt_ent **ret = (struct pt_ent **)kmalloc(PAGE_SIZE);

	if (ret == NULL)
		return NULL;

	for (i=0; i<PAGE_ENTRIES; i++){
		ret[i] = NULL;
	}
	return ret;
}

/*
 * Frees the page table after freeing any coremap entries and disk offsets that
 * were mapped to virtual addresses within it.
 *
 * Should only be called with the address space lock and a pin on all coremap entries
 * that the process owns.
 */
void pt_destroy(struct pt_ent **pt){
	int i, j;
	paddr_t pa;

	for (i=0; i<PAGE_ENTRIES; i++){
		if (pt[i] != NULL) {
			for (j=0; j<PAGE_ENTRIES; j++) {
				if (pte_get_exists(&pt[i][j])) {
					// If the page exists, we should free the coremap entry
					if (pte_get_present(&pt[i][j])){
						pa = pte_get_location(&pt[i][j]) << 12;
						free_coremap_page(pa, false /* iskern */);
					}
					// Swapped out - just have to free disk index
					else {
						swapfile_free_index(pte_get_location(&pt[i][j]));
					}
				}
			}
			kfree(pt[i]);
		}
	}
	kfree(pt);
}

/*
 * Returns pointer to a page table entry if the address has been used before
 * or NULL if it has not.
 */
struct pt_ent *get_pt_entry(struct addrspace *as, vaddr_t va){
	int index = PT_PRIMARY_INDEX(va);

	KASSERT(index >= 0 && index < 1024);

	struct pt_ent *pt_dir = as->page_table[index];
	if (pt_dir != NULL){
		return &pt_dir[PT_SECONDARY_INDEX(va)];
	}
	return NULL;
}

paddr_t va_to_pa(struct addrspace *as, vaddr_t va){
	struct pt_ent *pte = get_pt_entry(as,va);
	if (pte == NULL)
		return INVALID_PADDR;
	if (!pte_get_present(pte) || !pte_get_exists(pte))
		return INVALID_PADDR;
	paddr_t pa = (paddr_t)(pte_get_location(pte) << 12) + ADDRESS_OFFSET(va);
	return pa;
}

/*
 * Creates new page table entry for given virtual/physical mapping and permissions
 * Fails if the entry already exists
 */
int pt_insert(struct addrspace *as, vaddr_t va, int ppn, int permissions){
	int i;

	KASSERT(as != NULL);
	KASSERT((ppn & 0xfff00000) == 0);
	KASSERT(permissions >= 0 && permissions <= 7);

	// If a secondary page table does not exist, allocate one
	if (as->page_table[PT_PRIMARY_INDEX(va)] == NULL) {
		as->page_table[PT_PRIMARY_INDEX(va)] = kmalloc(PAGE_SIZE);

		if (as->page_table[PT_PRIMARY_INDEX(va)] == NULL)
			return ENOMEM;

		// Initalizes all entries in the allocated page table to zero
		for (i=0; i<PAGE_ENTRIES; i++){
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
	KASSERT(as != NULL);
	struct pt_ent *pte = get_pt_entry(as,va);
	if (pte == NULL)
		return -1;
	pte_set_exists(pte,0);
	return 0;
}

/*
 * Updates page table entry for the virtual address to map to a new location with new permissions
 * and 'present' status.  
 * Used for updating pages as they are swapped in or out
 */
int pt_update(struct addrspace *as, vaddr_t va, int ppn, int permissions, unsigned is_present){
	struct pt_ent *pte = get_pt_entry(as,va);
	if (pte == NULL)
		return -1;

	pte_set_location(pte,ppn);
	pte_set_present(pte,is_present);
	pte_set_exists(pte,1);
	pte_set_permissions(pte,permissions);

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
unsigned pte_get_present(struct pt_ent *pte){
	return pte->present;
}
void pte_set_present(struct pt_ent *pte, unsigned present){
	pte->present = (present > 0);
}
unsigned pte_get_exists(struct pt_ent *pte){
	return pte->exists;
}
void pte_set_exists(struct pt_ent *pte, unsigned exists){
	pte->exists = (exists > 0);
}
