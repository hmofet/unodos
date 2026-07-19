/* acpi_arena.h - fixed-arena free-list heap backing uACPI's allocator.
 *
 * Shared verbatim between Writer's Unlock and UnoDOS.  uACPI frees intermediate
 * AML objects all through namespace build + evaluation, so a never-free bump
 * heap grows without bound - it needs a REAL alloc/free.  This is a compact
 * boundary-tag (Knuth) allocator over a caller-provided region: next-fit search,
 * two-way coalescing, and a single-argument free (uACPI is built without
 * UACPI_SIZED_FREES, so each block recovers its own size from its boundary tag).
 *
 * The region is supplied by the host (e.g. UEFI AllocatePool, or a static BSS
 * buffer on bare metal) so this file pulls in no platform headers.
 *
 * Writer's Unlock additionally calls acpi_arena_reset() after each battery poll
 * to keep steady-state memory bounded regardless of interpreter churn.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Initialise the arena over [buf, buf+len).  The usable region is rounded to an
 * 8-byte-aligned sub-span of what is passed.  Returns 1 on success, 0 if the
 * region is too small to hold even one minimum block. */
int    acpi_arena_init(void *buf, size_t len);

/* Allocate/free.  Payloads are 8-byte aligned.  alloc returns NULL when out of
 * space (or on a 0-byte request); free(NULL) is a no-op. */
void  *acpi_arena_alloc(size_t n);
void   acpi_arena_free(void *p);

/* Drop every allocation and re-establish one big free block.  O(1).  Any pointer
 * previously returned by acpi_arena_alloc() is invalid afterwards. */
void   acpi_arena_reset(void);

/* 1 once acpi_arena_init() has succeeded. */
int    acpi_arena_ready(void);

/* Diagnostics: bytes currently handed out (payload + per-block tags), the
 * all-time-high of that figure, and the total usable region size.  Any pointer
 * may be NULL. */
void   acpi_arena_stats(size_t *used, size_t *peak, size_t *total);
