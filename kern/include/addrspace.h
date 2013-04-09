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

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

/*
 * Address space structure and operations.
 */


#include <vm.h>
#include "opt-dumbvm.h"
#include <array.h>

#define USE_DUMBVM 0
#define PT_TO_VADDR(i,j) (vaddr_t)((i << 24)+(j << 12))

#define VM_READ 1
#define VM_WRITE 2
#define VM_EXEC 4

#define MAX_REGIONS 10

struct vnode;


struct pt_ent {
	int page_paddr_base:20; // This field will contain disk offset if page not present
	int permissions:10;
	unsigned present:1;
	unsigned exists:1;
};


int pte_get_location(struct pt_ent *pte);
void pte_set_location(struct pt_ent *pte, int location);
int pte_get_permissions(struct pt_ent *pte);
void pte_set_permissions(struct pt_ent *pte, int permissions);
unsigned pte_get_present(struct pt_ent *pte);
void pte_set_present(struct pt_ent *pte, unsigned present);
unsigned pte_get_exists(struct pt_ent *pte);
void pte_set_exists(struct pt_ent *pte, unsigned exists);


/*
 * Page table helper methods
 *
 *    pt_create - allocates a first-level page table and returns it
 *    pt_destroy - frees primary page table and all non-NULL secondary ones
 *    get_pt_entry - returns the page table entry for the given VA/AS combo or NULL if doesn't exist
 *    va_to_pa - returns the PPN + offset corresponding to the given VA, if a mapping exists, or NOMAP otherwise
 *    pt_insert - creates a pte for the given mapping, allocating secondary page table if necessary.  If the
 *                mapping already exists, does nothing.  Returns 0 on success.
 *    pt_update - updates location of an existing entry and AND old permissions with new.  Returns 0 on success.
 */

struct pt_ent **pt_create(void);
void pt_destroy(struct pt_ent **pt);
struct pt_ent *get_pt_entry(struct addrspace *as, vaddr_t va);
paddr_t va_to_pa(struct addrspace *as, vaddr_t va); //TODO: CREATE IF DOESN'T EXIST
int pt_insert(struct addrspace *as, vaddr_t va, int ppn, int permissions);
int pt_remove(struct addrspace *as, vaddr_t va);
int pt_update(struct addrspace *as, vaddr_t va, 
	int ppn, int permissions, unsigned is_present);


/*
 * Address space - data structure associated with the virtual memory
 * space of a process.
 *
 * You write this.
 */

struct region {
	vaddr_t base;
	size_t sz;
	int readable;
	int writeable;
	int executable;
};

struct addrspace {
#if USE_DUMBVM
	// DumbVM fields
	vaddr_t as_vbase1;
	paddr_t as_pbase1;
	size_t as_npages1;
	vaddr_t as_vbase2;
	paddr_t as_pbase2;
	size_t as_npages2;
	paddr_t as_stackpbase;
#else
	// ASST3 Fields
	struct lock *pt_lock;
	struct pt_ent **page_table;
	// Heap pointers
	vaddr_t heap_start;
	vaddr_t heap_end;
	struct array *regions;
	bool is_loading;

#endif
};

/*
 * Functions in addrspace.c:
 *
 *    as_create - create a new empty address space. You need to make
 *                sure this gets called in all the right places. You
 *                may find you want to change the argument list. May
 *                return NULL on out-of-memory error.
 *
 *    as_copy   - create a new address space that is an exact copy of
 *                an old one. Probably calls as_create to get a new
 *                empty address space and fill it in, but that's up to
 *                you.
 *
 *    as_activate - make the specified address space the one currently
 *                "seen" by the processor. Argument might be NULL,
 *                meaning "no particular address space".
 *
 *    as_destroy - dispose of an address space. You may need to change
 *                the way this works if implementing user-level threads.
 *
 *    as_define_region - set up a region of memory within the address
 *                space.
 *
 *    as_prepare_load - this is called before actually loading from an
 *                executable into the address space.
 *
 *    as_complete_load - this is called when loading from an executable
 *                is complete.
 *
 *    as_define_stack - set up the stack region in the address space.
 *                (Normally called *after* as_complete_load().) Hands
 *                back the initial stack pointer for the new process.
 */

struct addrspace *as_create(void);
int               as_copy(struct addrspace *src, struct addrspace **ret);
void              as_activate(struct addrspace *);
void              as_destroy(struct addrspace *);

int               as_define_region(struct addrspace *as,
                                   vaddr_t vaddr, size_t sz,
                                   int readable,
                                   int writeable,
                                   int executable);
int               as_prepare_load(struct addrspace *as);
int               as_complete_load(struct addrspace *as);
int               as_define_stack(struct addrspace *as, vaddr_t *initstackptr);

int as_get_permissions(struct addrspace *as, vaddr_t va);


/*
 * Functions in loadelf.c
 *    load_elf - load an ELF user program executable into the current
 *               address space. Returns the entry point (initial PC)
 *               in the space pointed to by ENTRYPOINT.
 */

int load_elf(struct vnode *v, vaddr_t *entrypoint);


#endif /* _ADDRSPACE_H_ */
