// Chord detector: two-button combos (the Quick Menu's L+R) next to the members' own actions.
// Each member of a combo is UP, PENDING (pressed, the other may still come within the window),
// ALONE (fired as a single press, still held) or CHORDED (part of a fired combo, suppressed until
// released). See frontend.h for the rules.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"

namespace {

enum class State { up, pending, alone, chorded };

struct Combo {
  std::string id, member[2];
  State state[2] = {State::up, State::up};
  double since[2] = {0, 0};  // press time (PENDING)
  bool comboDown = false;    // COMBO_DOWN sent, COMBO_UP not yet
};

struct Event {
  rnf_chord_kind kind;
  std::string input;
  int member;
  double time;
};

}  // namespace

struct rnf_chord {
  double window = RNF_CHORD_WINDOW;
  std::vector<Combo> combos;
  std::deque<Event> queue;
  std::string polled;  // the last polled event's input (rnf_chord_event.input)

  bool find(const std::string& id, size_t& ci, int& mi) const {
    for (size_t i = 0; i < combos.size(); ++i)
      for (int m = 0; m < 2; ++m)
        if (combos[i].member[m] == id) {
          ci = i;
          mi = m;
          return true;
        }
    return false;
  }

  void push(rnf_chord_kind k, const std::string& input, int member, double t) { queue.push_back({k, input, member, t}); }

  void tick(double t) {
    for (Combo& c : combos)
      for (int m = 0; m < 2; ++m)
        if (c.state[m] == State::pending && t - c.since[m] >= window) {
          c.state[m] = State::alone;
          push(RNF_CHORD_ALONE_DOWN, c.member[m], m, c.since[m] + window);
        }
  }

  void feed(Combo& c, int m, bool down, double t) {
    tick(t);  // a member pending for the whole window fired before this event
    int o = 1 - m;
    if (down) {
      if (c.state[m] != State::up) return;  // repeat of a held member
      if (c.comboDown) {
        c.state[m] = State::chorded;  // re-pressed while the chord is still held: nothing
      } else if (c.state[o] == State::pending) {
        c.state[m] = c.state[o] = State::chorded;
        c.comboDown = true;
        push(RNF_CHORD_COMBO_DOWN, c.id, -1, t);
      } else {
        c.state[m] = State::pending;
        c.since[m] = t;
      }
      return;
    }
    switch (c.state[m]) {
      case State::up: return;
      case State::pending:  // a tap shorter than the window: fires on its release
        push(RNF_CHORD_ALONE_DOWN, c.member[m], m, t);
        push(RNF_CHORD_ALONE_UP, c.member[m], m, t);
        break;
      case State::alone: push(RNF_CHORD_ALONE_UP, c.member[m], m, t); break;
      case State::chorded:
        if (c.state[o] != State::chorded && c.comboDown) {
          c.comboDown = false;
          push(RNF_CHORD_COMBO_UP, c.id, -1, t);
        }
        break;
    }
    c.state[m] = State::up;
  }
};

extern "C" {

rnf_chord* rnf_chord_new(double window) {
  RNF_GUARD_BEGIN
  auto* c = new rnf_chord;
  if (window > 0) c->window = window;
  return c;
  RNF_GUARD_END(nullptr)
}

void rnf_chord_free(rnf_chord* c) { delete c; }

int rnf_chord_add(rnf_chord* c, const char* a, const char* b) {
  if (!c || !a || !b || !*a || !*b || std::strcmp(a, b) == 0) return -1;
  RNF_GUARD_BEGIN
  size_t ci;
  int mi;
  if (c->find(a, ci, mi) || c->find(b, ci, mi)) return -1;
  Combo k;
  k.member[0] = a;
  k.member[1] = b;
  k.id = std::string(a) + "+" + b;
  c->combos.push_back(std::move(k));
  return int(c->combos.size() - 1);
  RNF_GUARD_END(-1)
}

size_t rnf_chord_configure(rnf_chord* c, const rnf_binding* bindings, size_t count, const char* action) {
  if (!c) return 0;
  RNF_GUARD_BEGIN
  c->combos.clear();
  c->queue.clear();
  const char* want = action ? action : "hk.menu";
  for (size_t i = 0; bindings && i < count; ++i) {
    if (!bindings[i].input || !bindings[i].action || std::strcmp(bindings[i].action, want) != 0) continue;
    char *a = nullptr, *b = nullptr;
    if (rnf_input_combo_split(bindings[i].input, &a, &b)) rnf_chord_add(c, a, b);
    rnf_string_free(a);
    rnf_string_free(b);
  }
  return c->combos.size();
  RNF_GUARD_END(0)
}

size_t rnf_chord_combo_count(const rnf_chord* c) { return c ? c->combos.size() : 0; }

int rnf_chord_is_member(const rnf_chord* c, const char* id) {
  if (!c || !id) return 0;
  size_t ci;
  int mi;
  return c->find(id, ci, mi) ? 1 : 0;
}

int rnf_chord_feed(rnf_chord* c, const char* id, int pressed, double t) {
  if (!c || !id) return 0;
  RNF_GUARD_BEGIN
  size_t ci;
  int mi;
  if (!c->find(id, ci, mi)) {
    c->tick(t);
    return 0;
  }
  c->feed(c->combos[ci], mi, pressed != 0, t);
  return 1;
  RNF_GUARD_END(0)
}

void rnf_chord_tick(rnf_chord* c, double t) {
  if (!c) return;
  RNF_GUARD_BEGIN
  c->tick(t);
  RNF_GUARD_END()
}

double rnf_chord_deadline(const rnf_chord* c) {
  if (!c) return 0;
  double best = 0;
  for (const Combo& k : c->combos)
    for (int m = 0; m < 2; ++m)
      if (k.state[m] == State::pending) {
        double d = k.since[m] + c->window;
        if (best == 0 || d < best) best = d;
      }
  return best;
}

int rnf_chord_poll(rnf_chord* c, rnf_chord_event* out) {
  if (!c || c->queue.empty()) return 0;
  RNF_GUARD_BEGIN
  Event e = std::move(c->queue.front());
  c->queue.pop_front();
  c->polled = std::move(e.input);
  if (out) {
    out->kind = e.kind;
    out->input = c->polled.c_str();
    out->member = e.member;
    out->time = e.time;
  }
  return 1;
  RNF_GUARD_END(0)
}

void rnf_chord_reset(rnf_chord* c) {
  if (!c) return;
  c->queue.clear();
  for (Combo& k : c->combos) {
    k.state[0] = k.state[1] = State::up;
    k.comboDown = false;
  }
}

}  // extern "C"
