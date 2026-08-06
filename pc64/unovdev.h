/* unovdev - virtio-mmio transport + device models.  See unovdev.c.
 *
 * The seam is deliberately one function: a backend that has decoded a guest
 * MMIO access hands over an address, a direction and a size, and gets an
 * answer.  Nothing here knows what a VMCS or a VMCB is. */
#ifndef UNO_VDEV_H
#define UNO_VDEV_H

#define UNO_VDEV_API 1

/* 1 = this address belongs to a device and *val has been read or written. */
int uno_vdev_mmio(unsigned long long gpa, int is_write, unsigned size,
                  unsigned long long *val);

void uno_vdev_reset(void);
unsigned long long uno_vdev_base(void);

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
