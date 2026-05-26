ARCH    ?= aarch64
BUILD   := build/$(ARCH)

ifeq ($(ARCH),aarch64)
    CROSS         ?= aarch64-linux-gnu-
    ARCH_CFLAGS   := -mcpu=cortex-a53 -mgeneral-regs-only
    LINKER_LD     := arch/aarch64/linker.ld
    ARCH_OBJS     := \
        $(BUILD)/arch/aarch64/boot.o \
        $(BUILD)/arch/aarch64/uart.o \
        $(BUILD)/arch/aarch64/vectors.o \
        $(BUILD)/arch/aarch64/trap.o \
        $(BUILD)/arch/aarch64/mmu.o \
        $(BUILD)/arch/aarch64/timer.o \
        $(BUILD)/arch/aarch64/switch.o \
        $(BUILD)/arch/aarch64/thread.o \
        $(BUILD)/arch/aarch64/userblob.o
    # User-side init binary, linked at VA 0x10000000 and incbin'd
    # into userblob.S so the kernel ELF carries the raw bytes.
    USER_BUILD    := build/user
    USER_BIN      := $(USER_BUILD)/init.bin
    USERBLOB_EXTRA_DEP := $(USER_BIN)
    KERNEL_OBJS   := \
        $(BUILD)/kernel/printk.o \
        $(BUILD)/kernel/pmm.o \
        $(BUILD)/kernel/string.o \
        $(BUILD)/kernel/kmem.o \
        $(BUILD)/kernel/sched.o \
        $(BUILD)/kernel/streams.o \
        $(BUILD)/kernel/klog.o \
        $(BUILD)/kernel/vfs.o \
        $(BUILD)/kernel/stream_head.o \
        $(BUILD)/kernel/syscall.o \
        $(BUILD)/kernel/uaccess.o \
        $(BUILD)/kernel/user.o \
        $(BUILD)/kernel/main.o
    ELF           := $(BUILD)/kernel8.elf
    KERNEL        := $(BUILD)/kernel8.img
    QEMU          := qemu-system-aarch64
    QEMU_ARGS     := -M raspi3b -serial mon:stdio -serial null -display none
else ifeq ($(ARCH),arm)
    CROSS         ?= arm-linux-gnueabi-
    ARCH_CFLAGS   := -mcpu=cortex-a15 -marm -mfloat-abi=soft -mgeneral-regs-only
    LINKER_LD     := arch/arm/linker.ld
    ARCH_OBJS     := \
        $(BUILD)/arch/arm/boot.o \
        $(BUILD)/arch/arm/uart.o \
        $(BUILD)/arch/arm/vectors.o \
        $(BUILD)/arch/arm/trap.o \
        $(BUILD)/arch/arm/mmu.o \
        $(BUILD)/arch/arm/switch.o \
        $(BUILD)/arch/arm/thread.o \
        $(BUILD)/arch/arm/timer.o \
        $(BUILD)/arch/arm/main.o
    KERNEL_OBJS   := \
        $(BUILD)/kernel/printk.o \
        $(BUILD)/kernel/pmm.o \
        $(BUILD)/kernel/string.o \
        $(BUILD)/kernel/kmem.o \
        $(BUILD)/kernel/sched.o \
        $(BUILD)/kernel/streams.o \
        $(BUILD)/kernel/klog.o \
        $(BUILD)/kernel/vfs.o \
        $(BUILD)/kernel/stream_head.o \
        $(BUILD)/kernel/syscall.o \
        $(BUILD)/kernel/uaccess.o \
        $(BUILD)/kernel/ksh.o
    ELF           := $(BUILD)/kernel-arm.elf
    KERNEL        := $(BUILD)/kernel-arm.img
    QEMU          := qemu-system-arm
    QEMU_ARGS     := -M virt -cpu cortex-a15 -m 256 -nographic
else
    $(error Unknown ARCH=$(ARCH); use ARCH=aarch64 or ARCH=arm)
endif

CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

CFLAGS  := -Wall -Wextra -Werror -std=gnu11 \
           -ffreestanding -nostdlib -nostartfiles \
           -fno-stack-protector -fno-pie -fno-pic \
           $(ARCH_CFLAGS) \
           -Iinclude \
           -O2 -g

ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static

LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

OBJS := $(ARCH_OBJS) $(KERNEL_OBJS)

.PHONY: all run clean

all: $(KERNEL)

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(LINKER_LD)
	$(LD) $(LDFLAGS) -T $(LINKER_LD) -o $@ $(OBJS) $(LIBGCC)

$(KERNEL): $(ELF)
	$(OBJCOPY) -O binary $< $@

run: $(KERNEL)
	$(QEMU) $(QEMU_ARGS) -kernel $(KERNEL)

clean:
	rm -rf build

# --- User-side init binary (AArch64 only) ----------------------------
# Compile user/init.c with the same cross toolchain as the kernel,
# link with user/linker.ld at VA 0x10000000, objcopy to a raw .bin,
# then arch/aarch64/userblob.S incbin's the result into the kernel.

ifeq ($(ARCH),aarch64)

USER_CC     := $(CROSS)gcc
USER_LD     := $(CROSS)ld
USER_CFLAGS := -Wall -Wextra -Werror -std=gnu11 \
               -ffreestanding -nostdlib -nostartfiles \
               -fno-stack-protector -fno-pie -fno-pic \
               -mcpu=cortex-a53 -mgeneral-regs-only \
               -O2 -g

$(USER_BUILD)/init.o: user/init.c user/syscall.h | $(USER_BUILD)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/init.elf: $(USER_BUILD)/init.o user/linker.ld
	$(USER_LD) -nostdlib -static -T user/linker.ld -o $@ $<

$(USER_BIN): $(USER_BUILD)/init.elf
	$(OBJCOPY) -O binary $< $@

$(USER_BUILD):
	mkdir -p $@

# Tell make that userblob.o depends on the binary it incbin's.
$(BUILD)/arch/aarch64/userblob.o: $(USER_BIN)

endif
