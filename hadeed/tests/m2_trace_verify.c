typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned long usize;
typedef long isize;

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
#define STAGE1_SECTORS 8UL
#define KERNEL_LBA 9UL
#define KERNEL_PHYS 0x00100000UL
#define PML4_ADDR 0x00009000UL
#define PDPT_ADDR 0x0000a000UL
#define PD_ADDR 0x0000b000UL

static u8 boot[512];
static u8 stage1[4096];
static u8 kernel[65536];
static u8 elf[65536];

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
    usize n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
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
    write_text(2L, "M2_TRACE_FAIL code=");
    {
        char d[4];
        unsigned long x = (unsigned long)code;
        unsigned int n = 0U;
        if (x >= 100UL) d[n++] = (char)('0' + ((x / 100UL) % 10UL));
        if (x >= 10UL) d[n++] = (char)('0' + ((x / 10UL) % 10UL));
        d[n++] = (char)('0' + (x % 10UL));
        d[n] = '\0';
        write_text(2L, d);
    }
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
    for (i = 0; i < n; ++i) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static isize find_bytes(const u8 *buf, usize len, const u8 *needle, usize nlen)
{
    usize i;
    if (nlen == 0UL || nlen > len) return -1;
    for (i = 0; i + nlen <= len; ++i) {
        if (bytes_equal(buf + i, needle, nlen)) return (isize)i;
    }
    return -1;
}

static isize find_text(const u8 *buf, usize len, const char *s)
{
    return find_bytes(buf, len, (const u8 *)s, cstrlen(s));
}

static long open_ro(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, 0L, 0L);
}

static void pread_exact(long fd, u8 *buf, usize len, u64 off, long code, const char *msg)
{
    usize done = 0;
    while (done < len) {
        const long n = syscall4(SYS_PREAD64, fd, (long)(buf + done), (long)(len - done), (long)(off + done));
        if (n <= 0L) fail(code, msg);
        done += (usize)n;
    }
}

static usize read_file(long fd, u8 *buf, usize cap, long code, const char *msg)
{
    usize done = 0;
    while (done < cap) {
        const long n = syscall3(SYS_READ, fd, (long)(buf + done), (long)(cap - done));
        if (n < 0L) fail(code, msg);
        if (n == 0L) break;
        done += (usize)n;
    }
    if (done == cap) fail(code, "file too large");
    return done;
}

static usize parse_boot_dap(void)
{
    usize i;
    for (i = 0; i + 7UL <= 128UL; ++i) {
        if (boot[i] == 0xbeU && boot[i + 3] == 0xb4U && boot[i + 4] == 0x42U &&
            boot[i + 5] == 0xcdU && boot[i + 6] == 0x13U) {
            const u16 ptr = rd16(boot + i + 1);
            if (ptr < 0x7c00U || (u32)ptr + 16U > 0x7e00U) fail(22L, "boot DAP pointer outside sector");
            return (usize)(ptr - 0x7c00U);
        }
    }
    fail(21L, "INT13 AH=42 sequence missing");
}

static usize parse_stage1_dap(void)
{
    usize i;
    for (i = 0; i + 7UL <= sizeof stage1; ++i) {
        if (stage1[i] == 0xbeU && stage1[i + 3] == 0xb4U && stage1[i + 4] == 0x42U &&
            stage1[i + 5] == 0xcdU && stage1[i + 6] == 0x13U) {
            const u16 ptr = rd16(stage1 + i + 1);
            if (ptr < 0x8000U || (u32)ptr + 16U > 0x9000U) fail(31L, "stage1 DAP pointer outside image");
            return (usize)(ptr - 0x8000U);
        }
    }
    fail(30L, "stage1 INT13 AH=42 sequence missing");
}

static void verify_gdt(usize desc_off)
{
    const u16 limit = rd16(stage1 + desc_off);
    const u32 base = rd32(stage1 + desc_off + 2UL);
    usize gdt_off;
    u64 code;
    u64 data;
    if (base < 0x8000U || base >= 0x9000U) fail(42L, "GDT base outside stage1");
    gdt_off = (usize)(base - 0x8000U);
    if (limit < 39U || gdt_off + 40UL > sizeof stage1) fail(43L, "GDT too short");
    code = rd64(stage1 + gdt_off + 24UL);
    data = rd64(stage1 + gdt_off + 32UL);
    if (((code >> 47) & 1UL) != 1UL || ((code >> 53) & 1UL) != 1UL ||
        ((code >> 45) & 3UL) != 0UL || ((code >> 44) & 1UL) != 1UL || ((code >> 43) & 1UL) != 1UL) {
        fail(44L, "selector 0x18 is not ring0 present 64-bit code");
    }
    if (((data >> 47) & 1UL) != 1UL || ((data >> 45) & 3UL) != 0UL ||
        ((data >> 44) & 1UL) != 1UL || ((data >> 43) & 1UL) != 0UL || ((data >> 41) & 1UL) != 1UL) {
        fail(45L, "selector 0x20 is not ring0 writable data");
    }
    trace("gdt.cs64.selector18", (u64)base + 24UL, code, "boot/stage1.asm");
    trace("gdt.data.selector20", (u64)base + 32UL, data, "boot/stage1.asm");
}

static void verify_elf_rodata(usize elf_len, usize kernel_len, u64 *debug_addr, u64 *banner_addr)
{
    u64 shoff;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
    const u8 *shstr;
    usize shstr_size;
    u16 i;
    if (elf_len < 64UL || elf[0] != 0x7fU || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 2U || elf[5] != 1U) {
        fail(61L, "kernel ELF64 header invalid");
    }
    shoff = rd64(elf + 0x28);
    shentsize = rd16(elf + 0x3a);
    shnum = rd16(elf + 0x3c);
    shstrndx = rd16(elf + 0x3e);
    if (shentsize < 64U || shstrndx >= shnum || shoff + (u64)shentsize * shnum > elf_len) fail(62L, "ELF section table invalid");
    {
        const u8 *s = elf + (usize)shoff + (usize)shstrndx * shentsize;
        const u64 off = rd64(s + 24);
        const u64 size = rd64(s + 32);
        if (off + size > elf_len) fail(63L, "ELF shstrtab invalid");
        shstr = elf + (usize)off;
        shstr_size = (usize)size;
    }
    for (i = 0U; i < shnum; ++i) {
        const u8 *s = elf + (usize)shoff + (usize)i * shentsize;
        const u32 name_off = rd32(s);
        const u64 addr = rd64(s + 16);
        const u64 off = rd64(s + 24);
        const u64 size = rd64(s + 32);
        const char *name;
        if (name_off >= shstr_size) continue;
        name = (const char *)(shstr + name_off);
        if (cstrlen(name) == 7UL && name[0] == '.' && name[1] == 'r' && name[2] == 'o' && name[3] == 'd' &&
            name[4] == 'a' && name[5] == 't' && name[6] == 'a') {
            const usize flat_off = (usize)(addr - KERNEL_PHYS);
            isize dpos;
            isize bpos;
            if (addr < KERNEL_PHYS || off + size > elf_len || flat_off + size > kernel_len) fail(64L, "rodata bounds invalid");
            if (!bytes_equal(kernel + flat_off, elf + (usize)off, (usize)size)) fail(65L, "flat rodata differs from ELF");
            dpos = find_text(kernel + flat_off, (usize)size, "LM64\n");
            bpos = find_text(kernel + flat_off, (usize)size, "HADEED64");
            if (dpos < 0 || bpos < 0) fail(66L, "required rodata strings missing");
            *debug_addr = addr + (u64)dpos;
            *banner_addr = addr + (u64)bpos;
            trace("kernel.rodata.debug", *debug_addr, 0x4c4d36340aUL, "kernel/main.c");
            trace("kernel.rodata.banner", *banner_addr, 0x4841444545443634UL, "kernel/main.c");
            return;
        }
    }
    fail(67L, ".rodata section missing");
}

void _start(void)
{
    long image_fd;
    long elf_fd;
    usize dap;
    usize sdap;
    usize kernel_len;
    usize elf_len;
    isize pos;
    u16 sectors;
    u16 off16;
    u16 seg16;
    u64 lba;
    u64 debug_addr = 0;
    u64 banner_addr = 0;

    image_fd = open_ro(IMAGE_PATH);
    if (image_fd < 0L) fail(10L, "cannot open build/hadeed.img");
    pread_exact(image_fd, boot, sizeof boot, 0UL, 11L, "cannot read boot sector");
    if (boot[510] != 0x55U || boot[511] != 0xaaU) fail(20L, "0x55AA signature missing");
    trace("boot.signature", 0x7dfeUL, 0xaa55UL, "boot/bootloader.asm");

    dap = parse_boot_dap();
    if (boot[dap] != 0x10U || boot[dap + 1] != 0U) fail(23L, "boot DAP header invalid");
    sectors = rd16(boot + dap + 2UL);
    off16 = rd16(boot + dap + 4UL);
    seg16 = rd16(boot + dap + 6UL);
    lba = rd64(boot + dap + 8UL);
    if (sectors != 8U || off16 != 0x8000U || seg16 != 0U || lba != 1UL) fail(24L, "boot DAP values invalid");
    trace("boot.int13.ah42", 0x7c00UL + (u64)dap, 0x42UL, "boot/bootloader.asm");
    trace("boot.dap.stage1", 0x7c00UL + (u64)dap, ((u64)sectors << 48) | ((u64)seg16 << 32) | off16, "boot/bootloader.asm");
    trace("boot.dap.stage1_lba", 0x7c00UL + (u64)dap + 8UL, lba, "boot/bootloader.asm");

    pread_exact(image_fd, stage1, sizeof stage1, STAGE1_LBA * SECTOR, 12L, "cannot read stage1 sectors");
    sdap = parse_stage1_dap();
    if (stage1[sdap] != 0x10U || stage1[sdap + 1] != 0U) fail(32L, "stage1 DAP header invalid");
    sectors = rd16(stage1 + sdap + 2UL);
    off16 = rd16(stage1 + sdap + 4UL);
    seg16 = rd16(stage1 + sdap + 6UL);
    lba = rd64(stage1 + sdap + 8UL);
    if (sectors == 0U || off16 != 0U || seg16 != 0x1000U || lba != KERNEL_LBA) fail(33L, "kernel DAP values invalid");
    kernel_len = (usize)sectors * SECTOR;
    if (kernel_len > sizeof kernel) fail(34L, "kernel DAP exceeds verifier buffer");
    pread_exact(image_fd, kernel, kernel_len, KERNEL_LBA * SECTOR, 13L, "cannot read kernel payload");
    trace("stage1.dap.kernel", 0x8000UL + (u64)sdap, ((u64)sectors << 48) | ((u64)seg16 << 32) | off16, "boot/stage1.asm");
    trace("stage1.dap.kernel_lba", 0x8000UL + (u64)sdap + 8UL, lba, "boot/stage1.asm");

    {
        static const u8 a20[] = {0xe4,0x92,0x0c,0x02,0x24,0xfe,0xe6,0x92};
        pos = find_bytes(stage1, sizeof stage1, a20, sizeof a20);
        if (pos < 0) fail(40L, "A20 port 0x92 sequence missing");
        trace("stage1.a20.port92", 0x8000UL + (u64)pos, 0x02UL, "boot/stage1.asm");
    }
    {
        static const u8 paging[] = {
            0x66,0xc7,0x06,0x00,0x90,0x03,0xa0,0x00,0x00,
            0x66,0xc7,0x06,0x04,0x90,0x00,0x00,0x00,0x00,
            0x66,0xc7,0x06,0x00,0xa0,0x03,0xb0,0x00,0x00,
            0x66,0xc7,0x06,0x04,0xa0,0x00,0x00,0x00,0x00,
            0x66,0xc7,0x06,0x00,0xb0,0x83,0x00,0x00,0x00,
            0x66,0xc7,0x06,0x04,0xb0,0x00,0x00,0x00,0x00
        };
        const u64 pml4e = PDPT_ADDR | 3UL;
        const u64 pdpte = PD_ADDR | 3UL;
        const u64 pde = 0x83UL;
        pos = find_bytes(stage1, sizeof stage1, paging, sizeof paging);
        if (pos < 0) fail(41L, "page-table write sequence missing");
        if ((pml4e & 3UL) != 3UL || (pml4e >> 63) != 0UL || (pdpte & 3UL) != 3UL || (pdpte >> 63) != 0UL ||
            (pde & 0x83UL) != 0x83UL || (pde >> 63) != 0UL) fail(41L, "identity map flags invalid");
        trace("paging.pml4_0", PML4_ADDR, pml4e, "boot/stage1.asm");
        trace("paging.pdpt_0", PDPT_ADDR, pdpte, "boot/stage1.asm");
        trace("paging.pd_0_2m", PD_ADDR, pde, "boot/stage1.asm");
    }
    {
        static const u8 lgdt_head[] = {0x0f,0x01,0x16};
        pos = find_bytes(stage1, sizeof stage1, lgdt_head, sizeof lgdt_head);
        if (pos < 0 || (usize)pos + 5UL > sizeof stage1) fail(46L, "lgdt missing");
        {
            const u16 desc_addr = rd16(stage1 + (usize)pos + 3UL);
            if (desc_addr < 0x8000U || desc_addr + 6U > 0x9000U) fail(47L, "lgdt descriptor address invalid");
            verify_gdt((usize)(desc_addr - 0x8000U));
            trace("stage1.lgdt", 0x8000UL + (u64)pos, desc_addr, "boot/stage1.asm");
        }
    }
    {
        static const u8 crseq[] = {
            0x0f,0x20,0xe0,0x66,0x83,0xc8,0x20,0x0f,0x22,0xe0,
            0x66,0xb8,0x00,0x90,0x00,0x00,0x0f,0x22,0xd8,
            0x66,0xb9,0x80,0x00,0x00,0xc0,0x0f,0x32,0x66,0x0d,0x00,0x01,0x00,0x00,0x0f,0x30,
            0x0f,0x20,0xc0,0x66,0x0d,0x01,0x00,0x00,0x80,0x0f,0x22,0xc0
        };
        pos = find_bytes(stage1, sizeof stage1, crseq, sizeof crseq);
        if (pos < 0) fail(48L, "CR4/CR3/EFER/CR0 sequence missing");
        trace("stage1.cr4.pae", 0x8000UL + (u64)pos, 0x20UL, "boot/stage1.asm");
        trace("stage1.cr3", 0x8000UL + (u64)pos + 10UL, PML4_ADDR, "boot/stage1.asm");
        trace("stage1.efer.lme", 0x8000UL + (u64)pos + 19UL, 0x100UL, "boot/stage1.asm");
        trace("stage1.cr0.pe_pg", 0x8000UL + (u64)pos + 34UL, 0x80000001UL, "boot/stage1.asm");
    }
    {
        usize i;
        int found = 0;
        for (i = 0; i + 8UL <= sizeof stage1; ++i) {
            if (stage1[i] == 0x66U && stage1[i + 1] == 0xeaU && rd16(stage1 + i + 6UL) == 0x18U) {
                const u32 target = rd32(stage1 + i + 2UL);
                if (target < 0x8000U || target >= 0x9000U) fail(49L, "long-mode far jump target invalid");
                trace("stage1.far_jump.cs18", 0x8000UL + i, target, "boot/stage1.asm");
                found = 1;
                break;
            }
        }
        if (!found) fail(49L, "far jump to selector 0x18 missing");
    }

    {
        static const u8 prologue[] = {0xfa,0x48,0xc7,0xc4,0,0,8,0,0x48,0x31,0xed,0xfc,0x48,0x8d,0x3d};
        static const u8 to_end[] = {0x48,0x8d,0x0d};
        static const u8 clear_call[] = {0x48,0x29,0xf9,0x31,0xc0,0xf3,0xaa,0xe8};
        u64 first, end, target;
        if (kernel_len < 38 || !bytes_equal(kernel, prologue, sizeof prologue) ||
            !bytes_equal(kernel + 19, to_end, sizeof to_end) ||
            !bytes_equal(kernel + 26, clear_call, sizeof clear_call)) fail(50L, "kernel zeroing prologue invalid");
        first = KERNEL_PHYS + 19 + (u64)(long)(int)rd32(kernel + 15);
        end = KERNEL_PHYS + 26 + (u64)(long)(int)rd32(kernel + 22);
        target = KERNEL_PHYS + 38 + (u64)(long)(int)rd32(kernel + 34);
        if (first < KERNEL_PHYS || first >= end || end > 0x200000 ||
            target < KERNEL_PHYS || target >= first || target >= KERNEL_PHYS + kernel_len)
            fail(51L, "kernel clear range or C entry outside mapped image");
        trace("kernel.entry.zero", first, end - first, "kernel/entry.asm");
        trace("kernel.entry.call_kmain", KERNEL_PHYS + 33, target, "kernel/entry.asm");
    }
    elf_fd = open_ro(ELF_PATH);
    if (elf_fd < 0L) fail(60L, "cannot open build/kernel.elf");
    elf_len = read_file(elf_fd, elf, sizeof elf, 60L, "cannot read kernel ELF");
    (void)syscall1(SYS_CLOSE, elf_fd);
    verify_elf_rodata(elf_len, kernel_len, &debug_addr, &banner_addr);
    {
        int screen = 0, banner = 0, marker = 0, port = 0;
        /* The linked kernel uses 32-bit absolute addresses for low memory. */
        for (usize i = 0; i + 4 <= kernel_len; ++i) {
            u32 operand = rd32(kernel + i);
            screen |= operand == 0xb87c8U;
            banner |= operand == banner_addr;
            marker |= operand == debug_addr;
            port |= kernel[i] == 0xe6 && kernel[i + 1] == 0xe9;
        }
        if (!screen || !banner || !marker || !port)
            fail(68L, "VGA or debug console reference missing");
        trace("kernel.console.vga", 0xb87c8UL, banner_addr, "kernel/main.c");
        trace("kernel.console.debug", 0xe9UL, debug_addr, "kernel/main.c");
    }

    (void)syscall1(SYS_CLOSE, image_fd);
    write_text(1L, "M2_TRACE_PASS\n");
    syscall1(SYS_EXIT, 0L);
    for (;;) { }
}
