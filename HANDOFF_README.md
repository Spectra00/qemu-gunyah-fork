# qemu-gunyah-fork — not part of the current build

> **Previous approach:** a QEMU/Gunyah/Debian 12 implementation was built and documented first. It is preserved on the `archive/qemu-gunyah-debian12` branch (tag `qemu-debian12-final`) if we ever need to reference or pull from it. We moved off it because DroidVM's crosvm backend already has working, benchmarked GPU acceleration for this phone; on QEMU that would have had to be rebuilt from scratch.

## Current build schema

The project now runs DroidVM's **crosvm** backend on the Gunyah hypervisor with an **Ubuntu 26.04 (resolute) arm64** guest and DroidVM's existing 3D stack:

- host: DroidVM app + bundled crosvm/virglrenderer (drm2kgsl native context) + DroidVM's Gunyah host kernel modules;
- guest: `Droid-VM/droidvm-guest-additions` (DKMS `gunyah_guest` + patched `virtio-gpu`) and the `mesa-guest` deb (Turnip over virtio);
- target: Valve's ARM64 Linux Steam client + Proton 11 ARM64 (FEX), GPU-accelerated through drm2kgsl.

**QEMU is not used anywhere in that build, so this repository has no active work.** The plan, Phase 0 bring-up steps and device notes live in `Spectra00/DroidVM-dev-fork` → `HANDOFF_README.md` on `master`.

Use this repository only to look up the archived QEMU work, for example the Gunyah DTB/initrd fix, the `GH_VM_START` retry behaviour (exit status 83), or the restricted-DMA-pool analysis.
