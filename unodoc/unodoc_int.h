/* ===========================================================================
 * unodoc internals - shared between the core and the per-format files, not
 * part of the public contract (unodoc.h is).  Nothing here is stable.
 * ======================================================================== */
#ifndef UNODOC_INT_H
#define UNODOC_INT_H

#include <stdint.h>

/* ---- little-endian scalar reads out of a byte buffer -----------------------
 * Every Office binary format is little-endian.  These take a byte pointer,
 * never a cast struct: the on-disk records are unaligned. */
static inline uint16_t ud_rd16(const unsigned char *p)
{ return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static inline uint32_t ud_rd32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static inline uint64_t ud_rd64(const unsigned char *p)
{ return (uint64_t)ud_rd32(p) | ((uint64_t)ud_rd32(p + 4) << 32); }

static inline void ud_wr16(unsigned char *p, uint16_t v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static inline void ud_wr32(unsigned char *p, uint32_t v)
{ p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

static inline void ud_wr64(unsigned char *p, uint64_t v)
{ ud_wr32(p, (uint32_t)v); ud_wr32(p + 4, (uint32_t)(v >> 32)); }

/* ---- the v1 internal text encoding: CP-1252 -------------------------------
 * Office 97's own 8-bit text is CP-1252, and the OS's font engine is
 * ASCII-today / CP-1252-next (a filed request), so 8-bit CP-1252 is the
 * internal text type for phase 1-4.  These two convert at the UTF-16
 * boundary the container and the formats sit on. */

/* CP-1252 byte -> Unicode code point (BMP).  Only 0x80..0x9F differ from
 * Latin-1; the undefined slots there are passed through as C1 controls so
 * the mapping is total and round-trips. */
uint16_t ud_cp1252_to_uc(unsigned char b);

/* Unicode code point -> CP-1252 byte, or '?' when it has no CP-1252 form. */
unsigned char ud_uc_to_cp1252(uint16_t uc);

/* Uppercase a UTF-16 code unit the way CFB's directory ordering does.
 * Covers ASCII, Latin-1 and the cased CP-1252 specials; everything else is
 * returned unchanged (see UNODOC.md - this is exact for every name Office
 * generates, which is what the ordering rule is checked against). */
uint16_t ud_upper16(uint16_t uc);

#endif /* UNODOC_INT_H */
