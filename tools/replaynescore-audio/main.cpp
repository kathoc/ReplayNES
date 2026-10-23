#include "core/ReplayNEScoreAdapter.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void u16(std::ostream& out,uint16_t value){for(unsigned i=0;i<2;++i)out.put(char(value>>(8*i)));}
void u32(std::ostream& out,uint32_t value){for(unsigned i=0;i<4;++i)out.put(char(value>>(8*i)));}
std::vector<uint8_t> pulseRom(){
  std::vector<uint8_t> out(16+16384+8192,0);
  out[0]='N';out[1]='E';out[2]='S';out[3]=0x1a;out[4]=1;out[5]=1;
  const std::vector<uint8_t> code{0xa9,1,0x8d,0x15,0x40,0xa9,0x9f,0x8d,0x00,0x40,
    0xa9,0,0x8d,0x02,0x40,0xa9,9,0x8d,0x03,0x40,0x4c,0x14,0x80};
  std::copy(code.begin(),code.end(),out.begin()+16);
  out[16+0x3ffc]=0;out[16+0x3ffd]=0x80;out[16+0x3ffa]=0;out[16+0x3ffb]=0x80;
  return out;
}
void wav(const std::string& path,const std::vector<int16_t>& samples){
  if(samples.size()>UINT32_MAX/2U)throw std::runtime_error("WAV is too large");
  std::ofstream out(path,std::ios::binary);if(!out)throw std::runtime_error("cannot open WAV output");
  const uint32_t bytes=uint32_t(samples.size()*2U);
  out.write("RIFF",4);u32(out,36U+bytes);out.write("WAVEfmt ",8);u32(out,16);u16(out,1);u16(out,1);
  u32(out,48000);u32(out,96000);u16(out,2);u16(out,16);out.write("data",4);u32(out,bytes);
  for(const int16_t sample:samples)u16(out,uint16_t(sample));
  if(!out)throw std::runtime_error("WAV write failed");
}
}
int main(int argc,char** argv){
  if(argc<2||argc>3){std::cerr<<"usage: replaynescore-audio-capture OUTPUT.wav [frames]\n";return 2;}
  try{
    const unsigned frames=argc==3?unsigned(std::stoul(argv[2])):12U;
    if(frames==0||frames>600U)throw std::runtime_error("frames must be 1..600");
    const auto rom=pulseRom();rn::ReplayNEScoreAdapter core;
    auto status=core.loadROM(rom.data(),rom.size());if(!status)throw std::runtime_error(status.message);
    std::vector<int16_t> samples;
    for(unsigned frame=0;frame<frames;++frame){
      status=core.stepFrame(0,0,false);if(!status)throw std::runtime_error(status.message);
      size_t count=0;const int16_t* audio=core.audio(&count);
      if(!audio||count!=rn::audioSamplesForFrame(frame))throw std::runtime_error("frame audio count mismatch");
      samples.insert(samples.end(),audio,audio+count);
    }
    wav(argv[1],samples);
    std::cout<<"frames="<<frames<<" samples="<<samples.size()<<" rate=48000 profile=1\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
