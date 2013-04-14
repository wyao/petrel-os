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
#include <uio.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/*
 * Wrap rma_stealmem in a spinlock.
 */

void
vm_bootstrap(void)
{
	coremap_bootstrap();
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	struct addrspace *as;
	uint32_t ehi, elo, pa;
	int tlbindex, ret, spl;
	int permissions = VM_READ + VM_WRITE;
	bool valid = false;

	faultaddress &= PAGE_FRAME; // Page align
	KASSERT(faultaddress < MIPS_KSEG0);

	as = curthread->t_addrspace;
	if (as == NULL)
		return EFAULT;

	// Validate faultaddress
	if (faultaddress >= as->heap_start && faultaddress <= as->heap_end) {
		valid = true; // In heap
	}
	else if (faultaddress >= USERSTACK - PAGE_SIZE * STACK_PAGES) {
		valid = true; // In stack or kernel memory
	}
	else {
		permissions = as_get_permissions(as,faultaddress);
		valid = permissions >= 0;
	}
	if (!valid) {
		return EFAULT;
	}

	struct pt_ent *pte = get_pt_entry(as,faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
	    // Check permissions - are we allowed to write? 
	    // (either by permission or if we are in the middle of loading)
	    KASSERT(pte != NULL);
		if (!(pte_get_permissions(pte) & VM_WRITE) && !as->is_loading)
			return EFAULT;

		// If so, mark TLB and coremap entries dirty then return
		paddr_t pa = (pte_get_location(pte)<<12);

		cme_set_state(cm_get_index(pa),CME_DIRTY);

		elo = (pa & TLBLO_PPAGE) | TLBLO_DIRTY | TLBLO_VALID;
		ehi = faultaddress & TLBHI_VPAGE;

		spl = splhigh();

		tlbindex = tlb_probe(faultaddress,0);
		if (tlbindex < -1) {
			tlb_random(ehi, elo);
		}
		else {
			tlb_write(ehi, elo, tlbindex);
		}
		cme_set_use(cm_get_index(pa), 1);
		splx(spl);

		return 0;

	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	lock_acquire(as->pt_lock);
	if (pte == NULL || !pte_get_exists(pte)) {
		// First time accessing page
		paddr_t new = alloc_one_page(curthread->t_addrspace,faultaddress);

		if (new == 0) {
			lock_release(as->pt_lock);
			return ENOMEM;
		}

		KASSERT(PADDR_IS_VALID(new));

		bzero((void *)PADDR_TO_KVADDR(new), PAGE_SIZE);
		
		ret = pt_insert(as,faultaddress,new>>12,permissions); // Should permissions be RW?
		if (ret) {
			lock_release(as->pt_lock);
			return ret;
		}

		// Give the coremap entry a new offset
		cme_set_offset(cm_get_index(new),swapfile_reserve_index());
		cme_set_busy(cm_get_index(new),0);
	}
	else { // Page exists either in memory or in swap
		if (pte_get_present(pte)){
			pa = (uint32_t)(pte_get_location(pte)<<12);
			ret = PADDR_IS_VALID(pa);
			if (!ret)
				KASSERT(0);

			// TODO prob and actually write to random index
			ehi = faultaddress & TLBHI_VPAGE;
			elo = (pa & TLBLO_PPAGE) | TLBLO_VALID;

			spl = splhigh();
			tlb_random(ehi, elo);
			cme_set_use(cm_get_index(pa), 1);
			splx(spl);
		}
		else {
			// Page is in swap space
			paddr_t new = alloc_one_page(curthread->t_addrspace,faultaddress);
			ret = swapin(as,faultaddress,new);

			KASSERT(!ret);
			cme_set_busy(cm_get_index(new),0);
		}
	}
	lock_release(as->pt_lock);

	return 0;
}

