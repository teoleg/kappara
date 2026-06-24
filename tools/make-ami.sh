#!/usr/bin/env bash
#
# tools/make-ami.sh -- build a raw EBS-compatible boot disk image
#
# AWS.md stage G.  Produces a single GPT-partitioned raw image
# with one EFI System Partition (FAT32) containing
# \EFI\BOOT\BOOTAA64.EFI -- our kernel.img, which is also a valid
# PE32+ EFI Application (stage B).  Upload this file to S3, then
# run `aws ec2 import-image` or `register-image` to turn it into
# an AMI you can launch on a Graviton instance.
#
# Why not also stamp /home onto the same image?
#
# On AWS, root volumes are throwaway-and-redeploy ("immutable
# infrastructure"); persistent data lives on a separate EBS volume
# that survives stop/start, snapshots independently, encrypts with
# a separate KMS key, etc.  At launch time, attach a second
# (initially raw / zeroed) EBS volume; kappara's nvme_init picks
# it up as nvme0n1, and stage F.1's "kfs_mount or mkimage" logic
# formats it on first boot.  Subsequent reboots preserve /home.
#
# Locally under QEMU the same shape works: pass two `-device nvme`
# args, the first one carrying this AMI image, the second one a
# blank file.
#
# Usage:
#   tools/make-ami.sh <kernel.img> <out.img> [esp_size_mb]
#
# Defaults: esp_size_mb=128 (room for the kernel + future
# extensions; minimum is whatever the FAT32 spec requires, ~33 MB).

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <kernel.img> <out.img> [esp_size_mb]" >&2
    exit 1
fi

KERNEL=$1
OUT=$2
ESP_MB=${3:-128}

if [[ ! -f $KERNEL ]]; then
    echo "make-ami: kernel image not found: $KERNEL" >&2
    exit 1
fi

# Locate tools that may live in /sbin -- coreutils PATH on some
# distros doesn't include them.
PATH=$PATH:/sbin:/usr/sbin
for t in parted mkfs.fat mcopy mmd; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "make-ami: missing tool '$t' (install parted dosfstools mtools)" >&2
        exit 1
    }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Sizing: 1 MiB GPT header + ESP partition + 33 sectors GPT
# backup at the end.  Round up to MiB so partition alignment is
# clean.
SIZE_MB=$((ESP_MB + 2))
echo "make-ami: building $OUT (${SIZE_MB} MiB; ESP=${ESP_MB} MiB)"

# 1. Empty raw image
truncate -s ${SIZE_MB}M "$OUT"

# 2. GPT + one ESP partition spanning ~all of it.
parted -s "$OUT" mklabel gpt
parted -s "$OUT" mkpart ESP fat32 1MiB 100%
parted -s "$OUT" set 1 esp on

# 3. Build the FAT32 filesystem in a sidecar file, then splice it
#    into the partition slot.  mkfs.fat refuses to operate on a
#    raw partition slice of another file, so the sidecar dance is
#    the portable workaround.
START_BYTE=$(parted -ms "$OUT" unit B print \
             | awk -F: '/^1:/ { sub("B","",$2); print $2 }')
FAT_BYTES=$(( $(stat -c %s "$OUT") - START_BYTE ))
FAT=$TMP/esp.fat
truncate -s "$FAT_BYTES" "$FAT"
mkfs.fat -F32 -n KAPPARA "$FAT" >/dev/null

# 4. \EFI\BOOT\BOOTAA64.EFI = our kernel (it's a PE32+ EFI app).
mmd  -i "$FAT" ::/EFI ::/EFI/BOOT
mcopy -i "$FAT" "$KERNEL" ::/EFI/BOOT/BOOTAA64.EFI

# 5. Splice the FAT image into the ESP slot of the GPT disk.  Use
#    512-byte blocks (== ESP partitions are sector-aligned), not
#    bs=1 -- a 128 MiB ESP at bs=1 takes minutes for no good reason.
START_SECTOR=$(( START_BYTE / 512 ))
dd if="$FAT" of="$OUT" bs=512 seek="$START_SECTOR" conv=notrunc \
   status=none

echo "make-ami: done -- $OUT ($(stat -c %s "$OUT") bytes)"
echo "make-ami:   ESP starts at byte $START_BYTE"
echo "make-ami:   boot via: qemu-system-aarch64 -bios AAVMF_CODE.fd \\"
echo "make-ami:               -drive if=none,id=hd,format=raw,file=$OUT \\"
echo "make-ami:               -device virtio-blk-device,drive=hd"
