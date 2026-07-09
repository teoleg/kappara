# Single-arch build: QEMU `virt` (generic aarch64) -- the on-ramp to
# AWS EC2 Graviton (see docs/AWS.md).  Pi-specific drivers used to
# live alongside this in arch/aarch64/; they're now in attic/raspi3b/
# for history.  Reinstating them means copying the files back and
# restoring the aarch64 ARCH branch that was here.
BUILD   := build

CROSS   ?= aarch64-linux-gnu-
ARCH_CFLAGS := -mcpu=cortex-a72 -mgeneral-regs-only \
               -Iuts/aarch64 -DPLATFORM_VIRT
LINKER_LD   := uts/virt/linker.ld
USER_BUILD  := build/user
USER_BIN    := $(USER_BUILD)/init.bin
USERBLOB_EXTRA_DEP := $(USER_BIN)
ARCH_OBJS   := \
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
    $(BUILD)/uts/virt/efi_main.o \
    $(BUILD)/uts/virt/acpi.o \
    $(BUILD)/uts/virt/pcie.o \
    $(BUILD)/uts/virt/nvme.o \
    $(BUILD)/uts/virt/ena.o \
    $(BUILD)/uts/aarch64/userblob.o \
    $(BUILD)/uts/aarch64/helloblob.o \
    $(BUILD)/uts/aarch64/usrblobs.o
KERNEL_OBJS := \
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
    $(BUILD)/uts/os/net/arp.o \
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
                 -semihosting-config enable=on,target=native \
                 -global virtio-mmio.force-legacy=false \
                 -netdev user,id=n0,hostfwd=tcp::2323-:23,hostfwd=tcp::2121-:21,hostfwd=tcp::30000-:30000,hostfwd=tcp::30001-:30001,hostfwd=tcp::30002-:30002,hostfwd=tcp::30003-:30003,hostfwd=tcp::30004-:30004,hostfwd=tcp::30005-:30005,hostfwd=tcp::30006-:30006,hostfwd=tcp::30007-:30007 \
                 -device virtio-net-device,netdev=n0

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

$(KERNEL): $(ELF)
	$(OBJCOPY) -O binary $< $@
	@# AWS.md stage B: pad the file so its size matches the PE
	@# SizeOfImage we baked into the header.  Without this the
	@# .text section's SizeOfRawData (= __kernel_end - __kernel_start
	@# - 0x1000) extends past the actual file end (objcopy doesn't
	@# carry trailing BSS bytes), and EDK II's PE loader refuses
	@# with EFI_UNSUPPORTED.
	@./tools/pad_pe.py $@

# Foreground exec so QEMU owns the controlling TTY -- otherwise
# `-nographic` shows the boot splash + shell prompt but stdin
# keystrokes never reach QEMU (they go to whatever's in the
# terminal's foreground process group, which would be `wait`,
# not us).  Quit with Ctrl-A x (QEMU's `-nographic` escape).
run: $(KERNEL)
	@exec $(QEMU) $(QEMU_ARGS) -kernel $(KERNEL)

# AWS.md stage G: produce a single GPT-partitioned raw image with an
# ESP (FAT32) carrying our kernel as \EFI\BOOT\BOOTAA64.EFI.  Upload
# the result to S3 and import as an AMI on EC2.  Locally the same
# image boots under QEMU + AAVMF -- see `make ami-run`.
AMI_IMG := $(BUILD)/kappara-ami.img

.PHONY: ami ami-run
ami: $(AMI_IMG)
$(AMI_IMG): $(KERNEL) tools/make-ami.sh
	@./tools/make-ami.sh $(KERNEL) $@

# Smoke-boot the AMI under AAVMF.  Needs qemu-efi-aarch64 +
# parted/dosfstools/mtools installed.  Brings up a second NVMe
# namespace as /home so kappara's stage F.1 mount finds it; a
# real EC2 instance would attach this as a second EBS volume.
AMI_HOME    := $(BUILD)/kappara-ami-home.img
AMI_VARS    := $(BUILD)/kappara-ami-vars.fd
AAVMF_CODE  := /usr/share/AAVMF/AAVMF_CODE.fd
AAVMF_VARS  := /usr/share/AAVMF/AAVMF_VARS.fd

ami-run: $(AMI_IMG)
	@cp $(AAVMF_VARS) $(AMI_VARS)
	@[ -f $(AMI_HOME) ] || truncate -s 64M $(AMI_HOME)
	@exec $(QEMU) -M virt,gic-version=3 -cpu cortex-a72 -nographic -m 256 \
	    -semihosting-config enable=on,target=native \
	    -global virtio-mmio.force-legacy=false \
	    -drive if=pflash,format=raw,readonly=on,file=$(AAVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(AMI_VARS) \
	    -drive id=ami,format=raw,if=none,file=$(AMI_IMG) \
	    -device virtio-blk-device,drive=ami,bootindex=1 \
	    -drive id=home,format=raw,if=none,file=$(AMI_HOME) \
	    -device nvme,drive=home,serial=kappara-home \
	    -netdev user,id=n0 \
	    -device virtio-net-device,netdev=n0

# ARCH=virt only: boot QEMU virt headless in the background, wait
# for the in-kernel telnetd to come up on hostfwd port 2323, then
# launch an `nc` client in the foreground.  On exit (Ctrl-C or `quit`
# inside the shell) both the client and the background QEMU get
# torn down so you don't leave a dangling vCPU thread behind.
#
# Use this when you want to drive the shell over telnet -- the kernel
# splash + boot trace go to /tmp/kappara-virt.log instead of stdout,
# and stdin/stdout belong to the telnet session on tty4.
.PHONY: run-telnet run-ftp smoke-ftp smoke-sdk smoke-linux smoke-linux-mmap test
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

# INDIE.md stage 1 deliverable: build hello.c outside the kappara tree
# via kappara-cc, upload it via FTP, exec it via telnet, check output.
# Proves "external developer can ship code for kappara" end-to-end.
smoke-sdk: $(KERNEL) sdk
	@command -v ftp >/dev/null 2>&1 || { \
		echo "smoke-sdk: need 'ftp' on PATH"; exit 1; }
	@command -v nc >/dev/null 2>&1 || { \
		echo "smoke-sdk: need 'nc' on PATH"; exit 1; }
	@TMPDIR=$$(mktemp -d); \
	 LOG=/tmp/kappara-sdk.log; \
	 trap "rm -rf $$TMPDIR" EXIT INT TERM; \
	 echo "==> build hello.c from $$TMPDIR (no uts/ access)"; \
	 cp tools/sdk-test/hello.c $$TMPDIR/hello.c; \
	 ( cd $$TMPDIR && $(abspath $(SDK_BUILD))/bin/kappara-cc hello.c -o hello ) \
	     2>&1 | grep -v "^$$" | grep -v "build-id"; \
	 test -f $$TMPDIR/hello || { echo "smoke-sdk: build failed"; exit 1; }; \
	 file $$TMPDIR/hello | grep -q "ELF 64-bit LSB pie executable" \
	     || { echo "smoke-sdk: wrong ELF type"; file $$TMPDIR/hello; exit 1; }; \
	 echo "==> boot kappara virt"; \
	 rm -f $$LOG; \
	 $(QEMU) $(QEMU_ARGS) -kernel $(KERNEL) > $$LOG 2>&1 & \
	 QPID=$$!; \
	 trap "kill $$QPID 2>/dev/null; rm -rf $$TMPDIR; wait 2>/dev/null" EXIT INT TERM; \
	 for i in 1 2 3 4 5 6 7 8 9 10; do \
	     sleep 1; \
	     grep -q 'ftpd: listening' $$LOG 2>/dev/null \
	      && grep -q 'telnetd: listening' $$LOG 2>/dev/null && break; \
	 done; \
	 grep -q 'ftpd: listening' $$LOG 2>/dev/null || { \
	     echo "smoke-sdk: ftpd did not come up"; tail -20 $$LOG; exit 1; }; \
	 echo "==> upload hello to /home"; \
	 printf 'user anonymous any\nbinary\ncd /home\nput %s/hello hello\nquit\n' $$TMPDIR \
	     | HOME=/tmp ftp -pinv 127.0.0.1 2121 > /dev/null 2>&1 \
	     || { echo "smoke-sdk: ftp upload failed"; exit 1; }; \
	 echo "==> exec /home/hello over telnet"; \
	 OUT=$$( ( sleep 1; printf 'exec /home/hello 42\r'; sleep 5 ) \
	          | timeout 10 nc localhost 2323 2>&1 ); \
	 echo "$$OUT" | grep -q 'hello from outside the tree' \
	     || { echo "smoke-sdk: expected hello message not seen"; \
	          echo "$$OUT"; exit 1; }; \
	 echo "$$OUT" | grep -q 'first arg: 42 (atoi: 42)' \
	     || { echo "smoke-sdk: argv/atoi didn't work"; \
	          echo "$$OUT"; exit 1; }; \
	 echo "==> smoke-sdk PASS"

# INDIE.md Path B stage 1: run a Linux-ABI static-pie binary.  Builds
# a tiny C file that uses raw Linux syscall numbers (write=64,
# exit=93) -- no kappara headers, no libc.  Uploads via FTP, exec's,
# checks the binary printed its "hello".
smoke-linux: $(KERNEL)
	@command -v ftp >/dev/null 2>&1 || { \
		echo "smoke-linux: need 'ftp' on PATH"; exit 1; }
	@command -v nc >/dev/null 2>&1 || { \
		echo "smoke-linux: need 'nc' on PATH"; exit 1; }
	@TMPDIR=$$(mktemp -d); \
	 LOG=/tmp/kappara-linux.log; \
	 trap "rm -rf $$TMPDIR" EXIT INT TERM; \
	 echo "==> build linux-abi hello.c using raw aarch64 syscalls"; \
	 cp tools/sdk-test/linux-hello.c $$TMPDIR/hello.c; \
	 aarch64-linux-gnu-gcc -static-pie -nostdlib -nostartfiles \
	     -ffreestanding -fno-stack-protector -mgeneral-regs-only \
	     -Wl,-e,_start \
	     -o $$TMPDIR/hello $$TMPDIR/hello.c 2>&1 | grep -v "^$$" || true; \
	 test -f $$TMPDIR/hello || { echo "smoke-linux: build failed"; exit 1; }; \
	 echo "==> boot kappara virt"; \
	 rm -f $$LOG; \
	 $(QEMU) $(QEMU_ARGS) -kernel $(KERNEL) > $$LOG 2>&1 & \
	 QPID=$$!; \
	 trap "kill $$QPID 2>/dev/null; rm -rf $$TMPDIR; wait 2>/dev/null" EXIT INT TERM; \
	 for i in 1 2 3 4 5 6 7 8 9 10; do \
	     sleep 1; \
	     grep -q 'ftpd: listening' $$LOG 2>/dev/null && break; \
	 done; \
	 grep -q 'ftpd: listening' $$LOG 2>/dev/null || { \
	     echo "smoke-linux: ftpd did not come up"; tail -20 $$LOG; exit 1; }; \
	 echo "==> upload Linux-ABI binary to /home"; \
	 printf 'user anonymous any\nbinary\ncd /home\nput %s/hello hello\nquit\n' $$TMPDIR \
	     | HOME=/tmp ftp -pinv 127.0.0.1 2121 > /dev/null 2>&1 \
	     || { echo "smoke-linux: ftp upload failed"; exit 1; }; \
	 echo "==> exec /home/hello over telnet"; \
	 OUT=$$( ( sleep 1; printf 'exec /home/hello\r'; sleep 5 ) \
	          | timeout 10 nc localhost 2323 2>&1 ); \
	 echo "$$OUT" | grep -q 'hello from Linux-ABI' \
	     || { echo "smoke-linux: expected hello not seen"; \
	          echo "$$OUT"; exit 1; }; \
	 echo "==> smoke-linux PASS"

# INDIE.md Path B stage 3: mmap/munmap round-trip via Linux ABI.
smoke-linux-mmap: $(KERNEL)
	@TMPDIR=$$(mktemp -d); \
	 LOG=/tmp/kappara-linux-mmap.log; \
	 trap "rm -rf $$TMPDIR" EXIT INT TERM; \
	 echo "==> build linux-mmap.c"; \
	 aarch64-linux-gnu-gcc -static-pie -nostdlib -nostartfiles \
	     -ffreestanding -fno-stack-protector -mgeneral-regs-only \
	     -Wl,-e,_start \
	     -o $$TMPDIR/mmap tools/sdk-test/linux-mmap.c \
	     2>&1 | grep -v "^$$" || true; \
	 test -f $$TMPDIR/mmap || { echo "smoke-linux-mmap: build failed"; exit 1; }; \
	 echo "==> boot kappara virt"; \
	 rm -f $$LOG; \
	 $(QEMU) $(QEMU_ARGS) -kernel $(KERNEL) > $$LOG 2>&1 & \
	 QPID=$$!; \
	 trap "kill $$QPID 2>/dev/null; rm -rf $$TMPDIR; wait 2>/dev/null" EXIT INT TERM; \
	 for i in 1 2 3 4 5 6 7 8 9 10; do \
	     sleep 1; \
	     grep -q 'ftpd: listening' $$LOG 2>/dev/null && break; \
	 done; \
	 grep -q 'ftpd: listening' $$LOG 2>/dev/null || { \
	     echo "smoke-linux-mmap: ftpd did not come up"; tail -20 $$LOG; exit 1; }; \
	 echo "==> upload + exec"; \
	 printf 'user anonymous any\nbinary\ncd /home\nput %s/mmap mmap\nquit\n' $$TMPDIR \
	     | HOME=/tmp ftp -pinv 127.0.0.1 2121 > /dev/null 2>&1 \
	     || { echo "smoke-linux-mmap: ftp upload failed"; exit 1; }; \
	 OUT=$$( ( sleep 1; printf 'exec /home/mmap\r'; sleep 5 ) \
	          | timeout 10 nc localhost 2323 2>&1 ); \
	 echo "$$OUT" | grep -q 'page contents OK' \
	     || { echo "smoke-linux-mmap: mmap roundtrip failed"; \
	          echo "$$OUT"; exit 1; }; \
	 echo "==> smoke-linux-mmap PASS"

# Unified test runner.  Runs every smoke target in sequence + cmd/test
# all under QEMU.  This is the canonical "is HEAD healthy?" check.
test: smoke-ftp smoke-sdk smoke-linux smoke-linux-mmap
	@TMPDIR=$$(mktemp -d); \
	 trap "rm -rf $$TMPDIR" EXIT; \
	 printf '#!/bin/sh\nsleep 5\necho "test all"\nsleep 14\n' \
	     > $$TMPDIR/in.sh; \
	 chmod +x $$TMPDIR/in.sh; \
	 echo "==> cmd/test all under QEMU"; \
	 OUT=$$($$TMPDIR/in.sh | timeout 24 $(QEMU) $(QEMU_ARGS) \
	         -kernel $(KERNEL) 2>&1); \
	 echo "$$OUT" | grep -E '^test:' | tail -1; \
	 echo "$$OUT" | grep -q 'test: 15 passed, 0 failed' \
	     || { echo "test: cmd/test all did not pass 15/15"; \
	          echo "$$OUT" | tail -20; exit 1; }; \
	 echo "==> ALL TESTS PASS"

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

$(HELLO_ELF): $(USER_BUILD)/hello.o user/hello_linker.ld
	$(USER_LD) -nostdlib -static -s -z max-page-size=4096 -T user/hello_linker.ld -o $@ $<

$(USER_BUILD):
	mkdir -p $@

# ---- ld-kappara.so: stage 5 user-space dynamic linker (DYNAMIC.md) ----
# Loaded by the kernel at LD_VA = 0x30000000 alongside any ET_DYN
# application.  Trap frame enters here; pass-through for now (parses
# auxv for AT_ENTRY, jumps).  See lib/ld-kappara/ld_start.S.
LDK_BUILD   := build/ld-kappara
LDK_ELF     := $(LDK_BUILD)/ld-kappara.so
LDK_OBJS    := $(LDK_BUILD)/ld_start.o $(LDK_BUILD)/ld_main.o

LDK_CFLAGS  := -Wall -Wextra -Werror -std=gnu11 \
               -ffreestanding -nostdlib -nostartfiles \
               -fno-stack-protector -fno-pie -fno-pic \
               -mcpu=cortex-a53 -mgeneral-regs-only \
               -O2 -g -Iinclude

$(LDK_BUILD):
	mkdir -p $@

$(LDK_BUILD)/ld_start.o: lib/ld-kappara/ld_start.S | $(LDK_BUILD)
	$(USER_CC) $(LDK_CFLAGS) -c $< -o $@

$(LDK_BUILD)/ld_main.o: lib/ld-kappara/ld_main.c | $(LDK_BUILD)
	$(USER_CC) $(LDK_CFLAGS) -c $< -o $@

$(LDK_ELF): $(LDK_OBJS) lib/ld-kappara/linker.ld
	$(USER_LD) -nostdlib -static -s -z max-page-size=4096 \
	           -T lib/ld-kappara/linker.ld -o $@ $(LDK_OBJS)

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
               $(LIBC_DIR)/src/time.c    \
               $(LIBC_DIR)/src/dlfcn.c
LIBC_OBJS   := $(patsubst $(LIBC_DIR)/src/%.c,$(CMD_BUILD)/libc/%.o,$(LIBC_SRCS))
LIBC_CRT0   := $(CMD_BUILD)/libc/crt0.o
LIBC_SETJMP := $(CMD_BUILD)/libc/setjmp.o
LIBC_A      := $(CMD_BUILD)/libc/libc.a
LIBC_SO     := $(CMD_BUILD)/libc/libc.so

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

# DYNAMIC.md stage 6: libc.so shared object.  Same .o files (already
# -fPIC from stage 2), linked with -shared and a proper soname so cmd
# binaries can DT_NEEDED it.  Loaded by the kernel at LIBC_VA = 0x40000000
# for every ET_DYN exec; kernel applies its R_AARCH64_RELATIVE relocs
# and resolves cmd binaries' GLOB_DAT / JUMP_SLOT entries against its
# dynsym (move to ld.so + dlopen in stage 7).
$(LIBC_SO): $(LIBC_OBJS) $(LIBC_SETJMP)
	$(USER_LD) -shared -nostdlib -soname libc.so \
	           -Bsymbolic \
	           -z max-page-size=4096 -s \
	           -o $@ $(LIBC_OBJS) $(LIBC_SETJMP)

# DYNAMIC.md stage 7: tiny self-contained shared object for testing
# dlopen / dlsym end-to-end.  No DT_NEEDED, just two exported functions
# and one RELATIVE-relocated string pointer.
DLTEST_BUILD := build/dltest
DLTEST_SO    := $(DLTEST_BUILD)/libdltest.so

$(DLTEST_BUILD):
	mkdir -p $@

$(DLTEST_BUILD)/dltest.o: lib/libdltest/dltest.c | $(DLTEST_BUILD)
	$(USER_CC) -Wall -Wextra -Werror -std=gnu11 \
	           -ffreestanding -nostdlib -nostartfiles \
	           -fno-stack-protector -fPIC \
	           -mcpu=cortex-a53 -mgeneral-regs-only \
	           -O2 -g \
	           -I$(LIBC_DIR)/include \
	           -c $< -o $@

# libdltest.so DT_NEEDED's libc.so so its printf / strlen calls go
# through the dlopen resolver's cross-DSO path.
$(DLTEST_SO): $(DLTEST_BUILD)/dltest.o $(LIBC_SO)
	$(USER_LD) -shared -nostdlib -soname libdltest.so \
	           -z max-page-size=4096 -s \
	           --no-dynamic-linker \
	           -o $@ $< -L$(CMD_BUILD)/libc -l:libc.so

# ---- INDIE.md stage 1: publishable SDK ------------------------------
# Collect the artefacts an external build needs (libc.so, crt0.o,
# headers, linker scripts, the kappara-cc wrapper) into build/sdk/
# so people can build cmd-shaped binaries without checking out uts/.
SDK_BUILD := build/sdk

$(SDK_BUILD)/.stamp: $(LIBC_SO) $(LIBC_CRT0) $(LDK_ELF) \
                    tools/kappara-cc.in user/prog_linker.ld
	rm -rf $(SDK_BUILD)
	mkdir -p $(SDK_BUILD)/sysroot/include
	mkdir -p $(SDK_BUILD)/sysroot/lib
	mkdir -p $(SDK_BUILD)/bin
	# libc headers
	cp -r $(LIBC_DIR)/include/. $(SDK_BUILD)/sysroot/include/
	# selected kernel ABI headers (syscall numbers, ELF types)
	mkdir -p $(SDK_BUILD)/sysroot/include/kappara/abi
	cp include/kappara/abi/syscall.h $(SDK_BUILD)/sysroot/include/kappara/abi/
	cp include/kappara/abi/elf.h     $(SDK_BUILD)/sysroot/include/kappara/abi/
	# libc.so + crt0
	cp $(LIBC_SO)   $(SDK_BUILD)/sysroot/lib/libc.so
	cp $(LIBC_CRT0) $(SDK_BUILD)/sysroot/lib/crt0.o
	cp user/prog_linker.ld $(SDK_BUILD)/sysroot/lib/prog_linker.ld
	# ld-kappara.so for completeness (kernel currently embeds its own
	# copy, but binaries that ship via PT_INTERP -> /lib/ld-kappara.so
	# would reference this artefact).
	cp $(LDK_ELF) $(SDK_BUILD)/sysroot/lib/ld-kappara.so
	# kappara-cc wrapper.  SDK_ROOT is filled in at install time
	# (or pointed at $(SDK_BUILD) when used in-tree).
	sed 's,@SDK_ROOT@,$(abspath $(SDK_BUILD)),g' \
	    tools/kappara-cc.in > $(SDK_BUILD)/bin/kappara-cc
	chmod +x $(SDK_BUILD)/bin/kappara-cc
	touch $@

sdk: $(SDK_BUILD)/.stamp
	@echo "==> SDK ready: $(SDK_BUILD)"
	@echo "    kappara-cc: $(SDK_BUILD)/bin/kappara-cc"
	@echo "    sysroot:    $(SDK_BUILD)/sysroot"

sdk-tarball: sdk
	tar -C $(SDK_BUILD) -czf $(SDK_BUILD)/../kappara-sdk.tar.gz .
	@echo "==> SDK tarball: build/kappara-sdk.tar.gz ($$(du -h build/kappara-sdk.tar.gz | cut -f1))"

.PHONY: sdk sdk-tarball

# ---- /usr/bin standalone ELF programs --------------------------------
CMD_BUILD  := build/cmd
CMD_NAMES  := ps ping ifconfig netstat test tcpconnect ftpd \
              ls ll cat cp mv rm head tail wc grep echo uptime \
              nm ldd objdump host mount more
CMD_ELFS   := $(addprefix $(CMD_BUILD)/, $(addsuffix .elf, $(CMD_NAMES)))

CMD_CFLAGS := -Wall -Wextra -Werror -std=gnu11 \
              -ffreestanding -nostdlib -nostartfiles \
              -fno-stack-protector -fPIE \
              -mcpu=cortex-a53 -mgeneral-regs-only \
              -O2 -g \
              -I$(LIBC_DIR)/include \
              -Iinclude

$(CMD_BUILD)/%.o: cmd/%.c | $(CMD_BUILD)
	$(USER_CC) $(CMD_CFLAGS) -c $< -o $@

$(CMD_BUILD)/%.elf: $(CMD_BUILD)/%.o $(LIBC_CRT0) $(LIBC_SO) user/prog_linker.ld
	$(USER_LD) -nostdlib -pie -s -z max-page-size=4096 \
	           --no-dynamic-linker \
	           -T user/prog_linker.ld -o $@ $(LIBC_CRT0) $< \
	           -L$(CMD_BUILD)/libc -l:libc.so

$(CMD_BUILD):
	mkdir -p $@

$(BUILD)/uts/aarch64/usrblobs.o: $(CMD_ELFS) $(LDK_ELF) $(LIBC_SO) $(DLTEST_SO) uts/aarch64/usrblobs.S
	$(CC) $(ASFLAGS) -c uts/aarch64/usrblobs.S -o $@

# Tell make that userblob.o / helloblob.o depend on the files they incbin.
$(BUILD)/uts/aarch64/userblob.o:  $(USER_BIN)
$(BUILD)/uts/aarch64/helloblob.o: $(HELLO_ELF)

# Pull in compiler-emitted header deps so editing a header forces a
# rebuild of every .o that includes it.  Placed at the very end so
# the .d files' rules don't poison the default-goal selection.
-include $(DEPS)
