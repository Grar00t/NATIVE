#include "memory.h"

#define E820_INFO_ADDR 0x5000UL
#define E820_ENTRIES_ADDR 0x5010UL
#define E820_USABLE 1U
#define LOW_RESERVED_END 0x00200000UL
#define PML4_ADDR 0x00009000UL
#define PTE_PRESENT 0x001UL
#define PTE_WRITE 0x002UL
#define PTE_PS 0x080UL
#define PTE_ADDR_MASK 0x000ffffffffff000UL
#define PT_POOL_TABLES 24U
#define BUDDY_TOTAL_BITS 131071UL

__attribute__((used, section(".rodata.m4")))
const u64 m4_e820_info_addr = E820_INFO_ADDR;
__attribute__((used, section(".rodata.m4")))
const u64 m4_e820_entries_addr = E820_ENTRIES_ADDR;

__attribute__((used, section(".rodata.m4")))
const u32 buddy_order_offsets[HADEED_MAX_ORDER + 2U] = {
    0U, 65536U, 98304U, 114688U, 122880U, 126976U,
    129024U, 130048U, 130560U, 130816U, 130944U, 131008U,
    131040U, 131056U, 131064U, 131068U, 131070U, 131071U
};

__attribute__((used, section(".m4meta"), aligned(64)))
u8 buddy_free_bitmap[16384];

__attribute__((section(".m4meta"), aligned(64)))
static u8 buddy_allocated_bitmap[sizeof buddy_free_bitmap];

__attribute__((used, section(".m4meta"), aligned(64)))
u8 page_usable_bitmap[8192];

__attribute__((used, section(".m4pt"), aligned(4096)))
static u64 page_table_pool[PT_POOL_TABLES][512];

__attribute__((section(".m4meta")))
static u32 page_table_pool_used;

static void bytes_zero(u8 *p, usize n)
{
    usize i;
    for (i = 0UL; i < n; ++i) p[i] = 0U;
}

static int bit_get(const u8 *map, u64 bit)
{
    return (map[bit >> 3] >> (bit & 7UL)) & 1U;
}

static void bit_set(u8 *map, u64 bit)
{
    map[bit >> 3] = (u8)(map[bit >> 3] | (u8)(1U << (bit & 7UL)));
}

static void bit_clear(u8 *map, u64 bit)
{
    map[bit >> 3] = (u8)(map[bit >> 3] & (u8)~(u8)(1U << (bit & 7UL)));
}

static u64 buddy_bit(u32 order, u64 page)
{
    return (u64)buddy_order_offsets[order] + (page >> order);
}

static int buddy_is_free(u64 page, u32 order)
{
    return bit_get(buddy_free_bitmap, buddy_bit(order, page));
}

static void buddy_mark_free(u64 page, u32 order)
{
    bit_set(buddy_free_bitmap, buddy_bit(order, page));
}

static void buddy_mark_used(u64 page, u32 order)
{
    bit_clear(buddy_free_bitmap, buddy_bit(order, page));
}

static void add_usable_run(u64 start_page, u64 end_page)
{
    u64 page = start_page;
    while (page < end_page) {
        u32 order = 0U;
        while (order < HADEED_MAX_ORDER) {
            const u64 next_pages = 1UL << (order + 1U);
            if ((page & (next_pages - 1UL)) != 0UL || page + next_pages > end_page) break;
            ++order;
        }
        buddy_mark_free(page, order);
        page += 1UL << order;
    }
}

/* Only complete usable pages may be allocated. Any reserved byte excludes a page. */
static void apply_e820(const struct e820_entry *entry, int usable)
{
    const u64 limit = HADEED_MAX_PHYS_PAGES * HADEED_PAGE_SIZE;
    const u64 mask = HADEED_PAGE_SIZE - 1UL;
    u64 end, first, last, page;

    if ((entry->attributes & 1U) == 0U || entry->length == 0UL || entry->base >= limit)
        return;
    end = entry->length > limit - entry->base ? limit : entry->base + entry->length;
    first = (entry->base + (usable ? mask : 0UL)) / HADEED_PAGE_SIZE;
    last = (end + (usable ? 0UL : mask)) / HADEED_PAGE_SIZE;
    for (page = first; page < last; ++page) {
        if (usable) bit_set(page_usable_bitmap, page);
        else bit_clear(page_usable_bitmap, page);
    }
}

u64 memory_init(void)
{
    const volatile u32 *const info = (const volatile u32 *)E820_INFO_ADDR;
    const struct e820_entry *entries = (const struct e820_entry *)E820_ENTRIES_ADDR;
    return memory_init_map(entries, info[1] == sizeof *entries ? info[0] : 0U);
}

u64 memory_init_map(const struct e820_entry *entries, u32 count)
{
    u64 usable_pages = 0UL;
    u64 page;
    u32 i;

    bytes_zero(buddy_free_bitmap, sizeof buddy_free_bitmap);
    bytes_zero(buddy_allocated_bitmap, sizeof buddy_allocated_bitmap);
    bytes_zero(page_usable_bitmap, sizeof page_usable_bitmap);
    page_table_pool_used = 0U;
    if (!entries || count > E820_MAX_ENTRIES) return 0UL;

    for (i = 0U; i < count; ++i) {
        if (entries[i].type == E820_USABLE) apply_e820(&entries[i], 1);
    }
    for (i = 0U; i < count; ++i) {
        if (entries[i].type != E820_USABLE) apply_e820(&entries[i], 0);
    }

    for (page = 0UL; page < LOW_RESERVED_END / HADEED_PAGE_SIZE; ++page) {
        bit_clear(page_usable_bitmap, page);
    }

    page = LOW_RESERVED_END / HADEED_PAGE_SIZE;
    while (page < HADEED_MAX_PHYS_PAGES) {
        u64 run_start;
        if (!bit_get(page_usable_bitmap, page)) {
            ++page;
            continue;
        }
        run_start = page;
        while (page < HADEED_MAX_PHYS_PAGES && bit_get(page_usable_bitmap, page)) {
            ++usable_pages;
            ++page;
        }
        add_usable_run(run_start, page);
    }

    return usable_pages;
}

u32 pages_to_order(u64 pages)
{
    u32 order = 0U;
    u64 block = 1UL;
    if (pages == 0UL) return HADEED_MAX_ORDER + 1U;
    while (block < pages && order < HADEED_MAX_ORDER) {
        block <<= 1;
        ++order;
    }
    if (block < pages) return HADEED_MAX_ORDER + 1U;
    return order;
}

__attribute__((noinline)) u64 page_alloc(u32 order)
{
    u32 current;
    if (order > HADEED_MAX_ORDER) return 0UL;
    for (current = order; current <= HADEED_MAX_ORDER; ++current) {
        const u64 blocks = HADEED_MAX_PHYS_PAGES >> current;
        u64 block;
        for (block = 0UL; block < blocks; ++block) {
            const u64 page = block << current;
            if (!buddy_is_free(page, current)) continue;
            buddy_mark_used(page, current);
            while (current > order) {
                const u64 split_page = page + (1UL << (current - 1U));
                --current;
                buddy_mark_free(split_page, current);
            }
            bit_set(buddy_allocated_bitmap, buddy_bit(order, page));
            return page * HADEED_PAGE_SIZE;
        }
    }
    return 0UL;
}

__attribute__((noinline)) void page_free(u64 physical_page, u32 order)
{
    u64 page;
    if (order > HADEED_MAX_ORDER || (physical_page & (HADEED_PAGE_SIZE - 1UL)) != 0UL) return;
    page = physical_page / HADEED_PAGE_SIZE;
    if (page >= HADEED_MAX_PHYS_PAGES || (page & ((1UL << order) - 1UL)) != 0UL) return;

    if (!bit_get(buddy_allocated_bitmap, buddy_bit(order, page))) return;
    bit_clear(buddy_allocated_bitmap, buddy_bit(order, page));

    while (order < HADEED_MAX_ORDER) {
        const u64 buddy = page ^ (1UL << order);
        if (buddy >= HADEED_MAX_PHYS_PAGES || !buddy_is_free(buddy, order)) break;
        buddy_mark_used(buddy, order);
        if (buddy < page) page = buddy;
        ++order;
    }
    buddy_mark_free(page, order);
}

static u64 *new_table(void)
{
    u64 *table = page_table_pool[page_table_pool_used++];
    bytes_zero((u8 *)table, HADEED_PAGE_SIZE);
    return table;
}

__attribute__((noinline)) int map_pages_4k(u64 physical_page, u64 virtual_address, usize count)
{
    const u64 first = (virtual_address >> 12) & 511UL;
    const u64 top = virtual_address >> 47;
    u64 *table = (u64 *)PML4_ADDR;
    unsigned shift;
    usize i;

    if (count == 0 || count > 512UL - first || (top != 0 && top != 0x1ffffUL) ||
        (virtual_address & (HADEED_PAGE_SIZE - 1UL)) != 0 ||
        (physical_page & ~PTE_ADDR_MASK) != 0 ||
        count - 1UL > (PTE_ADDR_MASK - physical_page) / HADEED_PAGE_SIZE) return -1;

    for (shift = 39; shift >= 21; shift -= 9) {
        u64 *slot = &table[(virtual_address >> shift) & 511UL];
        if ((*slot & PTE_PRESENT) != 0) {
            if ((*slot & PTE_PS) != 0 || (*slot & PTE_WRITE) == 0) return -1;
            table = (u64 *)(usize)(*slot & PTE_ADDR_MASK);
            continue;
        }
        /* Below the first absent entry the rest of the subtree is empty. */
        if ((shift - 12U) / 9U > PT_POOL_TABLES - page_table_pool_used) return -1;
        do {
            table = new_table();
            *slot = (u64)(usize)table | PTE_PRESENT | PTE_WRITE;
            shift -= 9U;
            slot = &table[(virtual_address >> shift) & 511UL];
        } while (shift > 12U);
        break;
    }

    for (i = 0; i < count; ++i) {
        u64 entry = table[first + i];
        u64 expected = physical_page + i * HADEED_PAGE_SIZE;
        if ((entry & PTE_PRESENT) && ((entry & PTE_ADDR_MASK) != expected || !(entry & PTE_WRITE)))
            return -1;
    }
    for (i = 0; i < count; ++i) {
        u64 va = virtual_address + i * HADEED_PAGE_SIZE;
        if (!(table[first + i] & PTE_PRESENT)) {
            table[first + i] = (physical_page + i * HADEED_PAGE_SIZE) | PTE_PRESENT | PTE_WRITE;
            __asm__ volatile ("invlpg (%0)" : : "r" (va) : "memory");
        }
    }
    return 0;
}

int map_page_4k(u64 physical_page, u64 virtual_address)
{
    return map_pages_4k(physical_page, virtual_address, 1);
}
