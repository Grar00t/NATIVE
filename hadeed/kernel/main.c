#include "heap.h"

typedef unsigned short vga_u16;

int native_tensor_check(void);
void idt_install(void);
int idt_check(void);
void pic_remap_irq0(void);
void pit_init_100hz(void);

volatile u64 g_ticks;

__attribute__((used, section(".rodata.m2"))) static const char m2_banner[] = "HADEED64";
__attribute__((used, section(".rodata.m2"))) static const char m2_debug_marker[] = "LM64\n";
__attribute__((used, section(".rodata.m4"))) static const char m4_pages_prefix[] = "USABLE_PAGES ";
__attribute__((used, section(".rodata.m4"))) static const char m4_ok_marker[] = "M4 OK\n";
__attribute__((used, section(".rodata.m4"))) static const char m4_fail_marker[] = "M4 FAIL\n";
__attribute__((used, section(".rodata.m4"))) static const u16 m4_page_counts[] = {1, 2, 3, 4, 5, 8, 16, 17, 257};

static void debug_putc(char ch)
{
    __asm__ volatile ("outb %0, $0xe9" :: "a" ((u8)ch));
}

static void debug_write(const char *s)
{
    while (*s) debug_putc(*s++);
}

static void debug_write_u64(u64 value)
{
    char digits[20];
    usize count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) debug_putc(digits[--count]);
}

__attribute__((noreturn)) static void finish(const char *marker)
{
    u32 status = marker == m4_ok_marker ? 16U : 17U;
    debug_write(marker);
    __asm__ volatile ("outl %0, %1" :: "a"(status), "Nd"((u16)0xf4));
    __asm__ volatile ("cli" ::: "memory");
    for (;;) __asm__ volatile ("hlt");
}

/* Both views refer to kernel-owned pages reserved by the linker. */
static volatile u8 mapping_probe[2][HADEED_PAGE_SIZE]
    __attribute__((section(".m4meta"), aligned(4096)));

static int check_page_mappings(void)
{
    const u64 va = 0xffff800080000000UL;
    const u64 pa = (u64)(usize)mapping_probe;
    volatile u8 *const first = (volatile u8 *)(usize)va;
    volatile u8 *const second = (volatile u8 *)(usize)(va + HADEED_PAGE_SIZE);
    if (map_page_4k(pa, va + HADEED_PAGE_SIZE) != 0) return 0;
    if (map_pages_4k(pa, va, 2) == 0) return 0;
    /* A rejected span must leave its first slot available. */
    if (map_page_4k(pa + HADEED_PAGE_SIZE, va) != 0 ||
        map_page_4k(pa + HADEED_PAGE_SIZE, va) != 0) return 0;
    first[19] = 0xa5;
    second[23] = 0x5a;
    return mapping_probe[1][19] == 0xa5 && mapping_probe[0][23] == 0x5a;
}

/* The three freed blocks and their free tail should form one full arena. */
__attribute__((noinline)) static int check_heap_reuse(u64 expected)
{
    void *whole = kmalloc(16 * HADEED_PAGE_SIZE - 32);
    if ((u64)(usize)whole != expected) return 0;
    kfree(whole);
    return 1;
}

__attribute__((noreturn)) void kmain(void)
{
    volatile vga_u16 *const vga = (volatile vga_u16 *)0xb8000;
    const usize base = 12 * 80 + 36;
    const usize count = sizeof m4_page_counts / sizeof m4_page_counts[0];
    u64 blocks[sizeof m4_page_counts / sizeof m4_page_counts[0]];
    u32 orders[sizeof m4_page_counts / sizeof m4_page_counts[0]];
    u64 usable_pages, probe, first;
    volatile u8 *a, *b, *c;
    usize i;

    for (i = 0; i < sizeof m2_banner - 1; ++i)
        vga[base + i] = (vga_u16)(0x0f00U | (u8)m2_banner[i]);
    for (i = 0; i < sizeof m2_banner - 1; ++i)
        if (vga[base + i] != (vga_u16)(0x0f00U | (u8)m2_banner[i])) finish(m4_fail_marker);
    debug_write(m2_debug_marker);
    if (g_ticks != 0) finish(m4_fail_marker);
    idt_install();
    if (!idt_check()) finish(m4_fail_marker);
    pic_remap_irq0();
    pit_init_100hz();
    while (g_ticks < 3) __asm__ volatile ("sti; hlt; cli" ::: "memory");
    debug_write("PIT_TICKS ");
    debug_write_u64(g_ticks);
    debug_putc('\n');

    usable_pages = memory_init();
    if (!usable_pages) finish(m4_fail_marker);
    debug_write(m4_pages_prefix);
    debug_write_u64(usable_pages);
    debug_putc('\n');
    for (i = 0; i < count; ++i) {
        orders[i] = pages_to_order(m4_page_counts[i]);
        blocks[i] = page_alloc(orders[i]);
        if (!blocks[i]) finish(m4_fail_marker);
    }
    for (i = count; i > 0; --i) page_free(blocks[i - 1], orders[i - 1]);
    probe = page_alloc(0);
    if (!probe) finish(m4_fail_marker);
    page_free(probe, 0);

    heap_init();
    a = kmalloc(24);
    b = kmalloc(4096);
    c = kmalloc(33);
    if (!a || !b || !c) finish(m4_fail_marker);
    a[0] = 0x5a;
    b[4095] = 0xa5;
    c[32] = 0x3c;
    if (a[0] != 0x5a || b[4095] != 0xa5 || c[32] != 0x3c) finish(m4_fail_marker);
    first = (u64)(usize)a;
    kfree((void *)c);
    kfree((void *)b);
    kfree((void *)a);
    if (!check_heap_reuse(first) || !check_page_mappings()) finish(m4_fail_marker);
    debug_write("PAGE_MAP OK\n");

    if (!native_tensor_check()) finish(m4_fail_marker);
    debug_write("I8_GEMM 256\n");
    finish(m4_ok_marker);
}
