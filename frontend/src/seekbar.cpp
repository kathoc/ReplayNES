// The paused seek bar's controller model (docs/design/UI_REDESIGN.md): the UI confirm / cancel
// buttons, the one speed curve of every held seek (rewind, fast-forward, a held marker) and the
// A/B markers of the selected practice slot. See frontend.h for the rules.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <vector>

#include "common.hpp"

struct rnf_markers {
  std::vector<uint64_t> frames;  // sorted, at most two
  uint64_t length = 0;
  int focus = -1;                // -1: the seek bar
  bool editing = false;
  uint64_t origin = 0;           // the edited marker's frame before the edit
  uint64_t originPlayhead = 0;   // the playhead before the edit
  std::vector<uint64_t> originFrames;

  rnf_markers_result none() const { return rnf_markers_result{RNF_MARKERS_IGNORED, 0, 0, RNF_MARKERS_WRITE_NONE, 0, 0}; }
  rnf_markers_result changed() const { return rnf_markers_result{RNF_MARKERS_CHANGED, 0, 0, RNF_MARKERS_WRITE_NONE, 0, 0}; }
  // The slot as the markers say now.
  void write(rnf_markers_result& r) const {
    if (frames.empty()) {
      r.write = RNF_MARKERS_WRITE_CLEAR;
    } else if (frames.size() == 1) {
      r.write = RNF_MARKERS_WRITE_A_ONLY;
      r.a = frames[0];
    } else {
      r.write = RNF_MARKERS_WRITE_RANGE;
      r.a = frames[0];
      r.b = frames[1];
    }
  }
  void clampFocus() {
    if (frames.empty()) focus = -1;
    else if (focus >= int(frames.size())) focus = int(frames.size()) - 1;
  }
};

extern "C" {

const char* rnf_ui_confirm_element(int south_confirm) { return south_confirm ? "face.south" : "face.east"; }
const char* rnf_ui_cancel_element(int south_confirm) { return south_confirm ? "face.east" : "face.south"; }

int rnf_hold_speed(int tick) {
  if (tick <= RNF_HOLD_SPEED_FAST_AFTER) return 2;
  if (tick <= RNF_HOLD_SPEED_FASTEST_AFTER) return 3;
  return 4;
}

rnf_markers* rnf_markers_new(void) {
  RNF_GUARD_BEGIN
  return new rnf_markers;
  RNF_GUARD_END(nullptr)
}

void rnf_markers_free(rnf_markers* m) { delete m; }

void rnf_markers_sync(rnf_markers* m, const rnf_timeline_range* slot, uint64_t take_length) {
  if (!m || m->editing) return;
  m->length = take_length;
  m->frames.clear();
  if (slot) {
    m->frames.push_back(slot->a);
    if (slot->has_b && slot->b != slot->a) m->frames.push_back(slot->b);
    std::sort(m->frames.begin(), m->frames.end());
  }
  m->clampFocus();
}

size_t rnf_markers_count(const rnf_markers* m) { return m ? m->frames.size() : 0; }
uint64_t rnf_markers_frame(const rnf_markers* m, size_t i) { return m && i < m->frames.size() ? m->frames[i] : 0; }
int rnf_markers_focus(const rnf_markers* m) { return m ? m->focus : -1; }
int rnf_markers_editing(const rnf_markers* m) { return m && m->editing ? 1 : 0; }

rnf_markers_result rnf_markers_confirm(rnf_markers* m, uint64_t playhead) {
  rnf_markers_result r{};
  if (!m) return r;
  r = m->changed();
  if (m->editing) {  // commit: the playhead stays where the marker is
    m->editing = false;
    m->focus = -1;
    if (m->frames != m->originFrames) m->write(r);
    return r;
  }
  if (m->focus >= 0) {  // edit the focused marker: the picture goes to it
    m->editing = true;
    m->origin = m->frames[size_t(m->focus)];
    m->originFrames = m->frames;
    m->originPlayhead = playhead;
    r.seek = 1;
    r.seek_frame = m->origin;
    return r;
  }
  // The seek bar: a marker at the playhead.
  if (m->frames.size() >= 2) return rnf_markers_result{RNF_MARKERS_FULL, 0, 0, RNF_MARKERS_WRITE_NONE, 0, 0};
  if (std::find(m->frames.begin(), m->frames.end(), playhead) != m->frames.end())
    return rnf_markers_result{RNF_MARKERS_OCCUPIED, 0, 0, RNF_MARKERS_WRITE_NONE, 0, 0};
  m->frames.push_back(playhead);
  std::sort(m->frames.begin(), m->frames.end());
  m->write(r);
  return r;
}

rnf_markers_result rnf_markers_cancel(rnf_markers* m) {
  rnf_markers_result r{};
  if (!m) return r;
  if (m->editing) {  // revert the marker and the picture
    m->editing = false;
    m->frames = m->originFrames;
    m->focus = int(std::find(m->frames.begin(), m->frames.end(), m->origin) - m->frames.begin());
    m->clampFocus();
    r = m->changed();
    r.seek = 1;
    r.seek_frame = m->originPlayhead;
    return r;
  }
  if (m->focus >= 0) {
    m->focus = -1;
    return m->changed();
  }
  return m->none();
}

rnf_markers_result rnf_markers_move(rnf_markers* m, int dx, int dy, uint64_t playhead) {
  if (!m) return rnf_markers_result{};
  if (m->editing) {
    if (dx) return rnf_markers_nudge(m, dx < 0 ? -1 : 1);
    return m->changed();  // up / down do nothing while editing (not the game's either)
  }
  if (m->focus < 0) {
    if (dy < 0 && !m->frames.empty()) {
      // The marker nearest the playhead (the left one on a tie).
      size_t best = 0;
      auto dist = [&](uint64_t f) { return f > playhead ? f - playhead : playhead - f; };
      for (size_t i = 1; i < m->frames.size(); ++i)
        if (dist(m->frames[i]) < dist(m->frames[best])) best = i;
      m->focus = int(best);
      return m->changed();
    }
    return m->none();  // left / right step the playhead, down: nothing for the markers
  }
  if (dy > 0) m->focus = -1;
  else if (dx < 0) m->focus = 0;
  else if (dx > 0) m->focus = int(m->frames.size()) - 1;
  return m->changed();
}

rnf_markers_result rnf_markers_nudge(rnf_markers* m, int64_t frames) {
  if (!m || !m->editing || m->focus < 0 || size_t(m->focus) >= m->frames.size()) return m ? m->none() : rnf_markers_result{};
  rnf_markers_result r = m->changed();
  if (frames == 0) return r;
  uint64_t cur = m->frames[size_t(m->focus)];
  bool hasOther = m->frames.size() == 2;
  uint64_t other = hasOther ? m->frames[size_t(1 - m->focus)] : 0;
  int64_t want = int64_t(cur) + frames;
  int64_t hi = int64_t(m->length);
  int64_t next = std::clamp<int64_t>(want, 0, hi);
  if (hasOther && uint64_t(next) == other) {
    // Never on the other marker: jump over it (they swap roles), or stop next to it at the end.
    int64_t step = frames > 0 ? 1 : -1;
    int64_t past = next + step;
    next = past >= 0 && past <= hi ? past : next - step;
  }
  if (uint64_t(next) == cur) return r;
  m->frames[size_t(m->focus)] = uint64_t(next);
  std::sort(m->frames.begin(), m->frames.end());
  m->focus = int(std::find(m->frames.begin(), m->frames.end(), uint64_t(next)) - m->frames.begin());
  r.seek = 1;
  r.seek_frame = uint64_t(next);
  return r;
}

rnf_markers_result rnf_markers_delete(rnf_markers* m) {
  if (!m || m->editing || m->focus < 0) return m ? m->none() : rnf_markers_result{};
  m->frames.erase(m->frames.begin() + m->focus);
  m->focus = m->frames.empty() ? -1 : 0;
  rnf_markers_result r = m->changed();
  m->write(r);
  return r;
}

rnf_markers_result rnf_markers_leave(rnf_markers* m) {
  if (!m) return rnf_markers_result{};
  rnf_markers_result r = m->none();
  if (m->editing) r = rnf_markers_cancel(m);
  if (m->focus >= 0) {
    m->focus = -1;
    r.outcome = RNF_MARKERS_CHANGED;
  }
  return r;
}

rnf_seek_face_action rnf_markers_face(const rnf_markers* m, int north) {
  if (!m || m->editing) return RNF_SEEK_FACE_NONE;
  if (north) return m->frames.empty() ? RNF_SEEK_FACE_NONE : RNF_SEEK_FACE_PRACTICE;
  return m->focus >= 0 ? RNF_SEEK_FACE_DELETE_MARKER : RNF_SEEK_FACE_NEXT_SLOT;
}

}  // extern "C"
