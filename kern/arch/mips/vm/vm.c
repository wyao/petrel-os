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

#define DUMBVM_STACKPAGES    22//12

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
	#if USE_DUMBVM
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	as = curthread->t_addrspace;
	if (as == NULL) {
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;

	#else

	struct addrspace *as;
	uint32_t ehi, elo;
	int tlbindex, permissions, ret;
	uint32_t pa;

	faultaddress &= PAGE_FRAME; // Page align
	KASSERT(faultaddress < MIPS_KSEG0);

	as = curthread->t_addrspace;
	if (as == NULL)
		return EFAULT;

	// TODO: ensure that faultaddress falls within a region, heap, or stack

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
		tlbindex = tlb_probe(faultaddress,0);
		if (tlbindex < -1) {
			tlb_random(ehi, elo);
		}
		else {
			tlb_write(ehi, elo, tlbindex);
		}
		return 0;

	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	lock_acquire(as->pt_lock);
	// Page exists
	if (pte != NULL){
		if (pte_get_present(pte)){
			pa = (uint32_t)(pte_get_location(pte)<<12);
			KASSERT(PADDR_IS_VALID(pa));

			// TODO prob and actually write to random index
			ehi = faultaddress & TLBHI_VPAGE;
			elo = (pa & TLBLO_PPAGE) | TLBLO_VALID;
			tlb_random(ehi, elo);
		}
		else {
			// Page not present, but may exist (ie in swap)
			permissions = 7;//as_get_permissions(as,faultaddress);

			// Virtual address did not fall within a defined region
			if (permissions < 0){
				lock_release(as->pt_lock);
				return EFAULT;
			}

			pa = (uint32_t) (pte_get_location(pte)); // No shift here
			ret = pt_insert(as,faultaddress,pa,permissions);
			if (ret) {
				lock_release(as->pt_lock);
				return ret;
			}

			// Page is in swap space (TODO)
		}
	}
	else {
		/* First time accessing page table entry in directory.
		 * Find new page and zero it.
		 */
		paddr_t new = alloc_one_page(curthread,faultaddress);

		KASSERT(PADDR_IS_VALID(new));

		bzero((void *)PADDR_TO_KVADDR(new), PAGE_SIZE);

		permissions = 7;//as_get_permissions(as,faultaddress);
		
		// Virtual address did not fall within a defined region
		if (permissions < 0){
			lock_release(as->pt_lock);
			return EFAULT;
		}
		
		ret = pt_insert(as,faultaddress,new>>12,permissions); // Should permissions be RW?
		if (ret) {
			lock_release(as->pt_lock);
			return ret;
		}
		cme_set_busy(cm_get_index(new),0);
	}
	lock_release(as->pt_lock);

	return 0;
	#endif
}

