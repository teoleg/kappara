CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

CFLAGS  := -Wall -Wextra -Werror -std=gnu11 \
           -ffreestanding -nostdlib -nostartfiles \
           -fno-stack-protector -fno-pie -fno-pic \
           -mgeneral-regs-only -mcpu=cortex-a53 \
           -Iinclude \
           -O2 -g

ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static

BUILD   := build

OBJS := \
	$(BUILD)/arch/aarch64/boot.o \
	$(BUILD)/arch/aarch64/uart.o \
	$(BUILD)/arch/aarch64/vectors.o \
	$(BUILD)/arch/aarch64/trap.o \
	$(BUILD)/arch/aarch64/mmu.o \
	$(BUILD)/arch/aarch64/timer.o \
	$(BUILD)/arch/aarch64/switch.o \
	$(BUILD)/kernel/printk.o \
	$(BUILD)/kernel/pmm.o \
	$(BUILD)/kernel/string.o \
	$(BUILD)/kernel/kmem.o \
	$(BUILD)/kernel/sched.o \
	$(BUILD)/kernel/main.o

ELF    := $(BUILD)/kernel8.elf
KERNEL := $(BUILD)/kernel8.img

.PHONY: all run clean

all: $(KERNEL)

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) arch/aarch64/linker.ld
	$(LD) $(LDFLAGS) -T arch/aarch64/linker.ld -o $@ $(OBJS)

$(KERNEL): $(ELF)
	$(OBJCOPY) -O binary $< $@

run: $(KERNEL)
	qemu-system-aarch64 -M raspi3b -kernel $(KERNEL) \
		-serial mon:stdio -serial null -display none

clean:
	rm -rf $(BUILD)
