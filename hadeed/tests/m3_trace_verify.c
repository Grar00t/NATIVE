typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned long usize;
typedef long isize;
typedef int i32;

#define SYS_READ 0L
#define SYS_WRITE 1L
#define SYS_OPEN 2L
#define SYS_CLOSE 3L
#define SYS_PREAD64 17L
#define SYS_EXIT 60L

#define IMAGE_PATH "build/hadeed.img"
#define ELF_PATH "build/kernel.elf"
#define SECTOR 512UL
#define STAGE1_LBA 1UL
#define KERNEL_LBA 9UL
#define KERNEL_PHYS 0x00100000UL
#define IDT_PHYS 0x00102000UL
#define IDT_BYTES 4096UL

struct symbol {
    u64 value;
    u64 size;
};

static u8 stage1[4096];
static u8 kernel[65536];
static u8 elf[131072];
static usize kernel_len;
static usize elf_len;
static const u8 *symtab;
static usize sym_count;
static usize sym_entsize;
static const u8 *strtab;
static usize strtab_size;

static long syscall1(long n, long a1)
{
    long r;
    __asm__ volatile ("syscall" : "=a" (r) : "a" (n), "D" (a1) : "rcx", "r11", "memory");
    return r;
}

static long syscall3(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile ("syscall" : "=a" (r) : "a" (n), "D" (a1), "S" (a2), "d" (a3) : "rcx", "r11", "memory");
    return r;
}

static long syscall4(long n, long a1, long a2, long a3, long a4)
{
    register long r10 __asm__("r10") = a4;
    long r;
    __asm__ volatile ("syscall" : "=a" (r) : "a" (n), "D" (a1), "S" (a2), "d" (a3), "r" (r10) : "rcx", "r11", "memory");
    return r;
}

static usize cstrlen(const char *s)
{
    usize n = 0UL;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static int streq(const char *a, const char *b)
{
    usize i = 0UL;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return 0;
        ++i;
    }
    return a[i] == b[i];
}

static void write_text(long fd, const char *s)
{
    usize left = cstrlen(s);
    while (left != 0UL) {
        const long n = syscall3(SYS_WRITE, fd, (long)s, (long)left);
        if (n <= 0L) {
            syscall1(SYS_EXIT, 119L);
            for (;;) { }
        }
        s += (usize)n;
        left -= (usize)n;
    }
}

static void write_hex(u64 value)
{
    char out[19];
    static const char digits[] = "0123456789abcdef";
    unsigned int i;
    out[0] = '0';
    out[1] = 'x';
    for (i = 0U; i < 16U; ++i) {
        const unsigned int shift = (15U - i) * 4U;
        out[2U + i] = digits[(value >> shift) & 0xfUL];
    }
    out[18] = '\0';
    write_text(1L, out);
}

static void trace(const char *event, u64 addr, u64 value, const char *src)
{
    write_text(1L, "{\"event\":\"");
    write_text(1L, event);
    write_text(1L, "\",\"addr\":\"");
    write_hex(addr);
    write_text(1L, "\",\"value\":\"");
    write_hex(value);
    write_text(1L, "\",\"src\":\"");
    write_text(1L, src);
    write_text(1L, "\"}\n");
}

__attribute__((noreturn)) static void fail(long code, const char *message)
{
    char d[4];
    unsigned long x = (unsigned long)code;
    unsigned int n = 0U;

    write_text(2L, "M3_TRACE_FAIL code=");
    if (x >= 100UL) d[n++] = (char)('0' + ((x / 100UL) % 10UL));
    if (x >= 10UL) d[n++] = (char)('0' + ((x / 10UL) % 10UL));
    d[n++] = (char)('0' + (x % 10UL));
    d[n] = '\0';
    write_text(2L, d);
    write_text(2L, " message=");
    write_text(2L, message);
    write_text(2L, "\n");
    syscall1(SYS_EXIT, code);
    for (;;) { }
}

static u16 rd16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 rd64(const u8 *p)
{
    return (u64)rd32(p) | ((u64)rd32(p + 4) << 32);
}

static int bytes_equal(const u8 *a, const u8 *b, usize n)
{
    usize i;
    for (i = 0UL; i < n; ++i) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static isize find_bytes(const u8 *buf, usize len, const u8 *needle, usize nlen)
{
    usize i;
    if (nlen == 0UL || nlen > len) return -1;
    for (i = 0UL; i + nlen <= len; ++i) {
        if (bytes_equal(buf + i, needle, nlen)) return (isize)i;
    }
    return -1;
}

static long open_ro(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, 0L, 0L);
}

static void pread_exact(long fd, u8 *buf, usize len, u64 off, long code, const char *msg)
{
    usize done = 0UL;
    while (done < len) {
        const long n = syscall4(SYS_PREAD64, fd, (long)(buf + done), (long)(len - done), (long)(off + done));
        if (n <= 0L) fail(code, msg);
        done += (usize)n;
    }
}

static usize read_file(long fd, u8 *buf, usize cap, long code, const char *msg)
{
    usize done = 0UL;
    while (done < cap) {
        const long n = syscall3(SYS_READ, fd, (long)(buf + done), (long)(cap - done));
        if (n < 0L) fail(code, msg);
        if (n == 0L) break;
        done += (usize)n;
    }
    if (done == cap) fail(code, "file too large");
    return done;
}

static void load_kernel_from_image(void)
{
    long fd;
    usize i;
    u16 sectors = 0U;

    fd = open_ro(IMAGE_PATH);
    if (fd < 0L) fail(10L, "cannot open build/hadeed.img");
    pread_exact(fd, stage1, sizeof stage1, STAGE1_LBA * SECTOR, 11L, "cannot read stage1");

    for (i = 0UL; i + 16UL <= sizeof stage1; ++i) {
        if (stage1[i] == 0x10U && stage1[i + 1UL] == 0U &&
            rd16(stage1 + i + 4UL) == 0U && rd16(stage1 + i + 6UL) == 0x1000U &&
            rd64(stage1 + i + 8UL) == KERNEL_LBA) {
            sectors = rd16(stage1 + i + 2UL);
            break;
        }
    }
    if (sectors == 0U || (usize)sectors * SECTOR > sizeof kernel) fail(12L, "kernel DAP invalid");
    kernel_len = (usize)sectors * SECTOR;
    pread_exact(fd, kernel, kernel_len, KERNEL_LBA * SECTOR, 13L, "cannot read kernel payload");
    (void)syscall1(SYS_CLOSE, fd);
    trace("m3.kernel_payload", KERNEL_LBA * SECTOR, kernel_len, "boot/stage1.asm");
}

static void load_elf_symbols(void)
{
    long fd;
    u64 shoff;
    u16 shentsize;
    u16 shnum;
    u16 i;

    fd = open_ro(ELF_PATH);
    if (fd < 0L) fail(20L, "cannot open build/kernel.elf");
    elf_len = read_file(fd, elf, sizeof elf, 21L, "cannot read kernel ELF");
    (void)syscall1(SYS_CLOSE, fd);

    if (elf_len < 64UL || elf[0] != 0x7fU || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' ||
        elf[4] != 2U || elf[5] != 1U) fail(22L, "ELF64 header invalid");
    shoff = rd64(elf + 0x28);
    shentsize = rd16(elf + 0x3a);
    shnum = rd16(elf + 0x3c);
    if (shentsize < 64U || shoff + (u64)shentsize * shnum > elf_len) fail(23L, "ELF section table invalid");

    for (i = 0U; i < shnum; ++i) {
        const u8 *const sh = elf + (usize)shoff + (usize)i * shentsize;
        if (rd32(sh + 4UL) == 2U) {
            const u64 off = rd64(sh + 24UL);
            const u64 size = rd64(sh + 32UL);
            const u32 link = rd32(sh + 40UL);
            const u64 entsize = rd64(sh + 56UL);
            const u8 *linked;
            u64 str_off;
            u64 str_size;
            if (link >= shnum || entsize < 24UL || off + size > elf_len) fail(24L, "ELF symtab invalid");
            linked = elf + (usize)shoff + (usize)link * shentsize;
            str_off = rd64(linked + 24UL);
            str_size = rd64(linked + 32UL);
            if (str_off + str_size > elf_len) fail(25L, "ELF strtab invalid");
            symtab = elf + (usize)off;
            sym_count = (usize)(size / entsize);
            sym_entsize = (usize)entsize;
            strtab = elf + (usize)str_off;
            strtab_size = (usize)str_size;
            return;
        }
    }
    fail(26L, "ELF symtab missing");
}

static struct symbol find_symbol(const char *name, long code)
{
    usize i;
    for (i = 0UL; i < sym_count; ++i) {
        const u8 *const s = symtab + i * sym_entsize;
        const u32 name_off = rd32(s);
        if (name_off < strtab_size && streq((const char *)(strtab + name_off), name)) {
            struct symbol out;
            out.value = rd64(s + 8UL);
            out.size = rd64(s + 16UL);
            return out;
        }
    }
    fail(code, name);
}

static const u8 *kernel_ptr(u64 address, usize need, long code, const char *msg)
{
    const u64 end = address + need;
    if (address < KERNEL_PHYS || end < address || end > KERNEL_PHYS + kernel_len) fail(code, msg);
    return kernel + (usize)(address - KERNEL_PHYS);
}

static int resolve_rip_target(u64 insn, usize insn_len, usize disp_off, u64 expected)
{
    const u8 *p = kernel_ptr(insn, insn_len, 32L, "RIP-relative instruction outside kernel");
    const long disp = (long)(i32)rd32(p + disp_off);
    const u64 target = insn + insn_len + (u64)disp;
    return target == expected;
}

static isize find_direct_call(struct symbol from, u64 target, usize start)
{
    const u8 *p = kernel_ptr(from.value, (usize)from.size, 33L, "caller outside kernel");
    usize i;
    for (i = start; i + 5UL <= from.size; ++i) {
        if (p[i] == 0xe8U) {
            const long rel = (long)(i32)rd32(p + i + 1UL);
            const u64 dst = from.value + i + 5UL + (u64)rel;
            if (dst == target) return (isize)i;
        }
    }
    return -1;
}

static void verify_idt_and_stubs(void)
{
    const struct symbol idt = find_symbol("idt", 40);
    const struct symbol table = find_symbol("isr_stub_table", 41);
    const struct symbol common = find_symbol("isr_common", 42);
    const struct symbol dispatch = find_symbol("isr_dispatch", 43);
    const u8 *entries = kernel_ptr(table.value, (usize)table.size, 44, "ISR table outside image");
    const u8 *handler = kernel_ptr(common.value, (usize)common.size, 44, "common ISR outside image");
    const u32 error_vectors = (1U<<8)|(1U<<10)|(1U<<11)|(1U<<12)|(1U<<13)|
                              (1U<<14)|(1U<<17)|(1U<<21)|(1U<<29)|(1U<<30);
    if (idt.value != IDT_PHYS || idt.size != IDT_BYTES || table.size != 48 * 8)
        fail(45, "interrupt table base or size differs");
    for (unsigned v = 0; v < 48; ++v) {
        u64 addr = rd64(entries + v * 8);
        const u8 *stub = kernel_ptr(addr, 9, 46, "ISR stub outside image");
        usize i = 0;
        if (v >= 32 || !(error_vectors & (1U << v))) {
            if (stub[0] != 0x6a || stub[1] != 0) fail(47, "missing synthetic error word");
            i = 2;
        }
        if (stub[i] != 0x6a || stub[i+1] != v) fail(48, "wrong ISR vector");
        i += 2;
        long displacement;
        if (stub[i] == 0xeb) { displacement = (signed char)stub[i+1]; i += 2; }
        else if (stub[i] == 0xe9) { displacement = (i32)rd32(stub+i+1); i += 5; }
        else fail(49, "ISR has no jump to shared entry");
        if (addr + i + (u64)displacement != common.value) fail(50, "wrong common ISR target");
    }
    isize call = find_direct_call(common, dispatch.value, 0);
    if (call < 1 || handler[(usize)call - 1] != 0xfc || common.size < 2 ||
        rd16(handler + common.size - 2) != 0xcf48U) fail(51, "ISR dispatch or return differs");
    trace("idt.stub_targets", table.value, 48, "kernel/isr.asm");
}

static usize entry_prefix(const u8 *code, usize size)
{
    static const u8 endbr64[] = {0xf3, 0x0f, 0x1e, 0xfa};
    return size >= sizeof endbr64 && bytes_equal(code, endbr64, sizeof endbr64)
        ? sizeof endbr64 : 0;
}

static void verify_pic(void)
{
    const struct symbol pic = find_symbol("pic_remap_irq0", 60L);
    static const u8 expected[] = {
        0xb0,0x11,0xe6,0x20,0xe6,0x80,
        0xb0,0x11,0xe6,0xa0,0xe6,0x80,
        0xb0,0x20,0xe6,0x21,0xe6,0x80,
        0xb0,0x28,0xe6,0xa1,0xe6,0x80,
        0xb0,0x04,0xe6,0x21,0xe6,0x80,
        0xb0,0x02,0xe6,0xa1,0xe6,0x80,
        0xb0,0x01,0xe6,0x21,0xe6,0x80,
        0xb0,0x01,0xe6,0xa1,0xe6,0x80,
        0xb0,0xfe,0xe6,0x21,0xe6,0x80,
        0xb0,0xff,0xe6,0xa1,0xe6,0x80
    };
    const u8 *code = kernel_ptr(pic.value, (usize)pic.size, 61L, "PIC function outside kernel");
    usize start = entry_prefix(code, pic.size);
    if (pic.size - start < sizeof expected || !bytes_equal(code + start, expected, sizeof expected))
        fail(62L, "8259 ICW/mask sequence mismatch");
    trace("pic.icw1_icw4_masks", pic.value + start, 0x11202804020101feUL, "kernel/pic.c");
}

static void verify_pit(void)
{
    const struct symbol pit = find_symbol("pit_init_100hz", 70L);
    static const u8 expected[] = {0xb0,0x36,0xe6,0x43,0xb0,0x9c,0xe6,0x40,0xb0,0x2e,0xe6,0x40};
    const u8 *code = kernel_ptr(pit.value, (usize)pit.size, 71L, "PIT function outside kernel");
    usize start = entry_prefix(code, pit.size);
    if (pit.size - start < sizeof expected || !bytes_equal(code + start, expected, sizeof expected))
        fail(72L, "PIT mode/divisor sequence mismatch");
    trace("pit.channel0_mode3", pit.value + start, 0x36UL, "kernel/pit.c");
    trace("pit.divisor_11932", pit.value + start + 4UL, 0x2e9cUL, "kernel/pit.c");
}

static void verify_irq0_counter(void)
{
    const struct symbol dispatch = find_symbol("isr_dispatch", 80);
    const struct symbol ticks = find_symbol("g_ticks", 81);
    const struct symbol eoi = find_symbol("pic_send_eoi", 82);
    const u8 *code = kernel_ptr(dispatch.value, (usize)dispatch.size, 83, "ISR outside payload");
    int read = 0, write = 0, acknowledge = 0;
    for (usize i = 0; i < dispatch.size; ++i) {
        if (i + 7 <= dispatch.size && (code[i] & 0xf8U) == 0x48U &&
            (code[i + 2] & 0xc7U) == 5 &&
            resolve_rip_target(dispatch.value + i, 7, 3, ticks.value)) {
            if (code[i+1] == 0x8b) read = 1;
            if (code[i+1] == 0x89) write = 1;
            if (code[i+1] == 0xff && code[i+2] == 5) read = write = 1;
        }
        /* addq $1, g_ticks(%rip) updates memory directly. */
        if (i + 8 <= dispatch.size && code[i] == 0x48 && code[i+1] == 0x83 &&
            code[i+2] == 0x05 && code[i+7] == 1 &&
            resolve_rip_target(dispatch.value + i, 8, 3, ticks.value)) read = write = 1;
        if (i + 5 <= dispatch.size && (code[i] == 0xe8 || code[i] == 0xe9)) {
            u64 target = dispatch.value + i + 5 + (u64)(long)(i32)rd32(code + i + 1);
            if (target == eoi.value) acknowledge = 1;
        }
    }
    if (ticks.size != 8 || !read || !write || !acknowledge)
        fail(85, "IRQ counter or PIC acknowledgement reference missing");
    trace("irq0.counter", dispatch.value, ticks.value, "kernel/idt.c");
    trace("irq0.eoi", dispatch.value, eoi.value, "kernel/pic.c");
}

static void verify_main_path(void)
{
    const struct symbol main_sym = find_symbol("kmain", 90);
    const char *const init[] = {"idt_install", "idt_check", "pic_remap_irq0", "pit_init_100hz"};
    const u8 *code = kernel_ptr(main_sym.value, (usize)main_sym.size, 91, "C entry outside payload");
    static const u8 wait_irq[] = {0xfb, 0xf4, 0xfa};
    usize pos = 0;
    for (usize i = 0; i < sizeof init / sizeof init[0]; ++i) {
        isize call = find_direct_call(main_sym, find_symbol(init[i], 92).value, pos);
        if (call < 0) fail(93, "interrupt initialization order differs");
        pos = (usize)call + 5;
    }
    if (find_bytes(code + pos, (usize)main_sym.size - pos, wait_irq, sizeof wait_irq) < 0)
        fail(94, "missing interruptible wait after PIT initialization");
    trace("main.timer_wait", main_sym.value, 3, "kernel/main.c");
}

void _start(void)
{
    load_kernel_from_image();
    load_elf_symbols();
    verify_idt_and_stubs();
    verify_pic();
    verify_pit();
    verify_irq0_counter();
    verify_main_path();
    write_text(1L, "M3_TRACE_PASS\n");
    syscall1(SYS_EXIT, 0L);
    for (;;) { }
}
