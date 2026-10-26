import struct,sys
d=open('orig/LEGOStarWarsSaga_unp.exe','rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
n=struct.unpack_from('<H',d,pe+6)[0]; opt=struct.unpack_from('<H',d,pe+20)[0]
base=struct.unpack_from('<I',d,pe+24+28)[0]
secs=[]
for i in range(n):
    o=pe+24+opt+40*i
    vs,va,rs,ro=struct.unpack_from('<IIII',d,o+8); secs.append((va,vs,ro,rs))
def off(a):
    r=a-base
    for va,vs,ro,rs in secs:
        if va<=r<va+max(vs,rs): return ro+r-va
def val(a,sz):
    o=off(a)
    if o is None or o+sz>len(d): return None
    return struct.unpack_from('<d' if sz==8 else '<f',d,o)[0]
# x87 mem-operand arithmetic: D8/DC /0 fadd /1 fmul /4 fsub /5 fsubr /6 fdiv /7 fdivr; D9/DD /0 fld
names={0:'fadd',1:'fmul',2:'fcom',3:'fcomp',4:'fsub',5:'fsubr',6:'fdiv',7:'fdivr'}
def scan(lo,hi,targets):
    for a in range(lo,hi):
        o=off(a)
        b=d[o]
        if b in (0xD8,0xDC,0xD9,0xDD) and d[o+1]&0xC7==0x05:
            sz=4 if b in (0xD8,0xD9) else 8
            ad=struct.unpack_from('<I',d,o+2)[0]
            v=val(ad,sz)
            if v is None: continue
            for t in targets:
                if abs(v-t)<1e-9*max(1,abs(t)):
                    op=('fld' if b in(0xD9,0xDD) else names[(d[o+1]>>3)&7]) if not(b in(0xD9,0xDD) and ((d[o+1]>>3)&7)!=0) else None
                    if op: print(hex(a),op,'m%d'%(sz*8),hex(ad),v)
if __name__=='__main__':
    lo=int(sys.argv[1],16);hi=int(sys.argv[2],16);ts=[float(x) for x in sys.argv[3:]]
    scan(lo,hi,ts)
