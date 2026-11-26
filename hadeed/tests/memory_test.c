#include "memory.h"
#include <stdio.h>
#include <stdlib.h>

#define PAGE HADEED_PAGE_SIZE
#define BASE (2UL * 1024 * 1024)
#define LIMIT (HADEED_MAX_PHYS_PAGES * PAGE)
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static void overlap_and_rounding(void)
{
    const struct e820_entry available = {BASE - PAGE, 9 * PAGE, 1, 1};
    const struct e820_entry reserved = {BASE + 3 * PAGE - 1, 2, 2, 1};
    struct e820_entry map[] = {
        available, reserved, {BASE, 8 * PAGE, 2, 0}, {BASE + PAGE, 0, 3, 1},
        {BASE + 8 * PAGE, PAGE, 1, 0}, {LIMIT, ~0UL, 1, 1}
    };
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned seen = 0;
        CHECK(memory_init_map(map, 6) == 6);
        for (unsigned i = 0; i < 6; ++i) {
            u64 addr = page_alloc(0);
            CHECK(addr >= BASE && addr < BASE + 8 * PAGE);
            unsigned bit = 1U << ((addr - BASE) / PAGE);
            CHECK((seen & bit) == 0);
            seen |= bit;
        }
        CHECK(seen == 0xf3U && page_alloc(0) == 0);
        map[0] = reserved;
        map[1] = available;
    }
    const struct e820_entry partial = {BASE + 1, 3 * PAGE - 2, 1, 1};
    CHECK(memory_init_map(&partial, 1) == 1);
    CHECK(page_alloc(0) == BASE + PAGE);
    CHECK(page_alloc(0) == 0);
}

static void address_limits(void)
{
    struct e820_entry map[] = {
        {LIMIT - 4 * PAGE, ~0UL, 1, 1}, {LIMIT - PAGE - 1, ~0UL, 2, 1}
    };
    CHECK(memory_init_map(map, 2) == 2);
    CHECK(page_alloc(1) == LIMIT - 4 * PAGE);
    CHECK(page_alloc(0) == 0);
    struct e820_entry full[E820_MAX_ENTRIES] = {{BASE, PAGE, 1, 1}};
    CHECK(memory_init_map(full, E820_MAX_ENTRIES) == 1);
    CHECK(memory_init_map(full, E820_MAX_ENTRIES + 1U) == 0);
    CHECK(page_alloc(0) == 0);
    CHECK(memory_init_map(NULL, 1) == 0);
    CHECK(memory_init_map(full, 0) == 0);
}

static void allocation_lifetime(void)
{
    const struct e820_entry map = {BASE, 16 * PAGE, 1, 1};
    CHECK(memory_init_map(&map, 1) == 16);
    CHECK(page_alloc(4) == BASE);
    page_free(BASE, 3);
    page_free(BASE + PAGE, 0);
    page_free(BASE - PAGE, 0);
    CHECK(page_alloc(0) == 0);
    page_free(BASE, 4);
    page_free(BASE, 4);
    CHECK(page_alloc(3) == BASE);
    CHECK(page_alloc(3) == BASE + 8 * PAGE);
    CHECK(page_alloc(0) == 0);
    page_free(BASE + 8 * PAGE, 3);
    page_free(BASE, 3);
    CHECK(page_alloc(4) == BASE);
    CHECK(page_alloc(0) == 0);
    CHECK(pages_to_order(0) > HADEED_MAX_ORDER);
    CHECK(pages_to_order(HADEED_MAX_PHYS_PAGES + 1) > HADEED_MAX_ORDER);
    CHECK(page_alloc(HADEED_MAX_ORDER + 1) == 0);
}

int main(void)
{
    overlap_and_rounding();
    address_limits();
    allocation_lifetime();
    puts("E820 overlap, address limits and buddy allocation lifetime passed");
    return 0;
}
