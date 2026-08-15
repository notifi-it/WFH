#!/usr/bin/env bash
# Pull what LVGL actually rendered off the board as a PNG.
# Needs CONFIG_WFH_DUMP_FRAMEBUFFER=y in the build.
#
#   tools/grab-screen.sh [port] [out.png]
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
s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)

buf, end = b'', time.time() + 150
while time.time() < end:
    c = s.read(16384)
    if c: buf += c
    if b'<<<END>>>' in buf and b'<<<FB' in buf: break
s.close()

m = re.search(r'<<<FB (\d+) (\d+) (\d+)>>>(.*?)<<<END>>>', buf.decode('utf-8', 'replace'), re.S)
if not m:
    sys.exit("no framebuffer in stream — is CONFIG_WFH_DUMP_FRAMEBUFFER on?")

w, h, stride = map(int, m.groups()[:3])
b64 = re.sub(r'[^A-Za-z0-9+/=]', '', m.group(4))
b64 = b64[:len(b64) - len(b64) % 4]        # serial can clip the final chunk
raw = base64.b64decode(b64)
raw += bytes(stride * h - len(raw)) if len(raw) < stride * h else b''

rows = bytearray()
for y in range(h):
    line = raw[y * stride:(y + 1) * stride]
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
print(f"wrote {out}  ({w}x{h})")
PY
