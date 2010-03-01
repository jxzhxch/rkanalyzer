/*
	Changlog:
	2009.7.9		First Ver. Base Operations.
*/


#include "rk_main.h"
#include "rk_nx.h"
#include "list.h"
#include "mm.h"
#include "cpu_mmu.h"
#include "initfunc.h"
#include "string.h"
#include "asm.h"
#include "printf.h"
#include "current.h"
#include "constants.h"

#ifdef RK_ANALYZER

//#define NO_WR_SET

volatile struct os_dependent os_dep;
volatile bool rk_has_setup;
volatile bool is_debug_print;
static spinlock_t mmarea_lock;
static spinlock_t setup_lock;
static LIST1_DEFINE_HEAD (struct mm_protected_varange, list_varanges);
static LIST1_DEFINE_HEAD (struct mm_protected_page, list_pages);
static LIST1_DEFINE_HEAD (struct mm_protected_page_samegfns, list_delay_release_pages);

void toogle_debug_print(void)
{
	is_debug_print = (!(is_debug_print));
	if(is_debug_print){
		printf("[RKAnalyzer][DbgPrint ON]\n");
	}
	else{
		printf("[RKAnalyzer][DbgPrint OFF]\n");
	}
}

bool is_debug(void)
{
	return is_debug_print;
}

int dbgprint(const char *format, ...)
{
	va_list ap;
	int r = 0;
	
	if(is_debug_print){
		va_start(ap, format);
		r = vprintf(format, ap);
		va_end(ap);
	}
	
	return r;
}

bool rk_try_setup_global (os_dependent_setter os_dep_setter)
{
	spinlock_lock(&setup_lock);
	if(rk_has_setup){
		spinlock_unlock(&setup_lock);
		return false;
	}
	
	rk_has_setup = true;
	os_dep.dr_dispatcher = NULL;
	os_dep.va_kernel_start = MAX_VA;
	os_dep.unknown_code_check_dispatcher = NULL;
	os_dep.switch_print_dispatcher = NULL;
	
	//NB: Set OS Dependent values before doing other module(such as NX) setup
	os_dep_setter();
	rk_nx_try_setup_global();
	spinlock_unlock(&setup_lock);
	return true;
}

bool rk_try_setup_per_vcpu (void)
{
	struct rk_tf_state *p_rk_tf = current->vmctl.get_struct_rk_tf();
	
	spinlock_lock(&(p_rk_tf->init_lock));
	if(p_rk_tf->initialized){
		spinlock_unlock(&(p_rk_tf->init_lock));
		return false;
	}
	
	p_rk_tf->initialized = true;
	rk_nx_try_setup_per_vcpu();
	spinlock_unlock(&(p_rk_tf->init_lock));
	return true;
}

static void
rk_init_global (void)
{
	rk_has_setup = false;
	is_debug_print = true;
	spinlock_init(&mmarea_lock);
	spinlock_init(&setup_lock);
	LIST1_HEAD_INIT (list_pages);
	LIST1_HEAD_INIT (list_varanges);
	LIST1_HEAD_INIT (list_delay_release_pages);
	printf("MM Protect Area List Initialized...\n");
}

static void
rk_init_vcpu (void)
{
	struct rk_tf_state *rk_tf = current->vmctl.get_struct_rk_tf();
	rk_tf->initialized = false;
	rk_tf->disable_protect = false;
	rk_tf->nx_enable = false;
	spinlock_init(&(rk_tf->init_lock));
}

static void rk_mark_page_readonly_single(unsigned int spt_index, virt_t addr)
{
	u64 pte = 0;
	pmap_t m;
	
	if((spt_index < 0) || (spt_index >= NUM_OF_SPT))
		return;
	
	pmap_open_vmm (&m, current->spt_array[spt_index].cr3tbl_phys, current->spt_array[spt_index].levels);
	pmap_seek (&m, addr, 1);
	pte = pmap_read(&m);
	pte = pte & (~PTE_RW_BIT);
	
	if(pte & PTE_P_BIT)
		pmap_write (&m, pte, 0xFFF);
		
	pmap_close(&m);
}

static void rk_unmark_page_readonly_single(unsigned int spt_index, virt_t addr)
{
	u64 pte = 0;
	pmap_t m;
	
	if((spt_index < 0) || (spt_index >= NUM_OF_SPT))
		return;
	
	pmap_open_vmm (&m, current->spt_array[spt_index].cr3tbl_phys, current->spt_array[spt_index].levels);
	pmap_seek (&m, addr, 1);
	pte = pmap_read(&m);
	pte = pte | PTE_RW_BIT;
	
	if(pte & PTE_P_BIT)
		pmap_write (&m, pte, 0xFFF);
		
	pmap_close(&m);
}

static void rk_mark_page_readonly_batch(virt_t currentaddr){
	//rk_unmark_page_readonly_single(KERNEL_LEGAL_SPT, currentaddr);
	rk_mark_page_readonly_single(KERNEL_ILLEGAL_SPT, currentaddr);
#ifndef RK_ANALYZER_NO_USER_TRACE
	rk_mark_page_readonly_single(USER_SPT, currentaddr);
#endif
}

static void rk_unmark_page_readonly_batch(virt_t currentaddr){
	//rk_unmark_page_readonly_single(KERNEL_LEGAL_SPT, currentaddr);
	rk_unmark_page_readonly_single(KERNEL_ILLEGAL_SPT, currentaddr);
#ifndef RK_ANALYZER_NO_USER_TRACE
	rk_unmark_page_readonly_single(USER_SPT, currentaddr);
#endif
}

static struct mm_protected_page* rk_get_page_by_pfn(ulong pfn)
{
	struct mm_protected_page *page = NULL;
	
	LIST1_FOREACH (list_pages, page) {
		if(page->pfn == pfn)
		{
			return page;
		}
	}

	return NULL;
}

static struct mm_protected_page* get_page_by_gfns(u64 gfns, struct mm_protected_page* exclude_page)
{
	struct mm_protected_page *page = NULL;

	LIST1_FOREACH (list_pages, page) {
		if((!(page->never_paged_in_yet)) && (page->gfns == gfns) && (page != exclude_page))
		{
			return page;
		}
	}

	return NULL;
}

static struct mm_protected_page_samegfns_list* rk_get_page_samegfns_list_by_gfns(u64 gfns, struct mm_protected_page* exclude_page)
{
	struct mm_protected_page *page = get_page_by_gfns(gfns, exclude_page);
	
	if(page == NULL){
		return NULL;
	}

	return page->p_page_samegfns_list;
}

//return val: the page to be upgrade
static struct mm_protected_page_samegfns* rk_upgrade_to_original_page(ulong pfn)
{
	//Remove from derive list
	struct mm_protected_page *page = NULL;
	struct mm_protected_page_samegfns *page_samegfns = NULL;
	struct mm_protected_page_samegfns *page_samegfns_n = NULL;

	LIST1_FOREACH (list_pages, page) {
		if(!(page->never_paged_in_yet))
		{
			LIST1_FOREACH_DELETABLE (page->p_page_samegfns_list->pages_of_samegfns, page_samegfns, page_samegfns_n){
				if(page_samegfns->pfn == pfn){
					LIST1_DEL(page->p_page_samegfns_list->pages_of_samegfns, page_samegfns);
					return page_samegfns;
				}
			}
		}
	}

	return NULL;
}

static void rk_downgrade_to_derived_page(struct mm_protected_page *page)
{
	//Add to derive list
	struct mm_protected_page_samegfns *page_samegfns = alloc(sizeof(struct mm_protected_page_samegfns));

	page_samegfns->pfn = page->pfn;
	page_samegfns->page_wr_setbysystem = page->page_wr_setbysystem;
	page_samegfns->list_belong = page->p_page_samegfns_list;
	LIST1_ADD (page->p_page_samegfns_list->pages_of_samegfns, page_samegfns);
}

static void rk_scan_for_pages_and_add_by_gfns(u64 gfns, struct mm_protected_page_samegfns_list *page_samegfns_list)
{
	u64 pte = 0, pte_gfns = 0;
	virt_t currentaddr = 0;
	pmap_t m;
	struct mm_protected_page_samegfns *page_samegfns;

	if(page_samegfns_list == NULL){
		return;
	}

	pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
	currentaddr = 0;
	while(currentaddr < 0xFFFFFFFF){
		pmap_seek (&m, currentaddr, 1);
		pte = pmap_read(&m);
		if(pte & PTE_P_BIT){
			pte_gfns = (pte & PTE_ADDR_MASK64) >> PAGESIZE_SHIFT;
			if(pte_gfns == gfns){
				page_samegfns = alloc(sizeof(struct mm_protected_page_samegfns));
				LIST1_ADD(page_samegfns_list->pages_of_samegfns, page_samegfns);
				page_samegfns->pfn = (currentaddr >> PAGESIZE_SHIFT);
				page_samegfns->list_belong = page_samegfns_list;
				if((pte & PTE_RW_BIT) != 0){
					page_samegfns->page_wr_setbysystem = false;
				}else{
					page_samegfns->page_wr_setbysystem = true;
				}
				rk_mark_page_readonly_batch(currentaddr);
			}
		}

		currentaddr = currentaddr | ~((MAX_VA >> PAGESIZE_SHIFT) << PAGESIZE_SHIFT);
		if(currentaddr == 0xFFFFFFFF)
			break;

		currentaddr ++;
	}
	pmap_close(&m);
}

static bool rk_is_area_overlapped_with_current_ones(virt_t startaddr, virt_t endaddr, ulong *properties, bool display)
{
	struct mm_protected_varange *varange = NULL;
	
	LIST1_FOREACH (list_varanges, varange){
		if((startaddr >= varange->startaddr) && (startaddr <= varange->endaddr)){
			if(display){
				printf("[RKAnalyzer]Error Add Area[0x%lX 0x%lX]: Overlapped Memory Area[0x%lX 0x%lX]!\n", 
					startaddr, endaddr, varange->startaddr, varange->endaddr);
				printf("Caller: 0x%lX, New Caller: 0x%lX\n", varange->properties[0], (properties == NULL ? 0 : properties[0]) );
			}
			return true;
		}
		
		if((endaddr >= varange->startaddr) && (endaddr <= varange->endaddr)){
			if(display){
				printf("[RKAnalyzer]Error Add Area[0x%lX 0x%lX]: Overlapped Memory Area[0x%lX 0x%lX]!\n", 
					startaddr, endaddr, varange->startaddr, varange->endaddr);
				printf("Caller: 0x%lX, New Caller: 0x%lX\n", varange->properties[0], (properties == NULL ? 0 : properties[0]) );
			}
			return true;
		}
	}
	
	return false;
}

static bool rk_protect_mmarea_original_core(virt_t startaddr, virt_t endaddr, char* areatag, 
mmprotect_callback callback_func, ulong *properties, int properties_count)
{
	//Two Step:
	//(1) Add this area to list
	//(2) Set those pages which contain this area to WP = 0

	struct mm_protected_area *mmarea = NULL;
	struct mm_protected_page *page = NULL;
	struct mm_protected_varange *varange = NULL;
	struct mm_protected_page_samegfns_list* page_samegfns_list = NULL;
	struct mm_protected_page_samegfns *page_samegfns = NULL;

	int areataglen = 0;
	u64 pte = 0;
	virt_t currentaddr = 0, currentendaddr = 0, nextaddr = 0;
	pmap_t m;

	if(startaddr > endaddr){
		printf("[RKAnalyzer]Error Add Area: Invalid Parameter!\n");
		return false;
	}

	if(rk_is_area_overlapped_with_current_ones(startaddr, endaddr, properties, true)){
		return false;
	}

	// Create a New VA Range for original
	varange = alloc(sizeof(struct mm_protected_varange));
	LIST2_HEAD_INIT(varange->areas_in_varange, forvarange);
	varange->startaddr = startaddr;
	varange->endaddr = endaddr;
	varange->callback_func = callback_func;
	if(areatag){
		areataglen = (strlen(areatag) > AREA_TAG_MAXLEN ? AREA_TAG_MAXLEN : strlen(areatag));
		memcpy(varange->areatag, areatag, sizeof(char) * areataglen);
		varange->areatag[areataglen] = 0;
	}else{
		memset(varange->areatag, 0, AREA_TAG_MAXLEN + 1);
	}

	if((properties != NULL) && (properties_count > 0)){
		memcpy(varange->properties, properties, sizeof(ulong) * properties_count);
	}
	LIST1_ADD(list_varanges, varange);

	pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);

	currentaddr = startaddr;
	while(currentaddr <= endaddr){
		mmarea = alloc(sizeof(struct mm_protected_area));

		//TODO:Add Support for Large Pages(4M)
		//TODO:Scan for pages with same gfns

		//Search For Page to insert
		page = rk_get_page_by_pfn(currentaddr >> PAGESIZE_SHIFT);
		if(page == NULL)
		{
			//Create a New Page
			page = alloc(sizeof(struct mm_protected_page));
			LIST2_HEAD_INIT(page->areas_in_page, forpage);
			page->pfn = currentaddr >> PAGESIZE_SHIFT;
			page_samegfns = rk_upgrade_to_original_page(page->pfn);
			LIST1_ADD(list_pages, page);

			//Modify Page Table
			if(page_samegfns == NULL){
				pmap_seek (&m, currentaddr, 1);
				pte = pmap_read(&m);

				if(pte & PTE_P_BIT) {
					if((pte & PTE_RW_BIT) != 0){
						page->page_wr_setbysystem = false;
					}else{
						page->page_wr_setbysystem = true;
					}
					rk_mark_page_readonly_batch(currentaddr);

					page->gfns = (pte & PTE_ADDR_MASK64) >> PAGESIZE_SHIFT;
					page->never_paged_in_yet = false;
					//Remeber to exclude self, because self is not inited yet!
					page_samegfns_list = rk_get_page_samegfns_list_by_gfns(page->gfns, page);
					if(page_samegfns_list != NULL){
						page->p_page_samegfns_list = page_samegfns_list;
						page->p_page_samegfns_list->refcount ++;
					}
					else{
						//No same gfns original page exisiting. We need to scan the page table and find potential same gfns
						page->p_page_samegfns_list = alloc(sizeof(struct mm_protected_page_samegfns_list));
						page->p_page_samegfns_list->refcount = 1;
						page->p_page_samegfns_list->gfns = page->gfns;
						LIST1_HEAD_INIT(page->p_page_samegfns_list->pages_of_samegfns);
						
						//In fact, there is no need to scan for those "potential mapping" pages here
						//We can just flush the entire SPT, so when those pages are mapped, it will cause page fault and caught by us later!
					}
				}
				else {
					page->never_paged_in_yet = true;
					page->page_wr_setbysystem = true;
					page->gfns = MAX_VA;
					page->p_page_samegfns_list = NULL;
				}
			}
			else{
				page->page_wr_setbysystem = page_samegfns->page_wr_setbysystem;
				page->gfns = page_samegfns->list_belong->gfns;
				page->never_paged_in_yet = false;
				page->p_page_samegfns_list = page_samegfns->list_belong;
				page->p_page_samegfns_list->refcount ++;
				free(page_samegfns);
			}
		}

		currentendaddr = currentaddr | ~((MAX_VA >> PAGESIZE_SHIFT) << PAGESIZE_SHIFT);
		nextaddr = currentendaddr + 1;
		if(currentendaddr > endaddr){
			currentendaddr = endaddr;
		}

		//Set mmarea
		mmarea->startaddr = currentaddr;
		mmarea->endaddr = currentendaddr;
		mmarea->page = page;
		mmarea->varange = varange;

		LIST2_ADD (varange->areas_in_varange, forvarange, mmarea);
		LIST2_ADD (page->areas_in_page, forpage, mmarea);

		if(nextaddr == 0){
			break;
		}
		currentaddr = nextaddr;
	}
	
	pmap_close (&m);
	
	return true;
}

bool rk_protect_mmarea(virt_t startaddr, virt_t endaddr, char* areatag, 
mmprotect_callback callback_func, ulong *properties, int properties_count)
{
	bool ret = false;

	spinlock_lock(&mmarea_lock);
	ret =  rk_protect_mmarea_original_core(startaddr, endaddr, areatag, callback_func, properties, properties_count);
	spinlock_unlock(&mmarea_lock);

	return ret;
}

static void check_page_samegfns_list_for_delete(struct mm_protected_page_samegfns_list *list)
{
	struct mm_protected_page_samegfns *page_samegfns = NULL;
	struct mm_protected_page_samegfns *page_samegfns_n = NULL;
	u64 pte = 0;
	pmap_t m;

	if(list->refcount <= 0){
		pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
		LIST1_FOREACH_DELETABLE (list->pages_of_samegfns, page_samegfns, page_samegfns_n){
			LIST1_DEL(list->pages_of_samegfns, page_samegfns);
			pmap_seek (&m, page_samegfns->pfn << PAGESIZE_SHIFT, 1);
			pte = pmap_read(&m);
			if(pte & PTE_P_BIT){
				if(!(page_samegfns->page_wr_setbysystem)){
					//restore W/R bit
					rk_unmark_page_readonly_batch(page_samegfns->pfn << PAGESIZE_SHIFT);
				}
				free(page_samegfns);
			}
			else{
				LIST1_ADD(list_delay_release_pages, page_samegfns);
			}
		}
		pmap_close (&m);
	}
}

static void check_page_for_delete(struct mm_protected_page *page, bool forcepresent)
{
	u64 pte = 0;
	pmap_t m;

	if(LIST2_EMPTY(page->areas_in_page)){
		if(!(page->never_paged_in_yet)){
			pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
			pmap_seek (&m, page->pfn << PAGESIZE_SHIFT, 1);
			pte = pmap_read(&m);
			if((pte & PTE_P_BIT) || (forcepresent))  {
				if(pte & PTE_P_BIT){
					if(!(page->page_wr_setbysystem)){
						//restore W/R bit
						rk_unmark_page_readonly_batch(page->pfn << PAGESIZE_SHIFT);
					}
				}
				page->p_page_samegfns_list->refcount --;
				if(page->p_page_samegfns_list->refcount > 0){
					rk_downgrade_to_derived_page(page);
				}
				check_page_samegfns_list_for_delete(page->p_page_samegfns_list);
				LIST1_DEL(list_pages, page);
				free(page);
			}
			pmap_close (&m);
		}
		else{
			LIST1_DEL(list_pages, page);
			free(page);
		}
	}
}

static bool rk_unprotect_mmarea_core(virt_t startaddr, virt_t endaddr)
{
	// Delete All Original Areas in va belongs to [startaddr endaddr]
	// Also Remove derived areas

	struct mm_protected_area *mmarea = NULL;
	struct mm_protected_area *mmarea_n = NULL;
	struct mm_protected_varange *varange = NULL;
	struct mm_protected_varange *varange_n = NULL;
	bool varange_found = false;

	LIST1_FOREACH_DELETABLE (list_varanges, varange, varange_n){
		if((varange->startaddr == startaddr) || (varange->endaddr == endaddr))
		{
			varange_found = true;
			LIST1_DEL(list_varanges, varange);
			break;
		}
	}

	if(!varange_found){
		return false;
	}

	//TODO:Add Support for Large Pages(4M)

	//All areas in va range
	LIST2_FOREACH_DELETABLE (varange->areas_in_varange, forvarange, mmarea, mmarea_n){
		// Self in Page
		LIST2_DEL(mmarea->page->areas_in_page, forpage, mmarea);
		check_page_for_delete(mmarea->page, false);
		// Self in VA range
		LIST2_DEL(varange->areas_in_varange, forvarange, mmarea);
		free(mmarea);
	}

	free(varange);

	return true;
}


bool rk_unprotect_mmarea(virt_t startaddr, virt_t endaddr)
{
	bool ret = false;

	spinlock_lock(&mmarea_lock);
	ret =  rk_unprotect_mmarea_core(startaddr, endaddr);
	spinlock_unlock(&mmarea_lock);

	return ret;
}

// IF not original pages, then return derived page in ppage_samegfns
// else, ppage_samegfns = NULL
static struct mm_protected_page* get_page_by_addr_in_same_page(virt_t addr, struct mm_protected_page_samegfns **ppage_samegfns)
{
	struct mm_protected_page *page = NULL;
	struct mm_protected_page_samegfns *page_samegfns = NULL;

	LIST1_FOREACH (list_pages, page){
		if(page->pfn == (addr >> PAGESIZE_SHIFT)){
			return page;
		}

		if(!(page->never_paged_in_yet)){
			LIST1_FOREACH (page->p_page_samegfns_list->pages_of_samegfns, page_samegfns){
				if(page_samegfns->pfn == (addr >> PAGESIZE_SHIFT)){
					if(ppage_samegfns != NULL){
						*ppage_samegfns = page_samegfns;
						return NULL;
					}
				}
			}
		}
	}

	return NULL;
}

static void update_RW_for_page(struct mm_protected_page *page)
{
	u64 pte = 0;
	pmap_t m;

	pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
	pmap_seek (&m, (page->pfn << PAGESIZE_SHIFT), 1);
	pte = pmap_read(&m);
	if(pte & PTE_P_BIT) {
		if((pte & PTE_RW_BIT) != 0){
			page->page_wr_setbysystem = false;
		}
		rk_mark_page_readonly_batch(page->pfn << PAGESIZE_SHIFT);
	}
	else {
		//The page is not present now.
		//Should Never Happen
		printf("[RkAnalyzer]Strange Status. Page Should be Present. pfn = 0x%lX\n", page->pfn);
	}
	pmap_close(&m);
}

static void update_RW_for_page_samegfns(struct mm_protected_page_samegfns *page_samegfns)
{
	u64 pte = 0;
	pmap_t m;

	pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
	pmap_seek (&m, (page_samegfns->pfn << PAGESIZE_SHIFT), 1);
	pte = pmap_read(&m);
	if(pte & PTE_P_BIT) {
		if((pte & PTE_RW_BIT) != 0){
			page_samegfns->page_wr_setbysystem = false;
		}
		rk_mark_page_readonly_batch((page_samegfns->pfn << PAGESIZE_SHIFT));
	}
	else {
		//The page is not present now.
		//Should Never Happen
		printf("[RkAnalyzer]Strange Status. Page Should be Present. pfn = 0x%lX\n", page_samegfns->pfn);
	}
	pmap_close(&m);
}

static void try_delay_release(virt_t addr)
{
	struct mm_protected_page_samegfns *page_samegfns = NULL;
	struct mm_protected_page_samegfns *page_samegfns_n = NULL;
	u64 pte = 0;
	pmap_t m;

	LIST1_FOREACH_DELETABLE (list_delay_release_pages, page_samegfns, page_samegfns_n){
		if(page_samegfns->pfn == (addr >> PAGESIZE_SHIFT)){
			LIST1_DEL (list_delay_release_pages, page_samegfns);
			pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
			pmap_seek (&m, (page_samegfns->pfn << PAGESIZE_SHIFT), 1);
			pte = pmap_read(&m);
			if(pte & PTE_P_BIT) {
				if(!(page_samegfns->page_wr_setbysystem)){
					//restore W/R bit
					rk_unmark_page_readonly_batch(page_samegfns->pfn << PAGESIZE_SHIFT);
				}
				free(page_samegfns);
			}
			else {
				//The page is not present now.
				//Should Never Happen
				printf("[RkAnalyzer]Strange Status. Page Should be Present. pfn = 0x%lX\n", page_samegfns->pfn);
				free(page_samegfns);
			}
			pmap_close (&m);
			return;
		}
	}
}

void rk_manipulate_mmarea_if_need(virt_t newvirtaddr, u64 gfns){

	//TODO:Consider the condition that a Large Page(4M) contains a Small Page(4K)

	struct mm_protected_page *page = NULL;
	struct mm_protected_page *page_by_gfns = NULL;
	struct mm_protected_page_samegfns *page_samegfns = NULL;
	struct mm_protected_page_samegfns_list* page_samegfns_list = NULL;
	u64 pte = 0;
	pmap_t m;

	spinlock_lock(&mmarea_lock);

	// Always remember our target: Protect the physical pages which are mapped on some specific virtual pages
	// When this function is called, it means that the physical page of frame number [gfns] is being mapped to the virtual pages contain [newvirtaddr]
	// So, the potential threat here are the followings:
	// 1. If newvirtaddr belongs to the specific virtual pages we are protecting(original ones), and the physical pages is not the same as before,and 
	// it neither points to another original page, it means the page is remapped
	// KO: Remove pages in mm_protected_page_samegfns list, Change the gfns value of the page, and scan for potential pages with same gfns.
	// 2. If newvirtaddr belongs to the specific virtual pages we are protecting(original ones), and the physical pages is not the same as before,and 
	// it points to another original page
	// KO: Don't know how to KO it yet
	// 3. If newvirtaddr belongs to the specific virtual pages we are protecting(derived ones), and the physical pages is not the same as before, and 
	// it neither points to another original page.
	// KO: Simply kick the derived page out. It is of no use any more.
	// 4. If newvirtaddr belongs to the specific virtual pages we are protecting(derived ones), and the physical pages is not the same as before, and 
	// it points to another original page.
	// KO: kick the derived page out from the former list. Add it to its new host's list.
	// 5. If newvirtaddr belongs to the specific virtual pages we are protecting(original ones or derived ones), and the physical pages is the same as 
	// before, it means the page is paged in to memory
	// KO: Check the R/W flag of the PTE, set it if it's not correct
	// 6. If newvirtaddr does not belong to the specific virtual pages we are protecting(original ones or derived ones), 
	// and the physical pages is one of the pages we are protecting, it means a page map attack
	// KO: Just add the page to the list of same_gfns_pages of one original page
	// 7. If newvirtaddr does not belong to the specific virtual pages we are protecting(original ones or derived ones), 
	// and the physical pages is NOT one of the pages we are protecting.
	// KO: It's none of our business. Leave it there.

	try_delay_release(newvirtaddr);

	page_samegfns = NULL;
	page = get_page_by_addr_in_same_page(newvirtaddr, &page_samegfns);
	if(page != NULL){
		if((!(page->never_paged_in_yet)) && (page->gfns != gfns)){
			if((page_by_gfns = get_page_by_gfns(gfns, page)) == NULL){
				//Route 1
				page->p_page_samegfns_list->refcount --;
				check_page_samegfns_list_for_delete(page->p_page_samegfns_list);

				page->gfns = gfns;
				page->p_page_samegfns_list = alloc(sizeof(struct mm_protected_page_samegfns_list));
				page->p_page_samegfns_list->refcount = 1;
				page->p_page_samegfns_list->gfns = page->gfns;
				LIST1_HEAD_INIT(page->p_page_samegfns_list->pages_of_samegfns);
				rk_scan_for_pages_and_add_by_gfns(page->p_page_samegfns_list->gfns, page->p_page_samegfns_list);

				update_RW_for_page(page);
			}
			else{
				//Route 2
				page->gfns = gfns;
				page->p_page_samegfns_list = page_by_gfns->p_page_samegfns_list;
				page->p_page_samegfns_list->refcount ++;
				update_RW_for_page(page);
			}
		}
		else{
			//Route 5
			if(page->never_paged_in_yet){
				page->never_paged_in_yet = false;
				page->gfns = gfns;
				page_samegfns_list = rk_get_page_samegfns_list_by_gfns(page->gfns, page);
				if(page_samegfns_list != NULL){
					page->p_page_samegfns_list = page_samegfns_list;
					page->p_page_samegfns_list->refcount ++;
				}
				else{
					//No same gfns original page exisiting. We need to scan the page table and find potential same gfns
					page->p_page_samegfns_list = alloc(sizeof(struct mm_protected_page_samegfns_list));
					page->p_page_samegfns_list->refcount = 1;
					page->p_page_samegfns_list->gfns = page->gfns;
					LIST1_HEAD_INIT(page->p_page_samegfns_list->pages_of_samegfns);
					rk_scan_for_pages_and_add_by_gfns(page->p_page_samegfns_list->gfns, page->p_page_samegfns_list);
				}
			}
			update_RW_for_page(page);
			//Special: Check for empty page here for delete, as the deletion may have been delayed because of not present of the page.
			check_page_for_delete(page, true);
		}
	}
	else
	{
		if(page_samegfns != NULL)
		{
			if(page_samegfns->list_belong->gfns != gfns){
				if((page_by_gfns = get_page_by_gfns(gfns, NULL)) == NULL){
					//Route 3
					LIST1_DEL(page_samegfns->list_belong->pages_of_samegfns, page_samegfns);
					pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
					pmap_seek (&m, (page_samegfns->pfn << PAGESIZE_SHIFT), 1);
					pte = pmap_read(&m);
					if(pte & PTE_P_BIT) {
						if(!(page_samegfns->page_wr_setbysystem)){
							//restore W/R bit
							rk_unmark_page_readonly_batch((page_samegfns->pfn << PAGESIZE_SHIFT));
						}
						free(page_samegfns);
					}
					else {
						//The page is not present now.
						//Should Never Happen
						printf("[RkAnalyzer]Strange Status. Page Should be Present. Addr = 0x%lX\n", newvirtaddr);
						free(page_samegfns);
					}
					pmap_close (&m);
				}
				else{
					//Route 4
					LIST1_DEL(page_samegfns->list_belong->pages_of_samegfns, page_samegfns);
					LIST1_ADD(page_by_gfns->p_page_samegfns_list->pages_of_samegfns, page_samegfns);
					page_samegfns->list_belong = page_by_gfns->p_page_samegfns_list;

					update_RW_for_page_samegfns(page_samegfns);
				}
			}
			else{
				//Route 5
				update_RW_for_page_samegfns(page_samegfns);
			}
		}
		else{
			if((page_by_gfns = get_page_by_gfns(gfns, NULL)) != NULL){
				//Route 6
				page_samegfns = alloc(sizeof(struct mm_protected_page_samegfns));
				page_samegfns->pfn = (newvirtaddr >> PAGESIZE_SHIFT);
				page_samegfns->list_belong = page_by_gfns->p_page_samegfns_list;
				LIST1_ADD(page_by_gfns->p_page_samegfns_list->pages_of_samegfns, page_samegfns);

				pmap_open_vmm (&m, current->spt_array[current->current_spt_index].cr3tbl_phys, current->spt_array[current->current_spt_index].levels);
				pmap_seek (&m, (page_samegfns->pfn << PAGESIZE_SHIFT), 1);
				pte = pmap_read(&m);
				if(pte & PTE_P_BIT) {
					if((pte & PTE_RW_BIT) != 0){
						page_samegfns->page_wr_setbysystem = false;
					}else{
						page_samegfns->page_wr_setbysystem = true;
					}
					rk_mark_page_readonly_batch((page_samegfns->pfn << PAGESIZE_SHIFT));
				}
				else
				{
					//The page is not present now.
					//Should Never Happen
					printf("[RkAnalyzer]Strange Status. Page Should be Present. Addr = 0x%lX\n", newvirtaddr);
				}
				pmap_close (&m);
			}
			//Route 7
		}
	}

	spinlock_unlock(&mmarea_lock);
	
}

static enum rk_result rk_callfunc_if_addr_protected_core(virt_t virtaddr, bool display)
{

	//TODO:Add Support for Large Pages(4M)	

	struct mm_protected_area *mmarea = NULL;
	struct mm_protected_page *page = NULL;
	struct mm_protected_page_samegfns *page_samegfns = NULL;
	virt_t newstartaddr = 0, newendaddr = 0;
	enum rk_result ret = RK_UNPROTECTED_AREA;
	bool found = false;

	//TODO:Add Support for Large Pages(4M)
	
	LIST1_FOREACH (list_pages, page){
		if(page->pfn == virtaddr >> PAGESIZE_SHIFT){
			LIST2_FOREACH(page->areas_in_page, forpage, mmarea){
				if((virtaddr >= mmarea->startaddr) && (virtaddr <= mmarea->endaddr)){
					if(mmarea->varange->callback_func(mmarea, virtaddr, display)){
						if(!page->page_wr_setbysystem)
							return RK_PROTECTED;
						else
							return RK_PROTECTED_BYSYSTEM;
					}
				}
			}

			if(!page->page_wr_setbysystem)
				return RK_UNPROTECTED_IN_PROTECTED_AREA;
			else
				return RK_UNPROTECTED_IN_PROTECTED_AREA_BYSYSTEM;
		}
	}

	LIST1_FOREACH (list_pages, page){
		if(!(page->never_paged_in_yet)){
			LIST1_FOREACH(page->p_page_samegfns_list->pages_of_samegfns, page_samegfns){
				if(page_samegfns->pfn == virtaddr >> PAGESIZE_SHIFT){
					LIST2_FOREACH(page->areas_in_page, forpage, mmarea){
						newstartaddr = (page_samegfns->pfn << (PAGESIZE_SHIFT)) | 
										(mmarea->startaddr & ~((MAX_VA >> PAGESIZE_SHIFT) << PAGESIZE_SHIFT));
						newendaddr = (page_samegfns->pfn << (PAGESIZE_SHIFT)) | 
										(mmarea->endaddr & ~((MAX_VA >> PAGESIZE_SHIFT) << PAGESIZE_SHIFT));          
						if((virtaddr >= newstartaddr) && (virtaddr <= newendaddr)){
							if(mmarea->varange->callback_func(mmarea, virtaddr, display)){
								found = true;
								if(!page_samegfns->page_wr_setbysystem)
									ret = RK_PROTECTED;
								else
									ret = RK_PROTECTED_BYSYSTEM;
							}
						}
					}
					
					if(found){
						return ret;
					}

					if(!page_samegfns->page_wr_setbysystem)
						return RK_UNPROTECTED_IN_PROTECTED_AREA;
					else
						return RK_UNPROTECTED_IN_PROTECTED_AREA_BYSYSTEM;
				}
			}
		}
	}

	return RK_UNPROTECTED_AREA;
}

enum rk_result rk_callfunc_if_addr_protected(virt_t virtaddr, bool display)
{
	enum rk_result res = RK_UNPROTECTED_AREA;

	spinlock_lock(&mmarea_lock);
	res = rk_callfunc_if_addr_protected_core(virtaddr, display);
	spinlock_unlock(&mmarea_lock);

	return res;
}

void rk_entry_before_tf(void)
{
	ulong cr0toshadow = 0;	
	ulong val = 0;
	ulong ip = 0;
	int err = 0;
	struct rk_tf_state *p_rk_tf = current->vmctl.get_struct_rk_tf();

	asm_vmread (VMCS_GUEST_CR0, &cr0toshadow);
	cr0toshadow &= (~CR0_WP_BIT);
	asm_vmwrite (VMCS_GUEST_CR0, cr0toshadow);

	current->vmctl.read_ip(&ip);

	if(p_rk_tf->shouldreportvalue){
		if((err = read_linearaddr_l(p_rk_tf->addr, &val)) == VMMERR_SUCCESS){
			p_rk_tf->originalval = val;
		}else{
			p_rk_tf->originalval = 0xBAD0BEEF;
		}
		
		p_rk_tf->last_eip = ip;
	}
}

void rk_ret_from_tf(bool was_instruction_carried_out)
{
	ulong cr0 = 0;
	ulong ip = 0;
	ulong val = 0;
	int err = 0;
	struct rk_tf_state *p_rk_tf = current->vmctl.get_struct_rk_tf();

	current->vmctl.read_ip(&ip);
	
	if(p_rk_tf->shouldreportvalue){
		if(was_instruction_carried_out){
			if((err = read_linearaddr_l(p_rk_tf->addr, &val)) == VMMERR_SUCCESS){
			}else{
				val = 0xBAD0BEEF;
			}
			rk_callfunc_if_addr_protected(p_rk_tf->addr, true);
			printf("[CPU%d][RKAnalyzer][Report]LastEIP = 0x%lX, CurrentEIP = 0x%lX, Curernt.SPT = %d\n", get_cpu_id(), 
			p_rk_tf->last_eip, ip, current->current_spt_index);
			printf("[CPU%d][RKAnalyzer][Report]Addr = 0x%lX, LastValue = 0x%lX, CurrentValue = 0x%lX\n", get_cpu_id(), 
			p_rk_tf->addr, p_rk_tf->originalval, val);
		}
	}

	//restore CR0's WP
	asm_vmread (VMCS_GUEST_CR0, &cr0);

	cr0 |= CR0_WP_BIT;
	asm_vmwrite (VMCS_GUEST_CR0, cr0);
}

INITFUNC("global4", rk_init_global);
INITFUNC("vcpu0", rk_init_vcpu);
#endif