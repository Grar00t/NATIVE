CC      ?= gcc
AS      ?= as
LD      ?= ld
OBJCOPY ?= objcopy
QEMU    ?= qemu-system-x86_64
TIMEOUT ?= timeout

BUILD := build
STAGE1_SECTORS := 8
STAGE1_BYTES := 4096
KERNEL_LBA := 9
IMAGE_SECTORS := 2880

CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -ffreestanding -nostdlib \
          -fno-stack-protector -fno-pic -mno-red-zone -m64 -mgeneral-regs-only

KERNEL_OBJS := \
	$(BUILD)/kernel_entry.o \
	$(BUILD)/kernel_isr.o \
	$(BUILD)/kernel_main.o \
	$(BUILD)/kernel_idt.o \
	$(BUILD)/kernel_pic.o \
	$(BUILD)/kernel_pit.o \
	$(BUILD)/kernel_memory.o \
	$(BUILD)/kernel_heap.o $(BUILD)/gemm_i8.o $(BUILD)/tensor_check.o

all: $(BUILD)/hadeed.img $(BUILD)/m2_image_test $(BUILD)/m2_trace_verify $(BUILD)/m3_trace_verify $(BUILD)/m4_trace_verify

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/kernel_entry.o: kernel/entry.asm | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/kernel_isr.o: kernel/isr.asm | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/kernel_main.o: kernel/main.c kernel/memory.h kernel/heap.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel_idt.o: kernel/idt.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel_pic.o: kernel/pic.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel_pit.o: kernel/pit.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel_memory.o: kernel/memory.c kernel/memory.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel_heap.o: kernel/heap.c kernel/heap.h kernel/memory.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	$(LD) -m elf_x86_64 -nostdlib -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@test $$(wc -c < $@) -gt 0
	@test $$(wc -c < $@) -le 32768

$(BUILD)/stage1.o: boot/stage1.asm $(BUILD)/kernel.bin | $(BUILD)
	@bytes=$$(wc -c < $(BUILD)/kernel.bin); \
	sectors=$$(( (bytes + 511) / 512 )); \
	test $$sectors -gt 0; test $$sectors -le 64; \
	$(AS) --64 --defsym KERNEL_SECTORS=$$sectors $< -o $@

$(BUILD)/stage1.bin: $(BUILD)/stage1.o
	$(LD) -m elf_x86_64 -nostdlib -Ttext 0x8000 --oformat binary -e stage1 -o $@ $<
	@test $$(wc -c < $@) -gt 0
	@test $$(wc -c < $@) -le $(STAGE1_BYTES)

$(BUILD)/stage1.pad: $(BUILD)/stage1.bin
	dd if=/dev/zero of=$@ bs=1 count=$(STAGE1_BYTES) status=none
	dd if=$< of=$@ conv=notrunc status=none

$(BUILD)/boot.o: boot/bootloader.asm | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/boot.bin: $(BUILD)/boot.o
	$(LD) -m elf_x86_64 -nostdlib -Ttext 0x7c00 --oformat binary -e start -o $@ $<
	@test $$(wc -c < $@) -eq 512
	@test "$$(tail -c 2 $@ | od -An -tx1 | tr -d ' \n')" = 55aa

$(BUILD)/kernel.pad: $(BUILD)/kernel.bin
	@bytes=$$(wc -c < $<); sectors=$$(( (bytes + 511) / 512 )); \
	count=$$(( sectors * 512 )); \
	dd if=/dev/zero of=$@ bs=1 count=$$count status=none; \
	dd if=$< of=$@ conv=notrunc status=none

$(BUILD)/hadeed.img: $(BUILD)/boot.bin $(BUILD)/stage1.pad $(BUILD)/kernel.pad
	dd if=/dev/zero of=$@ bs=512 count=$(IMAGE_SECTORS) status=none
	dd if=$(BUILD)/boot.bin of=$@ conv=notrunc status=none
	dd if=$(BUILD)/stage1.pad of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(BUILD)/kernel.pad of=$@ bs=512 seek=$(KERNEL_LBA) conv=notrunc status=none

$(BUILD)/m2_image_test.o: tests/m2_image_test.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m2_image_test: $(BUILD)/m2_image_test.o tests/host_linker.ld
	$(LD) -m elf_x86_64 -nostdlib -T tests/host_linker.ld -o $@ $(BUILD)/m2_image_test.o

$(BUILD)/m2_trace_verify.o: tests/m2_trace_verify.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m2_trace_verify: $(BUILD)/m2_trace_verify.o tests/host_linker.ld
	$(LD) -m elf_x86_64 -nostdlib -T tests/host_linker.ld -o $@ $(BUILD)/m2_trace_verify.o

$(BUILD)/m3_trace_verify.o: tests/m3_trace_verify.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m3_trace_verify: $(BUILD)/m3_trace_verify.o tests/host_linker.ld
	$(LD) -m elf_x86_64 -nostdlib -T tests/host_linker.ld -o $@ $(BUILD)/m3_trace_verify.o

$(BUILD)/m4_trace_verify.o: tests/m4_trace_verify.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m4_trace_verify: $(BUILD)/m4_trace_verify.o tests/host_linker.ld
	$(LD) -m elf_x86_64 -nostdlib -T tests/host_linker.ld -o $@ $(BUILD)/m4_trace_verify.o

trace-verify: $(BUILD)/hadeed.img $(BUILD)/kernel.elf $(BUILD)/m2_trace_verify
	./$(BUILD)/m2_trace_verify

static-test: $(BUILD)/hadeed.img $(BUILD)/m2_image_test
	./$(BUILD)/m2_image_test

$(BUILD)/gemm_i8.o: ../asm/src/gemm_i8.s | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/tensor_check.o: kernel/tensor_check.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

m4-trace-verify: $(BUILD)/hadeed.img $(BUILD)/kernel.elf $(BUILD)/m4_trace_verify
	./$(BUILD)/m4_trace_verify

runtime-test: $(BUILD)/hadeed.img
	QEMU="$(QEMU)" TIMEOUT="$(TIMEOUT)" sh tests/boot.sh $<

check: static-test m4-trace-verify runtime-test

run: $(BUILD)/hadeed.img
	sh scripts/run-qemu.sh $<

clean:
	rm -rf $(BUILD)

.PHONY: all trace-verify static-test m4-trace-verify runtime-test check run clean
