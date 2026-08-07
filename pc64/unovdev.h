/* unovdev - virtio-mmio transport + device models.  See unovdev.c.
 *
 * The seam is deliberately one function: a backend that has decoded a guest
 * MMIO access hands over an address, a direction and a size, and gets an
 * answer.  Nothing here knows what a VMCS or a VMCB is. */
#ifndef UNO_VDEV_H
#define UNO_VDEV_H

#define UNO_VDEV_API 2

/* One descriptor's buffer, already bounds-checked into an address this
 * machine may touch.  `write` is the DEVICE's direction: 1 = the device
 * fills it (a block read's data, a status byte), 0 = the guest filled it. */
typedef struct {
    unsigned char *p;
    unsigned       len;
    int            write;
} uno_vseg;

/* 1 = this address belongs to a device and *val has been read or written. */
int uno_vdev_mmio(unsigned long long gpa, int is_write, unsigned size,
                  unsigned long long *val);

/* Is a virtio device wired to this PIC line asking for attention?  A level:
 * it stays up until the driver writes InterruptACK. */
int uno_vdev_mmio_irq_level(int irq);

/* The block device's backing file and what it has served, for the status
 * block: "1024 KB ro on vol 1, 12 reads 340 sectors" or "no ROOTFS.IMG". */
int uno_vdev_blk_str(char *buf, int cap);

/* The register traffic, one transaction per call: dev<<45 | write<<44 |
 * off<<32 | value, 0 past the end.  A driver reports its conclusion ("device
 * refuses features"); this is what it actually saw. */
unsigned long long uno_vdev_dbg_entry(int i);

void uno_vdev_reset(void);

/* Port I/O.  Easier than MMIO on x86: the exit carries the port, the size and
 * the direction, so there is nothing to decode.  `sink` is called with each
 * completed line the guest writes to COM1 - which is where a kernel talks
 * before it has a driver for anything. */
int uno_vdev_pio(unsigned port, int is_write, unsigned size,
                 unsigned long long *val, void (*sink)(const char *));
int uno_vdev_serial_chars(void);

/* The receive half: push a byte the guest will read from COM1, or queue a
 * string handed over when the driver arms the receive interrupt. */
void uno_vdev_serial_push(int c);
void uno_vdev_serial_seed(const char *s);
unsigned long long uno_vdev_base(void);

/* The 8259 pair's answer to "may a vector be injected right now".  `pending`
 * peeks; `take` commits (ISR up, an edge source marks itself delivered) and
 * returns the vector THE GUEST programmed in ICW2, or -1.  The caller checks
 * the guest can accept (IF set, no interrupt shadow) BEFORE taking: a taken
 * vector is owed an injection. */
int uno_vdev_irq_pending(void);
int uno_vdev_irq_take(void);

/* LCR | IER<<8 | MCR<<16 | master IMR<<24.  A guest that has gone quiet is
 * either not writing or writing somewhere nobody is listening, and these are
 * the registers that decide which. */
unsigned uno_vdev_pc_state(void);

/* IRR | ISR<<8 | IMR<<16 | inited<<24 | icw<<28 | ch0 mode<<32 |
 * ch0 reload<<40 | armed<<56 | oneshot_done<<57. */
unsigned long long uno_vdev_pic_state(void);

/* Place a queue's rings on the guest's behalf (A5's guest is eighteen
 * instructions; A6's guest is Linux and does this itself through the
 * registers). */
void uno_vdev_queue(unsigned long long desc, unsigned long long avail,
                    unsigned long long used, unsigned qnum);

/* What the device has consumed: the bytes, how many notifications, how many
 * descriptor bytes it walked. */
const char *uno_vdev_output(int *n, int *notifies, int *bytes);

/* Build a self-referential descriptor chain and walk it.  1 = the walk
 * refused and RETURNED, which is the only interesting outcome: a device that
 * trusts a chain to terminate hangs the machine on request. */
int uno_vdev_cycle_refused(unsigned long long desc_gpa, unsigned qnum);

#endif /* UNO_VDEV_H */
