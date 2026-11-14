/* sweep_fs_arena.c - bounded first-fit arena allocator (freestanding). */
#include "sweep_fs_arena.h"

#define FS_BLOCK_MAGIC 0x5357424Bu /* 'SWBK' */
#define FS_ALIGN        16u
#define FS_HEADER_SIZE  ((sizeof(FS_BLOCK) + (FS_ALIGN - 1u)) & ~(size_t) (FS_ALIGN - 1u))

typedef struct _FS_BLOCK {
    size_t           size;   /* payload bytes, excludes header */
    struct _FS_BLOCK* next;  /* next block in arena order, NULL at end */
    unsigned int     magic;
    unsigned int     free;   /* 1 = available */
} FS_BLOCK;

static size_t align_up(size_t n) {
    if (n > (size_t) -1 - (FS_ALIGN - 1u)) return (size_t) -1;
    return (n + (FS_ALIGN - 1u)) & ~(size_t) (FS_ALIGN - 1u);
}

static unsigned char* block_payload(FS_BLOCK* b) {
    return (unsigned char*) b + FS_HEADER_SIZE;
}

static FS_BLOCK* payload_block(void* ptr) {
    return (FS_BLOCK*) ((unsigned char*) ptr - FS_HEADER_SIZE);
}

static bool block_valid(const SWEEP_FS_ARENA* a, FS_BLOCK* b) {
    unsigned char* p = (unsigned char*) b;
    if (a == NULL || a->base == NULL || p < a->base) return false;
    if (p > a->base + a->bytes) return false;
    if ((size_t) (a->base + a->bytes - p) < FS_HEADER_SIZE) return false;
    return b->magic == FS_BLOCK_MAGIC && b->size <= a->bytes;
}

static void split_block(SWEEP_FS_ARENA* a, FS_BLOCK* b, size_t want) {
    unsigned char* tail;
    FS_BLOCK* nb;
    size_t remain;

    if (b->size < want) return;
    remain = b->size - want;
    if (remain < FS_HEADER_SIZE + FS_ALIGN) return;

    tail = block_payload(b) + want;
    nb = (FS_BLOCK*) tail;
    nb->size = remain - FS_HEADER_SIZE;
    nb->next = b->next;
    nb->magic = FS_BLOCK_MAGIC;
    nb->free = 1u;
    b->size = want;
    b->next = nb;
    (void) a;
}

bool sweep_fs_arena_init(SWEEP_FS_ARENA* arena, void* buffer, size_t size) {
    FS_BLOCK* b;

    if (arena == NULL || buffer == NULL) return false;
    if (size < FS_HEADER_SIZE + FS_ALIGN) return false;
    size &= ~(size_t) (FS_ALIGN - 1u);
    arena->base = (unsigned char*) buffer;
    arena->bytes = size;
    arena->used = 0u;
    arena->peak = 0u;

    b = (FS_BLOCK*) buffer;
    b->size = size - FS_HEADER_SIZE;
    b->next = NULL;
    b->magic = FS_BLOCK_MAGIC;
    b->free = 1u;
    return true;
}

void* sweep_fs_arena_alloc(void* user, size_t size) {
    SWEEP_FS_ARENA* a = (SWEEP_FS_ARENA*) user;
    FS_BLOCK* b;
    size_t want;

    if (a == NULL || a->base == NULL || size == 0u) return NULL;
    want = align_up(size);
    if (want == (size_t) -1) return NULL;

    for (b = (FS_BLOCK*) a->base; b != NULL; b = b->next) {
        if (!block_valid(a, b)) return NULL;
        if (b->free && b->size >= want) {
            split_block(a, b, want);
            b->free = 0u;
            a->used += b->size;
            if (a->used > a->peak) a->peak = a->used;
            return block_payload(b);
        }
    }
    return NULL;
}

void sweep_fs_arena_free(void* user, void* ptr) {
    SWEEP_FS_ARENA* a = (SWEEP_FS_ARENA*) user;
    FS_BLOCK* b;
    FS_BLOCK* start;
    FS_BLOCK* prev = NULL;

    if (a == NULL || ptr == NULL) return;
    b = payload_block(ptr);
    if (!block_valid(a, b) || b->free) return;

    b->free = 1u;
    a->used -= b->size;

    /* Coalesce forward. */
    while (b->next != NULL && block_valid(a, b->next) && b->next->free) {
        FS_BLOCK* next = b->next;
        b->size += FS_HEADER_SIZE + next->size;
        b->next = next->next;
    }

    /* Coalesce backward. */
    start = (FS_BLOCK*) a->base;
    while (start != b) {
        if (!block_valid(a, start)) break;
        prev = start;
        start = start->next;
    }
    if (prev != NULL && prev->free) {
        prev->size += FS_HEADER_SIZE + b->size;
        prev->next = b->next;
    }
}

void* sweep_fs_arena_realloc(void* user, void* ptr, size_t size) {
    SWEEP_FS_ARENA* a = (SWEEP_FS_ARENA*) user;
    FS_BLOCK* b;
    size_t want;

    if (ptr == NULL) return sweep_fs_arena_alloc(user, size);
    if (size == 0u) { sweep_fs_arena_free(user, ptr); return NULL; }
    if (a == NULL) return NULL;

    b = payload_block(ptr);
    if (!block_valid(a, b) || b->free) return NULL;
    want = align_up(size);
    if (want == (size_t) -1) return NULL;

    if (b->size >= want) {
        a->used -= b->size;
        split_block(a, b, want);
        a->used += b->size;
        return ptr;
    }

    {
        void* fresh = sweep_fs_arena_alloc(user, size);
        size_t i;
        unsigned char* d;
        const unsigned char* s;
        if (fresh == NULL) return NULL; /* original block untouched */
        d = (unsigned char*) fresh;
        s = (const unsigned char*) ptr;
        for (i = 0u; i < b->size && i < size; ++i) d[i] = s[i];
        sweep_fs_arena_free(user, ptr);
        return fresh;
    }
}
