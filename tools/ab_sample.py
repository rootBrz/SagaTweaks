# SPDX-License-Identifier: GPL-3.0-only
import struct,subprocess,sys,time,json,array
label=sys.argv[1]; out=sys.argv[2]; gap=2.0
pid=int(subprocess.check_output("pgrep -f 'S:.steamapps.common.Lego Star Wars Saga.LEGOStarWarsSaga.exe' | head -1",shell=True))
f=open(f"/proc/{pid}/mem","rb")
LO,HI=0x7ef000,0x29ab000
def snap():
    d={}
    a=LO
    while a<HI:
        try:
            f.seek(a); d[a]=f.read(0x10000)
        except Exception: pass
        a+=0x10000
    return d
t0=time.time(); A=snap(); t1=time.time(); time.sleep(max(0,gap-(time.time()-t0))); B=snap(); t2=time.time()
dt=(t2-t0)-(t2-t1)/2-(t1-t0)/2
res={}
for a,x in A.items():
    y=B.get(a)
    if not y or x==y: continue
    xa=array.array('i',x[:len(x)//4*4]); ya=array.array('i',y[:len(y)//4*4])
    for i in range(len(xa)):
        if xa[i]!=ya[i]:
            res[a+4*i]=(xa[i],ya[i])
json.dump({"label":label,"dt":dt,"res":{hex(k):v for k,v in res.items()}},open(out,"w"))
print(label,"changed dwords",len(res),"dt",round(dt,2))
