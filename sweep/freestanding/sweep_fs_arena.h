/* sweep_fs_arena.h - dependency-free arena allocator for freestanding Sweep.
 *
 * The hosted build uses malloc; a freestanding target cannot.  This arena is a
 * bounded first-fit allocator over caller-owned storage.  It implements exactly
 * the SWEEP_RUNTIME alloc/realloc/free contract (realloc(p, 0) frees, alloc(0)
 * returns NULL, failure leaves the original block untouched) and never touches
 * memory outside the supplied buffer.
 */
#ifndef SWEEP_FS_ARENA_H
#define SWEEP_FS_ARENA_H

#include <stdbool.h>
#include <stddef.h>

typedef struct _SWEEP_FS_ARENA {
    unsigned char* base;   /* start of caller storage */
    size_t         bytes;  /* total usable storage */
    size_t         used;   /* currently allocated payload bytes */
    size_t         peak;   /* high-water mark of used */
} SWEEP_FS_ARENA;

/* Initialize over [buffer, buffer+size).  buffer must be non-NULL, size must
 * be large enough for one block, and buffer should be 16-byte aligned. */
bool sweep_fs_arena_init(SWEEP_FS_ARENA* arena, void* buffer, size_t size);

/* SWEEP_RUNTIME operation implementations (user points at SWEEP_FS_ARENA). */
void* sweep_fs_arena_alloc(void* user, size_t size);
void* sweep_fs_arena_realloc(void* user, void* ptr, size_t size);
void  sweep_fs_arena_free(void* user, void* ptr);

#endif /* SWEEP_FS_ARENA_H */
