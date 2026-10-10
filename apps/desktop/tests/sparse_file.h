// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

/// Creates (or empties) `path` as a sparse file; write it afterwards opened in|out (no truncation).
bool createSparseFile(const std::string& path);
