// "Make Video" (docs/design/VTR_UX.md: one screen - Range, Quality, Length · approx. size, Details ›
// with the existing settings; MP4 via export/mp4_export.h: H.264 + AAC on a worker thread - FFmpeg on
// Linux, Media Foundation on Windows - the project is never touched and play continues), the
// "Video saved" toast (Open / Share) and the CRT display parameters.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "replaynes/product.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "app_model.h"
#include "emulation.h"
#include "imgui_internal.h"
#include "l10n.h"
#include "library.h"
#include "paths.h"
#include "platform/platform.h"
#include "render/crt_export.h"
#include "settings.h"
#include "ui.h"
#include "ui_theme.h"

namespace fs = std::filesystem;

namespace rnl {

using namespace theme;

namespace {
std::string str(char* p) {
  std::string s = p ? p : "";
  rnf_string_free(p);
  return s;
}
}  // namespace

CrtSettings UI::crtSettings() const {
  const Settings& s = *d_.settings;
  CrtSettings c;
  c.lines = CrtSettings::validLines(s.crtLines) ? s.crtLines : 240;
  c.beamGrowth = s.crtBeamGrowth;
  c.persistence = s.crtPersistence;
  c.supply = s.crtSupply;
  c.antennaDbuv = s.crtAntenna;
  return c.sanitized();
}

// ------------------------------------------------------------------ export

void UI::openExportDialog() {
  if (d_.emu->live()) { notice(TR("Development core: video export is unavailable")); return; }
  const EmuStatus& st = d_.emu->status();
  if (!st.hasSession) return;
  exportDialog_ = true;
  exportOpened_ = false;
  if (!exportJob_.running()) exportStarted_ = false;
  exportError_.clear();
  exportApplyFlash_ = d_.settings->flash != RN_FLASH_OFF;
  exportApplyCRT_ = d_.settings->crt && d_.emu->crtAvailable();
  exportPar87_ = d_.settings->par87;
  videoCursor_ = st.frame;
  videoRange_ = RNF_VIDEO_RANGE_ALL;
  videoSection_ = -1;
  exportDetails_ = false;
  if (exportPreset_ < 0) {  // first time: the macOS default canvas (1280x960)
    rnf_export_settings def = defaultExportSettings();
    exportPreset_ = 0;
    for (size_t i = 0, n = rnf_export_preset_count(); i < n; ++i) {
      rnf_export_preset p{};
      if (rnf_export_preset_get(i, &p) && p.kind == def.preset.kind && p.width == def.preset.width &&
          p.height == def.preset.height && p.scale == def.preset.scale)
        exportPreset_ = int(i);
    }
  }
  std::string base = !st.projectPath.empty() && !d_.app->isTempSession() ? fs::path(st.projectPath).stem().string()
                                                                         : fs::path(st.romPath).stem().string();
  std::snprintf(exportName_, sizeof exportName_, "%s", base.c_str());
}

namespace {
/// The chosen range as cursor frames [start, end); false when it is empty.
bool videoFrames(EmulationController* emu, rnf_video_range_kind kind, int slot, uint64_t cursor, uint64_t* start, uint64_t* end) {
  const EmuStatus& st = emu->status();
  rnf_timeline_range section{};
  bool has = false;
  if (kind == RNF_VIDEO_RANGE_SECTION)
    for (const rnf_timeline_range& r : emu->visibleRanges())
      if (r.slot == slot) {
        section = r;
        has = true;
      }
  return rnf_video_range(kind, std::min(cursor, st.takeLength), st.takeLength, has ? &section : nullptr, start, end) != 0;
}
}  // namespace

void UI::startExport() {
  const EmuStatus& st = d_.emu->status();
  rnf_export_settings s = defaultExportSettings();
  rnf_export_preset p{};
  if (rnf_export_preset_get(size_t(exportPreset_), &p)) s.preset = p;
  int crop = exportCropOverscan_ ? 8 : 0;
  s.crop_top = s.crop_bottom = s.crop_left = s.crop_right = crop;
  s.pixel_aspect_87 = exportPar87_;
  uint64_t a = 0, b = 0;
  if (!videoFrames(d_.emu, videoRange_, videoSection_, videoCursor_, &a, &b)) {
    exportError_ = TR("Nothing has been recorded yet");
    return;
  }
  // The whole tape: "to the end" (it may still grow while the dialog is open).
  bool all = videoRange_ == RNF_VIDEO_RANGE_ALL || (a == 0 && b >= st.takeLength);
  s.start_frame = all ? 0 : a;
  s.end_frame = all ? 0 : b;
  char* msg = nullptr;
  if (rnf_export_validate_media(&s, &d_.emu->media(), d_.emu->videoFrame().width, d_.emu->videoFrame().height, &msg) != RN_OK) {
    exportError_ = str(msg);
    return;
  }
  rnf_string_free(msg);
  ExportOptions eo;
  eo.settings = s;
  rn_flash_level level = d_.settings->flash == RN_FLASH_OFF ? RN_FLASH_STANDARD : rn_flash_level(d_.settings->flash);
  eo.flash = exportApplyFlash_ ? level : RN_FLASH_OFF;
  const auto& encoders = exportEncoderChoices();
  eo.quality = exportQuality_;
  if (exportEncoder_ > 0 && size_t(exportEncoder_) <= encoders.size()) eo.encoder = encoders[size_t(exportEncoder_ - 1)].id;
  if (exportApplyCRT_ && d_.emu->crtAvailable()) eo.makeProcessor = crtExportProcessorFactory(crtSettings());
  std::string dir = d_.library->root() + "/Exports";
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::string name = chooserProjectFileName(exportName_);  // same rules; RN_PRODUCT_PROJECT_EXT swapped for ".mp4"
  if (name.empty()) name = "export" RN_PRODUCT_PROJECT_EXT;
  name = name.substr(0, name.size() - (sizeof(RN_PRODUCT_PROJECT_EXT) - 1));
  std::string path = dir + "/" + name + ".mp4";
  for (int n = 2; fs::exists(path, ec); ++n) path = dir + "/" + name + " " + std::to_string(n) + ".mp4";
  if (!exportJob_.start(d_.emu->session(), eo, path)) {
    exportError_ = exportJob_.error();
    return;
  }
  exportStarted_ = true;
  exportError_.clear();
}

void UI::buildExportDialog() {
  if (!exportDialog_ || !dialogs_.empty()) return;
  ImGuiIO& io = ImGui::GetIO();
  if (!exportOpened_) {
    ImGui::OpenPopup("##export");
    exportOpened_ = true;
  }
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(S(980), io.DisplaySize.x * 0.95f), 0));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x, io.DisplaySize.y * 0.94f));
  bool close = false;
  if (ImGui::BeginPopupModal("##export", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushFont(nullptr, metrics_.title());
    ImGui::TextUnformatted(TR("Make Video"));
    ImGui::PopFont();
    bool appearing = ImGui::IsWindowAppearing();
    if (exportStarted_) {
      // Progress / result. A finished video closes the dialog: the "Video saved" toast (Open / Share).
      ImGui::TextUnformatted(Paths::display(exportJob_.outPath()).c_str());
      if (!exportJob_.finished()) {
        uint64_t done = exportJob_.framesDone(), total = exportJob_.framesTotal();
        ImGui::ProgressBar(float(exportJob_.progress()), ImVec2(S(820), S(28)));
        ImGui::TextDisabled("%s", TRF("%llu / %llu frames", {(unsigned long long)done, (unsigned long long)total}).c_str());
        if (appearing) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(TR("Run in Background"))) close = true;
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"))) exportJob_.cancel();
      } else if (exportJob_.succeeded()) {
        close = true;  // buildExportProgressPill shows the toast
      } else {
        ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s",
                           exportJob_.wasCancelled() ? TR("Export was cancelled") : exportJob_.error().c_str());
        if (appearing) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(TR("Close"))) close = true;
      }
    } else {
      // One screen: Range, Quality, Length · approx. size, Details › (the existing settings), the buttons.
      const EmuStatus& st = d_.emu->status();
      rnf_export_settings s = defaultExportSettings();
      rnf_export_preset p{};
      if (rnf_export_preset_get(size_t(exportPreset_), &p)) s.preset = p;
      int crop = exportCropOverscan_ ? 8 : 0;
      s.crop_top = s.crop_bottom = s.crop_left = s.crop_right = crop;
      s.pixel_aspect_87 = exportPar87_;
      rnf_export_geometry g{};
      rnf_export_geometry_compute_media(&s, &d_.emu->media(), d_.emu->videoFrame().width, d_.emu->videoFrame().height, &g);
      // The body scrolls (focus-driven) only if Details ever outgrows the screen; the buttons stay.
      float maxBody = io.DisplaySize.y * 0.94f - S(40) - S(100);
      ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxBody));
      ImGui::BeginChild("##exportbody", ImVec2(S(900), 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                        ImGuiWindowFlags_NoBackground);
      const float labelW = S(140);
      auto rowLabel = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(labelW);
      };
      // Range: All / Up to here / Section n (each A/B slot with A and B on this tape).
      rowLabel(TR("Range"));
      if (ImGui::RadioButton(TR("All (range)"), videoRange_ == RNF_VIDEO_RANGE_ALL)) videoRange_ = RNF_VIDEO_RANGE_ALL;
      ImGui::SameLine();
      ImGui::BeginDisabled(videoCursor_ == 0);
      if (ImGui::RadioButton(TR("Up to here"), videoRange_ == RNF_VIDEO_RANGE_TO_HERE)) videoRange_ = RNF_VIDEO_RANGE_TO_HERE;
      ImGui::EndDisabled();
      for (const rnf_timeline_range& r : d_.emu->visibleRanges()) {
        if (!r.has_b || r.b <= r.a) continue;
        ImGui::SameLine();
        std::string l = TRF("Section %lld", {r.slot + 1}) + "##videosection" + std::to_string(r.slot);
        if (ImGui::RadioButton(l.c_str(), videoRange_ == RNF_VIDEO_RANGE_SECTION && videoSection_ == r.slot)) {
          videoRange_ = RNF_VIDEO_RANGE_SECTION;
          videoSection_ = r.slot;
        }
      }
      // Quality: the presets over the bit rates (rnf_export_quality).
      rowLabel(TR("Quality (video)"));
      int preset = rnf_video_quality_preset(rnf_export_quality(exportQuality_));
      for (int q = 0; q < RNF_VIDEO_QUALITY_PRESETS; ++q) {
        if (q) ImGui::SameLine();
        std::string l = std::string(TR(rnf_video_quality_label(q))) + "##videoquality" + std::to_string(q);
        if (ImGui::RadioButton(l.c_str(), preset == q)) exportQuality_ = int(rnf_video_quality(q));
      }
      // Length · approx. size (an estimate for exactly this range / canvas / codec / quality).
      uint64_t a = 0, b = 0;
      bool any = videoFrames(d_.emu, videoRange_, videoSection_, videoCursor_, &a, &b);
      uint64_t frames = any ? b - a : 0;
      rowLabel(TR("Length"));
      ImGui::TextDisabled("%s", str(rnf_video_summary_for_period(frames, rnf_export_predict_size_media(&g, 0, exportQuality_, frames, &d_.emu->media(), exportAudioSampleRate(d_.emu->session()), nullptr), d_.emu->framePeriod())).c_str());
      // Details ›: codec / size / PAR / overscan / flash / CRT (the bit rate is Quality).
      ImGui::SetNextItemOpen(exportDetails_, ImGuiCond_Always);
      exportDetails_ = ImGui::CollapsingHeader(TR("Details ›"));
      if (exportDetails_) {
        const float colW = S(430), x0 = ImGui::GetCursorPosX();
        ImGui::TextUnformatted(TR("Size"));
        for (size_t i = 0, n = rnf_export_preset_count(), col = 0; i < n; ++i) {
          rnf_export_preset p{};
          if (!rnf_export_preset_get(i, &p)) continue;
          if (col % 3) ImGui::SameLine(x0 + colW * 2 * float(col % 3) / 3.0f);
          ++col;
          std::string l = str(rnf_export_preset_label(&p)) + "##preset" + std::to_string(i);
          if (ImGui::RadioButton(l.c_str(), exportPreset_ == int(i))) exportPreset_ = int(i);
        }
        ImGui::TextUnformatted(TR("Pixel Aspect Ratio"));
        if (ImGui::RadioButton(TR("1:1 (square pixels)"), !exportPar87_)) exportPar87_ = false;
        ImGui::SameLine();
        if (ImGui::RadioButton((d_.emu->crtAvailable() ? TR("8:7 (as on a CRT TV)") : TR("Pixel Aspect Ratio")), exportPar87_)) exportPar87_ = true;
        ImGui::Checkbox(TR("Hide overscan (crop 8 px on each side)"), &exportCropOverscan_);
        ImGui::Checkbox(TR("Apply Flash Reduction"), &exportApplyFlash_);
        ImGui::BeginDisabled(!d_.renderer->postProcessStatus().crtAvailable || !d_.emu->crtAvailable());
        ImGui::SameLine();
        ImGui::Checkbox(TR("Apply CRT Effect"), &exportApplyCRT_);
        ImGui::EndDisabled();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Codec"));
        ImGui::SameLine();
        if (ImGui::RadioButton(TR("H.264 (automatic encoder)"), exportEncoder_ == 0)) exportEncoder_ = 0;
        const auto& encoders = exportEncoderChoices();
        for (int i = 0; i < int(encoders.size()); ++i) {
          ImGui::SameLine();
          if (ImGui::RadioButton(encoders[size_t(i)].label, exportEncoder_ == i + 1)) exportEncoder_ = i + 1;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Name"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(440));
        ImGui::InputText("##exportname", exportName_, sizeof exportName_);
        ImGui::SameLine();
        ImGui::TextDisabled(".mp4");
        std::string canvas = std::to_string(g.canvas_width) + "×" + std::to_string(g.canvas_height);
        std::string picture = std::to_string(g.dst_width) + "×" + std::to_string(g.dst_height);
        ImGui::TextDisabled("%s: %s", TR("Output"),
                            TRF("%@ (picture %@, vertical ×%lld, nearest neighbor)", {canvas, picture, g.vertical_scale}).c_str());
        ImGui::TextDisabled("%s", TRF("Saved in %@", {Paths::display(d_.library->root() + "/Exports")}).c_str());
        (void)colW;
      }
      ImGui::EndChild();
      if (!exportError_.empty()) ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s", exportError_.c_str());
      ImGui::Spacing();
      if (appearing) {
        ImGui::SetKeyboardFocusHere();  // the main action first; the options are above it
        ImGui::SetNavCursorVisible(true);
      }
      ImGui::BeginDisabled(!any || st.takeLength == 0 || exportJob_.running());
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.85f, 1));
      if (ImGui::Button(TR("Export"), ImVec2(S(180), S(48)))) startExport();
      ImGui::PopStyleColor();
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel (make video)"), ImVec2(S(160), S(48)))) close = true;
      ImGui::SameLine(0, S(24));
      inlinePrompts(TR("Cancel (make video)"));
    }
    if (!appearing && !io.WantTextInput &&
        (ImGui::IsKeyPressed(cancelKey(), false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
      close = true;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (close) exportDialog_ = false;
}

// The export's end (dialog closed or not): "Video saved" with Open / Share, or what went wrong. Its
// progress is in the status label ("Making video… n%").
void UI::buildExportProgressPill() {
  if (!exportStarted_ || !exportJob_.finished()) return;
  if (exportDialog_ && !exportJob_.succeeded()) return;  // the dialog shows the error
  if (exportJob_.succeeded()) {
    savedVideo_ = exportJob_.outPath();
    savedVideoAt_ = -1;  // stamped by the toast
  } else {
    notice(exportJob_.wasCancelled() ? std::string(TR("Export was cancelled")) : exportJob_.error());
  }
  exportStarted_ = false;
}

void UI::buildSavedVideoToast(double now) {
  if (savedVideo_.empty()) return;
  if (savedVideoAt_ < 0) {
    savedVideoAt_ = now;
    savedVideoUntil_ = now + 8.0;
  }
  if (now >= savedVideoUntil_) {
    savedVideo_.clear();
    return;
  }
  const UiMetrics& m = metrics_;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImGuiIO& io = ImGui::GetIO();
  std::string title = TR("Video saved");
  std::string file = fs::path(savedVideo_).filename().string();
  float fs = m.label(), hs = m.hint(), h = m.hint() * 1.9f;
  float chipW = measure(hs, TR("Open")).x + measure(hs, TR("Share")).x + S(12) * 4 + S(10);
  float textW = std::max(measure(fs, title.c_str()).x, measure(hs, file.c_str()).x);
  float w = std::min(io.DisplaySize.x - S(32), S(20) + textW + S(24) + chipW + S(16));
  float boxH = fs + hs + S(28);
  LRect box{(io.DisplaySize.x - w) / 2, pill_.bottom() + S(12), w, boxH};
  dl->AddRectFilled(tl(box), br(box), IM_COL32(28, 28, 32, 240), m.tileRadius());
  dl->AddText(ImGui::GetFont(), fs, ImVec2(box.x + S(20), box.y + S(10)), kText, title.c_str());
  textFit(dl, hs, ImVec2(box.x + S(20), box.y + S(14) + fs), std::max(0.0f, box.w - chipW - S(56)), kTextDim, file);
  float cy = box.y + (box.h - h) / 2;
  bool share = false, open = false;
  LRect r = chip(dl, box.right() - S(16), cy, h, std::string(), TR("Share"), &share);
  chip(dl, r.x - S(10), cy, h, std::string(), TR("Open"), &open);
  // Open = the video itself; Share = its folder (the file manager, to hand it on).
  if (open) openFile(savedVideo_);
  if (share) openFolder(fs::path(savedVideo_).parent_path().string());
  if (open || share) savedVideo_.clear();
}

}  // namespace rnl
