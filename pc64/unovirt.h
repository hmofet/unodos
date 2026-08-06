/* ===========================================================================
 * unovirt - can this machine host a guest, and if not, WHY NOT.
 *
 * The capability half of the appliance programme (docs/UNOVIRT-PLAN.md, phase
 * A0).  Nothing here starts a guest, enters VMX operation, or writes an MSR:
 * it reads what the silicon and the firmware will allow, latches it, and hands
 * back a verdict with attribution.
 *
 * It is deliberately shaped like detachgate.c, for the same reason.  A machine
 * that cannot run an appliance is the NORMAL case, not an error: the commonest
 * cause by far is a firmware that disabled virtualization in setup and set the
 * lock bit, which no amount of software can undo.  A gate that answers "no,
 * because the firmware locked VMX off" is useful; one that faults on VMXON is
 * a bug report from a machine that was working correctly.
 * ======================================================================== */
#ifndef UNO_VIRT_H
#define UNO_VIRT_H

/* Bumped on any breaking change to the surface below, with a dated
 * pc64/UNOVIRT.md changelog entry (AGENTS.md §6).  1 = the capability gate. */
#define UNO_VIRT_API 1

/* Which extension this machine has.  There is no "both": the vendor decides. */
enum {
    UNO_HV_NONE = 0,
    UNO_HV_VMX,                 /* Intel VT-x                                */
    UNO_HV_SVM                  /* AMD-V                                     */
};

/* Why a guest cannot run here.  A bitmask because more than one can be true
 * and the first one found is not necessarily the one worth telling the
 * operator about (uno_vmm_blocker_str picks in the order below). */
enum {
    UNO_VMB_NO_CPU    = 1u << 0,  /* neither VMX nor SVM in CPUID            */
    UNO_VMB_FW_OFF    = 1u << 1,  /* present, disabled+locked by firmware    */
    UNO_VMB_NO_SLAT   = 1u << 2,  /* no EPT / no NPT: no second-stage MMU    */
    UNO_VMB_NO_WB     = 1u << 3,  /* EPT cannot be write-back                */
    UNO_VMB_NO_LONG   = 1u << 4,  /* cannot run a 64-bit guest               */
    UNO_VMB_ATTACHED  = 1u << 5,  /* still under the firmware                */
    UNO_VMB_LOW_RAM   = 1u << 6,  /* below the appliance floor               */
    UNO_VMB_NESTED    = 1u << 7   /* we are ourselves a guest (informational
                                     when the extension is exposed anyway)   */
};

/* Everything the probe read, latched on first call.  Fields that do not apply
 * to the detected vendor are 0. */
typedef struct uno_vm_caps {
    int   vendor;               /* UNO_HV_*                                  */
    int   probed;               /* the probe has run                         */
    int   in_hypervisor;        /* CPUID.1:ECX[31] - something is above us   */
    int   slat;                 /* EPT (Intel) / NPT (AMD)                   */
    int   slat_wb;              /* second-stage memory type WB available     */
    int   slat_2m, slat_1g;     /* large-page mappings for the carve         */
    int   unrestricted;         /* VMX: a guest may start in real mode       */
    int   preempt_timer;        /* VMX preemption timer (the slice clock)    */
    int   apicv;                /* virtual-interrupt delivery / AVIC         */
    int   vpid;                 /* VPID (Intel) / >0 ASIDs (AMD)             */
    int   long_guest;           /* 64-bit guest supported                    */
    int   nrip;                 /* AMD: NRIP save (resume without decoding)  */
    int   phys_bits;            /* CPUID.80000008:EAX[7:0]                   */
    unsigned rev;               /* VMCS revision id / SVM revision           */
    unsigned asids;             /* AMD: number of ASIDs                      */
    unsigned long long fc;      /* IA32_FEATURE_CONTROL / VM_CR, raw         */
    unsigned long long basic;   /* IA32_VMX_BASIC, raw                       */
    unsigned long long eptcap;  /* IA32_VMX_EPT_VPID_CAP, raw                */
    unsigned long long ram_mb;  /* usable RAM the firmware/E820 reported      */
    char  cpu[52];              /* CPUID brand string, trimmed               */
} uno_vm_caps;

/* Read the silicon once and latch it.  Safe to call before or after detach,
 * but the RAM figure is only available while boot services live on the UEFI
 * path, so the boot path calls this during init.  Never faults, never writes
 * an MSR, never enters VMX operation. */
const uno_vm_caps *uno_vmm_probe(void);

/* Could an appliance run on this machine RIGHT NOW?  1 = yes.  *blockers, when
 * non-NULL, receives the UNO_VMB_* mask either way.  Unlike the caps, this is
 * computed live: detaching from the firmware changes the answer. */
int uno_vmm_eligible(unsigned *blockers);

/* The single most useful sentence about a mask, in the operator's language:
 * "firmware disabled virtualization (enable it in firmware setup)".  Returns a
 * static string; "" when the mask is empty. */
const char *uno_vmm_blocker_str(unsigned blockers);

/* The guest carve this machine would get, in MiB, or 0 when it gets none.
 * Purely a function of installed RAM (docs/UNOVIRT-PLAN.md §3.4); phase A2
 * reserves it. */
unsigned uno_vmm_carve_mb(void);

/* A1: enter host virtualization mode, run the marker guest, then run a guest
 * that destroys itself.  1 = the foothold is real: we entered, a guest ran,
 * control came back, and a guest that went wrong took only itself with it.
 * Runs at most once per boot and latches; safe to call when ineligible (it
 * records the refusal and does nothing).  Requires a detached machine. */
int uno_vmm_selftest(void);

/* What the selftest found, as one line for the env block.  "" before it runs. */
const char *uno_vmm_selftest_str(void);

/* Two lines for the boot env block, the System window and the `vm` verb:
 *   "vmx rev 0x0d ept wb 2m 1g unrestricted vpid preempt apicv=no phys=39"
 *   "eligible: no - firmware disabled virtualization (...)"
 * Returns the length written, excluding the NUL. */
int uno_vmm_status_str(char *buf, int cap);

#endif /* UNO_VIRT_H */
