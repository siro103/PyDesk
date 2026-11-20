import math
import struct
from pathlib import Path


def distance(x, y, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    t = max(0, min(1, ((x-a[0])*dx + (y-a[1])*dy)/(dx*dx+dy*dy)))
    return math.hypot(x-a[0]-t*dx, y-a[1]-t*dy)


def layer(n):
    data = bytearray()
    for row in reversed(range(n)):
        for col in range(n):
            rgb = [0, 0, 0]
            covered = 0
            for dy in range(4):
                for dx in range(4):
                    x, y = (col+(dx+.5)/4)*32/n, (row+(dy+.5)/4)*32/n
                    if math.hypot(max(8-x, 0, x-24), max(8-y, 0, y-24)) > 7:
                        continue
                    covered += 1
                    symbol = min(distance(x, y, a, b) for a, b in [
                        ((7.5, 11), (12.5, 16)), ((12.5, 16), (7.5, 21)), ((16, 21), (24, 21))
                    ]) < 1.4
                    for i, v in enumerate((255, 255, 255) if symbol else (20,110,85)):
                        rgb[i] += v
            data.extend(bytes([*(v//max(covered,1) for v in reversed(rgb)), round(255*covered/16)]))
    mask = bytes(((n+31)//32)*4*n)
    return struct.pack("<IIIHHIIIIII",40,n,2*n,1,32,0,len(data),0,0,0,0)+data+mask


sizes = [16, 32, 48, 64]
layers = [layer(n) for n in sizes]
offset = 6 + 16*len(sizes)
entries = bytearray()
for n, data in zip(sizes, layers):
    entries.extend(struct.pack("<BBBBHHII",n,n,0,0,1,32,len(data),offset))
    offset += len(data)
(Path(__file__).parent/"build"/"PyDesk.ico").write_bytes(
    struct.pack("<HHH",0,1,len(sizes))+entries+b"".join(layers))
