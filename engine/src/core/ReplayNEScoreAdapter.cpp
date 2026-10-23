#include "core/ReplayNEScoreAdapter.h"
#include "core/ReplayNEScoreDisplayPalette.h"
#include "replaynes/nrom_cpu_ppu.hpp"
#include "replaynes/nrom_cpu_ppu_snapshot.hpp"
#include "util/Bytes.h"
#include <array>
#include <algorithm>
#include <bit>
#include <iomanip>
#include <sstream>
#include <limits>
#include <stdexcept>

namespace rn {
namespace {
using namespace replaynes;
ppu::CpuPortState initialPort() {
  ppu::CpuPortState s{};
  s.version = ppu::CpuPort::kVersion;
  s.registers.version = ppu::RegisterWindow::kVersion;
  s.registers.scroll.version = ppu::kScrollStateVersion;
  s.registers.oam.version = ppu::OamPort::kVersion;
  s.registers.oam.bytes.fill(0xff);
  for (unsigned i=2; i<256; i+=4) s.registers.oam.bytes[i]=0xe3;
  s.data.version = ppu::DataPort::kVersion;
  return s;
}
ppu::FramePipelineState initialFrame() {
  ppu::VisiblePipelineState p{};
  p.raster={ppu::RasterCounter::kVersion,0,false,0,0};
  p.scroll.version=ppu::kScrollStateVersion;
  p.background.version=ppu::BackgroundPipeline::kVersion;
  p.background.fetch.version=ppu::BackgroundFetch::kVersion;
  p.background.shift.version=ppu::kBackgroundShiftStateVersion;
  p.evaluation.secondary.fill(0xff);
  p.background_enabled=p.sprites_enabled=p.background_left=p.sprites_left=false;
  return {1,p};
}
uint32_t displayRgb(uint16_t code) { return kReplayNEScoreDisplayRgb[code]; }
}
struct ReplayNEScoreAdapter::Impl {
  std::shared_ptr<const replaynes::nrom::Image> image;
  std::unique_ptr<replaynes::machine::NromCpuPpu> machine;
  std::array<uint32_t, kVideoWidth*kVideoHeight> pixels{};
  std::array<uint8_t,2048> ram{};
  std::vector<int16_t> audio;
  uint64_t lastAudioSlot=0;
  int32_t dcInput=0, dcOutput=0;
  uint64_t frames=0, tick=0;
  Status fault;
  void boot() {
    const std::array<uint8_t,8192> chr{};
    machine=std::make_unique<replaynes::machine::NromCpuPpu>(
      image,replaynes::cpu::Registers{0,0,0,0,4,0},replaynes::machine::RamState{},
      image->chr().empty()?std::span<const uint8_t>(chr):std::span<const uint8_t>{},
      initialPort(),replaynes::machine::CpuPpuTiming{4,4,12},initialFrame(),
      replaynes::dma::Cadence::get,true,replaynes::apu::Phase::clock,true,true,true,true,true);
    tick=0; ram.fill(0); pixels.fill(0xff000000); audio.clear(); lastAudioSlot=0;
    dcInput=dcOutput=0; fault=Status::Ok();
  }
  Status fail(const replaynes::time::RunResult& result) {
    std::ostringstream out;
    out<<"ReplayNEScore stopped at master tick "<<tick;
    const auto state=machine->diagnostic_cpu_state();
    if(state) {
      out<<" PC=$"<<std::hex<<std::uppercase<<std::setw(4)<<std::setfill('0')<<state->registers.pc;
      replaynes::cpu::Cpu probe(state->registers);
      if(probe.restore(*state)) {
        bool ready=true;
        if(state->phase==replaynes::cpu::Phase::ready) ready=probe.begin_instruction();
        const auto access=ready?probe.prepare():std::nullopt;
        if(access) out<<" CPU "<<(access->direction==replaynes::cartridge::Direction::write?"write":"read")
          <<" $"<<std::setw(4)<<access->address<<" data=$"<<std::setw(2)<<unsigned(access->write_data);
      }
    }
    if(result.error) out<<": "<<result.error->message;
    fault=Error(Err::UnsupportedFormat,out.str());
    return fault;
  }
};
ReplayNEScoreAdapter::ReplayNEScoreAdapter():impl_(std::make_unique<Impl>()) {}
ReplayNEScoreAdapter::~ReplayNEScoreAdapter()=default;
Status ReplayNEScoreAdapter::loadROM(const uint8_t* data,size_t size) {
  if(!data || size==0) return Error(Err::InvalidArg,"ROM bytes are required");
  try {
    auto loaded=replaynes::nrom::Image::load({data,size});
    if(!loaded) return Error(Err::RomInvalid,"ReplayNEScore requires a supported mapper-0 NROM image");
    auto candidate=std::make_unique<Impl>(); candidate->image=std::move(loaded.image); candidate->boot();
    impl_=std::move(candidate); return Status::Ok();
  } catch(const std::exception& e) { return Error(Err::RomInvalid,e.what()); }
}
Status ReplayNEScoreAdapter::powerCycle() {
  if(!impl_->machine) return Error(Err::StateError,"No ROM loaded");
  try { impl_->boot(); return Status::Ok(); }
  catch(const std::exception& e) {return Error(Err::Internal,e.what());}
}
Status ReplayNEScoreAdapter::stepFrame(uint8_t p1,uint8_t p2,bool renderVideo) {
  auto& s=*impl_;
  if(!s.machine) return Error(Err::StateError,"No ROM loaded");
  if(!s.fault) return s.fault;
  if(!s.machine->set_buttons(p1,p2)) return Error(Err::Internal,"Pad update rejected");
  auto pending=s.pixels;
  std::vector<int16_t> cpuAudio;
  // One raster traversal is at most 262*341 dots, including the no-skip case.
  for(unsigned dots=0;dots<262U*341U;++dots) {
    if(s.tick>std::numeric_limits<uint64_t>::max()-4) return Error(Err::OutOfRange,"Master tick overflow");
    s.tick+=4;
    const auto result=s.machine->run_until(s.tick);
    if(!result.ok) return s.fail(result);
    const auto audioObservation=s.machine->last_cpu_audio();
    if(audioObservation && audioObservation->cpu_slots!=s.lastAudioSlot) {
      if(audioObservation->cpu_slots<=s.lastAudioSlot)
        return Error(Err::Internal,"ReplayNEScore audio slot did not advance");
      s.lastAudioSlot=audioObservation->cpu_slots;
      cpuAudio.push_back(audioObservation->pcm);
    }
    const auto dot=s.machine->last_render_dot();
    if(!dot || dot->tick!=s.tick) continue;
    if(dot->video_code) {
      if(renderVideo) {
        const auto& pos=dot->plan.position;
        if(pos.scanline>=240 || pos.dot<1 || pos.dot>256) return Error(Err::Internal,"Visible dot out of range");
        pending[size_t(pos.scanline)*256+pos.dot-1]=displayRgb(*dot->video_code);
      }
    }
    if(dot->plan.raster_event.frame_end) {
      const auto state=s.machine->export_state();
      if(!state) return Error(Err::Internal,"Frame boundary state unavailable");
      s.ram=state->bus.ram.cpu;
      if(renderVideo) s.pixels=std::move(pending);
      const auto count=audioSamplesForFrame(s.frames);
      if(cpuAudio.size()<count || count==0) return Error(Err::Internal,"ReplayNEScore audio input is incomplete");
      s.audio.assign(count,0);
      for(size_t i=0;i<count;++i) {
        const size_t begin=i*cpuAudio.size()/count;
        const size_t end=(i+1)*cpuAudio.size()/count;
        if(end<=begin) return Error(Err::Internal,"ReplayNEScore audio resample interval is empty");
        int64_t sum=0;
        for(size_t j=begin;j<end;++j) sum+=cpuAudio[j];
        const int32_t input=int32_t((sum+int64_t(end-begin)/2)/int64_t(end-begin));
        // Fixed-point one-pole DC blocker, R=65372/65536. State is part of this live adapter profile.
        int64_t filtered=int64_t(input)-s.dcInput+(int64_t(65372)*s.dcOutput)/65536;
        filtered=std::clamp<int64_t>(filtered,-32768,32767);
        s.dcInput=input; s.dcOutput=int32_t(filtered); s.audio[i]=int16_t(filtered);
      }
      ++s.frames; return Status::Ok();
    }
  }
  s.fault=Error(Err::Internal,"ReplayNEScore raster frame deadline exceeded"); return s.fault;
}
Status ReplayNEScoreAdapter::softReset() {
  auto& s=*impl_;
  if(!s.machine) return Error(Err::StateError,"No ROM loaded");
  if(!s.fault) return s.fault;
  if(!s.machine->set_reset_line(true))
    return Error(Err::Internal,"ReplayNESCore RESET assertion was rejected");
  const auto result=s.machine->run_cycles(7);
  s.tick=result.last_committed;
  if(!result.ok) return s.fail(result);
  if(!s.machine->set_reset_line(false)) {
    s.fault=Error(Err::Internal,"ReplayNESCore RESET release was rejected");
    return s.fault;
  }
  const auto state=s.machine->export_state();
  if(!state) {
    s.fault=Error(Err::Internal,"ReplayNESCore RESET state is unavailable");
    return s.fault;
  }
  s.tick=state->scheduler.last_committed;
  s.lastAudioSlot=state->cpu_cycles;
  s.ram=state->bus.ram.cpu;
  s.audio.clear();
  return Status::Ok();
}
Status ReplayNEScoreAdapter::saveState(std::vector<uint8_t>& out) {
  const auto& s=*impl_;
  if(!s.machine) return Error(Err::StateError,"No ROM loaded");
  if(!s.fault) return Error(Err::StateError,"Cannot save a faulted ReplayNEScore instance");
  const auto core=replaynes::state::save_nrom_cpu_ppu(*s.machine);
  if(!core || !core.value) return Error(Err::StateError,"ReplayNEScore machine snapshot failed");
  ByteWriter payload;
  payload.u32(0x31484e52U); payload.u32(1U); // RNH1, adapter payload schema 1.
  payload.u64(s.tick); payload.u64(s.lastAudioSlot);
  payload.u32(static_cast<uint32_t>(s.dcInput)); payload.u32(static_cast<uint32_t>(s.dcOutput));
  payload.u32(uint32_t(s.pixels.size()));
  for(uint32_t pixel:s.pixels) payload.u32(pixel);
  payload.u64(core.value->size()); payload.bytes(core.value->data(),core.value->size());
  std::vector<uint8_t> encoded;
  stateenv::write(encoded,compatId(),s.frames,payload.buf.data(),payload.buf.size());
  out.swap(encoded); return Status::Ok();
}
Status ReplayNEScoreAdapter::loadState(const uint8_t* data,size_t size) {
  auto& current=*impl_;
  if(!current.machine) return Error(Err::StateError,"No ROM loaded");
  uint64_t frame=0; const uint8_t* payload=nullptr; size_t payloadLen=0;
  Status envelope=stateenv::read(data,size,compatId(),frame,payload,payloadLen);
  if(!envelope.ok()) return envelope;
  try {
    ByteReader reader(payload,payloadLen);
    if(reader.u32()!=0x31484e52U || reader.u32()!=1U)
      return Error(Err::StateError,"Unsupported ReplayNEScore adapter state schema");
    const uint64_t tick=reader.u64(),lastSlot=reader.u64();
    const int32_t dcInput=std::bit_cast<int32_t>(reader.u32());
    const int32_t dcOutput=std::bit_cast<int32_t>(reader.u32());
    if(reader.u32()!=kVideoWidth*kVideoHeight)
      return Error(Err::StateError,"ReplayNEScore state has invalid video size");
    std::array<uint32_t,kVideoWidth*kVideoHeight> pixels{};
    for(auto& pixel:pixels) pixel=reader.u32();
    const uint64_t coreLen64=reader.u64();
    if(coreLen64>64U*1024U*1024U || coreLen64>reader.remaining())
      return Error(Err::StateError,"ReplayNEScore state has invalid machine length");
    const auto coreLen=size_t(coreLen64); const uint8_t* coreBytes=reader.ptr(coreLen);
    if(!reader.ok() || reader.remaining()!=0)
      return Error(Err::StateError,"ReplayNEScore state is truncated or has trailing bytes");
    auto candidate=std::make_unique<Impl>(); candidate->image=current.image; candidate->boot();
    const auto loaded=replaynes::state::load_nrom_cpu_ppu(*candidate->machine,{coreBytes,coreLen});
    if(!loaded) return Error(Err::StateError,"ReplayNEScore machine state rejected");
    const auto state=candidate->machine->export_state();
    if(!state || tick!=state->scheduler.last_committed || lastSlot!=state->cpu_cycles)
      return Error(Err::StateError,"ReplayNEScore host and machine state disagree");
    candidate->frames=frame; candidate->tick=tick; candidate->lastAudioSlot=lastSlot;
    candidate->dcInput=dcInput; candidate->dcOutput=dcOutput; candidate->pixels=pixels;
    candidate->ram=state->bus.ram.cpu; candidate->audio.clear();
    impl_=std::move(candidate); return Status::Ok();
  } catch(const std::exception& e) { return Error(Err::StateError,e.what()); }
}
uint64_t ReplayNEScoreAdapter::frameIndex()const{return impl_->frames;}
const uint32_t* ReplayNEScoreAdapter::video()const{return impl_->pixels.data();}
const uint8_t* ReplayNEScoreAdapter::cpuRam()const{return impl_->machine?impl_->ram.data():nullptr;}
const int16_t* ReplayNEScoreAdapter::audio(size_t* count)const{
  if(count)*count=impl_->audio.size();
  return impl_->audio.empty()?nullptr:impl_->audio.data();
}
} // namespace rn
