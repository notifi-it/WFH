#!/usr/bin/env bash
# Pull what LVGL actually rendered off the board as a PNG.
# Needs CONFIG_WFH_DUMP_FRAMEBUFFER=y in the build.
#
#   tools/grab-screen.sh [port] [out.png]
set -euo pipefail
PORT=${1:-/dev/cu.usbmodem1101}
OUT=${2:-screen.png}
python3 - "$PORT" "$OUT" <<'PY'
import base64, re, sys, serial
from PIL import Image
port, out = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=3); s.reset_input_buffer()
s.setDTR(False); s.setRTS(True); import time; time.sleep(0.15); s.setRTS(False)
buf, end = b'', time.time() + 150
while time.time() < end:
    c = s.read(16384)
    if c: buf += c
    if b'<<<END>>>' in buf and b'<<<FB' in buf: break
s.close()
m = re.search(r'<<<FB (\d+) (\d+) (\d+)>>>(.*?)<<<END>>>', buf.decode('utf-8','replace'), re.S)
if not m: sys.exit("no framebuffer in stream — is CONFIG_WFH_DUMP_FRAMEBUFFER on?")
w, h, stride = map(int, m.groups()[:3])
raw = base64.b64decode(re.sub(r'[^A-Za-z0-9+/=]', '', m.group(4)))
img = Image.new("RGB", (w, h)); px = img.load()
for y in range(h):
    row = raw[y*stride:(y+1)*stride]
    for x in range(w):
        v = row[x*2] | (row[x*2+1] << 8)
        px[x, y] = (((v>>11)&31)*255//31, ((v>>5)&63)*255//63, (v&31)*255//31)
img.save(out); print("wrote", out)
PY
