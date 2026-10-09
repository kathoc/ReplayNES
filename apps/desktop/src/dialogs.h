// Requests the app model makes of the UI: message dialogs with buttons (and an optional
// checkbox), the project / ROM file chooser and short notices. The ImGui UI implements them as
// gamepad-navigable modal popups; tests answer them directly.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace rnl {

struct Dialog {
  std::string title;
  std::string message;
  std::vector<std::string> buttons;  // left to right; an empty list = one "OK"
  int defaultIndex = 0;              // focused when it appears
  int cancelIndex = -1;              // B / Esc picks this one (-1: the last button)
  int destructiveIndex = -1;         // drawn in red
  std::string checkbox;              // "" = none
  bool checked = false;
  /// Button index and the checkbox state.
  std::function<void(int button, bool checked)> onResult;
};

struct ChooserRequest {
  enum class Mode { saveProject, openProject, openROM };
  Mode mode = Mode::saveProject;
  std::string title;
  std::string root;         // the chooser never leaves this folder
  std::string startDir;     // inside root ("" = root)
  std::string defaultName;  // saveProject: file name without ".nesrec"
  /// The chosen path (saveProject: <dir>/<name>.nesrec; the caller handles an existing item).
  std::function<void(const std::string& path)> onChosen;
  std::function<void()> onCancel;
};

class DialogHost {
 public:
  virtual ~DialogHost() = default;
  virtual void showDialog(Dialog d) = 0;
  virtual void showChooser(ChooserRequest r) = 0;
  /// A toast: informational, never takes the input, visible for `seconds` (at least 4).
  virtual void notice(const std::string& text, double seconds) = 0;
  void notice(const std::string& text) { notice(text, 4.0); }
};

/// Save-name rules of the chooser: trims, drops path separators and control characters, appends
/// ".nesrec". "" when nothing usable is left.
std::string chooserProjectFileName(const std::string& typed);

}  // namespace rnl
