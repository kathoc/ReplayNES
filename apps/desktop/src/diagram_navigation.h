// Spatial navigation shared by the controller diagram and its headless tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

namespace rnl {
// hubs (optional, one per center): a stick's arrows and its click share the click's index (the click
// itself too), -1 elsewhere. Moving into a stick from outside lands on its click (L3 / R3, drawn in the
// middle of the arrows, else reachable only from its own arrows); its arrows are a step from there.
template<class Point>
int diagramFocusNext(const std::vector<Point>& centers, int focus, int dx, int dy, const std::vector<int>* hubs = nullptr) {
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
  if (best < 0) return focus;
  if (hubs && hubs->size() == centers.size()) {
    const int to = (*hubs)[size_t(best)], from = (*hubs)[size_t(focus)];
    if (to >= 0 && to != from) return to;
  }
  return best;
}
}  // namespace rnl
