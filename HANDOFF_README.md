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

---

## 9. UPDATE — initrd fix confirmed on real hardware; new MSI/GH_VM_START bug found and precisely scoped

Everything below is from an actual on-device test session (OnePlus 15, same environment as section 2), run against a **fresh cross-compile of this fork** (commit `80b82bf`, built via `.github/workflows/build.yml`, downloaded as the `qemu-gunyah-arm64` Actions artifact and run manually in Termux via `su`), not the DroidVM app-bundled binary. Diagnostic logging added in `80b82bf` (a topology dump in `gunyah_start_vm()` right before `GH_VM_START`, plus per-`shm-<id>` vdevice logging in `gunyah_arm_fdt_customize()`) is what made this possible.

### 9.1 The initrd fix (commit `21fdc26`) is confirmed working end-to-end

Protected mode, direct kernel+initrd boot, **no block device attached** (to isolate the fix from the separate bug below):

```
[    0.176502] Trying to unpack rootfs image as initramfs...
[    0.291920] Freeing initrd memory: 13600K
...
[    0.473099] Run /init as init process
Loading, please wait...
Starting systemd-udevd version 252.39-1~deb12u2
...
Gave up waiting for root file system device.  Common problems: ...
ALERT!  PARTUUID=dcfd78c7-076c-48a6-905b-1485e18f1d16 does not exist.  Dropping to a shell!
```

The initrd unpacks and `/init` runs — the fix works. The only failure here is the *expected* one (no disk was attached in this run). `gh_report` also confirmed `VM_START OK` for this exact run. **Section 1's "kernel panics because initrd never unpacked" description is now stale/historical — that specific failure is fixed.**

### 9.2 New bug found: `GH_VM_START` is rejected by the Resource Manager whenever *any* PCI device requests MSI vectors — unrelated to block devices, iothreads, or the initrd fix

This is a **different bug from Issue 1** in section 2/the original report. Issue 1 was RM refusing to authorize an *unprotected* VM (`RM rejected message 56000004`). This is a **protected**-mode VM (`confidential-guest-support=prot0`, `-accel gunyah`) being rejected by a **different** RM message once a device topology requiring MSI is presented to it:

```
gunyah: RM rejected message 5600000b. Error: 2
misc gunyah: Failed to initialize VM: -19
```
which QEMU surfaces as:
```
Failed to start VM: No such device (errno=19)
```

Four on-device runs isolated the exact trigger, each changing exactly one variable from the last:

| Run | Devices attached | `msi_vectors` (from the topology dump) | Result |
|---|---|---|---|
| 1 | none (kernel+initrd only) | 0 | `VM_START OK` — boots as in 9.1 |
| 2 | `virtio-blk-pci` + iothread (matches original DroidVM config) | 7 | RM rejects `5600000b` |
| 3 | `virtio-blk-pci`, **no** iothread (sync I/O) | 7 | RM rejects `5600000b` — rules out iothread/ioeventfd |
| 4 | `virtio-keyboard-pci` only — no disk, no net, no iothread | 2 | RM rejects `5600000b` — rules out block devices specifically |

Every individual setup ioctl before `GH_VM_START` succeeds without error in all failing runs — all memory-slot lends (`GH_VM_ANDROID_LEND_USER_MEM`), all IRQFD registrations including the per-MSI-vector ones (`"N/N virtio IRQFDs created OK"`), `GH_VM_SET_DTB_CONFIG`, and `GH_VM_SET_BOOT_CONTEXT` all return success. **Only the final, holistic `GH_VM_START` call is rejected, and only when `msi_vectors > 0`.**

**Likely area:** when `msi_vectors > 0`, `gunyah_arm_build_dtb()` / `gunyah_arm_fdt_customize()` (`target/arm/gunyah.c`) additionally emit, versus the `msi_vectors == 0` case:
- the GICv2m frame's `arm,msi-num-spis` property (set to `gs->msi_vectors`), and
- one `/gunyah-vm-config/vdevices/bell-<label>` doorbell vdevice per MSI vector (labels `GUNYAH_MSI_SPI_BASE..GUNYAH_MSI_SPI_BASE+msi_vectors-1`), generated in the loop right after the 4 fixed bells in `gunyah_arm_fdt_customize()`.

Both look structurally sound by source inspection (matches the style of the always-present fixed-bell vdevices), so this reads as an RM-side rejection of that specific vdevice/GICv2m-MSI configuration — not an obviously malformed QEMU-side DTB. Confirming the exact mechanism will need either a firmware-level trace of RM's own decision (not available — Qualcomm's Resource Manager is closed-source) or bisecting the vdevice properties one at a time (e.g. try emitting the GICv2m `msi-num-spis` property without the per-vector `bell-*` vdevices, or vice versa, and see which one alone triggers the rejection).

**Attempted workaround, inconclusive:** tried forcing `virtio-blk-pci` into legacy INTx mode (`disable-legacy=off,disable-modern=on`) to avoid MSI entirely. This build's virtio-blk-pci model rejects that property combination outright (`Device doesn't support modern mode, and legacy mode is disabled` — a QEMU realize()-time error, unrelated to Gunyah) before ever reaching gunyah code. Not investigated further; a legacy-mode workaround remains untested.

**Practical implication:** in this build, any real disk or network device (all realistic use cases need MSI, not legacy INTx, for performance) currently cannot boot in protected mode on this device+RM-firmware combination. This is very likely why the original bug report's fuller DroidVM config (two `virtio-blk-pci` + net + audio + USB + VNC, all presumably requesting MSI) is described as reaching kernel boot successfully — that claim was never independently reproduced against *this exact rebuilt binary*; it may reflect a difference between DroidVM's actual shipped binary and this fork's current build, or a difference in RM firmware state on the device between then and now. Worth re-verifying against DroidVM's own binary specifically before assuming this is a regression in this fork.

### 9.3 Diagnostic instrumentation now in this fork (commit `80b82bf`)

- `accel/gunyah/gunyah-vm-start.c`: topology dump (`gh_report`) of every active memory slot, active slot count, `msi_vectors` + SPI range, `dtb_start`/`dtb_size`, `swiotlb_size` — printed immediately before `GH_VM_START`.
- `target/arm/gunyah.c`: logs each `shm-<id>` vdevice as `gunyah_arm_fdt_customize()` generates it.

Use these (`grep` for `"=== Gunyah VM topology"` / `"vdevice: slot"` in stderr) on any future repro instead of re-adding logging from scratch.

## 10. UPDATE — root cause of the `GH_VM_START` MSI rejection narrowed to the per-vector `bell-<label>` doorbell vdevices, confirmed on real hardware

Section 9.2 left two candidate causes for the `RM rejected message 5600000b` / `RM_ERROR_NORESOURCE` failure: the GICv2m `arm,msi-num-spis` property, or the per-MSI-vector `bell-<label>` doorbell vdevices. A single-variable bisection was run to distinguish them, using two throwaway diagnostic branches built off `10050e1`:

- **`diag/msi-a-no-msi-bells` @ `7e203c8`** — keeps the real `arm,msi-num-spis` value, but skips generating the per-vector `bell-<label>` vdevices entirely (wraps that loop in `if (0)` in `gunyah_arm_fdt_customize()`).
- **`diag/msi-b-numspis-zero` @ `bee012f`** — keeps the per-vector bell vdevices, but forces `arm,msi-num-spis` to `0` instead of the real vector count.

**Build A result (tested on-device, OnePlus 15, `virtio-keyboard-pci`, `msi_vectors=2`):**

```
DIAG-A: skipping 2 per-vector MSI bell vdevices (msi-num-spis left at 2) gunyah.c:gunyah_arm_fdt_customize:138
VM_START OK gunyah-vm-start.c:gunyah_start_vm:260
```

No `rejected`/`gunyah:` error lines appear anywhere in the parallel `dmesg` capture — a clean pass. The guest correctly enumerates the device (`pci 0000:00:01.0: [1af4:1052] type 00 class 0x090000`, `GICv2m: DT overriding V2M MSI_TYPER (base:48, num:2)`, `virtio-pci 0000:00:01.0: enabling device`), the initrd unpacks (`Freeing initrd memory: 13600K`), and boot proceeds to `/init`/`systemd-udevd`, stopping only for the expected reason (no root disk attached in this test) — identical to the known-good no-device baseline in section 9.2's Run 1.

**Conclusion: the per-vector `bell-<label>` doorbell vdevices are the cause of the RM rejection, not the `arm,msi-num-spis` GICv2m property.** RM accepts an MSI-capable GICv2m frame (real `msi-num-spis`) without complaint; it only rejects once the matching per-vector doorbell vdevices are also present. Build B (bells present, `msi-num-spis` forced to 0) was not run — it would only be confirmatory at this point, not decisive, since build A alone already isolates the variable.

**Next step:** investigate why RM's doorbell-object creation (`RM_ERROR_NORESOURCE`, i.e. RM believes some resource/handle/capability needed to create the doorbell is unavailable) fails specifically for these per-vector bells but not for the four always-present fixed-label bells emitted earlier in the same function. Likely angles:
- Diff the property set of a per-vector `bell-<label>` vdevice against a fixed bell vdevice (label/id numbering scheme, `gunyah-label`, IRQ/SPI number encoding, any capability/resource count field) for a structural difference RM's per-vdevice or per-VM resource accounting would reject.
- Check whether RM enforces a fixed maximum number of doorbell/bell vdevices per VM (the four fixed bells plus N per-vector bells may exceed some allowed doorbell count, if `RM_ERROR_NORESOURCE` reflects a capability-table/slot exhaustion) — public reference source: `quic/gunyah-resource-manager`.
- Consider whether the per-vector bells need a different vdevice type/config (e.g. reusing/aliasing IRQFD-based delivery already wired up in `gunyah_start_vm()`, rather than a doorbell object per vector) instead of one new doorbell vdevice per MSI vector.

Diagnostic branches (not merged to `main`, kept for reference): `diag/msi-a-no-msi-bells` (`7e203c8`), `diag/msi-b-numspis-zero` (`bee012f`).

## 11. UPDATE — why RM rejects the per-vector bells (analysis) and candidate fix `fix/msi-low-spi-window` (NOT yet tested on hardware)

Everything in this section comes from source reading plus a build that compiles in CI. **None of it has been run on the device yet.** 11.4 lists the on-device runs that will confirm or refute it.

### 11.1 What the public Resource Manager source says (`quic/gunyah-resource-manager`, 2026-03 drop)

- `0x5600000b` is `VM_INIT` (`include/vm_creation_message.h`). `Error: 2` is `RM_ERROR_NORESOURCE` (`include/rm_types.h`), and the Linux driver turns it into `-ENODEV` / errno 19. RM ran out of, or could not allocate, something. A malformed property would be `RM_ERROR_ARGUMENT_INVALID` or `RM_ERROR_MSG_INVALID`.
- RM never reads `arm,msi-num-spis`, `arm,msi-base-spi` or any GICv2m node. `platform_parse_gic()` only checks the GIC node's compatible string, `interrupt-controller`, `#interrupt-cells`, `reg` and redistributor stride. That agrees with section 10: the v2m property is inert.
- A doorbell vdevice (`parse_doorbell()` → `handle_doorbell()` → `configure_doorbell_with_peer()`) creates a hypervisor doorbell object, copies capabilities into the guest's and HLOS's cspaces, maps the fixed guest vIRQ from `interrupts` (`map_virq()` → `irq_manager_vm_virq_map()`), and binds it with `doorbell_bind_virq`. With `peer-default` the peer (HLOS) is the source and gets no vIRQ.
- **No path in the public source returns `NORESOURCE` for our bells.** A duplicate vIRQ gives `DENIED` (`dict_add`), an out-of-range one gives `ARGUMENT_INVALID`, and a DTB overlay that doesn't fit gives `ARGUMENT_INVALID`. The only "pool exhausted" `NORESOURCES` is dynamic vIRQ allocation (`irq_manager_vm_alloc_global`), which a doorbell with a fixed `interrupts` property never uses. The guest VIC is configured with `max_virqs = GIC_SPI_NUM` (988). **So the device's vendor RM differs from the public drop, and the exact rule can't be read from source.**

### 11.2 The working hypothesis: doorbell label/SPI must stay in 0x0–0xf

- The four fixed bells that always work use label = SPI ∈ {0x0, 0x1, 0x2, 0xf}. Every per-vector bell used label = SPI = 16 + n (0x10 and up). Label/SPI numbering is the only property that differs between them (section 10's property diff).
- This fork's bell format is copied from crosvm (`hypervisor/src/gunyah/aarch64.rs`: same `generate`, `label`, `peer-default`, `source-can-clear` and `interrupts`). crosvm is the VMM Android ships on Gunyah. Its aarch64 layout (`aarch64/src/lib.rs`) uses fixed SPIs 0 and 2 (serial), 1 (RTC), 3 (battery), 15 (VM watchdog) and per-device INTx SPIs from 4 upward, **with no MSI**. So in practice every doorbell a shipping RM is known to accept sits at 0x0–0xf. Our MSI bells were the first ones at 0x10 or above.
- **A per-VM doorbell count limit is unlikely.** A limit at or below 5 (4 fixed + 2 MSI = 6 already fails) would break crosvm protected VMs, which run with more doorbells than that. The `msi_vectors=1` run in 11.4 (5 doorbells) tests it directly.

### 11.3 Candidate fix — branch `fix/msi-low-spi-window` @ `d693fab`

- **Guest-visible MSI SPIs moved from 16+ to 3–14:** the free SPIs between the fixed bells 0x0–0x2 and bell-f. The base is runtime state, `GUNYAHState.msi_spi_base` (default `GUNYAH_MSI_SPI_BASE_DEFAULT = 3`). It drives the bell labels and SPIs, the IRQFD labels in `gunyah_start_vm()`, GICv2m `arm,msi-base-spi` and the emulated `V2M_MSI_TYPER`.
- **QEMU-internal routing kept separate:** a guest MSI write to `V2M_MSI_SETSPI_NS` for guest SPI `base + n` now raises QEMU GIC line `GUNYAH_MSI_ROUTE_BASE (16) + n`, where that vector's IRQFD notifier is registered. QEMU's own board lines 3–6 (PCIe INTx) and 8 (UART1) therefore never fire an MSI eventfd.
- **Capacity is 12 vectors.** If PCI devices request more, `msi_vectors` is clamped with a `warn_report` instead of producing a VM that RM rejects. Linux virtio-pci then falls back to 2 shared vectors per device, and devices that get fewer than 2 fall back to INTx, which is not wired under Gunyah. Keep total demand at or under 12 with `vectors=2` / `num-queues=1` on virtio-*-pci. For example, 6-vCPU virtio-blk-pci asks for 7 by default.
- **Environment overrides for on-device confirmation:**
  - `GUNYAH_MSI_SPI_BASE=<n>` sets the guest MSI SPI base. It must be ≥ 3 and ≠ 15. A base above 15 keeps the old unbounded layout, so `GUNYAH_MSI_SPI_BASE=16` reproduces the pre-fix layout as a control.
  - `GUNYAH_MSI_MAX_VECTORS=<n>` caps the vector count.
- New log line per bell: `MSI vector N: bell-<label> (SPI S)`. The topology dump now prints `SPI range a-b (QEMU route lines c-d)`.

### 11.4 On-device runs needed (same harness as section 9, `virtio-keyboard-pci`, 2 vectors)

| Run | Environment | Expected if the 11.2 hypothesis is right |
|---|---|---|
| 1 | *(none)*: bells at 0x3, 0x4 | `VM_START OK`, keyboard gets MSIs (`GICv2m: ... (base:35, num:2)`, virtio-input probes) |
| 2 | `GUNYAH_MSI_SPI_BASE=16` (control, old layout) | RM rejects `5600000b` again |
| 3 | `GUNYAH_MSI_SPI_BASE=16 GUNYAH_MSI_MAX_VECTORS=1` (one bell at 0x10, 5 doorbells total) | rejected → numbering, not count; accepted → a count limit exists after all |

If run 1 is also rejected, the hypothesis is wrong. Capture `/sys/firmware/devicetree/base/hypervisor/` from the build-A guest (`diag/msi-a-no-msi-bells`, which boots to an initramfs shell) to see which guest vIRQs RM assigns to its own vdevices. A clash with SPIs 3+ would point to the next constraint.

### 11.5 Run 1 confirmed on real hardware — the fix works

Tested on-device (OnePlus 15, artifact from CI run `36785583988` @ `d693fab`, `virtio-keyboard-pci`, no env overrides — the fix's default layout):

```
 • MSI vector 0: bell-3 (SPI 3) gunyah.c:gunyah_arm_fdt_customize:150
 • MSI vector 1: bell-4 (SPI 4) gunyah.c:gunyah_arm_fdt_customize:150
 • msi_vectors=2 SPI range 3-4 (QEMU route lines 16-17) gunyah-vm-start.c:gunyah_start_vm:248
 • VM_START OK gunyah-vm-start.c:gunyah_start_vm:262
```

with zero `rejected`/`gunyah:` lines anywhere in the parallel `dmesg` capture. The guest boots end to end: `GICv2m: DT overriding V2M MSI_TYPER (base:35, num:2)` (35 − 32 `GIC_INTERNAL` = SPI 3, matching the bell above), `pci 0000:00:01.0: [1af4:1052] type 00 class 0x090000`, `virtio-pci 0000:00:01.0: enabling device`, initrd unpacks (`Freeing initrd memory: 13600K`), `/init` runs, `systemd-udevd` starts — stopping only at the same benign point as every other no-disk run in this document (`Gave up waiting for root file system device`).

**This confirms the fix works on the real vendor RM firmware, not just in CI.** The 11.2 hypothesis (doorbell label/SPI must stay in 0x0–0xf) is validated by this positive result; runs 2 and 3 (the control and the count-vs-numbering probe) would only add confirming/diagnostic detail, not change this conclusion.

Runs 2/3 were attempted but did not complete: with `-nographic` and no disk attached, QEMU drops to the guest's initramfs emergency shell and ties it directly to the terminal. Any further commands pasted in the same terminal session before that shell is dealt with are consumed by the *guest* shell instead of the host, not run at all. Use `</dev/null` on the QEMU invocation (so the guest shell hits EOF and exits on its own) and run each test as its own separate paste/`su` session.

**Caveat found during review, not yet exercised by any test here:** `create_uart()` for `VIRT_UART1` (guest SPI 8, inside the new 3–14 MSI window) is only gated on whether a second `-serial`/chardev is passed on the QEMU command line, not on `gunyah_enabled()` — unlike PCIe INTx, which is fully skipped under Gunyah. Every command line in this document uses exactly one `-serial`, so this hasn't triggered, but a future config with two serial chardevs would alias UART1's real interrupt with an MSI bell in this window. Worth gating `create_uart(VIRT_UART1, ...)` (or reserving SPI 8) if a second serial port is ever needed under Gunyah.

## 12. CORRECTION — the control test falsifies the 11.2 hypothesis; the MSI/GH_VM_START rejection does not currently reproduce at all, with or without the fix

Section 11.5's "fix confirmed" framing was premature. Two further on-device tests overturn it:

- **Run 2** (`fix/msi-low-spi-window` @ `d693fab`, `GUNYAH_MSI_SPI_BASE=16` — reconstructs the exact pre-fix layout: bells at label/SPI 16 and 17, `arm,msi-base-spi` = 48, identical to what the unpatched code always produced): `VM_START OK`, zero rejections.
- **Run 3** (same branch, `GUNYAH_MSI_SPI_BASE=16 GUNYAH_MSI_MAX_VECTORS=1` — a single bell at label `0x10`, 5 doorbells total): also `VM_START OK`, zero rejections.
- **Control-stock test** (the literal pre-fix binary, commit `74874e2`, CI run `36780355787`, built *before* `fix/msi-low-spi-window` existed, no env overrides, no code changes of any kind): `virtio-keyboard-pci`, `msi_vectors=2`, bells at label/SPI 16/17, `GICv2m: ... (base:48, num:2)` — **the exact configuration that failed as Run 4 in section 9.2's table** (`RM rejected message 5600000b`) — now also returns `VM_START OK` with zero rejections.

The third result is decisive: it is not a reconstruction, it is the unmodified, never-touched-since binary from before this investigation's fix branch existed, running the identical failing test from section 9.2, and it now passes. **The per-vector doorbell label/SPI is not what the vendor RM is rejecting on.** Section 10/11.2's hypothesis (RM only accepts doorbell label/SPI 0x0–0xf) is falsified. `fix/msi-low-spi-window` has not been shown to fix anything real, because the failure it targets is not currently reproducible on this device at all, with or without the fix applied.

**What this points to instead:** something stateful on the RM/HLOS side — most plausibly a capability, handle, or other resource that got consumed by earlier failed `VM_INIT` attempts (every failing run in sections 9–10 called `GH_VM_START`, got rejected, and presumably tore down without RM ever cleanly releasing whatever it had reserved for the doorbells) and has since been freed by something external to this code: a device reboot, the DroidVM app process restarting, enough wall-clock time passing, or some other VM's teardown. This would make the original bug a **resource leak that accumulates across failed attempts and eventually self-clears**, not a structural property-encoding bug — which also fits section 11.1's own finding that the public RM source has no code path returning `NORESOURCE` for a doorbell with a merely-high-but-valid SPI number.

**Open question for the person running these tests:** has anything changed on the phone (reboot, app restart, time elapsed, other VMs run) between the section 9.2 failing tests and now? That would confirm or narrow the leak theory directly.

**Status of `fix/msi-low-spi-window`:** not disproven as a reasonable mitigation (packing MSIs into the low SPI range matching crosvm's convention is defensible on its own merits), but **not confirmed to fix the actual reported bug**, since the bug cannot currently be reproduced to test against. Do not merge this branch to `main` as "the fix" without first re-establishing a reliable repro of the original rejection — otherwise there's no way to know if a regression later is this fix's fault or just the same leak recurring. Next step: try to deliberately re-trigger the original failure (e.g. run several VMs back-to-back without a reboot in between, to test whether `NORESOURCE` returns after enough accumulated failed/improperly-torn-down attempts) before concluding anything further about root cause.

## 13. UPDATE — 9 consecutive clean runs on the exact original failing config, post-reboot; leading theory is now a reboot-clearable RM/HLOS resource state, not SPI numbering

Section 12 asked whether a phone reboot had occurred between the section 9.2 failures and the first non-reproduction. It had — the person confirmed a reboot happened before testing resumed. A manual stress test was then run to check whether repeated VM lifecycles alone (no reboot in between) bring the rejection back.

**Test design:** the literal unmodified pre-fix binary (`control-stock`, commit `74874e2` — same one used for section 12's control test), run 5 times back-to-back in the same `su` session (no reboot, no app restart between iterations), each with `virtio-keyboard-pci` and no other devices — i.e. exactly section 9.2's Run 4 / section 10's build-A config, repeated.

**Confirming this actually re-exercised the failing path (not just a similar one):** the topology dump line was captured for every iteration and checked directly, rather than assumed from the device flags alone (`virtio-keyboard-pci,disable-legacy=on,disable-modern=off` does **not** select legacy/INTx-only mode — `virtio_pci_pre_plugged()` forces `disable_legacy = ON_OFF_AUTO_ON` for every virtio-pci device whenever `gunyah_enabled()`, so under Gunyah this device is always modern/MSI-X-capable regardless of command-line flags; `virtio-input-pci` defaults to `vectors=2`). All 5 logs show:

```
msi_vectors=2 SPI range 16-17 gunyah-vm-start.c:gunyah_start_vm:246
```

`SPI range 16-17` is the pre-fix, unbounded layout — i.e. **byte-for-byte the same doorbell configuration that produced `RM rejected message 5600000b` in section 9.2's Run 4 and section 10's non-diagnostic baseline.** This was not a reconstruction or an approximation; it is the original failing layout, generated by the original unmodified binary.

**Result: all 5 runs returned `VM_START OK`, with zero `rejected`/`gunyah:` lines in the parallel `dmesg` capture, every time.** Combined with section 12's one-off control-stock test and the three `fix/msi-low-spi-window` runs (which also never reproduced the rejection), that is **9 consecutive clean completions since the reboot**, including 5 that specifically repeated the exact failing config back-to-back with no reboot or app restart in between.

**Conclusion:** repetition/accumulation alone, without a reboot, does not bring the rejection back — at least not within 5 repeated lifecycles of the exact originally-failing config. This weighs against a simple "N failed attempts exhaust a resource" leak model (if that were the mechanism, repeating the exact failing config 5 times with no intervening reboot should have reproduced it at least once, and instead every single repetition succeeded). It's more consistent with: the reboot itself cleared whatever state the original section 9.2–10 failing runs had left behind, and that state does not reliably reaccumulate from pass-only runs — i.e. the leak (if it is a leak) is likely tied to the *abrupt/improper teardown* of a *rejected* `VM_INIT`, not to successful VM lifecycles. Since none of these 9 runs failed, none of them could test reaccumulation from repeated failures — that would require deliberately forcing `GH_VM_START` to fail again, which is no longer possible on demand now that the original failure doesn't reproduce.

**Standing guidance unchanged:** `fix/msi-low-spi-window` remains unconfirmed as a fix for the real bug and should not be merged to `main` as "the fix." The practical state of this investigation is that the original rejection is not currently reproducible by any means tried so far (different SPI layouts, different vector counts, repeated back-to-back lifecycles), which blocks further confirmation of any fix until a reliable repro is found again — most likely by deliberately forcing failed `VM_INIT` attempts (if a way to do that is found) and testing whether failures specifically, not successes, are what cause the state to reaccumulate.

## 14. ANALYSIS — forcing a `VM_INIT` rejection on demand, and what "unclean teardown" actually means at the kernel level (source-only, nothing run on hardware)

### 14.1 Correction to 11.1: `Error: 2` does **not** mean a resource ran out

In the public RM, `svm_init()` (`src/vm_creation/second_vm.c`) reports **every** failure from `vm_config_create_vdevices()` to HLOS as `RM_ERROR_NORESOURCE`, whatever the real cause:

```c
error_t create_ret = vm_config_create_vdevices(vm->vm_config, parser_data);
if (create_ret != OK) { ...; err = RM_ERROR_NORESOURCE; goto out; }
```

So a duplicate vIRQ (`ERROR_DENIED`), a bad argument, a failed cap copy, too many vCPUs and so on all reach the kernel as `RM rejected message 5600000b. Error: 2`, which the kernel maps to `-ENODEV` (`rsc_mgr.c`). The original rejection therefore only tells us *some vdevice handler failed during VM_INIT*, not that anything was exhausted. Section 11.1's "RM ran out of something" reading was wrong. It also means **any deliberately invalid vdevice config produces exactly the same externally visible failure as the original bug.**

`vm_config_create_vdevices()` runs its handlers in a fixed order and stops at the first error, with no rollback of its own: interrupt controller → irqs → iomems → watchdog → vRTC → **vcpu** → rm-rpc → **doorbell** → msgqueue → msgqueue-pair → shm → virtio → pci → vGIC → platform → … → demand paging. Objects created before the failing handler, such as the doorbells and their capabilities copied into HLOS's cspace, are not destroyed then. The VM just goes to `VM_STATE_INIT_FAILED`.

### 14.2 What teardown after a rejection looks like (Android `drivers/virt/gunyah`, `android16-6.12`)

- `gunyah_vm_start()` (`vm_mgr.c`): on a `gunyah_rm_vm_init()` failure it sets `INIT_FAILED`, prints `Failed to initialize VM: -19` and returns. QEMU then prints `Failed to start VM` and `exit(1)`s.
- All cleanup happens in `_gunyah_vm_put()`, which runs when the last reference to the VM file descriptor is dropped. **A normal exit, `exit(1)` after the rejection, and SIGKILL all reach this same path**, because the kernel closes the process's fds either way. A separate "QEMU exited uncleanly" condition doesn't exist at the kernel level. What differs is **the VM state at release**:
  - From `RUNNING`: `gunyah_vm_stop()`, then reset, then dealloc.
  - From `INIT_FAILED`: `gunyah_rm_vm_reset()`, then `wait_event()` **with no timeout** for RM's `RESET`/`RESET_FAILED` notification, then `gunyah_rm_dealloc_vmid()`. RM destroys the partially created vdevices (doorbells included) only on that `VM_RESET`.
- So "a rejection leaks something" would show up as one of these, each observable from the host:
  - (a) `Failed to reset the vm` / `Failed post reset the vm` / `Failed to deallocate vmid` in dmesg;
  - (b) `WARNING:` splats from the `WARN_ON(gunyah_vm_reclaim_range…)` / `WARN_ON(gunyah_reclaim_parcels…)` memory-reclaim checks;
  - (c) the exiting `qemu-system-aarch64` stuck in uninterruptible `D` state, if RM never sends the reset notification;
  - (d) RM-side state the kernel can't see, which would only show as a later, otherwise valid `VM_INIT` failing.
  Sections 9–10 captured only the `rejected` lines, so (a)–(c) were never checked for the original failures.
- **Direct inspection:** the driver has no debugfs and no sysfs attributes. Its only `trace_*` calls are Android vendor hooks (`android_rvh_gh_*`), which tracefs can't see. RM's own log (`GET_LOG` message, `log.c`) isn't exposed by this driver. The driver does emit kobject uevents `EVENT=create` / `EVENT=destroy` with `vm_id`, and `destroy` fires only at the very end of `_gunyah_vm_put()`, so seeing it means teardown completed. **So RM's capability/doorbell accounting can't be read directly from the host**; it can only be inferred from (a)–(d).

### 14.3 A deterministic rejection that needs no code change: `-smp 16`

`handle_vcpu()` rejects `vcpu_count > rm_get_platform_max_cores()` with `ERROR_DENIED` (`"Error: invalid vcpu count(%u) vs max cores(%u)"`). That reaches the kernel as `5600000b` / `Error: 2` (14.1). The Snapdragon 8 Elite has 8 cores. QEMU accepts `-smp 16`, this fork's DTB builder emits 16 `/cpus` nodes, and the kernel's vCPU function bind only records the ID as a ticket label with no limit, so the run should get all the way to RM's `VM_INIT`.

Limitation: this failure happens in `handle_vcpu()`, **before** `handle_doorbell()`, so no doorbells or their HLOS caps exist when it aborts. It tests "does *any* rejected `VM_INIT` leave reboot-clearable state", not specifically "does a rejection *after doorbells were created* leak them". The latter needs a code change (14.5).

### 14.4 Proposed on-device test (copy-paste; uses the `control-stock` binary, commit `74874e2`)

Use your existing control-stock `virtio-keyboard-pci` command (the one from section 13 that printed `msi_vectors=2 SPI range 16-17` and `VM_START OK`) and change **only** the `-smp` argument. Call the two variants `GOOD` (your current `-smp`) and `BAD` (`-smp 16,sockets=1,cores=16,threads=1`).

**⚠ Warning before running:** if the leak theory is right, this is meant to recreate the bad state. Afterwards VMs, including the DroidVM app's, may fail to start until the phone is rebooted. If teardown hangs (14.2 (c)), the QEMU process may be unkillable until reboot.

```sh
# as root (su), once per iteration
dmesg -c > /dev/null                         # clear kernel log
<BAD command> 2> bad_N.log                   # expect exit with "Failed to start VM ... (errno=19)"
sleep 2
ps -A -o PID,STAT,NAME | grep qemu           # expect nothing; a 'D' entry means teardown hung
dmesg > bad_N.dmesg
grep -nE "rejected|Failed to (initialize|reset|deallocate)|post reset|WARNING|gunyah" bad_N.dmesg
grep -n "=== Gunyah VM topology" -A12 bad_N.log   # confirms 16 vCPUs + msi_vectors=2 reached GH_VM_START
```

1. **Calibrate (1×BAD):** expect `RM rejected message 5600000b. Error: 2` + `Failed to initialize VM: -19` in dmesg, `Failed to start VM: No such device (errno=19)` from QEMU, no `Failed to reset/deallocate`, no `WARNING`, no leftover `qemu` process.
   - If QEMU fails **before** the topology dump (a different error), `-smp 16` doesn't reach `VM_INIT` on this firmware; stop and report.
   - If RM *accepts* 16 vCPUs, try `-smp 32,sockets=1,cores=32,threads=1`.
2. **Accumulate:** alternate `BAD`, `GOOD`, `BAD`, `GOOD`, … for up to 10 pairs, capturing dmesg each time.
   - The leak theory predicts that some `GOOD` run eventually fails with `5600000b` even though its config is valid. That would give an on-demand repro, and the pair count says how many rejections it takes.
   - Any `Failed to reset/deallocate`, `WARNING` or stuck `D`-state process after a `BAD` run is direct evidence of a teardown leak, even if `GOOD` keeps passing.
3. **Optional control (accepted, then SIGKILL):** run `GOOD`, and as soon as `VM_START OK` appears, run `kill -9 $(pidof qemu-system-aarch64)`. Repeat 5×, then one plain `GOOD`. This exercises release from `RUNNING` with the guest barely started. 14.2 predicts it is no different from a normal exit, so it is low priority.

How to read it:
- `GOOD` starts failing after N `BAD`s → rejected `VM_INIT`s accumulate reboot-clearable state, and we have a repro to test `fix/msi-low-spi-window` (or any fix) against.
- 10 pairs with no `GOOD` failure and clean teardown logs → a rejection *before* doorbells doesn't leak. 14.5 is then the remaining way to test the doorbell-specific version of the theory.

### 14.5 Proposed (NOT implemented) diagnostic knob for a rejection *after* doorbells exist — needs a `.c` change, flagged for approval

To reproduce the original failure's shape more closely (RM aborting inside `handle_doorbell` after some doorbells and their HLOS caps were already created), a `diag/*` branch could add an env-gated, default-off switch in `gunyah_arm_fdt_customize()`'s per-vector bell loop: with `GUNYAH_DIAG_DUP_MSI_SPI=1`, the **last** MSI bell keeps its own label but reuses the previous bell's SPI in `interrupts`. In public RM terms, the 4 fixed bells and MSI bell 0 are created (caps copied to HLOS); the last bell's `map_virq()` then hits the duplicate (`dict_add` → `ERROR_DENIED`), `handle_doorbell` fails, and `VM_INIT` returns `NORESOURCE`. That is the same external signature, with doorbells half-built. It's about 5 lines and needs `msi_vectors ≥ 2` (the keyboard's default). The vendor RM may detect the duplicate elsewhere (for example at parse time), so the 14.4 calibration step would apply first. Not implemented, since nothing can validate it until 14.4 has run.

### 14.6 Bottom line

The original rejection can't be reproduced on demand today. A deterministic rejection is possible without code changes (`-smp 16`, 14.3), and it should be indistinguishable from the original at the `5600000b` / `Error: 2` level. Because of 14.1 that external match is guaranteed by RM's error reporting, not by a shared cause. It tests the general "rejected `VM_INIT` leaks reboot-clearable state" theory. If that comes back negative, 14.5 is the remaining targeted test. If both are negative, record the original failure as **currently non-reproducible; revisit if it recurs**. If it does recur, capture full `dmesg` (not just `rejected` lines) **before** rebooting, plus `ps -A -o PID,STAT,NAME | grep qemu`, and check the 14.2 (a)–(c) indicators. Rebooting destroys the only evidence.

## 15. UPDATE — 14.4's test run on real hardware: 6 clean BAD/GOOD pairs, then a GOOD run unexpectedly rejected, then the very next identical GOOD run passed with no reboot in between

Ran the 14.3/14.4 `-smp 16` calibration and the alternating BAD/GOOD sequence on-device (`control-stock`, commit `74874e2`, `virtio-keyboard-pci`). All commands and dmesg/ps checks as specified in 14.4.

**Calibration (`bad1`, standalone, before the pairs below):** `-smp 16,sockets=1,cores=16,threads=1` reached the topology dump (confirmed 16 vCPUs logged) and was rejected: `RM rejected message 5600000b. Error: 2` / `Failed to initialize VM: -19` in dmesg, `Failed to start VM: No such device (errno=19)` from QEMU. No `Failed to reset/deallocate`, no `WARNING`, no leftover `qemu-system-aarch64` process. `-smp 16` is a reliable, code-free way to force RM to reject `VM_INIT`, confirmed on the vendor RM.

**Pairs 1–6:** `badN` (smp=16) rejected every time with the same signature as calibration; the immediately following `goodN` (smp=6, the keyboard-only config, `msi_vectors=2 SPI range 16-17`) passed every time (`VM_START OK`), with no leftover process and no `WARNING`/reset-failure lines after either run. 6/6 pairs showed **no accumulation** — a single rejected `VM_INIT` did not poison the very next valid one.

**Pair 7 broke the pattern:** `bad7` rejected normally (same signature, clean teardown). The following `good7` — same exact valid command that had passed cleanly 6 times in a row — **also got rejected**: `gh-stderr.log` shows the full topology dump including `msi_vectors=2 SPI range 16-17` (the normal, previously-always-accepted layout), then `Failed to start VM: No such device (errno=19)`, with **zero guest boot output at all** (every prior `goodN` run printed the full Linux kernel boot log; `good7` printed none, meaning it never got past `GH_VM_START`). This is `GOOD` failing after `BAD`s, exactly as the leak theory in section 14.4 predicted it would look if it happened.

The dmesg check for `good7` came back empty for `gunyah`/`rejected` — but this phone's kernel log is extremely noisy (`[ADFR]`/display-pipeline lines print multiple times per second), and the ring buffer had very likely rotated the single gunyah line out before we read it; `gh-stderr.log`'s own errno=19 / topology-dump output is the authoritative record of what QEMU and the kernel driver returned for this run, and it unambiguously shows a rejection.

**But the state did not stick.** Immediately after (no reboot, no app restart, same `su` session), `good7b` — the identical `good7` command — printed the **full kernel boot log** and `VM_START OK`, with a freshly cleared dmesg afterward showing no `rejected`/`WARNING`/reset-failure lines and no leftover process.

**Revised theory:** this is not a simple monotonic leak that only clears on reboot (section 12/13's framing). The sequence `6 clean pairs → good7 rejected → good7b (same command) passes immediately` points instead to a **small, time-bounded resource that is transiently exhausted by back-to-back VM lifecycles and recovers asynchronously within about the 2-second gap already present between runs in this test script** (each run does `sleep 2` before checking `ps`/`dmesg`, then the next `mkdir`/`dmesg -c` before relaunching — so there's always a short gap, and it was sometimes, but not always, long enough). This would also explain section 13's earlier result (9/9 clean, including 5 back-to-back repeats of the exact original failing config) without contradiction: that test never hit the unlucky timing window this one did, and reboot was never actually required — any sufficient gap between attempts may be enough, and a reboot simply guarantees a long enough one.

**Open question, not yet resolved:** is the exhaustion driven by the *rate* of VM lifecycles (regardless of BAD vs GOOD), or specifically by *rejected* ones? Pairs 1–6 had the same ~2-second gap after each BAD as pair 7 did, and only pair 7 failed — so either this is probabilistic/load-dependent rather than a fixed threshold, or something about the *cumulative count* of prior rejections (7 BADs deep) mattered and pairs 1–6 just hadn't reached it yet. Both are consistent with the data so far; distinguishing them needs more pairs (continuing past 7) and/or deliberately varying the gap length between runs.

**Status:** `fix/msi-low-spi-window` remains unconfirmed as fixing anything — this failure mode has nothing to do with SPI numbering, since `good7`'s rejected config is the same `SPI range 16-17` layout that passed 6 times before it. Do not merge to `main`. Next step: keep running BAD/GOOD pairs past 7 to see whether failures recur at a roughly fixed cadence (supporting a count-based threshold) or sporadically (supporting a load/timing-based explanation), and consider testing with deliberately shorter or longer gaps between runs to see if that changes the failure rate.

**Pairs 8–10 (continuation):** ran 3 more pairs with the same template (`sleep 2` after each run, same ~2-second gap as pairs 1–7). All three passed cleanly — `bad8`/`bad9`/`bad10` rejected normally with clean teardown, `good8`/`good9`/`good10` all `VM_START OK` with no leftover process and no `WARNING` lines. **Final tally: 10 pairs run, only 1 unexpected failure (`good7`), which self-cleared on the immediate identical retry (`good7b`).** The failure did not recur at pair 8 (i.e., not "every 7th"), which argues against a simple fixed-count threshold and is more consistent with the sporadic/load-dependent framing. A ~10% failure rate on a valid config from a single sample is too small to pin down a precise mechanism, but it is enough to say: **this is not a simple monotonic leak, not tied to a specific pair count, and self-clears within seconds without a reboot.** One incidental note from this run: between pairs 7 and 8, Termux was force-stopped (not Ctrl+A X) after a guest hang at `(initramfs)`, which likely delivered an abrupt kill signal to `good7b`'s QEMU process after `VM_START OK` had already printed (release from `RUNNING` via signal, per 14.2's distinction) — this did not appear to affect pair 8's outcome, but is noted in case abrupt-kill-after-success turns out to matter for future tests.

## 16. ANALYSIS — what could recover within seconds, a gap-sweep test to make the failure reproducible, and where this leaves `fix/msi-low-spi-window` (source-only, nothing run on hardware)

### 16.1 The host kernel's teardown is synchronous; the asynchronous part is below it

- **Android Gunyah driver (`drivers/virt/gunyah`, `android16-6.12`):** no workqueues, no `call_rcu`/`synchronize_rcu`, no kthreads, no sleeps or timeouts anywhere in the driver. VM teardown (`_gunyah_vm_put()`, 14.2) runs when the last VM/vCPU file reference is released. For an exiting QEMU that happens in the exiting task's own `exit_files`/`exit_task_work`, **before** `exit_notify`. So by the time the shell's `wait` returns, the kernel has already sent `VM_RESET`, waited for RM's `RESET` notification, and deallocated the VMID. **Nothing in the host driver can still be releasing the previous VM when the next QEMU starts.** (Vendor modules loaded on this phone, such as `gunyah_host_share`, `gh_hugepage_reserve` and `nproc_guard`, are out-of-tree and not examined.)
- **Gunyah hypervisor (`quic/gunyah-hypervisor`):** every hypervisor object (doorbell, vCPU thread, VIC, address space, cspace cap table, …) is freed through `object_free_*()` → `rcu_enqueue()` (`hyp/core/object_standard/templates/object.c.tmpl`), and cap tables likewise (`cspace_destroy_cap_table` via `rcu_enqueue`). An object's memory goes back to its partition only **after an RCU grace period**. RM creates every VM object from its own partition (`gunyah_hyp_partition_create_doorbell(rm_get_rm_partition(), …)` etc.). So right after a teardown, memory RM has already "freed" can still be pending reclamation. A VM created in that window can hit a hypervisor allocation failure, which `vm_config_create_vdevices()` → `svm_init()` reports as `RM_ERROR_NORESOURCE` (14.1), i.e. `5600000b` / `Error: 2` / errno 19. Everything sits in the hypervisor/RM, out of the host's view, and clears by itself once the grace period finishes. **This is the only source-confirmed asynchronous mechanism on the path, and it matches every observation in section 15:** an isolated failure on a valid config, recovery within seconds without a reboot, and no host-side `WARNING`/reset-failure lines.
- **Caveats:** Gunyah RCU grace periods are normally milliseconds. Recovery taking about a second would mean RCU processing is delayed on this phone (CPUs idle or in deep power states, RM's own vCPU not being scheduled) or that the vendor RM/hypervisor defers something slower, such as sanitizing lent memory. Neither is visible from public source. An alternative that also fits 1-in-10: **another Gunyah VM** on the phone (Qualcomm trusted VMs, `gunyah_qtvm.c`) briefly using the same RM resources at that moment.

**Revised picture covering every observation so far (hypothesis):** RM has a finite pool of partition memory. Each VM's demand grows with vCPUs, doorbells (so MSI vectors) and VIC/address-space size. Memory from a just-destroyed VM returns to the pool only after an asynchronous reclaim. Before the section 9 reboot, after many rapid build-and-test cycles (and possibly leaked or fragmented partition memory), the margin was small. The extra per-vector doorbells were then enough to fail every time, which is why `msi_vectors > 0` failed 4/4 and `msi_vectors = 0` passed. After the reboot the margin is large, and only an unlucky rapid relaunch (pending reclaim) fails. This explains the original correlation with MSI without SPI numbering mattering, consistent with section 12's falsification.

### 16.2 Making it reproducible: sweep the gap between VM lifecycles (copy-paste)

GOOD→GOOD with no gap should stress pending reclamation the most, since a fully booted VM frees far more objects (6 vCPUs, all doorbells, a full address space) than a BAD run, which aborts in `handle_vcpu`. Run the **unchanged control-stock (`74874e2`) GOOD command** (keyboard, `msi_vectors=2`). Save as `/data/local/tmp/cycle.sh` and run as root:

```sh
#!/system/bin/sh
# usage: sh cycle.sh <gap_seconds> <iterations> <tag>
GAP=$1; N=$2; TAG=$3
GOOD_CMD='<paste your exact GOOD qemu-system-aarch64 command here>'
OUT=/data/local/tmp/cyc_$TAG; mkdir -p $OUT
# kernel log is very noisy (section 15); stream the Gunyah lines continuously instead of reading after the fact
dmesg -w 2>/dev/null | grep -iE "gunyah|rejected|Failed to (initialize|reset|deallocate)" > $OUT/dmesg.log &
DPID=$!
pass=0; fail=0; unk=0
i=1
while [ $i -le $N ]; do
  log=$OUT/run_$i.log
  echo "=== $TAG run $i start $(date +%s.%N)" >> $OUT/dmesg.log
  sh -c "exec $GOOD_CMD" < /dev/null > /dev/null 2> $log &
  pid=$!
  t=0
  while [ $t -lt 300 ]; do               # up to 30 s for an outcome
    grep -qE "VM_START OK|errno=19" $log && break
    sleep 0.1; t=$((t+1))
  done
  kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null   # release from RUNNING (or INIT_FAILED)
  if grep -q "VM_START OK" $log; then r=PASS; pass=$((pass+1))
  elif grep -q "errno=19" $log; then r=FAIL; fail=$((fail+1))
  else r=UNKNOWN; unk=$((unk+1)); fi
  echo "$TAG run $i $r $(date +%s.%N)" | tee -a $OUT/summary.txt
  [ "$GAP" != "0" ] && sleep $GAP
  i=$((i+1))
done
kill $DPID 2>/dev/null
echo "$TAG gap=$GAP pass=$pass fail=$fail unknown=$unk" | tee -a $OUT/summary.txt
ps -A -o PID,STAT,NAME | grep qemu    # expect nothing
```

Run in this order, rebooting **only** if a phase leaves VMs persistently failing:

```sh
sh /data/local/tmp/cycle.sh 10 10 gap10   # control: expect 0 failures
sh /data/local/tmp/cycle.sh 2  20 gap2    # section 15's spacing: expect ~0-10%
sh /data/local/tmp/cycle.sh 0  20 gap0    # back-to-back: the hypothesis predicts clearly more failures
```

- **Gap effect:** if failures cluster at `gap0` and disappear at `gap10`, we have an on-demand repro and the timing explanation is confirmed. If the rate doesn't depend on the gap, timing isn't the cause, and the "another Gunyah VM on the phone" alternative (16.1) moves up.
- **Demand effect (only if `gap0` still fails rarely):** rerun `gap0` with a heavier but still valid GOOD config: `-smp 8,sockets=1,cores=8,threads=1` (the platform maximum) plus `-device virtio-tablet-pci -device virtio-mouse-pci` (6 MSI vectors in total). The margin hypothesis predicts a higher failure rate with higher per-VM demand. That would also bring back the original section 9 "more MSI vectors, more failures" correlation.
- For every `FAIL`, keep `run_N.log` and the matching `dmesg.log` segment (between the `=== … start` markers). The streamed capture avoids the ring-buffer rotation that lost `good7`'s dmesg line.

`kill -9` matches 14.2: teardown from `RUNNING` takes the same kernel path as a clean QEMU exit. If the guest command uses a `server=on,wait=on` serial socket, either switch it to `wait=off` for this test or attach a client, otherwise QEMU blocks before `GH_VM_START` and every run reads `UNKNOWN`.

### 16.3 `fix/msi-low-spi-window`: recommend abandoning it as a bug fix

- Its premise, that RM rejects doorbell label/SPI ≥ 0x10, is falsified (section 12: SPI 16–17 passes on stock; section 15: the same SPI 16–17 config fails once and then passes). Nothing it changes is related to a rejection that depends on timing or capacity.
- It has a real cost: a hard cap of 12 MSI vectors, with warnings and fewer queues per device for realistic multi-device configs. **Recommendation: don't merge it.** Keep the branch only as the README's record, or cherry-pick just the README commits to `main`.

### 16.4 Retry-on-rejection: feasible, but only by relaunching the VM — high-level sketch (not implemented)

- **No in-process retry on the same VM.** After a rejection the kernel leaves the VM in `INIT_FAILED`. A second `GH_VM_START` ioctl returns `-ENODEV` at once (`gunyah_vm_ensure_started()` only starts from `NO_STATE`). A retry needs the VM fd fully released (teardown, 14.2) and a new VM created: a new `GH_CREATE_VM`, every memory lend, every vCPU/IRQFD function, the DTB config and the boot context. QEMU's Gunyah accel does all of that during machine init, so redoing it inside one QEMU process would be a large refactor.
- **`errno=19` can't be classified as transient.** Every `create_vdevices` failure becomes `NORESOURCE` (14.1), so a deterministic misconfiguration (for example `-smp 16`) looks the same as a transient failure. Retries must therefore be bounded.
- **Recommended shape (process-level retry):**
  1. QEMU: in `gunyah_start_vm()`, when `GH_VM_START` fails with `ENODEV`, log `GH_VM_START rejected (retryable)` and exit with a **new dedicated status** (for example 83, `GUNYAH_VM_START_RETRY_STATUS`), instead of `exit(1)`. Don't reuse 82: `GUNYAH_VM_RESTART_STATUS` already means "guest requested reset" (PSCI `SYSTEM_RESET`), and a launcher that relaunches on 82 immediately would loop forever on a deterministic rejection.
  2. Launcher (the DroidVM app or a shell wrapper): on exit 83, wait and relaunch with backoff (1 s, 2 s, 4 s), at most 3 attempts. Then report the failure as permanent, with the last stderr/dmesg attached.
  - About 5 lines of QEMU code plus launcher logic. Since teardown has finished by the time the process exits (16.1), the backoff is purely for the RM/hypervisor side to catch up.
- **Alternative without launcher changes:** QEMU re-`exec`s itself after a delay, with an attempt counter in the environment. That is riskier: every fd that isn't close-on-exec (Gunyah VM/vCPU fds included) would survive and block teardown, and the serial/QMP sockets DroidVM is connected to would drop. Not recommended.
- **Validation:** use the 16.2 `gap0` loop with the retrying build and a wrapper. The failure rate after retries should be zero, while the logs show the retries happening.

### 16.5 Bottom line

The evidence now points to a transient, capacity/timing-dependent rejection inside RM or the hypervisor. The likeliest source-backed mechanism is the hypervisor reclaiming freed objects via RCU, with a pool margin that shrinks under load. SPI numbering is not the cause. Next: run 16.2 to get a repro rate that depends on the gap. If it confirms the gap effect, the practical mitigation is the bounded process-level retry in 16.4, plus a short spacing between back-to-back launches. A QEMU-only fix for an RM-internal reclaim delay isn't possible.

## 17. CONFIRMED on real hardware — failure rate rises as the gap between VM lifecycles shrinks

Ran the 16.2 gap-sweep script on-device (`control-stock`, commit `74874e2`, unchanged `virtio-keyboard-pci` GOOD command, each run killed with `kill -9` right after `VM_START OK` or `errno=19` is observed, per the script). Two bugs in the originally proposed script were fixed before running: `ps -A -o PID,STAT,NAME` (the same unsupported toybox `ps` syntax that failed in section 9's first test) was changed to `ps -A | grep qemu`, and the `sleep 0.1` polling interval (fractional sleeps are unreliable on this device's toybox) was changed to `sleep 1` with a 30-iteration cap. The script otherwise ran exactly as written.

| Phase | Gap | Runs | PASS | FAIL | UNKNOWN |
|---|---|---|---|---|---|
| `gap10` | 10s | 10 | 10 | 0 | 0 |
| `gap2` | 2s | 20 | 20 | 0 | 0 |
| `gap0` | 0s (back-to-back) | 20 | 18 | **2 (10%)** | 0 |

Both `gap0` failures (runs 4 and 9) showed the same signature as every other rejection in this document: `errno=19` from QEMU, no leftover `qemu-system-aarch64` process afterward, and no `WARNING`/reset-failure lines in the streamed dmesg capture — a clean rejection and clean teardown, just like `good7` in section 15.

**This confirms the gap-dependency hypothesis from section 16.1.** The failure rate is a monotonic function of how little time elapses between tearing down one VM and starting the next: 0% at 10s, 0% at 2s (in this run — section 15's manual testing did see one failure at roughly this spacing, so the true rate at 2s is low but nonzero, just not large enough to show up in 20 runs here), and 10% at 0s. This is strong evidence for the hypervisor-side asynchronous object reclaim (RCU-deferred freeing, section 16.1) being the real mechanism, rather than the "another VM on the phone" alternative, which would not be expected to correlate with the gap we control.

**Practical conclusion:** `fix/msi-low-spi-window` should not be merged — confirmed again, this has nothing to do with SPI numbering. The real, now reproducible failure mode is a brief resource race between consecutive VM lifecycles. Section 16.4's proposed mitigation (QEMU exits with a dedicated retry-eligible status on `ENODEV` from `GH_VM_START`; the launcher relaunches with backoff, capped at 3 attempts) is the right shape of fix for this. Given we now have an on-demand-ish repro (`gap0`, ~10% failure rate), that retry behavior could actually be validated on real hardware: implement it, run the `gap0` sweep again with the retrying build, and confirm the failure rate drops to 0% with the retries visible in the logs.
