#!/bin/bash
# Boot-test the kernel under QEMU TCG with the same guest-visible DMA setup
# qemu-gunyah gives it:
#   - a restricted-dma-pool reserved-memory node placed just past the end
#     of /memory (as target/arm/gunyah.c does), referenced by the PCI host
#     bridge's memory-region;
#   - virtio-pci devices with VIRTIO_F_ACCESS_PLATFORM (iommu_platform=on),
#     which qemu-gunyah forces on every virtio-pci device.
# Root is the stock Debian 12 genericcloud arm64 image, mounted by PARTUUID
# with no initrd. TCG cannot reproduce LEND memory, so this checks that the
# kernel boots, binds the pool and mounts root, not the SIGBUS itself.
#
# The display is DroidVM's 2D virtio-gpu-pci screen. After the serial login
# prompt the test takes a QMP screendump of that screen (the surface QEMU's
# VNC server serves) and checks tty1's login prompt is drawn on it.
set -euo pipefail
K=$1        # vmlinuz
IMG=$2      # raw disk image
LOG=${LOG:-boot.log}
MODE=${MODE:-rdma}   # rdma: gunyah-shaped pool + ACCESS_PLATFORM; plain: control
WAIT=${WAIT:-900}
if [ "$MODE" = rdma ]; then AP=",iommu_platform=on"; else AP=""; fi

PU=$(sfdisk --part-uuid "$IMG" 1)
PU=${PU,,}
echo "root PARTUUID: $PU"

QARGS=(-M virt,gic-version=3 -cpu max,pauth-impdef=on -smp 2 -m 1024 -nodefaults -display none
       -drive "file=$IMG,if=none,id=d0,format=raw"
       -device "virtio-blk-pci,drive=d0,disable-legacy=on$AP"
       -device "virtio-rng-pci,disable-legacy=on$AP"
       -netdev user,id=n0
       -device "virtio-net-pci,netdev=n0,disable-legacy=on$AP"
       -device "virtio-gpu-pci,disable-legacy=on,disable-modern=off,xres=1280,yres=720,edid=on$AP")

qemu-system-aarch64 "${QARGS[@]}" -machine dumpdtb=virt.dtb
dtc -q -I dtb -O dts -o virt.dts virt.dtb
python3 - <<'PY'
import re
s = open("virt.dts").read()
# RAM is 0x40000000-0x80000000; advertise 768 MiB and put the 256 MiB pool
# right after it, outside /memory, the same shape qemu-gunyah emits.
s, n = re.subn(r"(memory@40000000 \{.*?reg = <)[^>]*(>)",
               r"\g<1>0x00 0x40000000 0x00 0x30000000\g<2>", s, flags=re.S)
assert n == 1, "memory node"
pool = """
	reserved-memory {
		#address-cells = <0x02>;
		#size-cells = <0x02>;
		ranges;
		restricted_dma_reserved@70000000 {
			reg = <0x00 0x70000000 0x00 0x10000000>;
			compatible = "restricted-dma-pool";
			alignment = <0x00 0x1000>;
			phandle = <0x7777>;
		};
	};
"""
i = s.rindex("};")
s = s[:i] + pool + s[i:]
s, n = re.subn(r"(pcie@10000000 \{)", r"\g<1>\n\t\tmemory-region = <0x7777>;", s)
assert n == 1, "pcie node"
open("virt-rdma.dts", "w").write(s)
PY
dtc -q -I dts -O dtb -o virt-rdma.dtb virt-rdma.dts
grep -n -A6 "reserved-memory\|memory-region\|memory@" virt-rdma.dts | head -30

# cloud-init=disabled: the stock image otherwise spends ~280 s probing for
# a cloud metadata source before the login prompt (test-only).
APPEND="root=PARTUUID=$PU rootwait ro console=tty0 console=ttyAMA0 hung_task_timeout_secs=30 hung_task_panic=0 cloud-init=disabled"
if [ "$MODE" = rdma ]; then DTB=(-dtb virt-rdma.dtb); else DTB=(); fi
echo "cmdline: $APPEND"
: > "$LOG"
timeout $((WAIT + 30)) qemu-system-aarch64 "${QARGS[@]}" "${DTB[@]}" \
  -kernel "$K" -append "$APPEND" -serial "file:$LOG" \
  -qmp "unix:qmp-$MODE.sock,server=on,wait=off" &
QPID=$!
ok=0
for i in $(seq 1 "$WAIT"); do
  if grep -q "login:" "$LOG"; then ok=1; break; fi
  if grep -qE "Kernel panic|end Kernel panic" "$LOG"; then break; fi
  kill -0 $QPID 2>/dev/null || break
  sleep 1
done
SHOT=screen-$MODE
rm -f "$SHOT".*
if [ $ok = 1 ]; then
  sleep 5   # let getty@tty1 draw its prompt as well
  python3 "$(dirname "$0")/screen-check.py" dump "qmp-$MODE.sock" "$PWD/$SHOT.ppm" || true
fi
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true

echo "=================== [$MODE] last 80 console lines"
tail -n 80 "$LOG" | sed 's/\x1b\[[0-9;]*m//g'
echo "=================== [$MODE] key lines"
grep -nE "virtio_gpu|virtio-gpu|\\[drm\\]|fbcon|frame buffer device|Linux version|Kernel command line|restricted DMA pool|assigned reserved memory|software IO TLB|virtio_blk|vda|EXT4-fs|VFS:|Kernel panic|Run /sbin/init|Debian GNU/Linux|login:|blocked for more than|swiotlb|DMA: Out of|Call trace" "$LOG" || true
echo "=================== [$MODE] checks"
st=0
if [ "$MODE" = rdma ]; then
grep -q "created restricted DMA pool" "$LOG" && echo "PASS restricted DMA pool created" || { echo "FAIL no restricted DMA pool"; st=1; }
grep -q "assigned reserved memory node restricted_dma_reserved" "$LOG" && echo "PASS virtio-pci devices bound to the pool" || { echo "FAIL devices not bound to the pool"; st=1; }
fi
grep -qE "EXT4-fs \(vda1\): mounted" "$LOG" && echo "PASS root mounted from vda1 (no initrd)" || { echo "FAIL root not mounted"; st=1; }
[ $ok = 1 ] && echo "PASS reached login prompt" || { echo "FAIL no login prompt"; st=1; }
grep -q "Initialized virtio_gpu" "$LOG" && echo "PASS virtio_gpu DRM driver initialized" || { echo "FAIL virtio_gpu not initialized"; st=1; }
grep -qE "fbcon: .*\\(fb0\\) is primary device" "$LOG" && echo "PASS fbcon on fb0" || { echo "FAIL fbcon not bound"; st=1; }
if [ -s "$SHOT.ppm" ]; then
  python3 "$(dirname "$0")/screen-check.py" lit "$SHOT.ppm" "$SHOT.png" && echo "PASS screendump is not blank" || { echo "FAIL screendump is blank"; st=1; }
  tesseract "$SHOT.png" "$SHOT" >/dev/null 2>&1 || true
  echo "--- OCR of the virtio-gpu screen (last lines):"
  grep -v '^[[:space:]]*$' "$SHOT.txt" | tail -n 12 || true
  grep -qi "login" "$SHOT.txt" && echo "PASS login prompt visible on the virtio-gpu screen" || { echo "FAIL no login prompt on the virtio-gpu screen"; st=1; }
else
  echo "FAIL no screendump"; st=1
fi
exit $st
