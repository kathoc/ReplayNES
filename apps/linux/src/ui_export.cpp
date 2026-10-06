// "Export…" (MP4, export/mp4_export.h: FFmpeg H.264 + AAC on a worker thread, the project is never
// touched and play continues) and the CRT display settings (render/post_process.h).
// SPDX-License-Identifier: GPL-2.0-or-later
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
#include "render/crt_export.h"
#include "settings.h"
#include "ui.h"

namespace fs = std::filesystem;

namespace rnl {

namespace {
const char* const kEncoders[] = {"libx264", "h264_vaapi", "libopenh264"};
std::string str(char* p) {
  std::string s = p ? p : "";
  rnf_string_free(p);
  return s;
}
void wrappedDisabled(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
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

// ------------------------------------------------------------------ CRT settings

void UI::buildCrtSettings() {
  Settings& s = *d_.settings;
  PostProcessStatus ps = d_.renderer->postProcessStatus();
  ImGui::SeparatorText(TR("CRT Display"));
  ImGui::BeginDisabled(!ps.crtAvailable);
  if (ImGui::Checkbox(TR("CRT display"), &s.crt)) changed();
  ImGui::EndDisabled();
  if (!ps.crtAvailable)
    ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s",
                       TRF("The CRT display isn’t available on this GPU: %@", {ps.crtError}).c_str());
  wrappedDisabled(TR("A Vulkan port of nesterm’s “CRT (physical model, experimental)”: NES pixel codes → RF/IF → "
                     "demodulation → screen (slot mask, scattering, persistence). It is an uncalibrated experimental "
                     "model, not a reproduction of any real TV. It only affects the display; recording, game progress "
                     "and reproducibility are unaffected. The picture is 4:3 (the pixel aspect setting isn’t used)."));
  ImGui::BeginDisabled(!s.crt);
  if (ImGui::Checkbox(TR("Thicker scanlines where brighter"), &s.crtBeamGrowth)) changed();
  if (ImGui::Checkbox(TR("Phosphor persistence"), &s.crtPersistence)) changed();
  if (ImGui::Checkbox(TR("Bright screens widen and dim the picture"), &s.crtSupply)) changed();
  float ant = float(s.crtAntenna);
  ImGui::SetNextItemWidth(S(360));
  if (ImGui::SliderFloat("##antenna", &ant, 20, 90, "%.0f dBuV")) s.crtAntenna = std::round(ant);
  if (ImGui::IsItemDeactivatedAfterEdit()) changed();
  ImGui::SameLine();
  ImGui::TextUnformatted(TR("Signal strength (source picture)"));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Scanlines"));
  ImGui::SameLine();
  if (ImGui::RadioButton(TR("Standard: 240 lines"), s.crtLines == 240)) {
    s.crtLines = 240;
    changed();
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(TR("Reduced-line experiment"), s.crtLines != 240)) {
    if (s.crtLines == 240) s.crtLines = 160;
    changed();
  }
  if (s.crtLines != 240) {
    int lines = s.crtLines;
    ImGui::SetNextItemWidth(S(360));
    if (ImGui::SliderInt("##lines", &lines, 110, 220)) s.crtLines = lines;
    if (ImGui::IsItemDeactivatedAfterEdit()) changed();
    ImGui::SameLine();
    ImGui::TextUnformatted(TR("Experiment: 110–220 lines"));
  }
  if (ImGui::Button(TR("Reset to nesterm Defaults"))) {
    s.crtLines = 240;
    s.crtBeamGrowth = s.crtPersistence = s.crtSupply = true;
    s.crtAntenna = 65;
    changed();
  }
  ImGui::EndDisabled();
  if (s.crt && ps.crtShown)
    ImGui::TextDisabled("%dx%d  x%.2f  GPU %.2f ms%s", ps.tubeWidth, ps.tubeHeight, ps.scale, ps.gpuMsP50,
                        ps.buildAhead ? "  (built ahead)" : "");
}

// ------------------------------------------------------------------ export

void UI::openExportDialog() {
  const EmuStatus& st = d_.emu->status();
  if (!st.hasSession) return;
  exportDialog_ = true;
  exportOpened_ = false;
  if (!exportJob_.running()) exportStarted_ = false;
  exportError_.clear();
  exportApplyFlash_ = d_.settings->flash != RN_FLASH_OFF;
  exportApplyCRT_ = d_.settings->crt;
  exportPar87_ = d_.settings->par87;
  exportEnd_ = int(st.takeLength);
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

void UI::startExport() {
  const EmuStatus& st = d_.emu->status();
  rnf_export_settings s = defaultExportSettings();
  rnf_export_preset p{};
  if (rnf_export_preset_get(size_t(exportPreset_), &p)) s.preset = p;
  int crop = exportCropOverscan_ ? 8 : 0;
  s.crop_top = s.crop_bottom = s.crop_left = s.crop_right = crop;
  s.pixel_aspect_87 = exportPar87_;
  if (exportWholeTake_) {
    s.start_frame = 0;
    s.end_frame = 0;
  } else {
    s.start_frame = uint64_t(std::max(0, exportStart_));
    s.end_frame = uint64_t(std::clamp(exportEnd_, 0, int(st.takeLength)));
  }
  char* msg = nullptr;
  if (rnf_export_validate(&s, &msg) != RN_OK) {
    exportError_ = str(msg);
    return;
  }
  rnf_string_free(msg);
  ExportOptions eo;
  eo.settings = s;
  rn_flash_level level = d_.settings->flash == RN_FLASH_OFF ? RN_FLASH_STANDARD : rn_flash_level(d_.settings->flash);
  eo.flash = exportApplyFlash_ ? level : RN_FLASH_OFF;
  if (exportEncoder_ > 0) eo.encoder = kEncoders[exportEncoder_ - 1];
  if (exportApplyCRT_) eo.makeProcessor = crtExportProcessorFactory(crtSettings());
  std::string dir = d_.library->root() + "/Exports";
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::string name = chooserProjectFileName(exportName_);  // same rules; ".nesrec" swapped for ".mp4"
  if (name.empty()) name = "export.nesrec";
  name = name.substr(0, name.size() - 7);
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
    ImGui::PushFont(nullptr, S(26));
    ImGui::TextUnformatted(TR("Export to MP4"));
    ImGui::PopFont();
    bool appearing = ImGui::IsWindowAppearing();
    if (exportStarted_) {
      // Progress / result.
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
        ExportResult r = exportJob_.result();
        ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1), "%s", TR("Export finished"));
        char dur[32];
        std::snprintf(dur, sizeof dur, "%.2f", r.duration);
        ImGui::TextDisabled("%s", TRF("%llu frames · %@ s · %llu audio samples",
                                      {(unsigned long long)r.frames, std::string(dur), (unsigned long long)r.audioSamples})
                                      .c_str());
        ImGui::TextDisabled("%s  (%s)", TRF("Replay hash %016llx", {(unsigned long long)r.rendererHash}).c_str(), r.encoder.c_str());
        if (appearing) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(TR("Close"))) close = true;
      } else {
        ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s",
                           exportJob_.wasCancelled() ? TR("Export was cancelled") : exportJob_.error().c_str());
        if (appearing) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(TR("Close"))) close = true;
      }
    } else {
      // Settings: two columns (picture | processing + range), the name, the result, then the
      // collapsible advanced options, so the buttons are always on screen at 1280x800.
      const EmuStatus& st = d_.emu->status();
      const float colW = S(460), gap = S(24);
      // The options scroll (focus-driven) if they ever outgrow the screen; the buttons below stay.
      float maxBody = io.DisplaySize.y * 0.94f - S(40) - S(100);
      ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxBody));
      ImGui::BeginChild("##exportbody", ImVec2(colW * 2 + gap + S(4), 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                        ImGuiWindowFlags_NoBackground);
      const float x0 = ImGui::GetCursorPosX();
      // A section title with a rule that stays inside its column (SeparatorText spans the window).
      auto section = [&](const char* title, float colRight) {
        ImGui::TextUnformatted(title);
        ImVec2 a = ImGui::GetItemRectMax(), b = ImGui::GetItemRectMin();
        float y = (a.y + b.y) * 0.5f;
        float right = ImGui::GetWindowPos().x + colRight - ImGui::GetScrollX();
        if (right > a.x + S(10)) ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + S(10), y), ImVec2(right, y), ImGui::GetColorU32(ImGuiCol_Separator));
      };
      ImGui::BeginGroup();
      ImGui::PushItemWidth(colW);
      section(TR("Size"), x0 + colW);
      for (size_t i = 0, n = rnf_export_preset_count(), col = 0; i < n; ++i) {
        rnf_export_preset p{};
        if (!rnf_export_preset_get(i, &p)) continue;
        if (col % 3) ImGui::SameLine(x0 + colW * float(col % 3) / 3.0f);
        ++col;
        std::string l = str(rnf_export_preset_label(&p)) + "##preset" + std::to_string(i);
        if (ImGui::RadioButton(l.c_str(), exportPreset_ == int(i))) exportPreset_ = int(i);
      }
      section(TR("Pixel Aspect Ratio"), x0 + colW);
      if (ImGui::RadioButton(TR("1:1 (square pixels)"), !exportPar87_)) exportPar87_ = false;
      if (ImGui::RadioButton(TR("8:7 (as on a CRT TV)"), exportPar87_)) exportPar87_ = true;
      ImGui::Checkbox(TR("Hide overscan (crop 8 px on each side)"), &exportCropOverscan_);
      ImGui::PopItemWidth();
      ImGui::Dummy(ImVec2(colW, 0));
      ImGui::EndGroup();
      ImGui::SameLine(x0 + colW + gap);
      ImGui::BeginGroup();
      ImGui::PushTextWrapPos(x0 + colW + gap + colW);
      section(TR("Processing"), x0 + colW + gap + colW);
      ImGui::Checkbox(TR("Apply Flash Reduction"), &exportApplyFlash_);
      rn_flash_level level = d_.settings->flash == RN_FLASH_OFF ? RN_FLASH_STANDARD : rn_flash_level(d_.settings->flash);
      ImGui::TextDisabled("%s", TRF("Level: %@ (Settings → Display). Off: exactly as recorded.", {str(rnf_flash_level_label(level))}).c_str());
      ImGui::BeginDisabled(!d_.renderer->postProcessStatus().crtAvailable);
      ImGui::Checkbox(TR("Apply CRT Effect"), &exportApplyCRT_);
      ImGui::EndDisabled();
      ImGui::TextDisabled("%s", TR("CRT Display settings, 4:3 (best with 1280×960). Slower."));
      section(TR("Range"), x0 + colW + gap + colW);
      ImGui::Checkbox(TR("Whole Take"), &exportWholeTake_);
      if (!exportWholeTake_) {
        ImGui::SetNextItemWidth(colW * 0.5f);
        ImGui::InputInt(TR("Start Frame"), &exportStart_);
        ImGui::SetNextItemWidth(colW * 0.5f);
        ImGui::InputInt(TR("End Frame"), &exportEnd_);
      }
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(colW, 0));
      ImGui::EndGroup();
      ImGui::Spacing();
      // Name + result.
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(TR("Name"));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(440));
      ImGui::InputText("##exportname", exportName_, sizeof exportName_);
      ImGui::SameLine();
      ImGui::TextDisabled(".mp4");
      rnf_export_settings s = defaultExportSettings();
      rnf_export_preset p{};
      if (rnf_export_preset_get(size_t(exportPreset_), &p)) s.preset = p;
      int crop = exportCropOverscan_ ? 8 : 0;
      s.crop_top = s.crop_bottom = s.crop_left = s.crop_right = crop;
      s.pixel_aspect_87 = exportPar87_;
      rnf_export_geometry g{};
      rnf_export_geometry_compute(&s, &g);
      std::string canvas = std::to_string(g.canvas_width) + "×" + std::to_string(g.canvas_height);
      std::string picture = std::to_string(g.dst_width) + "×" + std::to_string(g.dst_height);
      ImGui::TextDisabled("%s: %s", TR("Output"),
                          TRF("%@ (picture %@, vertical ×%lld, nearest neighbor)", {canvas, picture, g.vertical_scale}).c_str());
      uint64_t frames = exportWholeTake_ ? st.takeLength : uint64_t(std::max(0, exportEnd_ - exportStart_));
      ImGui::TextDisabled("%s: %s", TR("Length"),
                          TRF("%@ (%llu frames, 60.0988 fps, AAC 48 kHz)", {timecode(frames), (unsigned long long)frames}).c_str());
      if (ImGui::CollapsingHeader(TR("Advanced"))) {
        ImGui::TextDisabled("%s", TRF("Saved in %@", {Paths::display(d_.library->root() + "/Exports")}).c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Codec"));
        ImGui::SameLine();
        if (ImGui::RadioButton(TR("H.264 (automatic encoder)"), exportEncoder_ == 0)) exportEncoder_ = 0;
        for (int i = 0; i < 3; ++i) {
          ImGui::SameLine();
          if (ImGui::RadioButton(kEncoders[i], exportEncoder_ == i + 1)) exportEncoder_ = i + 1;
        }
        wrappedDisabled(TR("The export is made by re-running the recorded input from the start in a separate emulator. The "
                           "project is not changed, and you can keep playing while it exports."));
      }
      scrollWithRightStick();
      ImGui::EndChild();
      if (!exportError_.empty()) ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "%s", exportError_.c_str());
      ImGui::Spacing();
      if (appearing) {
        ImGui::SetKeyboardFocusHere();  // the main action first; the options are above it
        ImGui::SetNavCursorVisible(true);
      }
      ImGui::BeginDisabled(st.takeLength == 0 || exportJob_.running());
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.85f, 1));
      if (ImGui::Button(TR("Export"), ImVec2(S(180), S(48)))) startExport();
      ImGui::PopStyleColor();
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(160), S(48)))) close = true;
      ImGui::SameLine(0, S(24));
      inlinePrompts(TR("Cancel"));
    }
    if (!appearing && !io.WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
      close = true;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (close) exportDialog_ = false;
}

void UI::buildExportProgressPill() {
  if (exportDialog_ || !exportStarted_) return;
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  std::string text;
  if (exportJob_.running()) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", exportJob_.progress() * 100);
    text = std::string(TR("Export to MP4")) + "  " + b;
  } else if (exportJob_.finished()) {
    // Finished in the background: say so once (notice), then the pill goes away.
    notice(exportJob_.succeeded() ? std::string(TR("Export finished")) + ": " + Paths::display(exportJob_.outPath())
                                  : exportJob_.wasCancelled() ? std::string(TR("Export was cancelled")) : exportJob_.error());
    exportStarted_ = false;
    return;
  } else {
    return;
  }
  float fs = S(16);
  ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, 10000, 0, text.c_str());
  ImVec2 p(io.DisplaySize.x - ts.x - S(28), menuOpen_ ? S(68) : S(12));
  dl->AddRectFilled(p, ImVec2(p.x + ts.x + S(16), p.y + ts.y + S(8)), IM_COL32(30, 90, 160, 210), S(6));
  dl->AddText(nullptr, fs, ImVec2(p.x + S(8), p.y + S(4)), IM_COL32(255, 255, 255, 255), text.c_str());
}

}  // namespace rnl
