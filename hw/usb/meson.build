# Trimmed to what DroidVM uses: the USB core, qemu-xhci and the HID
# devices (usb-tablet, usb-kbd, usb-mouse).
system_ss.add(when: 'CONFIG_USB', if_true: files(
  'bus.c',
  'combined-packet.c',
  'core.c',
  'desc.c',
  'desc-msos.c',
  'libhw.c',
  'pcap.c',
))

system_ss.add(when: 'CONFIG_USB_XHCI', if_true: files('hcd-xhci.c'))
system_ss.add(when: 'CONFIG_USB_XHCI_PCI', if_true: files('hcd-xhci-pci.c'))

system_ss.add(when: 'CONFIG_USB_HID', if_true: files('dev-hid.c'))
