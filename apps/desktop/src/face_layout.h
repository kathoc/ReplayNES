// The face buttons' positions per controller model, as the user checked them (docs/design/FACE_CHECK.md):
// a model either reports its face buttons where they are, or swapped (south <-> east, west <-> north;
// e.g. the Retroid Pocket's layout setting). Remembered in <config>/face-layouts.txt.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <string>

#include "replaynes/frontend.h"

namespace rnl {

class FaceLayouts {
 public:
  explicit FaceLayouts(std::string file = "") : file_(std::move(file)) { load(); }
  bool known(const std::string& model) const { return layouts_.count(model) > 0; }
  bool swapped(const std::string& model) const {
    auto it = layouts_.find(model);
    return it != layouts_.end() && it->second;
  }
  void set(const std::string& model, bool swapped);
  void forget(const std::string& model);

 private:
  void load();
  void save() const;
  std::string file_;
  std::map<std::string, bool> layouts_;
};

/// SDL's face button (SDL_GAMEPAD_BUTTON_SOUTH 0, EAST 1, WEST 2, NORTH 3) where it is on a model that
/// reports them swapped; other buttons unchanged.
inline int faceButtonAt(int sdlButton, bool swapped) {
  if (!swapped || sdlButton < 0 || sdlButton > 3) return sdlButton;
  static const int other[4] = {1, 0, 3, 2};
  return other[sdlButton];
}

/// "face.south" ... the same way (labels, the check's picture).
inline const char* faceElementAt(const std::string& element, bool swapped) {
  if (!swapped) return nullptr;
  if (element == "face.south") return "face.east";
  if (element == "face.east") return "face.south";
  if (element == "face.west") return "face.north";
  if (element == "face.north") return "face.west";
  return nullptr;
}

/// Asked about: a controller whose layout is not certain - no known family (Xbox, PlayStation, Nintendo,
/// a Steam Deck) and not one of Steam's virtual pads (Steam has its own setting).
inline bool faceLayoutUncertain(rnf_controller_family family, bool steamVirtual) {
  return family == RNF_FAMILY_GENERIC && !steamVirtual;
}

}  // namespace rnl
