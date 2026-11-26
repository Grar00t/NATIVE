#include "heap.h"

#define HEAP_BASE 0xffff800040000000UL
#define HEAP_ARENA_ORDER 4U
#define HEAP_ARENA_PAGES (1UL << HEAP_ARENA_ORDER)
#define HEAP_ARENA_BYTES (HEAP_ARENA_PAGES * HADEED_PAGE_SIZE)
#define HEAP_MAX_ARENAS 4U
#define HEAP_TAG_GUARD 0x4841444545444d34UL
#define HEAP_USED 1UL
#define HEAP_TAG_BYTES 16UL
#define HEAP_OVERHEAD (HEAP_TAG_BYTES * 2UL)
#define HEAP_MIN_BLOCK 48UL

struct heap_tag { u64 size_flags; u64 guard; };
struct heap_arena { u64 physical; u64 virtual_base; };

__attribute__((used, section(".m4meta")))
static struct heap_arena heap_arenas[HEAP_MAX_ARENAS];
__attribute__((section(".m4meta")))
static u32 heap_arena_count;

void heap_init(void)
{
    u32 i;
    heap_arena_count = 0U;
    for (i = 0U; i < HEAP_MAX_ARENAS; ++i) {
        heap_arenas[i].physical = 0UL;
        heap_arenas[i].virtual_base = 0UL;
    }
}

static u64 align16(u64 value) { return (value + 15UL) & ~15UL; }
static u64 tag_size(const struct heap_tag *tag) { return tag->size_flags & ~15UL; }
static int tag_used(const struct heap_tag *tag) { return (tag->size_flags & HEAP_USED) != 0UL; }
static int valid_block(u64 address, u64 end)
{
    const struct heap_tag *head = (const struct heap_tag *)(usize)address;
    const struct heap_tag *foot;
    u64 size;

    if (end - address < HEAP_MIN_BLOCK || head->guard != HEAP_TAG_GUARD ||
        (head->size_flags & 14UL) != 0) return 0;
    size = tag_size(head);
    if (size < HEAP_MIN_BLOCK || size > end - address) return 0;
    foot = (const struct heap_tag *)(usize)(address + size - HEAP_TAG_BYTES);
    return foot->guard == HEAP_TAG_GUARD && foot->size_flags == head->size_flags;
}

static void write_tags(u64 address, u64 size, int used)
{
    struct heap_tag *const head = (struct heap_tag *)(usize)address;
    struct heap_tag *const foot = (struct heap_tag *)(usize)(address + size - HEAP_TAG_BYTES);
    const u64 flags = size | (used ? HEAP_USED : 0UL);
    head->size_flags = flags; head->guard = HEAP_TAG_GUARD;
    foot->size_flags = flags; foot->guard = HEAP_TAG_GUARD;
}

__attribute__((noinline)) static int new_arena(void)
{
    u64 physical;
    u64 virtual_base;
    if (heap_arena_count >= HEAP_MAX_ARENAS) return -1;
    physical = page_alloc(HEAP_ARENA_ORDER);
    if (physical == 0UL) return -1;
    virtual_base = HEAP_BASE + (u64)heap_arena_count * HEAP_ARENA_BYTES;
    if (map_pages_4k(physical, virtual_base, HEAP_ARENA_PAGES) != 0) {
        page_free(physical, HEAP_ARENA_ORDER);
        return -1;
    }
    heap_arenas[heap_arena_count].physical = physical;
    heap_arenas[heap_arena_count].virtual_base = virtual_base;
    ++heap_arena_count;
    write_tags(virtual_base, HEAP_ARENA_BYTES, 0);
    return 0;
}

static void *alloc_from_arena(u64 base, u64 bytes)
{
    u64 offset = 0UL;
    const u64 need = align16(bytes) + HEAP_OVERHEAD;
    while (offset < HEAP_ARENA_BYTES) {
        struct heap_tag *const head = (struct heap_tag *)(usize)(base + offset);
        if (!valid_block(base + offset, base + HEAP_ARENA_BYTES)) return (void *)0;
        const u64 size = tag_size(head);
        if (!tag_used(head) && size >= need) {
            const u64 remain = size - need;
            if (remain >= HEAP_MIN_BLOCK) {
                write_tags(base + offset, need, 1);
                write_tags(base + offset + need, remain, 0);
            } else {
                write_tags(base + offset, size, 1);
            }
            return (void *)(usize)(base + offset + HEAP_TAG_BYTES);
        }
        offset += size;
    }
    return (void *)0;
}

__attribute__((noinline)) void *kmalloc(usize bytes)
{
    u32 i;
    void *p;
    if (bytes == 0UL || bytes > HEAP_ARENA_BYTES - HEAP_OVERHEAD) return (void *)0;
    for (i = 0U; i < heap_arena_count; ++i) {
        p = alloc_from_arena(heap_arenas[i].virtual_base, (u64)bytes);
        if (p != (void *)0) return p;
    }
    if (new_arena() != 0) return (void *)0;
    return alloc_from_arena(heap_arenas[heap_arena_count - 1U].virtual_base, (u64)bytes);
}

static int arena_for_pointer(u64 p, u32 *index)
{
    u32 i;
    for (i = 0U; i < heap_arena_count; ++i) {
        const u64 base = heap_arenas[i].virtual_base;
        if (p >= base + HEAP_TAG_BYTES && p < base + HEAP_ARENA_BYTES) { *index = i; return 1; }
    }
    return 0;
}

__attribute__((noinline)) void kfree(void *ptr)
{
    const u64 p = (u64)(usize)ptr;
    u32 index;
    u64 head, end, previous = 0UL;

    if (p == 0 || (p & 15UL) || !arena_for_pointer(p, &index)) return;
    head = heap_arenas[index].virtual_base;
    end = head + HEAP_ARENA_BYTES;
    while (head < end && valid_block(head, end)) {
        const struct heap_tag *tag = (const struct heap_tag *)(usize)head;
        u64 next = head + tag_size(tag);
        if (head + HEAP_TAG_BYTES == p) {
            if (!tag_used(tag)) return;
            if (next < end) {
                const struct heap_tag *neighbor = (const struct heap_tag *)(usize)next;
                if (!valid_block(next, end)) return;
                if (!tag_used(neighbor)) next += tag_size(neighbor);
            }
            if (previous != 0UL && !tag_used((const struct heap_tag *)(usize)previous))
                head = previous;
            write_tags(head, next - head, 0);
            return;
        }
        if (p < next) return;
        previous = head;
        head = next;
    }
}
