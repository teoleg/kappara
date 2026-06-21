ARCH    ?= aarch64
BUILD   := build/$(ARCH)

ifeq ($(ARCH),aarch64)
    CROSS         ?= aarch64-linux-gnu-
    ARCH_CFLAGS   := -mcpu=cortex-a53 -mgeneral-regs-only -Iuts/aarch64
    LINKER_LD     := uts/aarch64/linker.ld
    ARCH_OBJS     := \
        $(BUILD)/uts/aarch64/boot.o \
        $(BUILD)/uts/aarch64/uart.o \
        $(BUILD)/uts/aarch64/miniuart.o \
        $(BUILD)/uts/aarch64/vectors.o \
        $(BUILD)/uts/aarch64/trap.o \
        $(BUILD)/uts/aarch64/mmu.o \
        $(BUILD)/uts/aarch64/timer.o \
        $(BUILD)/uts/aarch64/ipi.o \
        $(BUILD)/uts/aarch64/switch.o \
        $(BUILD)/uts/aarch64/thread.o \
        $(BUILD)/uts/aarch64/mailbox.o \
        $(BUILD)/uts/aarch64/framebuffer.o \
        $(BUILD)/uts/aarch64/font8x8.o \
        $(BUILD)/uts/aarch64/fbcon.o \
        $(BUILD)/uts/aarch64/userblob.o \
        $(BUILD)/uts/aarch64/helloblob.o \
        $(BUILD)/uts/aarch64/usrblobs.o
    # User-side init binary, linked at VA 0x10000000 and incbin'd
    # into userblob.S so the kernel ELF carries the raw bytes.
    USER_BUILD    := build/user
    USER_BIN      := $(USER_BUILD)/init.bin
    USERBLOB_EXTRA_DEP := $(USER_BIN)
    KERNEL_OBJS   := \
        $(BUILD)/uts/os/core/printk.o \
        $(BUILD)/uts/os/core/pmm.o \
        $(BUILD)/uts/os/core/string.o \
        $(BUILD)/uts/os/core/kmem.o \
        $(BUILD)/uts/os/core/klog.o \
        $(BUILD)/uts/os/core/uaccess.o \
        $(BUILD)/uts/os/core/kallsyms.o \
        $(BUILD)/uts/os/core/ftrace.o \
        $(BUILD)/uts/os/proc/process.o \
        $(BUILD)/uts/os/proc/sched.o \
        $(BUILD)/uts/os/proc/signal.o \
        $(BUILD)/uts/os/proc/syscall.o \
        $(BUILD)/uts/os/fs/vfs.o \
        $(BUILD)/uts/os/fs/ramdisk.o \
        $(BUILD)/uts/os/fs/kfs.o \
        $(BUILD)/uts/os/fs/procfs.o \
        $(BUILD)/uts/os/io/streams.o \
        $(BUILD)/uts/os/io/cdevsw.o \
        $(BUILD)/uts/os/io/bdevsw.o \
        $(BUILD)/uts/os/io/buf.o \
        $(BUILD)/uts/os/io/bram.o \
        $(BUILD)/uts/os/io/stream_head.o \
        $(BUILD)/uts/os/io/vt.o \
        $(BUILD)/uts/os/io/ldterm.o \
        $(BUILD)/uts/os/io/tty.o \
        $(BUILD)/uts/os/net/netif.o \
        $(BUILD)/uts/os/net/ipv4.o \
        $(BUILD)/uts/os/net/icmp.o \
        $(BUILD)/uts/os/net/udp.o \
        $(BUILD)/uts/os/net/tcp.o \
        $(BUILD)/uts/os/net/pktfilter.o \
        $(BUILD)/uts/os/net/slip.o \
        $(BUILD)/uts/os/net/lo.o \
        $(BUILD)/uts/os/user/user.o \
        $(BUILD)/uts/os/main.o
    KSYM_STUB_OBJ := $(BUILD)/uts/aarch64/kallsyms_stub.o
    KSYM_OBJ      := $(BUILD)/uts/aarch64/kallsyms.o
    KSYM_SRC      := $(BUILD)/uts/aarch64/kallsyms.S
    ELF           := $(BUILD)/kernel8.elf
    KERNEL        := $(BUILD)/kernel8.img
    QEMU          := qemu-system-aarch64
    QEMU_ARGS     := -M raspi3b -serial mon:stdio -serial null -display none
else ifeq ($(ARCH),virt)
    # QEMU `virt` machine (generic aarch64).  Reuses the aarch64
    # toolchain + shared PL011 driver via PLAT_PL011_BASE; brings its
    # own boot.S, GIC-based timer/ipi, and stubs for the Pi-only
    # drivers (framebuffer, mailbox, mini-UART) so the same main.c
    # boots through to the shell prompt.
    #
    # Phase 2 (current): full kernel bring-up, single-core.  Boots to
    # the same /usr/bin shell as raspi3b.
    # Phase 3: virtio-mmio probe + virtio-net driver (eth0 netif).
    # Phase 4: TCP integration vs Linux.
    # Phase 5: telnet server + hostfwd for external access.
    CROSS         ?= aarch64-linux-gnu-
    ARCH_CFLAGS   := -mcpu=cortex-a72 -mgeneral-regs-only \
                     -Iuts/aarch64 -DPLATFORM_VIRT
    LINKER_LD     := uts/virt/linker.ld
    USER_BUILD    := build/user
    USER_BIN      := $(USER_BUILD)/init.bin
    USERBLOB_EXTRA_DEP := $(USER_BIN)
    ARCH_OBJS     := \
        $(BUILD)/uts/virt/boot.o \
        $(BUILD)/uts/aarch64/uart.o \
        $(BUILD)/uts/aarch64/vectors.o \
        $(BUILD)/uts/aarch64/trap.o \
        $(BUILD)/uts/aarch64/mmu.o \
        $(BUILD)/uts/aarch64/switch.o \
        $(BUILD)/uts/aarch64/thread.o \
        $(BUILD)/uts/virt/timer.o \
        $(BUILD)/uts/virt/ipi.o \
        $(BUILD)/uts/virt/gic.o \
        $(BUILD)/uts/virt/framebuffer.o \
        $(BUILD)/uts/virt/fbcon.o \
        $(BUILD)/uts/virt/mailbox.o \
        $(BUILD)/uts/virt/miniuart.o \
        $(BUILD)/uts/virt/slip-stubs.o \
        $(BUILD)/uts/virt/virtio_net.o \
        $(BUILD)/uts/virt/telnetd.o \
        $(BUILD)/uts/aarch64/userblob.o \
        $(BUILD)/uts/aarch64/helloblob.o \
        $(BUILD)/uts/aarch64/usrblobs.o
    KERNEL_OBJS   := \
        $(BUILD)/uts/os/core/printk.o \
        $(BUILD)/uts/os/core/pmm.o \
        $(BUILD)/uts/os/core/string.o \
        $(BUILD)/uts/os/core/kmem.o \
        $(BUILD)/uts/os/core/klog.o \
        $(BUILD)/uts/os/core/uaccess.o \
        $(BUILD)/uts/os/core/kallsyms.o \
        $(BUILD)/uts/os/core/ftrace.o \
        $(BUILD)/uts/os/proc/process.o \
        $(BUILD)/uts/os/proc/sched.o \
        $(BUILD)/uts/os/proc/signal.o \
        $(BUILD)/uts/os/proc/syscall.o \
        $(BUILD)/uts/os/fs/vfs.o \
        $(BUILD)/uts/os/fs/ramdisk.o \
        $(BUILD)/uts/os/fs/kfs.o \
        $(BUILD)/uts/os/fs/procfs.o \
        $(BUILD)/uts/os/io/streams.o \
        $(BUILD)/uts/os/io/cdevsw.o \
        $(BUILD)/uts/os/io/bdevsw.o \
        $(BUILD)/uts/os/io/buf.o \
        $(BUILD)/uts/os/io/bram.o \
        $(BUILD)/uts/os/io/stream_head.o \
        $(BUILD)/uts/os/io/vt.o \
        $(BUILD)/uts/os/io/ldterm.o \
        $(BUILD)/uts/os/io/tty.o \
        $(BUILD)/uts/os/net/netif.o \
        $(BUILD)/uts/os/net/ipv4.o \
        $(BUILD)/uts/os/net/icmp.o \
        $(BUILD)/uts/os/net/udp.o \
        $(BUILD)/uts/os/net/tcp.o \
        $(BUILD)/uts/os/net/pktfilter.o \
        $(BUILD)/uts/os/net/lo.o \
        $(BUILD)/uts/os/user/user.o \
        $(BUILD)/uts/os/main.o
    KSYM_STUB_OBJ := $(BUILD)/uts/aarch64/kallsyms_stub.o
    KSYM_OBJ      := $(BUILD)/uts/aarch64/kallsyms.o
    KSYM_SRC      := $(BUILD)/uts/aarch64/kallsyms.S
    ELF           := $(BUILD)/kernel.elf
    KERNEL        := $(BUILD)/kernel.img
    QEMU          := qemu-system-aarch64
    # FTP needs the control port (21 -> host 2121) plus a small range
    # of data ports (30000..30007 -> host 30000..30007) for PASV.  The
    # range must match cmd/ftpd.c's PASV_BASE / PASV_N.
    QEMU_ARGS     := -M virt,gic-version=3 -cpu cortex-a72 -nographic -m 256 \
                     -global virtio-mmio.force-legacy=false \
                     -netdev user,id=n0,hostfwd=tcp::2323-:23,hostfwd=tcp::2121-:21,hostfwd=tcp::30000-:30000,hostfwd=tcp::30001-:30001,hostfwd=tcp::30002-:30002,hostfwd=tcp::30003-:30003,hostfwd=tcp::30004-:30004,hostfwd=tcp::30005-:30005,hostfwd=tcp::30006-:30006,hostfwd=tcp::30007-:30007 \
                     -device virtio-net-device,netdev=n0
else
    $(error Unknown ARCH=$(ARCH); use ARCH=aarch64 or ARCH=virt)
endif

CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

CFLAGS  := -Wall -Wextra -Werror -std=gnu11 \
           -ffreestanding -nostdlib -nostartfiles \
           -fno-stack-protector -fno-pie -fno-pic \
           $(ARCH_CFLAGS) \
           -Iinclude \
           -MMD -MP \
           -O2 -g

# Function tracing: `make TRACE=1` turns on gcc's -finstrument-functions
# so every C function entry/exit calls into uts/os/ftrace.c.  A handful
# of TUs MUST opt out -- otherwise the hook recurses through them while
# recording or while dumping the trace itself.  The opt-out list is the
# trace plumbing + everything it uses transitively:
#   ftrace.c    the tracer itself (would recurse on its own hooks)
#   printk.c    used by kprintf-based dump path
#   uart.c      used by printk
#   string.c    kmemset/kmemcpy are called from every code path; tracing
#               them would multiply event volume for zero diagnostic value
#   kallsyms.c  used by the dump's symbol-name formatter
TRACE ?= 0
ifeq ($(TRACE),1)
    CFLAGS += -finstrument-functions
endif

# Files that must NEVER be instrumented (whether TRACE is on or off):
NOINST_OBJS := \
    $(BUILD)/uts/os/ftrace.o \
    $(BUILD)/uts/os/printk.o \
    $(BUILD)/uts/os/string.o \
    $(BUILD)/uts/os/kallsyms.o
NOINST_OBJS += $(BUILD)/uts/aarch64/uart.o

$(NOINST_OBJS): CFLAGS += -fno-instrument-functions

# Strip -finstrument-functions from .S compilation: it is a C-only
# pass that GCC complains about when asked to handle an assembly file.
ASFLAGS := $(filter-out -finstrument-functions, $(CFLAGS))
LDFLAGS := -nostdlib -static

LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

OBJS := $(ARCH_OBJS) $(KERNEL_OBJS)
DEPS := $(OBJS:.o=.d)

.PHONY: all run clean

# Pin the default goal explicitly -- the `-include $(DEPS)` below would
# otherwise promote whichever .d file loads first to be the default
# target (its first rule wins).
.DEFAULT_GOAL := all

all: $(KERNEL)

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

ifneq ($(filter $(ARCH),aarch64 virt),)

# Two-pass link for kallsyms.  Pass 1 uses the stub (empty table) so
# we have an ELF to nm; tools/gen_kallsyms.sh emits the populated
# table; pass 2 relinks with the real .kallsyms object.  The .kallsyms
# section is placed after BSS in the linker script so growing it does
# not shift any code/data addresses between passes -- the addresses
# captured in pass 1 stay valid in pass 2.

$(BUILD)/kernel.tmp.elf: $(OBJS) $(KSYM_STUB_OBJ) $(LINKER_LD)
	$(LD) $(LDFLAGS) -T $(LINKER_LD) -o $@ $(OBJS) $(KSYM_STUB_OBJ) $(LIBGCC)

$(KSYM_SRC): $(BUILD)/kernel.tmp.elf tools/gen_kallsyms.sh
	@mkdir -p $(dir $@)
	./tools/gen_kallsyms.sh $(CROSS)nm $< > $@

$(KSYM_OBJ): $(KSYM_SRC)
	$(CC) $(ASFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(KSYM_OBJ) $(LINKER_LD)
	$(LD) $(LDFLAGS) -T $(LINKER_LD) -o $@ $(OBJS) $(KSYM_OBJ) $(LIBGCC)

else

$(ELF): $(OBJS) $(LINKER_LD)
	$(LD) $(LDFLAGS) -T $(LINKER_LD) -o $@ $(OBJS) $(LIBGCC)

endif

$(KERNEL): $(ELF)
	$(OBJCOPY) -O binary $< $@

# Trap INT/TERM and reap the qemu pid on the way out -- otherwise a
# Ctrl-C in `-nographic` mode leaves a zombie QEMU holding the
# hostfwd ports, and the next `make run` fails with "Could not set
# up host forwarding rule".  Same pattern run-telnet / run-ftp use.
run: $(KERNEL)
	@$(QEMU) $(QEMU_ARGS) -kernel $(KERNEL) & \
	  QPID=$$!; \
	  trap "kill $$QPID 2>/dev/null; wait 2>/dev/null" EXIT INT TERM; \
	  wait $$QPID

# ARCH=virt only: boot QEMU virt headless in the background, wait
# for the in-kernel telnetd to come up on hostfwd port 2323, then
# launch an `nc` client in the foreground.  On exit (Ctrl-C or `quit`
# inside the shell) both the client and the background QEMU get
# torn down so you don't leave a dangling vCPU thread behind.
#
# Use this when you want to drive the shell over telnet -- the kernel
# splash + boot trace go to /tmp/kappara-virt.log instead of stdout,
# and stdin/stdout belong to the telnet session on tty4.
ifeq ($(ARCH),virt)
.PHONY: run-telnet run-ftp smoke-ftp
run-telnet: $(KERNEL)
	@command -v nc >/dev/null 2>&1 || { \
		echo "run-telnet: need 'nc' on PATH (apt install netcat-openbsd)"; \
		exit 1; }
	@echo "==> booting kappara virt in background (log: /tmp/kappara-virt.log)"
	@$(QEMU) $(QEMU_ARGS) -kernel $(KERNEL) > /tmp/kappara-virt.log 2>&1 & \
	  QPID=$$!; \
	  trap "echo; echo '==> killing qemu (pid '$$QPID')'; kill $$QPID 2>/dev/null; wait 2>/dev/null" EXIT INT TERM; \
	  echo "==> waiting for telnetd on localhost:2323"; \
	  for i in 1 2 3 4 5 6 7 8 9 10; do \
	      sleep 1; \
	      if grep -q 'telnetd: listening' /tmp/kappara-virt.log 2>/dev/null; then \
	          break; \
	      fi; \
	  done; \
	  if ! grep -q 'telnetd: listening' /tmp/kappara-virt.log 2>/dev/null; then \
	      echo "run-telnet: telnetd did not come up; tail of log:"; \
	      tail -20 /tmp/kappara-virt.log; \
	      exit 1; \
	  fi; \
	  echo "==> connecting (nc localhost 2323) -- ^] then quit, or ^C to bail"; \
	  nc localhost 2323

# Boot kappara virt in the background and launch `ftp` in passive mode
# against the in-kernel ftpd.  Use this to interactively push binaries
# from the host into /home and run them via `exec /home/<name>` from a
# separate telnet session.  Cleans up the bg QEMU on exit.
run-ftp: $(KERNEL)
	@command -v ftp >/dev/null 2>&1 || { \
		echo "run-ftp: need 'ftp' on PATH (apt install ftp)"; \
		exit 1; }
	@echo "==> booting kappara virt in background (log: /tmp/kappara-virt.log)"
	@$(QEMU) $(QEMU_ARGS) -kernel $(KERNEL) > /tmp/kappara-virt.log 2>&1 & \
	  QPID=$$!; \
	  trap "echo; echo '==> killing qemu (pid '$$QPID')'; kill $$QPID 2>/dev/null; wait 2>/dev/null" EXIT INT TERM; \
	  echo "==> waiting for ftpd on localhost:2121"; \
	  for i in 1 2 3 4 5 6 7 8 9 10; do \
	      sleep 1; \
	      if grep -q 'ftpd: listening' /tmp/kappara-virt.log 2>/dev/null; then \
	          break; \
	      fi; \
	  done; \
	  if ! grep -q 'ftpd: listening' /tmp/kappara-virt.log 2>/dev/null; then \
	      echo "run-ftp: ftpd did not come up; tail of log:"; \
	      tail -20 /tmp/kappara-virt.log; \
	      exit 1; \
	  fi; \
	  echo "==> connecting (ftp -p 127.0.0.1 2121)"; \
	  echo "==> hints: USER any / PASS any / cd /home / put <file> / bye"; \
	  HOME=/tmp ftp -pinv 127.0.0.1 2121

# Scripted host-side smoke test: boot virt, upload an embedded ELF via
# FTP, download it back, byte-compare, then exec it over telnet and
# check the output looks sane.  All-or-nothing: any failure exits
# nonzero so CI can flag regressions.  See docs/FTPD.md "Operating
# recipe" for what this verifies.
smoke-ftp: $(KERNEL) build/cmd/ifconfig.elf
	@command -v ftp >/dev/null 2>&1 || { \
		echo "smoke-ftp: need 'ftp' on PATH"; exit 1; }
	@command -v nc >/dev/null 2>&1 || { \
		echo "smoke-ftp: need 'nc' on PATH"; exit 1; }
	@LOG=/tmp/kappara-smoke.log; \
	 DOWN=/tmp/kappara-smoke-elf-back; \
	 rm -f $$LOG $$DOWN; \
	 echo "==> boot kappara virt (log: $$LOG)"; \
	 $(QEMU) $(QEMU_ARGS) -kernel $(KERNEL) > $$LOG 2>&1 & \
	 QPID=$$!; \
	 trap "kill $$QPID 2>/dev/null; wait 2>/dev/null" EXIT INT TERM; \
	 for i in 1 2 3 4 5 6 7 8 9 10; do \
	     sleep 1; \
	     if grep -q 'ftpd: listening' $$LOG 2>/dev/null \
	        && grep -q 'telnetd: listening' $$LOG 2>/dev/null; then \
	         break; \
	     fi; \
	 done; \
	 grep -q 'ftpd: listening' $$LOG 2>/dev/null || { \
	     echo "smoke-ftp: ftpd did not come up"; tail -20 $$LOG; exit 1; }; \
	 echo "==> upload ifconfig.elf, RETR back, byte-compare"; \
	 printf 'user anonymous any\nbinary\ncd /home\nput build/cmd/ifconfig.elf foo\nget foo %s\nquit\n' \
	        $$DOWN \
	     | HOME=/tmp ftp -pinv 127.0.0.1 2121 > /dev/null 2>&1 \
	     || { echo "smoke-ftp: ftp client failed"; exit 1; }; \
	 cmp build/cmd/ifconfig.elf $$DOWN \
	     || { echo "smoke-ftp: roundtripped ELF differs from original"; exit 1; }; \
	 echo "==> exec uploaded ELF over telnet"; \
	 OUT=$$( ( sleep 1; printf 'exec /home/foo\r'; sleep 5 ) \
	          | timeout 10 nc localhost 2323 2>&1 ); \
	 echo "$$OUT" | grep -q 'eth0' \
	     || { echo "smoke-ftp: uploaded ELF did not produce expected eth0 output"; \
	          echo "$$OUT"; exit 1; }; \
	 echo "==> smoke-ftp PASS"
endif

# Pi/Debian host friendly defaults: no display window, single-thread
# TCG so QEMU can't spin more than one host CPU even when WFI on the
# raspi3b model doesn't properly idle the vCPU threads.  Wrap in
# taskset -c 0 if you also want to cap that one core's wall usage.
ifeq ($(ARCH),aarch64)
.PHONY: run-thrifty run-gui
run-thrifty: $(KERNEL)
	$(QEMU) -M raspi3b -display none -accel tcg,thread=single \
	        -semihosting-config enable=on,target=native \
	        -serial stdio -serial null -kernel $(KERNEL)

# Boot with the splash window.  Only safe where QEMU's display
# backend works (X / Wayland with the right libs); on some headless
# / Pi setups the GTK init segfaults the QEMU process.  Ctrl-C from
# the host terminal still kills QEMU (signal=on default on stdio).
run-gui: $(KERNEL)
	$(QEMU) -M raspi3b -semihosting-config enable=on,target=native \
	        -serial stdio -kernel $(KERNEL)
endif

clean:
	rm -rf build

# Last-resort QEMU killer for when Ctrl-A x doesn't work because
# your terminal multiplexer (screen, tmux) is eating Ctrl-A first.
# Use exact process name (-x) to avoid matching shells that happen
# to have the qemu binary path in their environment.
.PHONY: stop
stop:
	@pkill -x qemu-system-aarch64 2>/dev/null || true
	@pkill -x qemu-system-arm     2>/dev/null || true
	@echo "QEMU killed (if any was running)."

# --- User-side init binary (AArch64 only) ----------------------------
# Compile user/init.c with the same cross toolchain as the kernel,
# link with user/linker.ld at VA 0x10000000, objcopy to a raw .bin,
# then uts/aarch64/userblob.S incbin's the result into the kernel.

ifneq ($(filter $(ARCH),aarch64 virt),)

USER_CC     := $(CROSS)gcc
USER_LD     := $(CROSS)ld
USER_CFLAGS := -Wall -Wextra -Werror -std=gnu11 \
               -ffreestanding -nostdlib -nostartfiles \
               -fno-stack-protector -fno-pie -fno-pic \
               -mcpu=cortex-a53 -mgeneral-regs-only \
               -O2 -g

$(USER_BUILD)/init.o: user/init.c user/syscall.h user/kc.c | $(USER_BUILD)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/init.elf: $(USER_BUILD)/init.o user/linker.ld
	$(USER_LD) -nostdlib -static -T user/linker.ld -o $@ $<

$(USER_BIN): $(USER_BUILD)/init.elf
	$(OBJCOPY) -O binary $< $@

# ---- Standalone ELF user programs (loaded via sys_execve) ---------------
# hello: minimal "Hello from exec'd ELF" smoke test.
# Linked at 0x20000000 (EXEC_VA) via prog_linker.ld, kept as a full ELF
# (not stripped to binary) so the kernel ELF loader can parse program
# headers and map PT_LOAD segments.  Stripped to remove debug sections
# so the 64 KB elf_read_buf in sys_execve_impl is more than sufficient.

HELLO_ELF := $(USER_BUILD)/hello.elf

$(USER_BUILD)/hello.o: user/hello.c user/syscall.h | $(USER_BUILD)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(HELLO_ELF): $(USER_BUILD)/hello.o user/prog_linker.ld
	$(USER_LD) -nostdlib -static -s -z max-page-size=4096 -T user/prog_linker.ld -o $@ $<

$(USER_BUILD):
	mkdir -p $@

# ---- libc + /usr/bin standalone ELF programs -----------------------
CMD_BUILD  := build/cmd
LIBC_DIR    := lib/libc
LIBC_SRCS   := $(LIBC_DIR)/src/string.c \
               $(LIBC_DIR)/src/printf.c  \
               $(LIBC_DIR)/src/stdlib.c  \
               $(LIBC_DIR)/src/io.c      \
               $(LIBC_DIR)/src/signal.c  \
               $(LIBC_DIR)/src/malloc.c  \
               $(LIBC_DIR)/src/file.c    \
               $(LIBC_DIR)/src/ctype.c   \
               $(LIBC_DIR)/src/errno.c   \
               $(LIBC_DIR)/src/time.c
LIBC_OBJS   := $(patsubst $(LIBC_DIR)/src/%.c,$(CMD_BUILD)/libc/%.o,$(LIBC_SRCS))
LIBC_CRT0   := $(CMD_BUILD)/libc/crt0.o
LIBC_SETJMP := $(CMD_BUILD)/libc/setjmp.o
LIBC_A      := $(CMD_BUILD)/libc/libc.a

LIBC_CFLAGS := -Wall -Wextra -Werror -std=gnu11 \
               -ffreestanding -nostdlib -nostartfiles \
               -fno-stack-protector -fPIC \
               -mcpu=cortex-a53 -mgeneral-regs-only \
               -O2 -g \
               -I$(LIBC_DIR)/include

$(CMD_BUILD)/libc:
	mkdir -p $@

$(CMD_BUILD)/libc/%.o: $(LIBC_DIR)/src/%.c | $(CMD_BUILD)/libc
	$(USER_CC) $(LIBC_CFLAGS) -c $< -o $@

$(LIBC_CRT0): $(LIBC_DIR)/aarch64/crt0.S | $(CMD_BUILD)/libc
	$(USER_CC) $(filter-out -finstrument-functions,$(LIBC_CFLAGS)) \
	           -c $< -o $@

$(LIBC_SETJMP): $(LIBC_DIR)/aarch64/setjmp.S | $(CMD_BUILD)/libc
	$(USER_CC) $(filter-out -finstrument-functions,$(LIBC_CFLAGS)) \
	           -c $< -o $@

$(LIBC_A): $(LIBC_OBJS) $(LIBC_SETJMP)
	$(CROSS)ar rcs $@ $^

# ---- /usr/bin standalone ELF programs --------------------------------
CMD_BUILD  := build/cmd
CMD_NAMES  := ps ping ifconfig netstat test tcpconnect ftpd \
              ls ll cat cp mv rm head tail wc grep echo uptime
CMD_ELFS   := $(addprefix $(CMD_BUILD)/, $(addsuffix .elf, $(CMD_NAMES)))

CMD_CFLAGS := -Wall -Wextra -Werror -std=gnu11 \
              -ffreestanding -nostdlib -nostartfiles \
              -fno-stack-protector -fno-pie -fno-pic \
              -mcpu=cortex-a53 -mgeneral-regs-only \
              -O2 -g \
              -I$(LIBC_DIR)/include \
              -Iinclude

$(CMD_BUILD)/%.o: cmd/%.c | $(CMD_BUILD)
	$(USER_CC) $(CMD_CFLAGS) -c $< -o $@

$(CMD_BUILD)/%.elf: $(CMD_BUILD)/%.o $(LIBC_CRT0) $(LIBC_A) user/prog_linker.ld
	$(USER_LD) -nostdlib -static -s -z max-page-size=4096 \
	           -T user/prog_linker.ld -o $@ $(LIBC_CRT0) $< $(LIBC_A)

$(CMD_BUILD):
	mkdir -p $@

$(BUILD)/uts/aarch64/usrblobs.o: $(CMD_ELFS) uts/aarch64/usrblobs.S
	$(CC) $(ASFLAGS) -c uts/aarch64/usrblobs.S -o $@

# Tell make that userblob.o / helloblob.o depend on the files they incbin.
$(BUILD)/uts/aarch64/userblob.o:  $(USER_BIN)
$(BUILD)/uts/aarch64/helloblob.o: $(HELLO_ELF)

endif

# Pull in compiler-emitted header deps so editing a header forces a
# rebuild of every .o that includes it.  Placed at the very end so
# the .d files' rules don't poison the default-goal selection.
-include $(DEPS)
