#!/usr/bin/env python3
"""Write deterministic PCM metrics and a dependency-free spectrum PNG for a mono 48 kHz WAV."""
import argparse, cmath, json, math, struct, wave, zlib
from pathlib import Path

def fft(values):
    n=len(values); j=0
    for i in range(1,n):
        bit=n>>1
        while j&bit:j^=bit;bit>>=1
        j^=bit
        if i<j:values[i],values[j]=values[j],values[i]
    size=2
    while size<=n:
        root=cmath.exp(-2j*math.pi/size)
        for start in range(0,n,size):
            w=1+0j;half=size//2
            for k in range(half):
                a=values[start+k];b=values[start+k+half]*w
                values[start+k]=a+b;values[start+k+half]=a-b;w*=root
        size*=2

def png(path,width,height,pixels):
    raw=b''.join(b'\0'+bytes(pixels[y*width:(y+1)*width]) for y in range(height))
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,0,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b'')
    path.write_bytes(data)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('wav');ap.add_argument('json');ap.add_argument('png');args=ap.parse_args()
    with wave.open(args.wav,'rb') as source:
        if (source.getnchannels(),source.getsampwidth(),source.getframerate())!=(1,2,48000):raise SystemExit('expected mono signed 16-bit 48 kHz WAV')
        frames=source.getnframes();samples=list(struct.unpack('<%dh'%frames,source.readframes(frames)))
    if not samples:raise SystemExit('empty WAV')
    n=1
    while n<len(samples):n*=2
    values=[]
    for i in range(n):
        sample=samples[i] if i<len(samples) else 0
        window=0.5-0.5*math.cos(2*math.pi*i/(n-1))
        values.append(complex(sample*window,0))
    fft(values);magnitudes=[abs(v) for v in values[:n//2]]
    peak_bin=max(range(1,len(magnitudes)),key=magnitudes.__getitem__)
    reps=(60,120,440,880,1720,3500,8000,12000)
    metrics={'sample_rate_hz':48000,'channels':1,'format':'signed-int16','profile':1,
      'samples':len(samples),'duration_seconds':len(samples)/48000,'dc_mean':sum(samples)/len(samples),
      'peak':max(abs(v) for v in samples),'rms':math.sqrt(sum(v*v for v in samples)/len(samples)),
      'clip_count':sum(abs(v)==32768 or v==32767 for v in samples),
      'dominant_frequency_hz':peak_bin*48000/n,
      'representative_magnitude':{str(f):magnitudes[round(f*n/48000)] for f in reps}}
    Path(args.json).write_text(json.dumps(metrics,indent=2,sort_keys=True)+'\n')
    width,height=960,480;pix=bytearray([255])*(width*height)
    ceiling=max(1.0,max(magnitudes[1:round(12000*n/48000)+1]));previous=None
    for x in range(width):
        freq=12000*x/(width-1);idx=min(len(magnitudes)-1,round(freq*n/48000))
        level=20*math.log10(max(magnitudes[idx]/ceiling,1e-6));y=height-1-round((level+120)*((height-1)/120))
        y=max(0,min(height-1,y))
        if previous is not None:
            x0,y0=previous
            for step in range(abs(y-y0)+1):
                yy=y0+(y-y0)*step//max(1,abs(y-y0));pix[yy*width+x]=0
        pix[y*width+x]=0;previous=(x,y)
    png(Path(args.png),width,height,pix)
if __name__=='__main__':main()
