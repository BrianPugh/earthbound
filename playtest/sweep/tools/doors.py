import json,sys
pt=json.load(open('src/assets/maps/door_pointer_table.json'))
cfg=json.load(open('src/assets/maps/door_config_table.json'))
dd=json.load(open('src/assets/maps/door_data.json'))
def w(a,i): return a[i]|a[i+1]<<8
x0,y0,x1,y1=map(int,sys.argv[1:5])
for sy in range(y0//256,y1//256+1):
  for sx in range(x0//256,x1//256+1):
    i=(sy*32+sx)*4
    p=(pt[i]|pt[i+1]<<8|pt[i+2]<<16)-0xCF264F
    n=w(cfg,p); p+=2
    for k in range(n):
      y,x,t,off=cfg[p],cfg[p+1],cfg[p+2],w(cfg,p+3); p+=5
      X,Y=sx*256+x*8, sy*256+y*8
      s=f"door ({X},{Y}) type {t} off {off}"
      if t==2:
        dx=w(dd,off+8)*8; dy=(w(dd,off+6)&0x3FFF)*8; s+=f" -> {dx},{dy}"
      print(s)
