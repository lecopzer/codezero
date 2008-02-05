/*
 * Copyright (C) 2007 Bahadir Balban
 */
#include <l4/lib/printk.h>
#include <l4/lib/mutex.h>
#include <l4/lib/string.h>
#include <l4/generic/scheduler.h>
#include <l4/generic/space.h>
#include INC_SUBARCH(mm.h)
#include INC_SUBARCH(mmu_ops.h)
#include INC_GLUE(memory.h)
#include INC_PLAT(printascii.h)
#include INC_GLUE(memlayout.h)
#include INC_ARCH(linker.h)
#include INC_ARCH(asm.h)

/*
 * These are indices into arrays with pgd_t or pmd_t sized elements,
 * therefore the index must be divided by appropriate element size
 */
#define PGD_INDEX(x)		(((((unsigned long)(x)) >> 18) & 0x3FFC) / sizeof(pgd_t))
/* Strip out the page offset in this megabyte from a total of 256 pages. */
#define PMD_INDEX(x)		(((((unsigned long)(x)) >> 10) & 0x3FC) / sizeof (pmd_t))

/*
 * Removes initial mappings needed for transition to virtual memory.
 * Used one-time only.
 */
void remove_section_mapping(unsigned long vaddr)
{
	pgd_table_t *pgd = current->pgd;
	pgd_t pgd_i = PGD_INDEX(vaddr);
	if (!((pgd->entry[pgd_i] & PGD_TYPE_MASK)
	      & PGD_TYPE_SECTION))
		while(1);
	pgd->entry[pgd_i] = 0;
	pgd->entry[pgd_i] |= PGD_TYPE_FAULT;
	arm_invalidate_tlb();
}

/*
 * Maps given section-aligned @paddr to @vaddr using enough number
 * of section-units to fulfill @size in sections. Note this overwrites
 * a mapping if same virtual address was already mapped.
 */
void __add_section_mapping_init(unsigned int paddr,
				unsigned int vaddr,
				unsigned int size,
				unsigned int flags)
{
	pte_t *ppte;
	unsigned int l1_ptab;
	unsigned int l1_offset;

	/* 1st level page table address */
	l1_ptab = virt_to_phys(&kspace);

	/* Get the section offset for this vaddr */
	l1_offset = (vaddr >> 18) & 0x3FFC;

	/* The beginning entry for mapping */
	ppte = (unsigned int *)(l1_ptab + l1_offset);
	for(int i = 0; i < size; i++) {
		*ppte = 0;			/* Clear out old value */
		*ppte |= paddr;			/* Assign physical address */
		*ppte |= PGD_TYPE_SECTION;	/* Assign translation type */
		/* Domain is 0, therefore no writes. */
		/* Only kernel access allowed */
		*ppte |= (SVC_RW_USR_NONE << SECTION_AP0);
		/* Cacheability/Bufferability flags */
		*ppte |= flags;
		ppte++;				/* Next section entry */
		paddr += ARM_SECTION_SIZE;	/* Next physical section */
	}
	return;
}

void add_section_mapping_init(unsigned int paddr, unsigned int vaddr,
			      unsigned int size, unsigned int flags)
{
	unsigned int psection;
	unsigned int vsection;

	/* Align each address to the pages they reside in */
	psection = paddr & ~ARM_SECTION_MASK;
	vsection = vaddr & ~ARM_SECTION_MASK;

	if(size == 0)
		return;

	__add_section_mapping_init(psection, vsection, size, flags);

	return;
}

/* TODO: Make sure to flush tlb entry and caches */
void __add_mapping(unsigned int paddr, unsigned int vaddr,
		   unsigned int flags, pmd_table_t *pmd)
{
	unsigned int pmd_i = PMD_INDEX(vaddr);
	pmd->entry[pmd_i] = paddr;
	pmd->entry[pmd_i] |= PMD_TYPE_SMALL;	   /* Small page type */
	pmd->entry[pmd_i] |= flags;

	/* TODO: Is both required? Investigate */

	/* TEST:
	 * I think cleaning or invalidating the cache is not required,
	 * because the entries in the cache aren't for the new mapping anyway.
	 * It's required if a mapping is removed, but not when newly added.
	 */
	arm_clean_invalidate_cache();

	/* TEST: tlb must be flushed because a new mapping is present in page
	 * tables, and tlb is inconsistent with the page tables */
	arm_invalidate_tlb();
}

/* Return whether a pmd associated with @vaddr is mapped on a pgd or not. */
pmd_table_t *pmd_exists(pgd_table_t *pgd, unsigned long vaddr)
{
	unsigned int pgd_i = PGD_INDEX(vaddr);

	/* Return true if non-zero pgd entry */
	switch (pgd->entry[pgd_i] & PGD_TYPE_MASK) {
		case PGD_TYPE_COARSE:
			return (pmd_table_t *)
			       phys_to_virt((pgd->entry[pgd_i] &
					    PGD_COARSE_ALIGN_MASK));
			break;

		case PGD_TYPE_FAULT:
			return 0;
			break;

		case PGD_TYPE_SECTION:
			dprintk("Warning, a section is already mapped "
				"where a coarse page mapping is attempted:",
				(u32)(pgd->entry[pgd_i]
				      & PGD_SECTION_ALIGN_MASK));
				BUG();
			break;

		case PGD_TYPE_FINE:
			dprintk("Warning, a fine page table is already mapped "
				"where a coarse page mapping is attempted:",
				(u32)(pgd->entry[pgd_i]
				      & PGD_FINE_ALIGN_MASK));
			printk("Fine tables are unsupported. ");
			printk("What is this doing here?");
			BUG();
			break;

		default:
			dprintk("Unrecognised pmd type @ pgd index:", pgd_i);
			BUG();
			break;
	}
	return 0;
}

/* Convert a virtual address to a pte if it exists in the page tables. */
pte_t virt_to_pte_from_pgd(unsigned long virtual, pgd_table_t *pgd)
{
	pmd_table_t *pmd = pmd_exists(pgd, virtual);

	if (pmd)
		return (pte_t)pmd->entry[PMD_INDEX(virtual)];
	else
		return (pte_t)0;
}

/* Convert a virtual address to a pte if it exists in the page tables. */
pte_t virt_to_pte(unsigned long virtual)
{
	return virt_to_pte_from_pgd(virtual, current->pgd);
}

void attach_pmd(pgd_table_t *pgd, pmd_table_t *pmd, unsigned int vaddr)
{
	u32 pgd_i = PGD_INDEX(vaddr);
	u32 pmd_phys = virt_to_phys(pmd);

	/* Domain is 0, therefore no writes. */
	pgd->entry[pgd_i] = (pgd_t)pmd_phys;
	pgd->entry[pgd_i] |= PGD_TYPE_COARSE;
}

/*
 * Maps @paddr to @vaddr, covering @size bytes also allocates new pmd if
 * necessary. This flavor explicitly supplies the pgd to modify. This is useful
 * when modifying userspace of processes that are not currently running. (Only
 * makes sense for userspace mappings since kernel mappings are common.)
 */
void add_mapping_pgd(unsigned int paddr, unsigned int vaddr,
		     unsigned int size, unsigned int flags,
		     pgd_table_t *pgd)
{
	pmd_table_t *pmd;
	unsigned int numpages = (size >> PAGE_BITS);

	if (size < PAGE_SIZE) {
		printascii("Error: Mapping size must be in bytes not pages.\n");
		while(1);
	}
	if (size & PAGE_MASK)
		numpages++;

	/* Convert generic map flags to pagetable-specific */
	BUG_ON(!(flags = space_flags_to_ptflags(flags)));

	/* Map all consecutive pages that cover given size */
	for (int i = 0; i < numpages; i++) {
		/* Check if another mapping already has a pmd attached. */
		pmd = pmd_exists(pgd, vaddr);
		if (!pmd) {
			/*
			 * If this is the first vaddr in
			 * this pmd, allocate new pmd
			 */
			pmd = alloc_pmd();

			/* Attach pmd to its entry in pgd */
			attach_pmd(pgd, pmd, vaddr);
		}

		/* Attach paddr to this pmd */
		__add_mapping(page_align(paddr),
			      page_align(vaddr), flags, pmd);

		/* Go to the next page to be mapped */
		paddr += PAGE_SIZE;
		vaddr += PAGE_SIZE;
	}
}

void add_mapping(unsigned int paddr, unsigned int vaddr,
		 unsigned int size, unsigned int flags)
{
	add_mapping_pgd(paddr, vaddr, size, flags, current->pgd);
}

/*
 * Checks if a virtual address range has same or more permissive
 * flags than the given ones, returns 0 if not, and 1 if OK.
 */
int check_mapping_pgd(unsigned long vaddr, unsigned long size,
		      unsigned int flags, pgd_table_t *pgd)
{
	unsigned int npages = __pfn(align_up(size, PAGE_SIZE));
	pte_t pte;

	/* Convert generic map flags to pagetable-specific */
	BUG_ON(!(flags = space_flags_to_ptflags(flags)));
	
	for (int i = 0; i < npages; i++) {
		pte = virt_to_pte(vaddr + i * PAGE_SIZE);

		/* Check if pte perms are equal or gt given flags */
		if ((pte & PTE_PROT_MASK) >= (flags & PTE_PROT_MASK))
			continue;
		else
			return 0;
	}

	return 1;
}


int check_mapping(unsigned long vaddr, unsigned long size,
		  unsigned int flags)
{
	return check_mapping_pgd(vaddr, size, flags, current->pgd);
}

/* FIXME: Empty PMDs should be returned here !!! */
void __remove_mapping(pmd_table_t *pmd, unsigned long vaddr)
{
	pmd_t pmd_i = PMD_INDEX(vaddr);

	switch (pmd->entry[pmd_i] & PMD_TYPE_MASK) {
		case PMD_TYPE_LARGE:
			pmd->entry[pmd_i] = 0;
			pmd->entry[pmd_i] |= PMD_TYPE_FAULT;
			break;
		case PMD_TYPE_SMALL:
			pmd->entry[pmd_i] = 0;
			pmd->entry[pmd_i] |= PMD_TYPE_FAULT;
			break;
		default:
			printk("Unknown page mapping in pmd. Assuming bug.\n");
			BUG();
	}
	return;
}

void remove_mapping_pgd(unsigned long vaddr, pgd_table_t *pgd)
{
	pgd_t pgd_i = PGD_INDEX(vaddr);
	pmd_table_t *pmd;
	pmd_t pmd_i;

	/*
	 * Clean the cache to main memory before removing the mapping. Otherwise
	 * entries in the cache for this mapping will cause tranlation faults
	 * if they're cleaned to main memory after the mapping is removed.
	 */
	arm_clean_invalidate_cache();

	/* TEST:
	 * Can't think of a valid reason to flush tlbs here, but keeping it just
	 * to be safe. REMOVE: Remove it if it's unnecessary.
	 */
	arm_invalidate_tlb();

	/* Return true if non-zero pgd entry */
	switch (pgd->entry[pgd_i] & PGD_TYPE_MASK) {
		case PGD_TYPE_COARSE:
			// printk("Removing coarse mapping @ 0x%x\n", vaddr);
			pmd = (pmd_table_t *)
			      phys_to_virt((pgd->entry[pgd_i]
					   & PGD_COARSE_ALIGN_MASK));
			pmd_i = PMD_INDEX(vaddr);
			__remove_mapping(pmd, vaddr);
			break;

		case PGD_TYPE_FAULT:
			dprintk("Attempting to remove fault mapping. "
				"Assuming bug.\n", vaddr);
			BUG();
			break;

		case PGD_TYPE_SECTION:
				printk("Removing section mapping for 0x%lx",
				       vaddr);
				pgd->entry[pgd_i] = 0;
				pgd->entry[pgd_i] |= PGD_TYPE_FAULT;
			break;

		case PGD_TYPE_FINE:
			printk("Table mapped is a fine page table.\n"
			       "Fine tables are unsupported. Assuming bug.\n");
			BUG();
			break;

		default:
			dprintk("Unrecognised pmd type @ pgd index:", pgd_i);
			printk("Assuming bug.\n");
			BUG();
			break;
	}
	/* The tlb must be invalidated here because it might have cached the
	 * old translation for this mapping. */
	arm_invalidate_tlb();
}

void remove_mapping(unsigned long vaddr)
{
	remove_mapping_pgd(vaddr, current->pgd);
}


extern pmd_table_t *pmd_array;

/*
 * Moves the section mapped kspace that resides far apart from kernel as close
 * as possible to the kernel image, and unmaps the old 1MB kspace section which
 * is really largely unused.
 */
void relocate_page_tables(void)
{
	/* Adjust the end of kernel address to page table alignment. */
	unsigned long pt_new = align_up(_end_kernel, sizeof(pgd_table_t));
	unsigned long reloc_offset = (unsigned long)_start_kspace - pt_new;
	unsigned long pt_area_size = (unsigned long)_end_kspace -
				     (unsigned long)_start_kspace;

	BUG_ON(reloc_offset & (SZ_1K - 1))

	/* Map the new page table area into the current pgd table */
	add_mapping(virt_to_phys(pt_new), pt_new, pt_area_size,
		    MAP_IO_DEFAULT_FLAGS);

	/* Copy the entire kspace area, i.e. the pgd + static pmds. */
	memcpy((void *)pt_new, _start_kspace, pt_area_size);

	/* Update the only reference to current pgd table */
	current->pgd = (pgd_table_t *)pt_new;

	/*
	 * Since pmd's are also moved, update the pmd references in pgd by
	 * subtracting the relocation offset from each valid pmd entry.
	 * TODO: This would be best done within a helper function.
	 */
	for (int i = 0; i < PGD_ENTRY_TOTAL; i++)
		/* If there's a coarse 2nd level entry */
		if ((current->pgd->entry[i] & PGD_TYPE_MASK)
		    == PGD_TYPE_COARSE)
			current->pgd->entry[i] -= reloc_offset;

	/* Update the pmd array pointer. */
	pmd_array = (pmd_table_t *)((unsigned long)_start_pmd - reloc_offset);

	/* Switch the virtual memory system into new area */
	arm_clean_invalidate_cache();
	arm_drain_writebuffer();
	arm_invalidate_tlb();
	arm_set_ttb(virt_to_phys(current->pgd));
	arm_invalidate_tlb();

	/* Unmap the old page table area */
	remove_section_mapping((unsigned long)&kspace);

	/* Update the page table markers to the new area. Any references would
	 * go to these markers. */
	__pt_start = pt_new;
	__pt_end = pt_new + pt_area_size;

	printk("Initial page table area relocated from phys 0x%x to 0x%x\n",
	       virt_to_phys(&kspace), virt_to_phys(current->pgd));
}

/*
 * Useful for upgrading to page-grained control over a section mapping:
 * Remaps a section mapping in pages. It allocates a pmd, (at all times because
 * there can't really be an already existing pmd for a section mapping) fills
 * in the page information, and replaces the direct section physical translation
 * with the address of the pmd. Flushes the caches/tlbs.
 */
void remap_as_pages(void *vstart, void *vend)
{
	unsigned long pstart = virt_to_phys(vstart);
	unsigned long pend = virt_to_phys(vend);
	unsigned long paddr = pstart;
	pgd_t pgd_i = PGD_INDEX(vstart);
	pmd_t pmd_i = PMD_INDEX(vstart);
	pgd_table_t *pgd = (pgd_table_t *)current->pgd;
	pmd_table_t *pmd = alloc_pmd();
	u32 pmd_phys = virt_to_phys(pmd);
	int numpages = __pfn(page_align_up(pend) - pstart);

	BUG_ON((unsigned long)vstart & ARM_SECTION_MASK);
	BUG_ON(pmd_i);

	/* Fill in the pmd first */
	while (pmd_i < numpages) {
		pmd->entry[pmd_i] = paddr;
		pmd->entry[pmd_i] |= PMD_TYPE_SMALL; /* Small page type */
		pmd->entry[pmd_i] |= space_flags_to_ptflags(MAP_SVC_DEFAULT_FLAGS);
		paddr += PAGE_SIZE;
		pmd_i++;
	}

	/* Fill in the type to produce a complete pmd translator information */
	pmd_phys |= PGD_TYPE_COARSE;

	/* Make sure memory is coherent first. */
	arm_clean_invalidate_cache();
	arm_invalidate_tlb();

	/* Replace the direct section physical address with pmd's address */
	pgd->entry[pgd_i] = (pgd_t)pmd_phys;
	printk("Kernel area 0x%lx - 0x%lx remapped as %d pages\n",
	       (unsigned long)vstart, (unsigned long)vend, numpages);
}

void copy_pgds_by_vrange(pgd_table_t *to, pgd_table_t *from,
			 unsigned long start, unsigned long end)
{
	unsigned long start_i = PGD_INDEX(start);
	unsigned long end_i =  PGD_INDEX(end);
	unsigned long irange = (end_i != 0) ? (end_i - start_i)
			       : (PGD_ENTRY_TOTAL - start_i);

	memcpy(&to->entry[start_i], &from->entry[start_i],
	       irange * sizeof(pgd_t));
}
