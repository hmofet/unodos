/* cosmo64/i2c.c -- polled MTK I2C for bus 4 (the AW9523 keyboard's), MT6771.
 *
 * PLACEHOLDER awaiting the vendor-kernel fact extraction (register offsets,
 * ch_offset semantics, hs_only timing, clock ungating, pinmux). Every entry
 * point fails cleanly, so the keyboard reports absent and the shell runs
 * exactly as before -- including under QEMU, which models none of this.
 */

#include "cosmo64.h"

int c64_i2c_init(void)
{
    return -1;
}

int c64_i2c_write_reg(c64_u8 dev, c64_u8 reg, c64_u8 val)
{
    (void)dev; (void)reg; (void)val;
    return -1;
}

int c64_i2c_read_reg(c64_u8 dev, c64_u8 reg)
{
    (void)dev; (void)reg;
    return -1;
}
