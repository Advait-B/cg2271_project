#!/bin/bash
# Double-click: resets the ESP32 and records 30 s of its serial output to esp32_serial.log
LOG=~/Desktop/Y2S1/CG2271/Project/logs/esp32_serial.log
PORT=$(ls /dev/cu.SLAB_USBtoUART /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -1)
SECS=${1:-120}
python3 - "$PORT" "$LOG" "$SECS" <<'PY'
import sys,os,termios,fcntl,struct,time,select
port,log,secs=sys.argv[1],sys.argv[2],int(sys.argv[3])
fd=os.open(port,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
a=termios.tcgetattr(fd)
a[0]=0; a[1]=0; a[2]=termios.CS8|termios.CREAD|termios.CLOCAL; a[3]=0
a[4]=a[5]=termios.B115200
termios.tcsetattr(fd,termios.TCSANOW,a)
TIOCMSET=0x8004746d; DTR=0x002; RTS=0x004
def m(v): fcntl.ioctl(fd,TIOCMSET,struct.pack('I',v))
m(DTR); time.sleep(0.1); m(RTS); time.sleep(0.1); m(0)   # reset like esptool
out=open(log,'w'); hdr=f"=== {time.ctime()} port={port} ===\n"; out.write(hdr); print(hdr,end='')
end=time.time()+secs
while time.time()<end:
    r,_,_=select.select([fd],[],[],0.5)
    if r:
        try: d=os.read(fd,4096)
        except BlockingIOError: continue
        s=d.decode('utf-8','replace'); out.write(s); out.flush(); print(s,end='',flush=True)
out.write("\n=== end ===\n"); out.close(); os.close(fd)
PY
echo; echo "Saved to $LOG. You can close this window."
