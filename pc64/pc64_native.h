/* pc64_native.h - native platform services (M3): what the OS runs on once
 * ExitBootServices has cut the firmware away.  See pc64_native.c. */
#ifndef PC64_NATIVE_H
#define PC64_NATIVE_H

/* TSC time base: calibrate once while gBS->Stall is still available */
unsigned long long uno_native_rdtsc(void);
void uno_native_tsc_set(unsigned long long cycles_per_us);
int  uno_native_tsc_ok(void);
void uno_native_delay_us(unsigned long us);

/* CMOS RTC */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s);
int uno_native_rtc_write(int y, int mo, int d, int h, int mi, int s);

/* CF9 (+ i8042 pulse fallback) hard reset - does not return */
void uno_native_reset(void);

/* PS/2 i8042, polled.  present() is passive (safe while attached); init()
 * takes the controller and must only run once detached. */
int  uno_ps2_present(void);

/* what the i8042 bring-up actually bound: keyboard, mouse streaming, whether
 * the aux PORT passed its 0xA9 self-test, and the 0xF2 device id (-1 = no
 * answer). uno_ps2_present() only says the CONTROLLER answers - on a laptop
 * that is the keyboard's EC and says nothing about a mouse. */
void uno_ps2_status(int *kbd, int *aux, int *auxport, int *auxid);
int  uno_ps2_init(void);
void uno_ps2_pump(void);
int  uno_ps2_next_key(int *scan, int *uni, int *ctrl);
int  uno_ps2_mouse(int *dx, int *dy, int *btn);

#endif
