#!/usr/bin/env bash
# Pull what LVGL actually rendered off the board as a PNG.
# Needs CONFIG_WFH_DUMP_FRAMEBUFFER=y in the build.
#
#   tools/grab-screen.sh [port] [out.png]
#
# The dump arrives one framed line per row ("R<y>:<base64>"), so a byte lost
# in transit corrupts that row only — it is painted magenta and counted,
# instead of shearing everything after it. A capture that reports dropped
# rows is a capture you can't trust for colour diffing; regrab it.
#
# Writes the PNG with zlib and struct only — no Pillow. The ESP-IDF python
# env has pyserial and not much else, and a debugging tool that needs its own
# install is a tool you stop reaching for.
set -euo pipefail
PORT=${1:-/dev/cu.usbmodem1101}
OUT=${2:-screen.png}
python3 - "$PORT" "$OUT" <<'PY'
import base64, re, struct, sys, time, zlib, serial

port, out = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=3)
s.reset_input_buffer()
s.write(b'd')          # on-demand dump; no reset, the board keeps its state

buf, end = b'', time.time() + 60
while time.time() < end:
    c = s.read(16384)
    if c: buf += c
    if b'<<<END>>>' in buf and b'<<<FB' in buf: break
s.close()

m = re.search(rb'<<<FB (\d+) (\d+) (\d+)>>>(.*?)<<<END>>>', buf, re.S)
if not m:
    sys.exit("no framebuffer in stream — is CONFIG_WFH_DUMP_FRAMEBUFFER on?")

w, h, stride = int(m.group(1)), int(m.group(2)), int(m.group(3))
MAGENTA = bytes((0x1F, 0xF8)) * (stride // 2)   # matches the snapshot pre-fill

rows_raw = [None] * h
for line in m.group(4).split(b'\n'):
    lm = re.fullmatch(rb'R(\d+):([A-Za-z0-9+/=]+)\r?', line.strip())
    if not lm: continue
    y = int(lm.group(1))
    if y >= h: continue
    try:
        d = base64.b64decode(lm.group(2), validate=True)
    except Exception:
        continue
    if len(d) == stride:
        rows_raw[y] = d

lost = [y for y in range(h) if rows_raw[y] is None]

rows = bytearray()
for y in range(h):
    line = rows_raw[y] or MAGENTA
    rows.append(0)                                  # PNG filter: none
    for x in range(w):
        v = line[x * 2] | (line[x * 2 + 1] << 8)    # RGB565, little-endian
        rows += bytes((((v >> 11) & 31) * 255 // 31,
                       ((v >> 5) & 63) * 255 // 63,
                       (v & 31) * 255 // 31))

def chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data
            + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(bytes(rows), 6))
       + chunk(b'IEND', b''))
open(out, 'wb').write(png)
status = "clean" if not lost else f"{len(lost)} of {h} rows dropped (magenta)"
print(f"wrote {out}  ({w}x{h}, {status})")
PY
