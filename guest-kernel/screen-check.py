#!/usr/bin/env python3
"""Screen helpers for boot-test.sh.

  screen-check.py dump QMP_SOCKET OUT.ppm   QMP screendump of the console
  screen-check.py lit IN.ppm OUT.png        convert; fail if the screen is blank
"""
import json
import socket
import sys


def dump(sock_path, out):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    f = s.makefile("rw")

    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a} if a else {"execute": c}) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                return r

    json.loads(f.readline())  # greeting
    cmd("qmp_capabilities")
    r = cmd("screendump", filename=out)
    print("screendump:", r)
    return 0 if "return" in r else 1


def lit(ppm, png):
    from PIL import Image
    im = Image.open(ppm).convert("RGB")
    im.save(png)
    px = list(im.getdata())
    n = sum(1 for p in px if max(p) > 64)
    print(f"screendump {im.size[0]}x{im.size[1]}, {n} lit pixels "
          f"({100 * n / len(px):.2f}%)")
    return 0 if n > 2000 else 1


if __name__ == "__main__":
    sys.exit({"dump": dump, "lit": lit}[sys.argv[1]](*sys.argv[2:]))
