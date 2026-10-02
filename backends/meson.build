system_ss.add([files(
  'hostmem-ram.c',
  'hostmem.c',
  'rng-builtin.c',
  'rng.c',
  'confidential-guest-support.c',
), numa])

if host_os != 'windows'
  system_ss.add(files('rng-random.c'))
endif
