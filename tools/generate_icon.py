"""Generate the original application icon using only Python's standard library."""
from pathlib import Path
import struct, math
root=Path(__file__).resolve().parents[1]
def inside(x,y,left,top,w,h,r):
    dx=max(left+r-x,0,x-(left+w-r));dy=max(top+r-y,0,y-(top+h-r))
    return dx*dx+dy*dy<=r*r
images=[]
for size in [16,20,24,32,48,64,128,256]:
    pixels=bytearray()
    for y in range(size-1,-1,-1):
        for x in range(size):
            colors=[]
            for sy in [0.125,0.375,0.625,0.875]:
                for sx in [0.125,0.375,0.625,0.875]:
                    px=(x+sx)*128/size;py=(y+sy)*128/size
                    c=(0,0,0,0)
                    if inside(px,py,4,4,120,120,32):c=(22,34,43,255)
                    for bx,by,bh in [(28,52,40),(56,36,56),(84,20,72)]:
                        if inside(px,py,bx,by,16,bh,8):c=(102,226,190,255)
                    colors.append(c)
            # Premultiplication is not needed for ICO DIB straight-alpha pixels.
            a=sum(c[3] for c in colors)/16
            if a:
                rgb=[round(sum(c[i]*c[3]/255 for c in colors)/16*255/a) for i in range(3)]
            else:rgb=[0,0,0]
            pixels.extend(bytes([rgb[2],rgb[1],rgb[0],round(a)]))
    mask=bytes(((size+31)//32)*4*size)
    dib=struct.pack('<IiiHHIIiiII',40,size,size*2,1,32,0,len(pixels),0,0,0,0)+pixels+mask
    images.append((size,dib))
offset=6+16*len(images)
header=struct.pack('<HHH',0,1,len(images));directory=b''
for size,data in images:
    directory+=struct.pack('<BBBBHHII',0 if size==256 else size,0 if size==256 else size,0,0,1,32,len(data),offset);offset+=len(data)
(root/'assets'/'app.ico').write_bytes(header+directory+b''.join(d for _,d in images))
print('Generated assets/app.ico')
