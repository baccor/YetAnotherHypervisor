#pragma once
#include <linux/types.h>
#include <linux/stddef.h>

// on devirt the exithandler sets those:
// regs->rcx = original RSP (previrt sp)
// regs->rbx = return address to jump to
struct guest_regs {  // matches GPR push/pop macros
    __u64 r15, r14, r13, r12, r11, r10, r9, r8;
    __u64 rdi, rsi, rbp, rsp; //rsp = -1
    __u64 rbx, rdx, rcx, rax;
} __packed;

struct snap;
 
struct hsl {
    __u64 gvmcb_pa;// +0x00
    __u64 hvmcb_pa;// +0x08 
    __u64 self;// +0x10
    __u64 sharedta;// +0x18 
    __u64 padding;// +0x20 
    __u64 reserved;// +0x28  set to -1 
    __u64 hrsp;//+0x30
    __u64 pad;//+0x38
    __u64 gxcr0;// +0x40 
    __u64 hxcr0;// +0x48 
    __u64 gxss;// +0x50 
    __u64 hxss;// +0x58 
    __u64 gpkru;// +0x60 
    __u64 hpkru;// +0x68 
    __u64 fflgs;// +0x70 
    __u64 pad2; // +0x78
    __u64 extra1; // +0x80
    __u64 extra2; // +0x88
}; // 0x90


static_assert(sizeof(struct hsl)  == 0x90); // NEEDS to be 16 bytes aligned, otherwise xmmm restore/save fucks up
static_assert(sizeof(struct guest_regs) == 0x80);
static_assert(offsetof(struct guest_regs, rax) == 0x78);
static_assert(offsetof(struct guest_regs, rcx) == 0x70);
static_assert(offsetof(struct guest_regs, rdx) == 0x68);
static_assert(offsetof(struct guest_regs, rbx) == 0x60);
static_assert(offsetof(struct guest_regs, rsp) == 0x58);
static_assert(offsetof(struct guest_regs, rbp) == 0x50);
static_assert(offsetof(struct guest_regs, rsi) == 0x48);
static_assert(offsetof(struct guest_regs, rdi) == 0x40);
static_assert(offsetof(struct guest_regs, r8)  == 0x38);
static_assert(offsetof(struct guest_regs, r9)  == 0x30);
static_assert(offsetof(struct guest_regs, r10) == 0x28);
static_assert(offsetof(struct guest_regs, r11) == 0x20);
static_assert(offsetof(struct guest_regs, r12) == 0x18);
static_assert(offsetof(struct guest_regs, r13) == 0x10);
static_assert(offsetof(struct guest_regs, r14) == 0x08);
static_assert(offsetof(struct guest_regs, r15) == 0x00);
static_assert(offsetof(struct hsl, self) == 0x10);

#define HSL_FEAT_XSAVE BIT_ULL(0)
#define HSL_FEAT_XSAVES BIT_ULL(1)
#define HSL_FEAT_PKU BIT_ULL(2)

void virt(struct snap *svm);

int exithandler(struct guest_regs *regs, struct snap *snapsh);
