# DroidVM Fork — Fixing Missing Initrd in Gunyah-Accelerated Boot

This fork exists to fix a confirmed bug: **when booting a guest under `-accel gunyah` (Protected mode), the initrd is never delivered to the kernel**, because the Gunyah-specific device-tree builder never writes the `linux,initrd-start` / `linux,initrd-end` properties into `/chosen`. This document is the full handoff — read it before touching code.

---

## 1. Goal

Boot a stock Debian 12 `genericcloud-arm64` cloud image, direct-kernel-boot (no UEFI/GRUB), under Gunyah acceleration in Protected mode, on real hardware (OnePlus 15 / Snapdragon 8 Elite). The kernel currently panics at root-mount because its initrd (which carries the `virtio_blk` module — not built into this kernel) never gets unpacked. The two upstream pieces this needs (loading kernel+DTB into guest memory, and Protected-VM boot working at all) already function correctly; only initrd delivery is broken.

## 2. Test environment

- **Device:** OnePlus 15, Snapdragon 8 Elite
- **Root method:** Bootloader unlocked, rooted via **KernelSU-Next** + **ReZygisk**
- **Shell access:** Termux (F-Droid build), with `su` for root shell
- **DroidVM build:** dev build, post-`v0.0.6` release, with these kernel-side modules loaded (confirmed via `lsmod` and `/dev/gunyah` presence): `gh_hugepage_reserve`, `gh_unmovable`, `gunyah_host_share`, `nproc_guard`, `udmabuf`
- **Guest image:** Debian 12 `genericcloud-arm64` (kernel `6.1.0-52-cloud-arm64`, unmodified upstream Debian cloud kernel — `virtio_blk` is a module inside its stock initrd, not compiled in)
- **QEMU binary in use:** the app-bundled fork at `/data/data/cn.classfun.droidvm/usr/bin/qemu-system-aarch64` (this is the binary that needs the fix — locate its source in this fork's build tree)

## 3. Exact DroidVM VM configuration (all settings toggled)

| Setting | Value |
|---|---|
| Backend | **QEMU** (not CrosVM) |
| Protected VM | **Protected** (not "Normal", not "Protected without firmware") |
| Boot with UEFI | **OFF** |
| Boot protocol | **Linux boot protocol** (direct `-kernel`/`-initrd`, no bootloader/firmware) |
| Kernel path | extracted `vmlinuz` from the guest image's own `/boot` (matches its own GRUB config's kernel) |
| Initrd path | extracted `initrd.img` from the same location |
| Kernel cmdline | `root=PARTUUID=dcfd78c7-076c-48a6-905b-1485e18f1d16 ro console=ttyAMA0` |
| Memory | `3072M` |
| CPU topology | `6` vCPUs (`sockets=1,cores=3,threads=2`) |
| SWIOTLB size (Protected VM) | `256 MiB` |
| Prepare Lend mTHP | tested all three: Disabled / Single (whole region) / Chunked (≤256MiB) — **bug reproduces identically in all three**, ruling out memory-chunking as the cause |
| Disk 1 (boot) | `debian-12-genericcloud-arm64.qcow2`, VIRTIO bus, `cache=unsafe,aio=threads,discard=unmap` |
| Disk 2 (data) | `build-data.qcow2`, 60GB blank, VIRTIO bus, same disk options |
| Network | NAT, tap device, virtio-net-pci |
| Graphics | VNC on `127.0.0.1:0`, no password auth (this build's crypto backend lacks DES, so VNC password auth must stay off) |
| Audio | `aaudio` backend, virtio-sound-pci |
| USB | qemu-xhci + usb-tablet + usb-kbd |
| RNG | virtio-rng-pci from `/dev/urandom` |

The exact QEMU invocation this configuration produces (captured via `logcat --pid=<vm-process>`):
```
qemu-system-aarch64 -name Debian-12 -L /data/data/cn.classfun.droidvm/usr/share/qemu \
  -accel gunyah -machine virt,confidential-guest-support=prot0 \
  -cpu host -smp 6,sockets=1,cores=3,threads=2 -m 3072M \
  -object arm-confidential-guest,id=prot0,swiotlb-size=256M \
  -kernel /storage/emulated/0/VM/debian-12/boot/vmlinuz \
  -initrd /storage/emulated/0/VM/debian-12/boot/initrd.img \
  -append root=PARTUUID=dcfd78c7-076c-48a6-905b-1485e18f1d16 ro console=ttyAMA0 \
  -mem-prealloc -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0,disable-legacy=on,disable-modern=off \
  [... input/USB/PCI devices ...] \
  -object iothread,id=io0 \
  -drive file=/data/media/0/VM/debian-12/debian-12-genericcloud-arm64.qcow2,if=none,id=dr0,cache=unsafe,aio=threads,discard=unmap \
  -device virtio-blk-pci,drive=dr0,iothread=io0,disable-legacy=on,disable-modern=off,bootindex=1 \
  -object iothread,id=io1 \
  -drive file=/data/media/0/VM/debian-12/build-data.qcow2,if=none,id=dr1,cache=unsafe,aio=threads,discard=unmap \
  -device virtio-blk-pci,drive=dr1,iothread=io1,disable-legacy=on,disable-modern=off,bootindex=2 \
  -device virtio-net-pci,netdev=net_vmb6ce864f-0,mac=02:0e:16:43:e2:9a,disable-legacy=on,disable-modern=off \
  -netdev tap,ifname=vmb6ce864f-0,script=no,downscript=no,id=net_vmb6ce864f-0 \
  -audiodev aaudio,id=snd0 -device virtio-sound-pci,audiodev=snd0,disable-legacy=on,disable-modern=off \
  -device ramfb -vnc 127.0.0.1:0 \
  -chardev socket,id=uart0,path=.../Debian-12-uart.sock,server=on,wait=on -serial chardev:uart0 \
  -qmp unix:.../Debian-12-qmp.sock,server,nowait -nodefaults
```

## 4. Confirmed root cause

Ran the same `-kernel`/`-initrd` pair through two code paths and compared the resulting device tree:

**No Gunyah acceleration** (`-M virt,dumpdtb=...`, default TCG accelerator — a plain sanity baseline, *not* a Gunyah "Normal/Unprotected" run):
```
chosen {
        linux,initrd-end = <0x00 0x48d484f1>;
        linux,initrd-start = <0x00 0x48000000>;
        bootargs = "root=PARTUUID=... ro console=ttyAMA0";
```
Both initrd pointers present, non-zero, size-correct (~13.9MB initrd).

**`-accel gunyah`, Protected mode** (`protected_vm=true` confirmed in log — the actual path this app uses) builds its device tree via a completely separate, custom routine:
```
GH: Building minimal DTB from scratch (mem_base=0x80000000 mem_size=0xc0000000)
GH: DTB /chosen/bootargs: earlycon=... root=PARTUUID=... ro console=ttyAMA0
GH: DTB /chosen/stdout-path: /pl011@9000000
GH: DTB /config: kernel-address=0x80000000 kernel-size=0x1000000
GH: DTB /memory: base=0x80000000 size=0xb0000000 (total=0xc0000000 lend_only=1)
GH: DTB /cpus: 6 CPUs with PSCI
GH: DTB reserved-memory: restricted-dma-pool at 0x130000000 size 0x10000000
GH: DTB PCI host bridge: ECAM=0x3f000000 MMIO=0x10000000-0x3efeffff PIO=0x3eff0000 IRQs SPI 3-6
...
GH: Minimal DTB built with earlycon (totalsize=1048576)
```
**No `/chosen/linux,initrd-start` or `linux,initrd-end` line is ever printed.** Every other property this builder writes gets an explicit log line — initrd is simply not implemented in this code path. The guest kernel that boots here has zero knowledge an initrd exists, which is why no `Trying to unpack rootfs image as initramfs...` line ever appears, and why the guest panics with:
```
[    0.488532] /dev/root: Can't open blockdev
[    0.489320] VFS: Cannot open root device "PARTUUID=..." or unknown-block(0,0): error -6
[    0.491314] Please append a correct "root=" boot option; here are the available partitions:
[    0.492920] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)
```
with **zero** partitions ever listed.

This is **not a memory-placement bug** — kernel and DTB placement both work correctly (confirmed via `kernel entry from arm_load_kernel: 0x80200000` and successful `SET_DTB_CONFIG`/`DTB falls in slot[10] ... offset=0xfe00000` logging). It's a missing feature in the from-scratch Gunyah DTB builder.

**Important wrinkle:** the two paths use *different memory layouts*. The non-Gunyah path's RAM (and its initrd address `0x48000000`) sits at the standard `virt` board base `0x40000000`. The Gunyah path relocates RAM to base `0x80000000` (see `mem_base=0x80000000` above, and the log's repeated `skipping region ... (below 1GiB)` filtering of anything below that new base). **A fix cannot simply copy the non-Gunyah path's addresses** — it must compute a valid initrd load address inside the `0x80000000+` layout.

**Corroborating evidence the transport layer itself is fine:** the guest's PCI enumeration shows both configured virtio-blk drives correctly recognized as real devices:
```
pci 0000:00:05.0: [1af4:1042] type 00 class 0x010000
pci 0000:00:06.0: [1af4:1042] type 00 class 0x010000
```
So this isn't a virtio/PCI bug — it's specifically that no driver ever binds, because the module that provides it never gets unpacked.

## 5. Exact source location and required fix (confirmed by reading the actual upstream code)

The real source for this accelerator was tracked down to **`AnyLaySys/qemu-gunyah`** (https://github.com/AnyLaySys/qemu-gunyah), which DroidVM's bundled `qemu-system-aarch64` binary is built from (or a close derivative — its README's example invocation matches DroidVM's captured command line almost verbatim, down to the exact `-M virt,confidential-guest-support=prot0 -accel gunyah` and `swiotlb-size=256M` conventions). **The bug exists in this public upstream repo itself**, not in anything DroidVM's own build layer changed.

**Confirmed root cause, precisely:**
- `hw/arm/boot.c`'s generic `arm_load_kernel()` correctly loads the kernel, then the initrd, computing `binfo->initrd_start` / `binfo->initrd_size` and writing correct `linux,initrd-start` / `linux,initrd-end` properties onto the FDT — this is the same generic path that works correctly in a non-Gunyah boot.
- At `hw/arm/boot.c:502-503`, it then invokes `binfo->modify_dtb(binfo, fdt)`, which `hw/arm/virt.c:1552` wires to `gunyah_arm_build_dtb` (defined in `target/arm/gunyah.c:152`) for this machine type.
- **`gunyah_arm_build_dtb()` opens with `fdt_create_empty_tree(fdt, 0x100000)`** — this wipes the entire FDT the generic loader just correctly built, including the initrd properties, and reconstructs a fresh minimal tree containing only what it explicitly re-adds (`bootargs`, `stdout-path`, `kernel-address`, `memory`, `cpus`, GIC, PCI, swiotlb reserved-memory, etc.) — initrd is simply never one of them.
- `binfo->initrd_start` and `binfo->initrd_size` are still valid, already-correct fields on the same (`const`) `binfo` struct passed into `gunyah_arm_build_dtb()` — the initrd bytes are already sitting in guest memory at the right address by this point. **Nothing needs to be recomputed or re-copied — this is purely two missing FDT properties.**

**The fix** (in `target/arm/gunyah.c`, inside the existing `chosen` node block — right after the `stdout-path` property, around line 172-176):
```c
fdt_setprop_string(fdt, node, "bootargs", bootargs);
fdt_setprop_string(fdt, node, "stdout-path", "/pl011@9000000");

if (binfo->initrd_size) {
    fdt64_t initrd_prop;

    initrd_prop = cpu_to_fdt64(binfo->initrd_start);
    fdt_setprop(fdt, node, "linux,initrd-start", &initrd_prop, sizeof(initrd_prop));

    initrd_prop = cpu_to_fdt64(binfo->initrd_start + binfo->initrd_size);
    fdt_setprop(fdt, node, "linux,initrd-end", &initrd_prop, sizeof(initrd_prop));
}
```
This mirrors the exact style already used elsewhere in this same function for other 64-bit properties (e.g. the `memory` node's `fdt64_t reg[]` pattern), so it's consistent with the surrounding code rather than introducing a different convention.

**Important licensing/provenance note:** DroidVM ships this as a manually-committed prebuilt binary (`manual-build/arm64-v8a/usr/bin/qemu-system-aarch64` in `DroidVM-Prebuilt-Root`) with no source reference in its commit history (unlike the `edk2-gunyah` firmware binaries in the same tree, which *do* cite an upstream commit hash). Since QEMU is GPLv2, worth asking the maintainers directly which exact commit/fork the shipped binary was built from, both to properly attribute this fix and to confirm license compliance.

## 6. Known secondary issues (do not treat as the same bug — do not let a fix attempt block on these)

- **Issue 1 — Normal/Unprotected Gunyah mode is rejected at the RM firmware level** (`gunyah: RM rejected message 56000004. Error: 2` / `Failed to start VM: -19`), independent of QEMU/app logic — appears to be an OEM policy wall on this specific device+firmware, not fixable in this codebase. This means **Normal mode cannot be used as an A/B comparison** on this hardware; all testing must go through Protected mode.
- **virtio-blk I/O hang under LEND memory** was observed in earlier testing (once past the initrd stage in some configurations): `blkid`/`(udev-worker)` permanently blocked in `io_schedule` during partition read, escalating hang-detection warnings, zero progress. Suspected swiotlb/`ACCESS_PLATFORM` bounce-buffer DMA issue specific to LEND memory. **This may resurface as the next blocker once initrd loading is fixed** — flagging so it isn't mistaken for a regression from the initrd fix itself.
- A separate, intermittent `get_avail_index: host access to lent memory region ... in protected VM` error was seen even with `lend=0` in one earlier log — likely a distinct stale-permission-flag bug, unconfirmed whether still present in this build.

## 7. How to test a fix

Manual reproduction (no need to go through the DroidVM UI once you have a build):
```
su -c '/data/data/cn.classfun.droidvm/usr/bin/qemu-system-aarch64 \
  -name Debian-12 -L /data/data/cn.classfun.droidvm/usr/share/qemu \
  -accel gunyah -machine virt,confidential-guest-support=prot0 \
  -cpu host -smp 6,sockets=1,cores=3,threads=2 -m 3072M \
  -object arm-confidential-guest,id=prot0,swiotlb-size=256M \
  -kernel /storage/emulated/0/VM/debian-12/boot/vmlinuz \
  -initrd /storage/emulated/0/VM/debian-12/boot/initrd.img \
  -append "root=PARTUUID=dcfd78c7-076c-48a6-905b-1485e18f1d16 ro console=ttyAMA0" \
  -d guest_errors,unimp -D /sdcard/qemu-debug.log \
  -nographic'
```
**Success criteria:**
1. `grep -i initrd /sdcard/qemu-debug.log` (or the UART output) shows the DTB builder now logging initrd properties.
2. UART output shows `Trying to unpack rootfs image as initramfs...` followed by `Freeing initrd memory: ...`.
3. Boot proceeds past root-mount — either reaches a login prompt, or (if the secondary virtio-blk/LEND issue above is still present) hangs later at disk I/O rather than panicking immediately at "Cannot open root device". Either outcome is meaningful signal; only the second still needs Issue 6's secondary bug fixed.

To inspect the DTB independent of a full boot:
```
dtc -I dtb -O dts /sdcard/dtb.bin | grep -A5 chosen
```
(dump via `-M virt,dumpdtb=/sdcard/dtb.bin,...` with the same machine args, or by adding a debug hook after the fix to write out the constructed DTB).

## 8. Reference: the "before" state

The original upstream issue this fork addresses is filed as: *"Gunyah-accelerated boot never populates /chosen/linux,initrd-start|end — kernel has no way to find the initrd"* — see the linked issue on `Droid-VM/DroidVM` for the full original bug report, including the RM-rejection and virtio-blk-hang findings in complete detail.
