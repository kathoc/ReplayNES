// Spatial navigation shared by the controller diagram and its headless tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

namespace rnl {
template<class Point>
int diagramFocusNext(const std::vector<Point>& centers, int focus, int dx, int dy) {
  int best = -1;
  float bestScore = 1e9f;
  if (focus >= 0 && focus < int(centers.size())) {
    const auto c = centers[size_t(focus)];
    for (size_t i = 0; i < centers.size(); ++i) {
      if (int(i) == focus) continue;
      float vx = centers[i].x - c.x, vy = centers[i].y - c.y;
      float along = vx * float(dx) + vy * float(dy), across = std::fabs(vx * float(dy)) + std::fabs(vy * float(dx));
      if (along <= 1) continue;
      float score = along + across * 2.2f;
      if (score < bestScore) bestScore = score, best = int(i);
    }
  }
  return best >= 0 ? best : focus;
}
}  // namespace rnl
