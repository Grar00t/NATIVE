#ifndef HADEED_MEMORY_H
#define HADEED_MEMORY_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned long usize;

#define HADEED_PAGE_SIZE 4096UL
#define HADEED_MAX_ORDER 16U
#define HADEED_MAX_PHYS_PAGES 65536UL

extern const u32 buddy_order_offsets[HADEED_MAX_ORDER + 2U];
extern u8 buddy_free_bitmap[16384];
extern u8 page_usable_bitmap[8192];

#define E820_MAX_ENTRIES 128U
struct e820_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 attributes;
};
_Static_assert(sizeof(struct e820_entry) == 24, "E820 descriptor size");

u64 memory_init(void);
u64 memory_init_map(const struct e820_entry *entries, u32 count);
u32 pages_to_order(u64 pages);
__attribute__((noinline)) u64 page_alloc(u32 order);
__attribute__((noinline)) void page_free(u64 physical_page, u32 order);
/* Map a contiguous span within one 512-entry leaf table. */
int map_pages_4k(u64 physical_page, u64 virtual_address, usize count);
__attribute__((noinline)) int map_page_4k(u64 physical_page, u64 virtual_address);

#endif
