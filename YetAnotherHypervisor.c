
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/svm.h> 
#include <linux/types.h>
#include <asm/msr.h>
#include <asm/desc.h>
#include <linux/mm.h>
#include <linux/set_memory.h>
#include <asm/msr-index.h>
#include <asm/pgtable_types.h>
#include <asm/cpufeature.h>
#include <asm/fpu/types.h>
#include <asm/fpu/xcr.h>
#include <asm/pkru.h>
#include <linux/kprobes.h>
#include <asm/pgtable.h>
#include <linux/delay.h>
#include <uapi/asm/processor-flags.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <asm/special_insns.h>
#include <asm/debugreg.h>
#include <asm/page.h>
#include <asm/io.h>
#include "virt.h"




MODULE_LICENSE("GPL");
MODULE_AUTHOR("kyo"); 

#define hso 2 // host stack order, this one's enough for now
#define hss (PAGE_SIZE << hso) // the actual host stack size

static cpumask_t virtcpus;
 
struct snap {                  // main per cpu struct, has pretty much everything that's needed
	struct hsl  *hoststack;
	struct vmcb *vmcb;
	phys_addr_t vmcb_pa;
    struct vmcb *hvmcb;
    phys_addr_t hvmcb_pa;
    bool lnched;
};
static void *msrpm; // 2 pages

static DEFINE_PER_CPU(struct snap *, cpusnap);

// if guest switches cr3 it'll free the old one thus if vmexit returns to host it'll restore a corrupted cr3 and triple fault immediately.
// pinning the cr3 page makes the kernel unable to free it (no corruption). i guess it's somewhat a memory leak so it'd be better to shadow it but that's a todo.  
static DEFINE_PER_CPU(struct page *, pcr3);
static void clnup(void);
static int exvirt(void);
static bool asidflsh;


static_assert(offsetof(struct snap, hoststack) == 0);
static_assert(offsetof(struct snap, hvmcb) == 0x18);
static_assert(offsetof(struct snap, hvmcb_pa) == 0x20);
static_assert(hss == 0x4000);

static inline void dbg_putc(unsigned char c) {

    asm volatile("outb %0, $0xe9" : : "a"(c) : "memory");
}

static inline void dbg_puts(const char *s) {

    while (*s) dbg_putc(*s++);
}

typedef union {
    __u64 as_u64;
    struct {
        __u64 present        : 1;
        __u64 writable       : 1;
        __u64 user           : 1;
        __u64 write_through  : 1;
        __u64 cache_disable  : 1;
        __u64 accessed       : 1;
        __u64 ignored0       : 1;
        __u64 reserved0      : 1;
        __u64 ignored1       : 4;
        __u64 pfn            : 40;
        __u64 ignored2       : 11;
        __u64 nx             : 1;
    };
} pml4e;

typedef union {
    __u64 as_u64;
    struct {
        __u64 present        : 1;
        __u64 writable       : 1;
        __u64 user           : 1;
        __u64 write_through  : 1;
        __u64 cache_disable  : 1;
        __u64 accessed       : 1;
        __u64 ignored0       : 1;
        __u64 large          : 1;
        __u64 ignored1       : 4;
        __u64 pfn            : 40;
        __u64 ignored2       : 11;
        __u64 nx             : 1;
    };
} npt_pdpte;


typedef union {
    __u64 as_u64;
    struct {
        __u64 present        : 1;
        __u64 writable       : 1;
        __u64 user           : 1;
        __u64 write_through  : 1;
        __u64 cache_disable  : 1;
        __u64 accessed       : 1;
        __u64 dirty          : 1;
        __u64 large          : 1;
        __u64 global         : 1;
        __u64 ignored0       : 3;
        __u64 pat            : 1;
        __u64 reserved0      : 8;
        __u64 pfn            : 31; 
        __u64 ignored1       : 11;
        __u64 nx             : 1;
    };
} npt_pde;

typedef union {
    __u64 as_u64;
    struct {
        __u64 present        : 1;
        __u64 writable       : 1;
        __u64 user           : 1;
        __u64 write_through  : 1;
        __u64 cache_disable  : 1;
        __u64 accessed       : 1;
        __u64 dirty          : 1;
        __u64 pat            : 1;
        __u64 global         : 1;
        __u64 ignored0       : 3;
        __u64 pfn            : 40;
        __u64 ignored1       : 11;
        __u64 nx             : 1;
    };
} npt_pte;

typedef union {
    __u64 as_u64;
    struct {
        __u64 present        : 1;
        __u64 writable       : 1;
        __u64 user           : 1;
        __u64 write_through  : 1;
        __u64 cache_disable  : 1;
        __u64 accessed       : 1;
        __u64 ignored0       : 1;
        __u64 large          : 1;
        __u64 ignored1       : 4;
        __u64 pfn            : 40;
        __u64 ignored2       : 11;
        __u64 nx             : 1;
    };
} npt_pde_ptr;  // i could use a single struct instead of 3 but i'd rather have 3 similar structs than debug for several hours

typedef struct {
    npt_pte ptes[512];
} npt_pt;

typedef struct {
    npt_pdpte  pdpt[512];
    npt_pde pd[512][512];
} pml4e_tree;

typedef struct {
    pml4e   *pml4;
    pml4e_tree *trees[2];

} shrddta;       // that's like 2x262k 2MB pages so bare-metal's fine

static shrddta *shareddta;

static __u16 get_seg_attrib(__u16 selector)
{
    struct desc_ptr gdt_ptr;
    struct desc_struct *desc;
    __u16 index = selector >> 3;

    if ((selector & ~0x3) == 0)
        return 0;

    if (selector & 0x4) {
        pr_err("LDT selector 0x%x not supported in get_seg_attrib\n", selector);
        return 0;
    }

    native_store_gdt(&gdt_ptr);
    if ((index * sizeof(struct desc_struct)) > gdt_ptr.size) return 0;
    desc = &((struct desc_struct *)gdt_ptr.address)[index];

    return desc->type | (desc->s << 4) | (desc->dpl << 5) | (desc->p << 7) | (desc->avl << 8) | (desc->l << 9) | (desc->d << 10) | (desc->g << 11);
}

static __u32 get_seg_limit(__u16 selector)
{
    struct desc_ptr gdt_ptr;
    struct desc_struct *desc;
    unsigned long limit;
    __u16 index = selector >> 3;

    if ((selector & ~0x3) == 0) return 0;

    if (selector & 0x4) {
        pr_err("LDT selector 0x%x not supported in get_seg_limit\n", selector);
        return 0;
    }

    native_store_gdt(&gdt_ptr);
    if ((index * sizeof(struct desc_struct)) > gdt_ptr.size) return 0;

    desc = &((struct desc_struct *)gdt_ptr.address)[index];
    limit = get_desc_limit(desc);
    if (desc->g) limit = (limit << PAGE_SHIFT) | (PAGE_SIZE - 1);

    return limit;
}

static __u64 get_seg_base(__u16 selector)
{
    struct desc_ptr gdt_ptr;
    struct desc_struct *desc;
    __u16 index = selector >> 3;

    if ((selector & ~0x3) == 0) return 0;

    if (selector & 0x4) {
        pr_err("LDT selector 0x%x not supported in get_seg_base\n", selector);
        return 0;
    }

    native_store_gdt(&gdt_ptr);
    if ((index * sizeof(struct desc_struct)) > gdt_ptr.size) return 0;

    desc = &((struct desc_struct *)gdt_ptr.address)[index];
    return get_desc_base(desc);
}

static void addsave(struct vmcb_save_area *save) {  // additional save

    __u64 val;

    if (!rdmsrq_safe(MSR_IA32_SPEC_CTRL, &val)) save->spec_ctrl = val;

    if (!rdmsrq_safe(MSR_IA32_S_CET, &val)) save->s_cet = val;

    if (!rdmsrq_safe(MSR_IA32_PL0_SSP, &val)) save->ssp = val;

    if (!rdmsrq_safe(MSR_IA32_INT_SSP_TAB, &val)) save->isst_addr = val;

    if (!rdmsrq_safe(MSR_IA32_DEBUGCTLMSR, &val)) save->dbgctl = val;
}

static void getvmcb(struct vmcb_save_area *save) {    // some of these will get overwritten by vmsave anyway. 

    __u16 sel;
    struct desc_ptr gdtr, idtr;
    struct desc_struct *gdt;

    memset(save, 0, sizeof(*save));

    native_store_gdt(&gdtr);
    store_idt(&idtr);

    save->gdtr.base  = gdtr.address;
    save->gdtr.limit = gdtr.size;
    save->idtr.base  = idtr.address;
    save->idtr.limit = idtr.size;

    save->cr0 = native_read_cr0();
    save->cr2 = native_read_cr2();
    save->cr3 = __native_read_cr3();
    save->cr4 = native_read_cr4();

    rdmsrl(MSR_EFER, save->efer);
    save->cpl = 0;

    asm volatile("mov %%es, %0" : "=r"(sel));
    save->es.selector = sel;
    save->es.attrib   = get_seg_attrib(sel);
    save->es.limit    = get_seg_limit(sel);
    save->es.base     = get_seg_base(sel);

    asm volatile("mov %%cs, %0" : "=r"(sel));
    save->cs.selector = sel;
    save->cs.attrib   = get_seg_attrib(sel);
    save->cs.limit    = get_seg_limit(sel);
    save->cs.base     = get_seg_base(sel);

    asm volatile("mov %%ss, %0" : "=r"(sel));
    save->ss.selector = sel;
    save->ss.attrib   = get_seg_attrib(sel);
    save->ss.limit    = get_seg_limit(sel);
    save->ss.base     = get_seg_base(sel);

    asm volatile("mov %%ds, %0" : "=r"(sel));
    save->ds.selector = sel;
    save->ds.attrib   = get_seg_attrib(sel);
    save->ds.limit    = get_seg_limit(sel);
    save->ds.base     = get_seg_base(sel);

    asm volatile("mov %%fs, %0" : "=r"(sel));
    save->fs.selector = sel;
    save->fs.attrib   = get_seg_attrib(sel);
    save->fs.limit    = get_seg_limit(sel);
    rdmsrl(MSR_FS_BASE, save->fs.base);

    asm volatile("mov %%gs, %0" : "=r"(sel));
    save->gs.selector = sel;
    save->gs.attrib   = get_seg_attrib(sel);
    save->gs.limit    = get_seg_limit(sel);
    rdmsrl(MSR_GS_BASE, save->gs.base);

    native_store_gdt(&gdtr);
    gdt = (struct desc_struct *)gdtr.address;

    asm volatile("str %0" : "=r"(sel));
    save->tr.selector = sel;
    save->tr.attrib   = get_seg_attrib(sel);
    {
        struct ldttss_desc *tss = (struct ldttss_desc *)&gdt[sel >> 3];
        save->tr.base = ((__u64)tss->base0) | ((__u64)tss->base1 << 16) | ((__u64)tss->base2 << 24) | ((__u64)tss->base3 << 32);
        save->tr.limit = tss->limit0 | (tss->limit1 << 16);
    }

    asm volatile("sldt %0" : "=r"(sel));
    save->ldtr.selector = sel;
    if (sel) {
        struct ldttss_desc *ldt = (struct ldttss_desc *)&gdt[sel >> 3];
        save->ldtr.attrib = get_seg_attrib(sel);
        save->ldtr.base = ((__u64)ldt->base0) | ((__u64)ldt->base1 << 16) | ((__u64)ldt->base2 << 24) | ((__u64)ldt->base3 << 32);
        save->ldtr.limit = ldt->limit0 | (ldt->limit1 << 16);
    }

    rdmsrl(MSR_STAR,              save->star);
    rdmsrl(MSR_LSTAR,             save->lstar);
    rdmsrl(MSR_CSTAR,             save->cstar);
    rdmsrl(MSR_SYSCALL_MASK,      save->sfmask);
    rdmsrl(MSR_KERNEL_GS_BASE,    save->kernel_gs_base); // vmsave for all of these and some selectors would be cleaner here but we're doing that later sooooooooo
    rdmsrl(MSR_IA32_SYSENTER_CS,  save->sysenter_cs);  
    rdmsrl(MSR_IA32_CR_PAT,       save->g_pat);         // required for NPT, otherwise it's zeroed and makes everything uncacheable. we don't want that, i'd know.
    rdmsrl(MSR_IA32_SYSENTER_ESP, save->sysenter_esp);
    rdmsrl(MSR_IA32_SYSENTER_EIP, save->sysenter_eip);
    addsave(save);

    get_debugreg(save->dr6, 6);
    get_debugreg(save->dr7, 7);
    save->rflags = native_save_fl();
}


static void nrip(struct vmcb *vmcb) { 
    vmcb->save.rip = vmcb->control.next_rip;
}

static inline struct hsl *snap_hsl(struct snap *snapsh) {

    if (!snapsh || !snapsh->hoststack) return NULL;

    return (struct hsl *)((__u8 *)snapsh->hoststack + hss - sizeof(struct hsl));
}

static inline void refx(struct snap *snapsh) {

    struct hsl *h = snap_hsl(snapsh);
    __u64 xss;

    if (!h) return;

    if (h->fflgs & HSL_FEAT_XSAVE) h->hxcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);

    if ((h->fflgs & HSL_FEAT_XSAVES) && !rdmsrq_safe(MSR_IA32_XSS, &xss)) h->hxss = xss;

    if (h->fflgs & HSL_FEAT_PKU) h->hpkru = read_pkru();
}

static inline void savehost(struct snap *snapsh) {  // had addsave before but it was kinda pointless. i'm too lazy to get rid of this so it stays

    refx(snapsh);
}

static inline void sethost(struct snap *snapsh) {

    memset(snapsh->hvmcb, 0, PAGE_SIZE);
    getvmcb(&snapsh->hvmcb->save);
    wrmsrl(MSR_VM_HSAVE_PA, snapsh->hvmcb_pa);
    asm volatile("vmsave %0" :: "a"(snapsh->hvmcb_pa) : "memory");  // overwritten by vmsave as i said in getvmcb
    addsave(&snapsh->hvmcb->save);
}

static void ssvme(void *sd) {

	__u64 efer;

	rdmsrl(MSR_EFER, efer);
	if (!(efer & EFER_SVME)) wrmsrl(MSR_EFER, efer | EFER_SVME);
}

static void snapalloc(void *sd) {
    struct snap *s;
    struct page *vmcb_page;
    struct page *hvmcb_page;    

    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s) goto f;

    vmcb_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!vmcb_page) goto fsnap;

    s->vmcb    = page_address(vmcb_page);
    s->vmcb_pa = page_to_phys(vmcb_page);

    hvmcb_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!hvmcb_page) goto fvmcb;

    s->hvmcb = page_address(hvmcb_page);
    s->hvmcb_pa = page_to_phys(hvmcb_page);

    s->hoststack = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, hso);
    if (!s->hoststack) goto fhvmcb;

    this_cpu_write(cpusnap, s);
    return;

fhvmcb:
    __free_page(hvmcb_page);
fvmcb:
    __free_page(vmcb_page);
fsnap:
    kfree(s);
f:
    this_cpu_write(cpusnap, NULL);
}

static void snapfree(void *sd) {

    struct snap *s = this_cpu_read(cpusnap);
    if (!s) return;

    if (s->hoststack) free_pages((unsigned long)s->hoststack, hso);
    if (s->vmcb)      __free_page(virt_to_page(s->vmcb));
    if (s->hvmcb) __free_page(virt_to_page(s->hvmcb));
    kfree(s);
    this_cpu_write(cpusnap, NULL);
}

static void fsvme(void *sd) {

	__u64 efer;

	wrmsrl(MSR_VM_HSAVE_PA, 0);

	rdmsrl(MSR_EFER, efer);
	if (efer & EFER_SVME) {
		asm volatile("stgi" ::: "memory");
		wrmsrl(MSR_EFER, efer & ~EFER_SVME);
	}
}

static npt_pt *splitL(shrddta *s, __u64 pml4i, __u64 pdpti, __u64 pdi) {

    npt_pde *pde = &s->trees[pml4i]->pd[pdpti][pdi];
    __u64 base_pfn = (__u64)pde->pfn << 9;
    npt_pde_ptr *pde_ptr = (npt_pde_ptr *)pde;
    npt_pt *pt;
    int i;

    if (pde_ptr->present && !pde_ptr->large) return __va((__u64)pde_ptr->pfn << PAGE_SHIFT);

    pt = kzalloc(sizeof(npt_pt), GFP_ATOMIC);
    if (!pt)
        return NULL;

    for (i = 0; i < 512; i++) {
        pt->ptes[i].pfn      = base_pfn + i;
        pt->ptes[i].present  = 1;
        pt->ptes[i].writable = 1;
        pt->ptes[i].user     = 1;
    }

    pde_ptr->as_u64   = 0;
    pde_ptr->pfn      = __pa(pt) >> PAGE_SHIFT;
    pde_ptr->present  = 1;
    pde_ptr->writable = 1;
    pde_ptr->user     = 1;
    pde_ptr->large    = 0;

    return pt;
}

static npt_pt *nptget(shrddta *s, __u64 gpa) {

    __u64 pml4i = (gpa >> 39) & 0x1ff;
    __u64 pdpti = (gpa >> 30) & 0x1ff;
    __u64 pdi   = (gpa >> 21) & 0x1ff;
    npt_pdpte *pdpt;
    npt_pde *pd;
    npt_pt *pt;

    // gets or allocs a PDPT 
    if (!s->pml4[pml4i].present) {
        pdpt = kzalloc(sizeof(npt_pdpte) * 512, GFP_ATOMIC);
        if (!pdpt) return NULL;
        s->pml4[pml4i].pfn      = __pa(pdpt) >> PAGE_SHIFT;
        s->pml4[pml4i].present  = 1;
        s->pml4[pml4i].writable = 1;
        s->pml4[pml4i].user     = 1;
    } else pdpt = __va((__u64)s->pml4[pml4i].pfn << PAGE_SHIFT);

    // same thing but for PDs
    if (!pdpt[pdpti].present) {
        pd = kzalloc(sizeof(npt_pde) * 512, GFP_ATOMIC);
        if (!pd) return NULL;
        pdpt[pdpti].pfn      = __pa(pd) >> PAGE_SHIFT;
        pdpt[pdpti].present  = 1;
        pdpt[pdpti].writable = 1;
        pdpt[pdpti].user     = 1;
    } else pd = __va((__u64)pdpt[pdpti].pfn << PAGE_SHIFT);

    // ... for PTs
    npt_pde_ptr *pde_ptr = (npt_pde_ptr *)&pd[pdi];
    if (!pde_ptr->present) {
        pt = kzalloc(sizeof(npt_pt), GFP_ATOMIC);
        if (!pt) return NULL;
        pde_ptr->as_u64   = 0;
        pde_ptr->pfn      = __pa(pt) >> PAGE_SHIFT;
        pde_ptr->present  = 1;
        pde_ptr->writable = 1;
        pde_ptr->user     = 1;
        pde_ptr->large    = 0;
    } else if (pd[pdi].large) {
        // splits a large page into 4KB ptes
        pt = splitL(s, pml4i, pdpti, pdi);
        if (!pt){
            pr_err("failed to split a large page.");
            return NULL;
        }
    } else pt = __va((__u64)pde_ptr->pfn << PAGE_SHIFT);

    return pt;
}

static void nptmap(shrddta *s, __u64 gpa, __u64 phys) {

    __u64 pti = (gpa >> 12) & 0x1ff;

    npt_pt *pt = nptget(s, gpa);
    if (!pt) return;

    pt->ptes[pti].as_u64   = 0;
    pt->ptes[pti].pfn      = phys >> PAGE_SHIFT;
    pt->ptes[pti].present  = 1;
    pt->ptes[pti].writable = 1;
    pt->ptes[pti].user     = 1;
}

static void setmsrpm(void) {

    __u32 off2bse, off;
    __u8 *map = (__u8 *)msrpm;

    memset(msrpm, 0, 0x2000);

    off2bse = (MSR_EFER - 0xc0000000) * 2;
    off = (0x800 * 8) + off2bse;

    // looks like shit but it's just EFER intercept bit
    map[off / 8] |= (1 << ((off + 1) % 8));
}

static void bnpt(shrddta *s) {    // builds NPT
    __u64 pdptbpge, pdbpge, transpge;
    __u64 pml4i, pdpti, pdi;

    for (pml4i = 0; pml4i < 2; pml4i++) {
        pml4e_tree *tree = s->trees[pml4i];
        pml4e *entr = &s->pml4[pml4i];

        pdptbpge = __pa(&tree->pdpt);
        entr->pfn = pdptbpge >> PAGE_SHIFT;
        entr->present = 1;
        entr->writable = 1;
        entr->user = 1;
    
        for (pdpti = 0; pdpti < 512; pdpti++) {

            pdbpge = __pa(&tree->pd[pdpti][0]);
            tree->pdpt[pdpti].pfn = pdbpge >> PAGE_SHIFT;
            tree->pdpt[pdpti].present = 1;
            tree->pdpt[pdpti].writable = 1;
            tree->pdpt[pdpti].user = 1;

            for (pdi = 0; pdi < 512; pdi++) {
                transpge = (pml4i * 512 * 512) + (pdpti * 512) + pdi;
                tree->pd[pdpti][pdi].pfn = transpge;
                tree->pd[pdpti][pdi].present = 1;
                tree->pd[pdpti][pdi].writable = 1;
                tree->pd[pdpti][pdi].user = 1;
                tree->pd[pdpti][pdi].large = 1;

            }
        }
    }

}


static void setvmcb(void *sd) {

    struct snap *snapsh = this_cpu_read(cpusnap);

    memset(snapsh->vmcb, 0, PAGE_SIZE);

    snapsh->vmcb->control.asid = 1;
    snapsh->vmcb->control.clean = 0;
    snapsh->vmcb->control.nested_ctl = SVM_NESTED_CTL_NP_ENABLE;
    snapsh->vmcb->control.nested_cr3 = __pa(shareddta->pml4);
    snapsh->vmcb->control.msrpm_base_pa = __pa(msrpm);

    // ugly but that's from svm.h, refer to AMD'S APM vol.2 appendix C for *normal* intercept codes 
    snapsh->vmcb->control.intercepts[INTERCEPT_WORD3] |= BIT(INTERCEPT_CPUID - INTERCEPT_INTR); 
    snapsh->vmcb->control.intercepts[INTERCEPT_WORD3] |= BIT(INTERCEPT_MSR_PROT - INTERCEPT_INTR);
    snapsh->vmcb->control.intercepts[INTERCEPT_WORD3] |= BIT(INTERCEPT_SHUTDOWN - INTERCEPT_INTR);
    snapsh->vmcb->control.intercepts[INTERCEPT_WORD4] |= BIT(INTERCEPT_VMRUN - INTERCEPT_VMRUN);

    getvmcb(&snapsh->vmcb->save);

    __u64 cr3phy = snapsh->vmcb->save.cr3 & ~0xFFF;
    struct page *p = pfn_to_page(cr3phy >> PAGE_SHIFT);
    get_page(p);
    this_cpu_write(pcr3, p); 

    if (asidflsh) snapsh->vmcb->control.tlb_ctl = 3;
    else snapsh->vmcb->control.tlb_ctl = 1;

    sethost(snapsh);
    savehost(snapsh);
}


// kprints in this context can and WILL cause random deadlocks.
noinline int exithandler(struct guest_regs *regs, struct snap *snapsh) {

    regs->rax = snapsh->vmcb->save.rax; 

    snapsh->vmcb->control.tlb_ctl = 0;
    switch (snapsh->vmcb->control.exit_code) {


    // we need this for devirt. again, it'd be cleaner to do the same thing with a specific IO or vmmcalls so that we don't need to handle other CPUID calls(and there is a fair amount of them)
    case SVM_EXIT_CPUID: {
        __u32 eax, ebx, ecx, edx;
        __u64 grax = snapsh->vmcb->save.rax;

        if (grax == 0x6B7973) {
            regs->rcx = snapsh->vmcb->save.rsp;
            regs->rbx = snapsh->vmcb->control.next_rip;
            regs->rax = snapsh->vmcb->save.rflags;

            // returning as guest except this time not being virtualized so we gotta load guest state explicitly, or i could just add this to the asm path
            asm volatile("vmload %0" :: "a"(snapsh->vmcb_pa) : "memory");
            asm volatile("cli");
            asm volatile("stgi");

            __u64 efer, cr3;
            rdmsrl(MSR_EFER, efer);
            wrmsrl(MSR_EFER, efer & ~EFER_SVME);
            asm volatile("mov %%cr3, %0" : "=r"(cr3));
            asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
            return 1;
        }

        eax = (__u32)grax;
        ebx = (__u32)regs->rbx;
        ecx = (__u32)regs->rcx;
        edx = (__u32)regs->rdx;

		asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "0"(eax), "2"(ecx));  // native_cpuid should work fine, it's asm cuz of debugging but no reason to change it back

        snapsh->vmcb->save.rax = eax;
        regs->rax = snapsh->vmcb->save.rax;
        regs->rbx = ebx;
        regs->rcx = ecx;
        regs->rdx = edx;
        nrip(snapsh->vmcb);
        savehost(snapsh);
        return 0;

    }


    case SVM_EXIT_MSR: {  // basic svme unset/set protection

        __u32 msr;
        bool write;
        __u64 val;
        __u64 rax = snapsh->vmcb->save.rax;

        msr   = (__u32)regs->rcx;
        write = (snapsh->vmcb->control.exit_info_1 != 0);

        if (msr == MSR_EFER) {

            if (write) {
                // doesn't write SVME, and that's it really
                val = ((__u64)(regs->rdx & 0xffffffff) << 32) | (rax & 0xffffffff);
                val &= ~EFER_SVME;
                snapsh->vmcb->save.efer = val;
            } else {
                // returns EFER but with SVME cleared so guest doesn't know it's set
                snapsh->vmcb->save.rax = (snapsh->vmcb->save.efer & ~EFER_SVME) & 0xffffffff;
                regs->rdx = (snapsh->vmcb->save.efer & ~EFER_SVME) >> 32;
            }
        } else {

            if (write) {
                val = ((__u64)(regs->rdx & 0xffffffff) << 32) | (rax & 0xffffffff);
                wrmsrl(msr, val);
            } else {
            rdmsrl(msr, val);
            snapsh->vmcb->save.rax = val & 0xffffffff;
            regs->rax = snapsh->vmcb->save.rax;
            regs->rdx = val >> 32;
            }
        }
        nrip(snapsh->vmcb);
        savehost(snapsh);
        return 0; 
    }

    case SVM_EXIT_NPF: {

        // i need a better handler for this case so it doesn't cause random triple faults(hasn't done that yet tho).
        // under nested it'll fire because of KVM's internal mapping. this part handles it so that it doesn't panic on insmod and loads fine. (there's probably some edge case here) 
        // for bare-metal whole RAM's covered (1 TB) so it shouldn't hit unless you set permissions or get OOB

        __u64 gpa = snapsh->vmcb->control.exit_info_2 & ~0xfffULL;
        nptmap(shareddta, gpa, gpa);
        snapsh->vmcb->control.clean = 0;
        savehost(snapsh);
        return 0;
        
    }

    case SVM_EXIT_VMRUN: {

        snapsh->vmcb->control.event_inj = (1u << 31) | (3u << 8) | 6;
        savehost(snapsh);
        return 0;
    }

    default:                // waiting for something to happen?
        savehost(snapsh);
        return 0;
    }
}

static noinline int vmrunlp(void) {

	__u64 rflgs, rip, rsp;
	__u64 xcr0 = 0;
	__u64 xss = 0;
	struct snap *snapsh;
    struct hsl *hsll;

    // this part is slightly complicated but basically: we're snapshotting the next instruction's rip and current rsp & flags so that we can return here when asm stub calls vmrun.
    // when vmrun gets called it starts from here where it checks whether lnched is set(host sets it here along the way), at this point we're running as guest.
    // host itself is still in the asm context until devirt after which it jumps into the guest(basically does vmrun's job manually except that it loses its current context at that point)
    asm volatile(      
        "lea 1f(%%rip), %0\n\t"
        "mov %%rsp, %1\n\t"
        "pushfq; popq %2\n\t"
        "1:"
        : "=r"(rip), "=r"(rsp), "=r"(rflgs)
    );

    snapsh = this_cpu_read(cpusnap);
    if (!snapsh)
        return 0;

    if (READ_ONCE(snapsh->lnched)) {
        pr_err("V");
        return 1;
    }
    pr_err("N");

    hsll = (struct hsl *)((__u8 *)snapsh->hoststack + hss - sizeof(struct hsl));

    hsll->gvmcb_pa = snapsh->vmcb_pa;
    hsll->hvmcb_pa  = snapsh->hvmcb_pa;
    hsll->self = (__u64)snapsh;
    hsll->reserved = -1ULL;
    hsll->fflgs = 0;  // should be zeroed already
    hsll->gxcr0 = 0;
    hsll->hxcr0 = 0;
    hsll->gxss = 0;
    hsll->hxss = 0;
    hsll->gpkru = 0;
    hsll->hpkru = 0;

    // some state's not saved nor restored, we're doing that ourselves
    if (snapsh->vmcb->save.cr4 & X86_CR4_OSXSAVE) {
        xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
        hsll->fflgs |= HSL_FEAT_XSAVE;
        hsll->gxcr0 = xcr0;
        hsll->hxcr0 = xcr0;

        if (cpu_feature_enabled(X86_FEATURE_XSAVES) && !rdmsrq_safe(MSR_IA32_XSS, &xss)) {
            hsll->fflgs |= HSL_FEAT_XSAVES;
            hsll->gxss = xss;
            hsll->hxss = xss;
        }
    }

    if (cpu_feature_enabled(X86_FEATURE_PKU) && (((snapsh->vmcb->save.cr4 & X86_CR4_PKE) != 0) || ((hsll->gxcr0 & XFEATURE_MASK_PKRU) != 0))) {
        hsll->fflgs |= HSL_FEAT_PKU;
        hsll->gpkru = read_pkru();
        hsll->hpkru = hsll->gpkru;
    }

	snapsh->vmcb->save.rip = rip;
	snapsh->vmcb->save.rsp = rsp;
	snapsh->vmcb->save.rflags = rflgs;// this might break depending on host's state &| running context during insmod (IPI for example). regular context should be fine though.
	snapsh->vmcb->save.cpl = 0;

	WRITE_ONCE(snapsh->lnched, true);
	virt(snapsh);
	WRITE_ONCE(snapsh->lnched, false);

	return 0;
}

static int switch_to_cpu(unsigned int cpu) {

    unsigned int accpu;
    int ret;

    ret = set_cpus_allowed_ptr(current, cpumask_of(cpu));
    if (ret)
        return ret;

    accpu = get_cpu();
    put_cpu();

    if (accpu != cpu) {
        cond_resched();
        accpu = get_cpu();
        put_cpu();
    }

    if (accpu != cpu) {
        pr_err("failed to switch to cpu %u, running on cpu %u\n", cpu, accpu);
        return -EIO;
    }

    return 0;
}

static int onech(int (*fn)(void *), void *arg, unsigned int *nc, struct cpumask *smsk) { // basically sleepable on_each_cpu
    
    cpumask_var_t omsk;
    unsigned int cpu;
    unsigned int done = 0;
    int ret = 0;
    int restret;

    if (!alloc_cpumask_var(&omsk, GFP_KERNEL)) return -ENOMEM;

    cpumask_copy(omsk, current->cpus_ptr);

    for_each_online_cpu(cpu) {
        ret = switch_to_cpu(cpu);
        if (ret)
            break;

        ret = fn(arg);
        if (ret)
            break;

        if (smsk)
            cpumask_set_cpu(cpu, smsk);

        done++;
    }

    restret = set_cpus_allowed_ptr(current, omsk);
    if (!ret && restret)
        ret = restret;

    free_cpumask_var(omsk);

    if (nc)
        *nc = done;

    return ret;
}

static int virtcpu(void *s) {

	int ret;
    if (cpumask_test_cpu(raw_smp_processor_id(), &virtcpus)) {
        return 0;
    }

    migrate_disable();

    ssvme(NULL);

    snapalloc(NULL);
    if (!this_cpu_read(cpusnap)) {
        ret = -ENOMEM;
        goto out_mig;
    }

    setvmcb(NULL);

    preempt_disable();
    ret = vmrunlp();
    preempt_enable();
    if (ret > 0) {
        cpumask_set_cpu(raw_smp_processor_id(), &virtcpus);
        ret = 0;
        goto out_mig;
    }

    cpumask_clear_cpu(raw_smp_processor_id(), &virtcpus);

    ret = ret ? ret : -EIO;
    pr_err("vmrunlp failed on cpu %d: %d\n", smp_processor_id(), ret);

    snapfree(NULL);
    fsvme(NULL);
out_mig:
    migrate_enable();
    return ret;
}

// we have two ways of exit here, one is simplesvm'ishy and the other one's normal, for now the first one it is.
// the best way to devirt would be to either use vmmcalls or a specific IO, CPUID works but it's meh
static int devirtcpu(void *s) {

    __u32 eax = 0x6B7973;
    __u32 ebx = 0;
    __u32 ecx = 0;
    __u32 edx = 0;
    struct snap *snapsh;

    if (!cpumask_test_cpu(raw_smp_processor_id(), &virtcpus)) return 1;

    snapsh = this_cpu_read(cpusnap);
    if (!snapsh) return 1;

    asm volatile("cpuid"
        : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
        :
        : "memory");

    if (ecx != 0x6B) {  // we need that to act as an exit code, we could do that internally through per_cpu defines but i'll be updating this anyway
        pr_err("cpu %d not devirtualized\n", smp_processor_id()); 
        return 1;
    }

    snapsh->lnched = false;
    snapfree(NULL);
    fsvme(NULL);
    cpumask_clear_cpu(raw_smp_processor_id(), &virtcpus);
    return 0;
}

static int exvirt(void) {

    int ret = onech(devirtcpu, NULL, NULL, NULL);
    if (!cpumask_empty(&virtcpus) || ret) {
        pr_err("not all cpus devirtualized\n");
        return -EIO;
    }
    pr_info("all cpus devirtualized\n");
    return 0;
}

static int initshi(void) {
	int ret = 0;

    msrpm = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
    if (!msrpm) {ret = -ENOMEM; goto vmerr;}

    shareddta = kzalloc(sizeof(shrddta), GFP_KERNEL);
    if (!shareddta) {
    pr_err("failed to allocate NPT pages\n");
    pr_err("sizeof(shrddta)=%zu order=%u\n", sizeof(shrddta), get_order(sizeof(shrddta)));
    ret = -ENOMEM;
    goto vmerr;
    }
    shareddta->pml4 = (pml4e *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!shareddta->pml4) {
    pr_err("failed to allocate pml4 in NPT pages\n");
    ret = -ENOMEM;
    goto vmerr;
    }
    shareddta->trees[0] = (pml4e_tree *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(sizeof(pml4e_tree)));
    if (!shareddta->trees[0]) {
    pr_err("failed to allocate tree 0 in NPT pages\n");
    ret = -ENOMEM;
    goto vmerr;
    }
    shareddta->trees[1] = (pml4e_tree *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(sizeof(pml4e_tree)));
    if (!shareddta->trees[1]) {
    pr_err("failed to allocate tree 1 in NPT pages\n");
    ret = -ENOMEM;
    goto vmerr;
    }

    bnpt(shareddta);
    setmsrpm();

    cpumask_clear(&virtcpus);
    ret = onech(virtcpu, NULL, NULL, NULL);
    if (ret) goto vmerr;

    return 0;

vmerr:
    exvirt();
	clnup();
	return ret;

}

static bool svmsup(void) {  // basic checks
    __u32 eax, ebx, ecx, edx;
    __u64 vm_cr;
    
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (!(ecx & BIT(2))) {
        pr_err("SVM's not supported here\n");
        return false;
    }
    
    rdmsrl(MSR_VM_CR, vm_cr);
    if (vm_cr & BIT(4)) {
        pr_err("SVM disabled\n");
        return false;
    }

	cpuid(0x8000000A, &eax, &ebx, &ecx, &edx);
	if (!(edx & BIT(3))) {
    	pr_err("NextRipSave's not supported\n");  // we could use a fallback for this but i'm not doing allat
    	return false;
	}
    if ((edx & BIT(6))) asidflsh = true;
    
    return true;
}

static void clnup(void) {
    pr_err("initiating clnup\n");
    struct page *p = this_cpu_read(pcr3);
    if (msrpm) {free_pages((unsigned long)msrpm, 1); msrpm = NULL;}
    if (p) {
    put_page(p);
    this_cpu_write(pcr3, NULL);
    }
    if (shareddta) {
    if (shareddta->pml4) free_page((unsigned long)shareddta->pml4);
    if (shareddta->trees[0]) free_pages((unsigned long)shareddta->trees[0], get_order(sizeof(pml4e_tree)));
    if (shareddta->trees[1]) free_pages((unsigned long)shareddta->trees[1], get_order(sizeof(pml4e_tree)));
    kfree(shareddta);
    shareddta = NULL;
    }
} 

static int hyinit(void) {
    pr_info("YAH loaded (almost)\n");

	if (!svmsup()) return -EOPNOTSUPP;
	return initshi();
}

static void hyexit(void) {

	if (exvirt()) pr_err("not all cpus devirtualized\n");
    else {
        clnup();
        pr_info("YAH unloaded\n");
    }
}



module_init(hyinit);
module_exit(hyexit);
