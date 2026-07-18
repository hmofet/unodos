import sys,struct,zlib
d=open(sys.argv[1],"rb").read()
assert d[:2]==b"P6"
i=2; vals=[]
while len(vals)<3:
    while d[i] in b" \t\n\r": i+=1
    j=i
    while d[j] not in b" \t\n\r": j+=1
    vals.append(int(d[i:j])); i=j
i+=1
w,h,_=vals; px=d[i:i+w*h*3]
raw=bytearray()
for y in range(h):
    raw.append(0); raw+=px[y*w*3:(y+1)*w*3]
def ch(t,dd): return struct.pack(">I",len(dd))+t+dd+struct.pack(">I",zlib.crc32(t+dd))
open(sys.argv[2],"wb").write(b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))+ch(b"IDAT",zlib.compress(bytes(raw),6))+ch(b"IEND",b""))
print("wrote %s (%dx%d)"%(sys.argv[2],w,h))
