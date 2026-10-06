// Placeholder for the MP4 export (see export_hook.h): the menu entry stays disabled.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "export_hook.h"

namespace rnl {

bool exportAvailable(const ExportHost&) { return false; }
void exportOpen(const ExportHost&) {}
bool exportDrawUI(const ExportHost&) { return false; }

}  // namespace rnl
