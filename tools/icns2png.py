#!/usr/bin/env python3
import sys, struct, zlib

data = open(sys.argv[1], "rb").read()
out = sys.argv[2]
assert data[:4] == b"icns", "not an icns file"
total = struct.unpack(">I", data[4:8])[0]
chunks = {}
off = 8
while off + 8 <= len(data) and off < total:
    t = data[off:off+4]; ln = struct.unpack(">I", data[off+4:off+8])[0]
    if ln < 8 or off+ln > len(data):
        break
    chunks[t] = data[off+8:off+ln]
    off += ln

def decode_rle24(d, npix):
    if d[:4] == b"\x00\x00\x00\x00":
        d = d[4:]
    o = bytearray(); i = 0; need = npix*3
    while len(o) < need and i < len(d):
        n = d[i]; i += 1
        if n < 0x80:
            c = n+1; o += d[i:i+c]; i += c
        else:                          # n-0x80+3
            c = n-0x80+3; v = d[i]; i += 1; o += bytes([v])*c
    return o[:need]

for rgb_t, mask_t, size in ((b"it32", b"t8mk", 128), (b"il32", b"l8mk", 32), (b"is32", b"s8mk", 16)):
    if rgb_t in chunks and mask_t in chunks:
        npix = size*size
        planes = decode_rle24(chunks[rgb_t], npix)
        R, G, B = planes[0:npix], planes[npix:2*npix], planes[2*npix:3*npix]
        A = chunks[mask_t][:npix]
        if len(A) < npix:
            A = A + bytes([255])*(npix-len(A))
        rgba = bytearray(npix*4)
        for p in range(npix):
            rgba[p*4+0] = R[p]; rgba[p*4+1] = G[p]; rgba[p*4+2] = B[p]; rgba[p*4+3] = A[p]
        raw = bytearray()
        for y in range(size):
            raw.append(0); raw += rgba[y*size*4:(y+1)*size*4]
        comp = zlib.compress(bytes(raw), 9)

        def ch(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t+d) & 0xffffffff)

        png = b"\x89PNG\r\n\x1a\n"
        png += ch(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        png += ch(b"IDAT", comp)
        png += ch(b"IEND", b"")
        open(out, "wb").write(png)
        print("decoded %s+%s %dx%d -> %s (%d bytes)" % (rgb_t.decode(), mask_t.decode(), size, size, out, len(png)))
        sys.exit(0)
print("no decodable rgb+mask pair found"); sys.exit(2)
