#!/bin/sh
# gen-install-grub-blob.sh <outdir>
# Generates grub_boot.img (512-byte MBR sector) + grub_core.img (GRUB core
# image) for embedding into any disk formatted with a single MBR partition
# starting at LBA 2048 (kernel/mbr.c's MBR_TRACK_ALIGN_LBA). No sudo, no
# loop devices — grub-bios-setup targets a plain file via a device.map,
# the same technique grub-mkrescue uses internally for this repo's ISO.
set -e

OUT="$1"
if [ -z "$OUT" ]; then
    echo "usage: gen-install-grub-blob.sh <outdir>" >&2
    exit 1
fi
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

MODS="biosdisk part_msdos fat multiboot2 normal configfile"

# grub-bios-setup always tries to open the real OS block device backing
# --directory (to "guess a root device" — unused for the actual BIOS
# embedding bytes, but still opened unconditionally). Real disks here are
# root:disk 0660 and we have no sudo, so that open() would fail. On a
# tmpfs directory that guess short-circuits to the literal mount-source
# string "tmpfs" instead of touching a real device — it then resolves
# that as a relative path, so a same-named dummy file satisfies it. Do
# the whole GRUB dance on such a tmpfs work dir, then copy results out.
WORK="$(mktemp -d /tmp/gen-install-grub-blob.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
touch tmpfs

grub-mkimage -O i386-pc -o grub_core.img -p '(hd0,msdos1)/' $MODS
cp /usr/lib/grub/i386-pc/boot.img .

truncate -s 2M scratch.img
printf 'label: dos\nstart=2048, type=c\n' | sfdisk scratch.img >/dev/null
echo "(hd0) $WORK/scratch.img" > device.map

grub-bios-setup \
    --directory=. \
    --device-map=device.map \
    --core-image=grub_core.img \
    --skip-fs-probe \
    --force \
    scratch.img

# grub-bios-setup patches core.img in place (blocklist header + Reed-
# Solomon redundancy) before writing it after the MBR, so the bytes on
# scratch.img differ from the pristine grub-mkimage output — extract the
# actual embedded copy. With Reed-Solomon on (the default), GRUB embeds
# exactly 2x the core's sector count (util/setup.c: maxsec = 2*core_sectors,
# and pc_partition_map_embed claims the full maxsec whenever the post-MBR
# gap is large enough, as ours always is here) — verified empirically
# against two different module sets before hardcoding the factor of 2.
CORE_SIZE=$(wc -c < grub_core.img)
CORE_SECTORS=$(( ( (CORE_SIZE + 511) / 512 ) * 2 ))

dd if=scratch.img of="$OUT/grub_boot.img" bs=512 count=1 2>/dev/null
dd if=scratch.img of="$OUT/grub_core.img" bs=512 skip=1 count=$CORE_SECTORS 2>/dev/null

echo "gen-install-grub-blob: wrote $OUT/grub_boot.img ($(wc -c < "$OUT/grub_boot.img") bytes), $OUT/grub_core.img ($(wc -c < "$OUT/grub_core.img") bytes)"
