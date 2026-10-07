// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/crt_settings.h"

#include <algorithm>

namespace rnl {

CrtSettings CrtSettings::sanitized() const {
  CrtSettings s = *this;
  if (!validLines(s.lines)) s.lines = 240;
  s.antennaDbuv = std::min(90.0, std::max(20.0, s.antennaDbuv));
  s.ambientLux = std::min(500.0, std::max(0.0, s.ambientLux));
  return s;
}

bool CrtSettings::operator==(const CrtSettings& o) const {
  return lines == o.lines && beamGrowth == o.beamGrowth && persistence == o.persistence && supply == o.supply &&
         antennaDbuv == o.antennaDbuv && ambientLux == o.ambientLux;
}

}  // namespace rnl
