// What the error dialogs show about a project that could not be opened (manifest.json via
// rn_project_manifest_json): the ROM's name, last path and SHA-256, the core compatibility ID.
// Includes a minimal JSON reader (objects, arrays, strings, numbers, literals).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace rnl {

struct ManifestInfo {
  std::string romName = "?", romPath = "?", romSHA256 = "?", coreCompatID = "?";
};

/// Parses manifest JSON text; fields that are missing stay "?".
ManifestInfo parseManifest(const std::string& json);
ManifestInfo readManifest(const std::string& projectDir);

}  // namespace rnl
