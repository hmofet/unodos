/* ===========================================================================
 * unovirt_mgr - appliances as things a USER has, rather than a thing the
 * selftest happens to place.
 *
 * Everything below A6 treats the guest as a fixture: one kernel, at one
 * hardcoded path, armed by a debug flag, running until the machine stops.
 * This is the surface a manager application needs instead - a list of VMs the
 * user made, each with its own files, and one of them running at a time with
 * a console somebody can type into.
 *
 * ONE GUEST RUNS AT A TIME, and that is a real constraint rather than an
 * omission: there is a single VMCS, and arming a second guest vmclears and
 * reconfigures the block the first is using (pc64/UNOVIRT.md, A6b's first
 * finding).  `uno_vm_running()` is therefore an index, not a count.
 * ======================================================================== */
#ifndef UNO_VIRT_MGR_H
#define UNO_VIRT_MGR_H

#define UNO_VM_MAX      8
#define UNO_VM_NAME     24
#define UNO_VM_PATH     64
#define UNO_VM_CON_ROWS 200        /* console scrollback, in lines          */
#define UNO_VM_CON_COLS 128

typedef struct {
    char name[UNO_VM_NAME];
    char kernel[UNO_VM_PATH];      /* a bzImage on some volume              */
    char initrd[UNO_VM_PATH];      /* may be empty                          */
    char disk[UNO_VM_PATH];        /* a disk image; may be empty            */
    unsigned mem_mb;               /* what it is ALLOWED, bounded by the carve */
    int net;                       /* 1 = give it a network device          */
} uno_vm_def;

/* The registry.  Held in memory, written to EFI\UNODOS\VM\VMS.CFG on change,
 * and read back at first use - so a VM the user made is still there after a
 * reboot, which is most of what "create a VM" means. */
int  uno_vm_count(void);
const uno_vm_def *uno_vm_get(int i);         /* NULL when i is out of range */
int  uno_vm_add(const uno_vm_def *d);        /* index, or -1 when full      */
int  uno_vm_set(int i, const uno_vm_def *d); /* reconfigure                 */
int  uno_vm_del(int i);
int  uno_vm_save(void);                      /* 1 = written                 */

/* Lifecycle.  `start` places the kernel and arms the frame loop; it fails,
 * with a reason, on a machine that cannot host a guest at all. */
int  uno_vm_start(int i);
void uno_vm_stop(void);
int  uno_vm_running(void);                   /* index, or -1                */
const char *uno_vm_status(void);             /* one line, always safe       */

/* The console.  A ring of whole lines, because that is what the guest's
 * serial port produces and what a viewer wants; `seq` changes whenever
 * anything new arrives, so a viewer can redraw only when there is a reason. */
int  uno_vm_con_lines(void);
const char *uno_vm_con_line(int i);          /* 0 = oldest still held       */
unsigned uno_vm_con_seq(void);
void uno_vm_con_key(int ch);                 /* type INTO the guest         */
void uno_vm_con_clear(void);

/* Fed by the backend's serial sink; not for consumers. */
void uno_vm_con_push(const char *line);

/* What the running VM's files are, for the loader.  Empty string = the
 * built-in default, which is what every guest before A8 used. */
const char *uno_vm_path_kernel(void);
const char *uno_vm_path_initrd(void);
const char *uno_vm_path_disk(void);

#endif /* UNO_VIRT_MGR_H */
