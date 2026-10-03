hw_display_modules = {}

system_ss.add(when: 'CONFIG_EDID', if_true: files('edid-generate.c'))
system_ss.add(when: 'CONFIG_VIRTIO', if_true: files('virtio-dmabuf.c'))
system_ss.add(files('ramfb.c'))

if config_all_devices.has_key('CONFIG_VIRTIO_PCI')
  # 2D virtio-gpu core + PCI device: no OpenGL/virglrenderer needed.
  virtio_gpu_pci_ss = ss.source_set()
  virtio_gpu_pci_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_PCI'],
                        if_true: [files('virtio-gpu-base.c',
                                        'virtio-gpu.c',
                                        'virtio-gpu-pci.c',
                                        'virtio-gpu-udmabuf.c'), pixman])
  hw_display_modules += {'virtio-gpu-pci': virtio_gpu_pci_ss}

  # virgl 3D device on top of the same core and PCI base.
  virtio_gpu_gl_pci_ss = ss.source_set()
  virtio_gpu_gl_pci_ss.add(when: [opengl, virgl, 'CONFIG_VIRTIO_GPU',
                                  'CONFIG_VIRTIO_PCI'],
                           if_true: [files('virtio-gpu-gl-pci.c',
                                           'virtio-gpu-gl.c'), pixman])
  hw_display_modules += {'virtio-gpu-gl-pci': virtio_gpu_gl_pci_ss}
endif

modules += { 'hw-display': hw_display_modules }
