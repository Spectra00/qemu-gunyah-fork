#!/bin/bash
# Build a Debian 12 (bookworm) arm64 guest kernel for qemu-gunyah.
#
# Every guest RAM slot under qemu-gunyah is lent to the guest, so QEMU can
# only reach guest memory inside the restricted-dma-pool the DTB advertises
# (0x130000000 for DroidVM's 3 GiB VM). Stock Debian kernels ship with
# CONFIG_DMA_RESTRICTED_POOL unset, so virtio rings land in CMA and QEMU
# dies with SIGBUS on the first virtqueue access.
#
# This builds Debian's own patched 6.1 source with Debian's own cloud-arm64
# config, plus:
#   - CONFIG_DMA_RESTRICTED_POOL=y
#   - virtio-pci/blk/net/console/rng/input, ext4 and partition parsers
#     built in, so the kernel boots root=PARTUUID=... without an initrd.
# Runs inside a debian:bookworm container (native cross gcc-12).
set -euo pipefail

OUT=${OUT:-$PWD/out}
WORK=${WORK:-$PWD/work}
D=https://deb.debian.org/debian
mkdir -p "$OUT" "$WORK"

pkg_file() {  # dist arch package-regex -> pool path of newest match
  curl -fsSL "$D/dists/$1/main/binary-$2/Packages.xz" | xz -dc |
    awk -v re="$3" '/^Package: /{p=$2} /^Filename: /{if (p ~ re) print $2}' |
    sort -V | tail -1
}

SRC_DEB=$(pkg_file bookworm arm64 '^linux-source-6[.]1$')
IMG_DEB=$(pkg_file bookworm arm64 '^linux-image-6[.]1[.]0-[0-9]+-cloud-arm64$')
echo "source package: $SRC_DEB"
echo "config from:    $IMG_DEB"
SRC_VER=$(basename "$SRC_DEB" | sed -E 's/^linux-source-6\.1_([^_]+)_all\.deb$/\1/')
IMG_VER=$(basename "$IMG_DEB" | sed -E 's/^[^_]+_([^_]+)_arm64\.deb$/\1/')
echo "versions: source $SRC_VER, image $IMG_VER"
[ "$SRC_VER" = "$IMG_VER" ] || echo "WARNING: source and config package versions differ"

cd "$WORK"
curl -fsSL -o src.deb "$D/$SRC_DEB"
curl -fsSL -o img.deb "$D/$IMG_DEB"
dpkg-deb -x src.deb src
dpkg-deb -x img.deb img
tar -xf src/usr/src/linux-source-6.1.tar.xz
cd linux-source-6.1
cp ../img/boot/config-* .config
echo "base config: $(ls ../img/boot/config-*)"

S=scripts/config
# The fix itself.
$S --enable DMA_RESTRICTED_POOL --enable SWIOTLB --enable OF_RESERVED_MEM
# Boot without an initrd: everything between PCI and the root filesystem.
for o in PCI_HOST_GENERIC VIRTIO VIRTIO_PCI VIRTIO_PCI_LIB VIRTIO_PCI_LIB_LEGACY \
         VIRTIO_BLK VIRTIO_NET NET_FAILOVER FAILOVER VIRTIO_CONSOLE \
         HW_RANDOM HW_RANDOM_VIRTIO VIRTIO_INPUT \
         EXT4_FS JBD2 FS_MBCACHE CRC16 CRYPTO_CRC32C LIBCRC32C \
         EFI_PARTITION MSDOS_PARTITION; do
  $S --enable "$o"
done
# Distinct release name; no Debian signing certs in a source build.
$S --set-str LOCALVERSION "-gunyah-rdma" --disable LOCALVERSION_AUTO
$S --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS ""
$S --set-str BUILD_SALT "qemu-gunyah"
# No debug info: smaller and faster, and BTF would need pahole.
$S --disable DEBUG_INFO --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT \
   --disable DEBUG_INFO_DWARF4 --disable DEBUG_INFO_DWARF5 \
   --disable DEBUG_INFO_BTF --enable DEBUG_INFO_NONE

M="make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)"
$M olddefconfig

echo "=== config check"
fail=0
for o in DMA_RESTRICTED_POOL SWIOTLB OF_RESERVED_MEM PCI_HOST_GENERIC VIRTIO \
         VIRTIO_PCI VIRTIO_BLK VIRTIO_NET VIRTIO_CONSOLE HW_RANDOM_VIRTIO \
         VIRTIO_INPUT EXT4_FS EFI_PARTITION MSDOS_PARTITION; do
  if grep -qx "CONFIG_$o=y" .config; then
    echo "  CONFIG_$o=y"
  else
    echo "  CONFIG_$o NOT built in: $(grep -E "CONFIG_$o[ =]" .config || echo unset)"
    fail=1
  fi
done
[ $fail = 0 ] || { echo "required options did not land"; exit 1; }

$M Image modules
KR=$($M -s kernelrelease)
echo "kernel release: $KR"

gzip -9 -n -c arch/arm64/boot/Image > "$OUT/vmlinuz-$KR"
cp .config "$OUT/config-$KR"
cp System.map "$OUT/System.map-$KR"
rm -rf ../mods
$M modules_install INSTALL_MOD_PATH=../mods INSTALL_MOD_STRIP=1 >/dev/null
rm -f ../mods/lib/modules/"$KR"/build ../mods/lib/modules/"$KR"/source
tar -C ../mods -czf "$OUT/modules-$KR.tar.gz" lib
echo "$KR" > "$OUT/KERNELRELEASE"
cat > "$OUT/BUILD-INFO.txt" <<INFO
kernel release : $KR
source package : $SRC_DEB
config base    : $IMG_DEB
changes        : CONFIG_DMA_RESTRICTED_POOL=y; virtio-pci/blk/net/console/
                 rng/input, ext4, EFI/MSDOS partitions built in (=y);
                 LOCALVERSION=-gunyah-rdma; no debug info; Debian signing
                 certs not used.
INFO
(cd "$OUT" && sha256sum vmlinuz-* config-* System.map-* modules-* > SHA256SUMS)
ls -la "$OUT"
