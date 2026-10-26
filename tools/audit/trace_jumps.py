import re,sys
rows=[]
for l in open(sys.argv[1]):
    m=re.match(r'H ([\d.]+) dt ([\d.]+) n (\d+) p (\S+) (\S+) (\S+) v (\S+) (\S+) (\S+) f641 (\d+) f508 (\S+) in (\S+)',l)
    if m:
        t,dt,n,x,y,z,vx,vy,vz,f,f5,inp=m.groups()
        rows.append([float(t),float(dt),int(n),float(x),float(y),float(z),float(vx),float(vy),float(vz),int(f),f5,int(inp,16)])
last=-9;J=[]
for i in range(1,len(rows)):
    if rows[i][7]>=1 and rows[i-1][7]<=0.5 and rows[i][0]-last>0.4:
        J.append(i); last=rows[i][0]
print('jumps',len(J))
for i in J:
    t=rows[i][0]; seg=[r for r in rows[i:] if r[0]<t+4]
    mx=max(r[3] for r in seg); my=max(r[4] for r in seg)
    print('t%.1f start x%.3f y%.3f z%.2f vx%.3f | maxx %.3f maxy %.3f | end x%.2f y%.3f'%(t,rows[i][3],rows[i][4],rows[i][5],rows[i][6],mx,my,seg[-1][3],seg[-1][4]))
