

#ifndef QEMU_GUNYAH_H
#define QEMU_GUNYAH_H

#include "qemu/accel.h"
#include "qom/object.h"

#ifdef NEED_CPU_H
#include "cpu.h"
#endif

extern bool gunyah_allowed;
struct arm_boot_info;

void gunyah_set_swiotlb_size(uint64_t size);

#define gunyah_enabled() (gunyah_allowed)

#define GUNYAH_V2M_BASE 0x08020000
#define GUNYAH_V2M_SIZE 0x1000
/*
 * Guest-visible SPIs (and doorbell labels) used for MSI delivery. Each MSI
 * vector becomes a /gunyah-vm-config/vdevices/bell-<label> doorbell with
 * label == SPI. The Resource Manager on shipping Qualcomm firmware rejects
 * VM_INIT (RM_ERROR_NORESOURCE) as soon as any such doorbell uses label/SPI
 * 0x10 or above, while labels 0x0-0xf work (the same range crosvm uses:
 * fixed SPIs 0-3 and 15, per-device SPIs from 4). So MSIs are packed into
 * the free SPIs between the fixed bells (0x0, 0x1, 0x2) and bell-f.
 * GUNYAH_MSI_SPI_BASE in the environment overrides the base (diagnostics).
 */
#define GUNYAH_MSI_SPI_BASE_DEFAULT 3
#define GUNYAH_MSI_SPI_LIMIT 15
/*
 * QEMU-internal GIC input lines used to route guest MSI writes to the
 * per-vector IRQFD eventfds. Kept separate from the guest-visible SPIs so
 * that QEMU's own board IRQ lines (PCIe INTx on 3-6, UART1 on 8) never
 * alias an MSI vector.
 */
#define GUNYAH_MSI_ROUTE_BASE 16
#define GUNYAH_MSI_PHANDLE 4
#define GUNYAH_VM_RESTART_STATUS 82

#define TYPE_GUNYAH_ACCEL ACCEL_CLASS_NAME("gunyah")
typedef struct GUNYAHState GUNYAHState;
DECLARE_INSTANCE_CHECKER(GUNYAHState, GUNYAH_STATE,
                         TYPE_GUNYAH_ACCEL)

int gunyah_arm_set_dtb(uint64_t dtb_start, uint64_t dtb_size);
void gunyah_arm_build_dtb(const struct arm_boot_info *binfo, void *fdt);

bool gunyah_addr_is_lend(uint64_t gpa);
void gunyah_embedded_cleanup(void);
void gunyah_arm_fdt_customize(void *fdt, uint64_t mem_base,
                uint32_t gic_phandle);

#include "qemu/event_notifier.h"
void gunyah_gic_register_irq_notifiers(EventNotifier *notifiers,
                                        int count, int base_spi);

#endif  /* QEMU_GUNYAH_H */
