/* MicroPython HAL for UnoDOS pc64. Timing over the kernel TickCount (~60 Hz),
 * stdout to a ring buffer the host (Studio) surfaces.
 *
 * py/mphal.h #include's this first, then declares mp_hal_* only where the port
 * has not (#ifndef guards).  So: the ticks helpers are provided inline here
 * (macro-guarded so mphal.h skips its prototypes); the rest (stdout_tx_*,
 * delay_ms/us) are DEFINED in pc64_upy_port.c / the host driver with the
 * signatures mphal.h declares. */
#ifndef PC64_MPHALPORT_H
#define PC64_MPHALPORT_H

/* the kernel's 60 Hz tick (exported); on the host, a monotonic stub */
long uno_upy_ticks(void);

#define mp_hal_ticks_ms  mp_hal_ticks_ms
static inline mp_uint_t mp_hal_ticks_ms(void)  { return (mp_uint_t)(uno_upy_ticks() * 1000 / 60); }
#define mp_hal_ticks_us  mp_hal_ticks_us
static inline mp_uint_t mp_hal_ticks_us(void)  { return (mp_uint_t)(uno_upy_ticks() * 1000000 / 60); }
#define mp_hal_ticks_cpu mp_hal_ticks_cpu
static inline mp_uint_t mp_hal_ticks_cpu(void) { return 0; }

static inline void mp_hal_set_interrupt_char(char c) { (void)c; }

#endif
