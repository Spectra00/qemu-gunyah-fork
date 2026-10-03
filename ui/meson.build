system_ss.add(pixman)
specific_ss.add(when: ['CONFIG_SYSTEM_ONLY'], if_true: pixman)

system_ss.add(png)
system_ss.add(files(
  'console.c',
  'cursor.c',
  'dmabuf.c',
  'input-keymap.c',
  'input-leds.c',
  'input.c',
  'kbd-state.c',
  'keymaps.c',
  'qemu-pixman.c',
))
system_ss.add(files('console-vc-stubs.c'))

# VNC server, trimmed for DroidVM: no-auth only (no TLS/VeNCrypt, SASL,
# password auth, websockets or clipboard); JPEG/PNG tight variants off.
system_ss.add(zlib, files(
  'vnc.c',
  'vnc-enc-zlib.c',
  'vnc-enc-hextile.c',
  'vnc-enc-tight.c',
  'vnc-palette.c',
  'vnc-enc-zrle.c',
  'vnc-jobs.c',
))
if is_android
  system_ss.add(opengl, android_native_window, files(
    'agl.c',
    'egl-helpers.c',
    'shader.c',
  ))
endif
if dbus_display
  error('DBus display was removed from this trimmed build')
endif

if host_os == 'linux'
  system_ss.add(files('udmabuf.c'))
endif
if cocoa.found()
  error('Cocoa UI was removed from this trimmed build')
endif

if dbus_display
  error('DBus UI was removed from this trimmed build')
endif

keymaps = [
  ['linux', 'qcode'],
  ['qcode', 'linux'],
  ['qcode', 'qnum'],
  ['qnum', 'qcode'],
  ['usb', 'qcode'],
]

if have_system or xkbcommon.found()
  keycodemapdb_proj = subproject('keycodemapdb', required: true)
  foreach e : keymaps
    output = 'input-keymap-@0@-to-@1@.c.inc'.format(e[0], e[1])
    genh += custom_target(output,
                  output: output,
                  capture: true,
                  input: keycodemapdb_proj.get_variable('keymaps_csv'),
                  command: [python, keycodemapdb_proj.get_variable('keymap_gen').full_path(),
                            'code-map', '--lang', 'glib2',
                            '--varname', 'qemu_input_map_@0@_to_@1@'.format(e[0], e[1]),
                            '@INPUT0@', e[0], e[1]])
  endforeach
endif
