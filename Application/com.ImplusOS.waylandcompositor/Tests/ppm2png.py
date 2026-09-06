import struct, zlib, sys
src, dst = sys.argv[1], sys.argv[2]
d = open(src,'rb').read()
parts = d.split(b'\n',3)
w,h = map(int, parts[1].split()); px = parts[3]
def png(path,w,h,rgb):
    raw=b''.join(b'\x00'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t,data):
        return struct.pack('>I',len(data))+t+data+struct.pack('>I',zlib.crc32(t+data)&0xffffffff)
    out=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
    out+=chunk(b'IDAT',zlib.compress(raw,6))+chunk(b'IEND',b'')
    open(path,'wb').write(out)
png(dst,w,h,px)
# report non-background pixel count
from collections import Counter
c=Counter(px[i:i+3] for i in range(0,len(px),3))
print(dst, w, h, "distinct colours:", len(c), "most common:", c.most_common(3))
