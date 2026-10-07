// What the platform's frontend offers (compile-time; set by apps/linux and apps/windows CMake):
//   RNL_HAVE_STEAM       Settings -> System -> Add to Steam (Linux: steam_shortcut.h)
//   RNL_HAVE_MP4_EXPORT  Export… (Linux: FFmpeg; Windows: Media Foundation)
//   RNL_HAVE_CRT         the CRT display (Linux: Vulkan compute; Windows: Direct3D 11 compute)
// and the platform's wording (Windows: "Recycle Bin", "Explorer").
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef RNL_HAVE_STEAM
#define RNL_HAVE_STEAM 0
#endif
#ifndef RNL_HAVE_MP4_EXPORT
#define RNL_HAVE_MP4_EXPORT 1
#endif
#ifndef RNL_HAVE_CRT
#define RNL_HAVE_CRT 1
#endif

#include "l10n.h"

namespace rnl {

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif
/// Trashed items go to the Recycle Bin (wording).
constexpr bool kRecycleBin = kWindows;

}  // namespace rnl
