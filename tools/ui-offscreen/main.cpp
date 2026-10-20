// Bounded screenshot/interaction check: production UI::build, SDL software draw backend.
// Does not validate the platform Vulkan/D3D presenter or CRT. SPDX-License-Identifier: GPL-2.0-or-later
#include "app_model.h"
#include "fonts.h"
#include "input_router.h"
#include "png_writer.h"
#include "renderer.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui.h"
#include "imgui.h"
#include "render/crt_export.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace rnl;
namespace fs = std::filesystem;
namespace rnl {
FontFile findLatinFont() { return {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",0}; }
FontFile findJapaneseFont() { return {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",0}; }
FontFile findSymbolFont() { return {}; }
const std::vector<ExportEncoderChoice>& exportEncoderChoices() { static std::vector<ExportEncoderChoice> v; return v; }
std::string availableEncodersDescription() { return "Unavailable in UI test"; }
bool exportProject(rn_renderer* r, const ExportOptions&, const std::string&, const ExportProgressFn&, const ExportCancelFn&, ExportResult*, std::string* error) {
  rn_renderer_free(r); *error="Export unavailable in UI test"; return false;
}
std::function<std::unique_ptr<ExportVideoProcessor>(std::string*)> crtExportProcessorFactory(const CrtSettings&) { return {}; }
}
struct SoftwareRenderer : Renderer {
  SDL_Surface* surface=SDL_CreateSurface(1280,800,SDL_PIXELFORMAT_RGBA32);
  SDL_Renderer* renderer=SDL_CreateSoftwareRenderer(surface);
  DisplayPostProcess pp;
  SDL_Texture* game=nullptr;
  bool init(SDL_Window*, std::string*) override { return renderer; }
  bool initImGui() override { return true; }
  void shutdown() override {}
  void newImGuiFrame() override {}
  bool presentTiming() const override { return false; }
  std::string description() const override { return "offscreen SDL software"; }
  int width() const override { return 1280; } int height() const override { return 800; }
  std::vector<PresentDone> takeCompleted() override { return {}; }
  double lastAcquireWait() const override { return 0; }
  void setPostProcess(const DisplayPostProcess& v) override { pp=v; }
  const DisplayPostProcess& postProcess() const override { return pp; }
  PostProcessStatus postProcessStatus() const override { return {}; }
  void beginUIFrame() override {}
  bool thumbTexture(uint64_t,const uint32_t*,uint64_t*,float[4]) override { return false; }
  void requestScreenshot(const std::string& path) override {
    SDL_FlushRenderer(renderer);
    if(!writePNG(path,width(),height(),static_cast<uint8_t*>(surface->pixels))) throw std::runtime_error("PNG write failed");
  }
  uint64_t drawAndPresent(const uint32_t* picture,const GameRect& rect,ImDrawData* data,const FrameSignal* =nullptr) override {
    SDL_SetRenderDrawColor(renderer,12,15,21,255); SDL_RenderClear(renderer);
    if(picture && rect.visible) {
      if(!game) game=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_BGRA32,SDL_TEXTUREACCESS_STATIC,RN_VIDEO_WIDTH,RN_VIDEO_HEIGHT);
      checkSDL(SDL_UpdateTexture(game,nullptr,picture,RN_VIDEO_WIDTH*4));
      SDL_SetTextureScaleMode(game,SDL_SCALEMODE_NEAREST);
      SDL_FRect src{0,float(rect.crop),float(RN_VIDEO_WIDTH),float(RN_VIDEO_HEIGHT-rect.crop*2)};
      SDL_FRect dst{rect.x,rect.y,rect.w,rect.h};
      checkSDL(SDL_RenderTexture(renderer,game,&src,&dst));
    }
    for (auto* tex : *data->Textures) {
      if(tex->Status==ImTextureStatus_WantCreate) {
        auto* t=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,tex->Width,tex->Height);
        if(!t) throw std::runtime_error(SDL_GetError());
        SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t,SDL_SCALEMODE_LINEAR);
        tex->SetTexID(reinterpret_cast<ImTextureID>(t));
      }
      if(tex->Status==ImTextureStatus_WantCreate || tex->Status==ImTextureStatus_WantUpdates) {
        std::vector<uint8_t> rgba;
        const void* pixels=tex->GetPixels(); int pitch=tex->Width*4;
        if(tex->BytesPerPixel==1) {
          rgba.assign(size_t(tex->Width)*tex->Height*4,255);
          for(size_t i=0;i<rgba.size()/4;++i) rgba[i*4+3]=tex->Pixels[i];
          pixels=rgba.data();
        }
        if(!SDL_UpdateTexture(reinterpret_cast<SDL_Texture*>(tex->GetTexID()),nullptr,pixels,pitch)) throw std::runtime_error(SDL_GetError());
        tex->SetStatus(ImTextureStatus_OK);
      }
      if(tex->Status==ImTextureStatus_WantDestroy) {
        SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(tex->GetTexID())); tex->SetTexID(0); tex->SetStatus(ImTextureStatus_Destroyed);
      }
    }
    for(auto* list : data->CmdLists) {
      std::vector<SDL_Vertex> vertices;
      for(const auto& v:list->VtxBuffer) vertices.push_back({{v.pos.x,v.pos.y},{float(v.col&255)/255,float((v.col>>8)&255)/255,float((v.col>>16)&255)/255,float(v.col>>24)/255},{v.uv.x,v.uv.y}});
      for(const auto& cmd:list->CmdBuffer) {
        if(cmd.UserCallback) { if(cmd.UserCallback!=ImDrawCallback_ResetRenderState) cmd.UserCallback(list,&cmd); continue; }
        SDL_Rect clip{int(cmd.ClipRect.x),int(cmd.ClipRect.y),int(cmd.ClipRect.z-cmd.ClipRect.x),int(cmd.ClipRect.w-cmd.ClipRect.y)};
        SDL_SetRenderClipRect(renderer,&clip);
        std::vector<int> indices;
        for(unsigned i=0;i<cmd.ElemCount;++i) indices.push_back(int(list->IdxBuffer[cmd.IdxOffset+i]+cmd.VtxOffset));
        if(!SDL_RenderGeometry(renderer,reinterpret_cast<SDL_Texture*>(cmd.GetTexID()),vertices.data(),int(vertices.size()),indices.data(),int(indices.size()))) throw std::runtime_error(SDL_GetError());
      }
    }
    SDL_SetRenderClipRect(renderer,nullptr); SDL_RenderPresent(renderer); return 1;
  }
  static void checkSDL(bool ok) { if(!ok) throw std::runtime_error(SDL_GetError()); }
  ~SoftwareRenderer() { SDL_DestroyTexture(game); SDL_DestroyRenderer(renderer); SDL_DestroySurface(surface); }
};
struct CountingAudio : AudioSink { size_t samples=0; void setMuted(bool) override {} void push(const int16_t*,size_t n) override { samples+=n; } };
static void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main(int argc,char** argv) {
  if(argc!=2 && argc!=3) return 2;
  try {
    const fs::path out=argv[1]; fs::create_directories(out);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy"); check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD),SDL_GetError());
    SDL_Window* window=SDL_CreateWindow("UI evidence",1280,800,SDL_WINDOW_HIDDEN);
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.DisplaySize={1280,800}; io.DeltaTime=1.f/60; io.IniFilename=nullptr;
    io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures|ImGuiBackendFlags_RendererHasVtxOffset;
    rnf_l10n_set_language("en");
    {
      Settings settings; InputRouter input((out/"bindings.json").string(),&settings); input.load();
      CountingAudio audio; EmulationController emu(input.input(),&audio); ThumbnailManager thumbs; thumbs.enabled=false;
      Paths paths; paths.setLibraryRoot((out/"library").string()); paths.sessionRoot=(out/"session").string(); paths.configDir=(out/"config").string(); paths.ensure();
      LibraryModel library(paths.libraryRoot); library.ensureFolders(); SoftwareRenderer renderer; check(renderer.renderer,SDL_GetError());
      UI ui({nullptr,&emu,&library,&input,&thumbs,&renderer,&settings,window});
      AppModel app(paths,&emu,&library,&ui); ui.setApp(&app);
      emu.onNotice=[&](const std::string& text) { ui.notice(text); };
      emu.onError=[&](const std::string& title,const std::string& text) {
        std::fprintf(stderr,"%s: %s\n",title.c_str(),text.c_str());
        Dialog dialog; dialog.title=title; dialog.message=text; dialog.buttons={"OK"}; ui.showDialog(std::move(dialog));
      };
      ui.onSettingsChanged=[&] { app.selectedCore=settings.core; settings.save(paths.settingsFile()); };
      ui.loadFonts(false); ui.updateScale(1280,800); check(ui.openPage("settings.system"),"system page missing");
      int frame=0;
      auto draw=[&] { ImGui::NewFrame(); ui.build(double(++frame)/60); ImGui::Render(); renderer.drawAndPresent(emu.session()?emu.picture():nullptr,ui.gameRect(1280,800),ImGui::GetDrawData()); };
      draw(); draw(); renderer.requestScreenshot((out/"core-nestopia.png").string());
      const bool coreBuilt=rn_core_available(RN_CORE_REPLAYNESCORE)!=0;
      // Without the second core the System page has no Core row (core-nestopia.png; tests/test_frontend_menu.cpp).
      if(coreBuilt) {
      // Production keyboard navigation reaches the focused CHOICE row and invokes adjustItem.
      io.AddKeyEvent(ImGuiKey_RightArrow,true); draw(); io.AddKeyEvent(ImGuiKey_RightArrow,false); draw();
      check(settings.core=="replaynescore","UI key did not select ReplayNEScore");
      check(Settings::parse([&] { std::ifstream f(paths.settingsFile()); return std::string(std::istreambuf_iterator<char>(f),{}); }()).core=="replaynescore","core preference not saved");
      renderer.requestScreenshot((out/"core-replaynescore.png").string());
      }
      if(coreBuilt) {
        std::vector<uint8_t> rom(16+16384+8192); rom[0]='N';rom[1]='E';rom[2]='S';rom[3]=26;rom[4]=1;rom[5]=1;
        rom[16]=0x78;rom[17]=0x4c;rom[18]=1;rom[19]=0x80;
        for(int i=0;i<3;++i) rom[16+0x3ffb+i*2]=0x80;
        auto path=out/"live-test.nes"; { std::ofstream f(path,std::ios::binary); f.write(reinterpret_cast<char*>(rom.data()),rom.size()); }
        app.tryRom(path.string()); check(emu.live(),"Live core did not start"); ui.setMenu(false); emu.setPaused(false);
        emu.tick(); check(rn_frame(emu.session())==1,"Live frame failed");
        check(audio.samples>0,"Live core PCM was not delivered to the audio sink"); draw(); draw(); renderer.requestScreenshot((out/"core-live.png").string());
        check(ui.openPage("game"),"game page missing"); draw(); draw(); renderer.requestScreenshot((out/"core-live-menu.png").string());
        app.closeProject(); draw();
        if(argc==3) {
          app.tryRom(argv[2]); check(emu.live(),"Owned ROM failed to load"); draw(); ui.setMenu(false); emu.setPaused(false);
          for(int i=0;i<120 && !emu.paused();++i) emu.tick();
          draw(); draw(); renderer.requestScreenshot((out/"core-rom-stop.png").string());
          std::printf("Owned ROM: frame=%llu paused=%d\n", static_cast<unsigned long long>(rn_frame(emu.session())), emu.paused());
          ui.answerDialog(0); app.closeProject(); draw();
        }
      }
      check(ui.openPage("settings.system"),"system page missing"); draw();
      if(coreBuilt) { io.AddKeyEvent(ImGuiKey_LeftArrow,true); draw(); io.AddKeyEvent(ImGuiKey_LeftArrow,false); draw(); }
      check(settings.core=="nestopia","UI key did not switch back");
      rnf_l10n_set_language("ja"); ui.loadFonts(true); ui.updateScale(1280,800); draw(); draw();
      renderer.requestScreenshot((out/"core-japanese.png").string());
      std::printf("PASS: real UI choice keys, preference persistence, screenshots; experimental core available=%d\n", rn_core_available(RN_CORE_REPLAYNESCORE));
    }
    ImGui::DestroyContext(); SDL_DestroyWindow(window); SDL_Quit(); return 0;
  } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
