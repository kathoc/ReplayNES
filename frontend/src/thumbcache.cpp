// Thumbnails of the active take keyed by cursor frame. Thread-safe: written by the emulation
// thread (live capture, take / session changes) and a generator thread, read by the UI. The
// images are opaque frontend payloads kept alive with the frontend's retain / release.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "common.hpp"

namespace rnf {
bool isGridFrame(uint64_t x, double f);
uint64_t thumbStartPicture(double f);
}  // namespace rnf

struct rnf_thumb_cache {
  rnf_payload_fn retain = nullptr;
  rnf_payload_fn release = nullptr;
  rnf_change_fn onChange = nullptr;
  void* onChangeCtx = nullptr;
  std::mutex lock;
  std::map<uint64_t, void*> images;  // ordered: lookups of the latest earlier key are O(log n)
  uint64_t take = 0;
  uint64_t generation = 1;
  uint64_t version = 0;
  double step = 0;
  size_t capacity = 600;  // 60 KB each

  void notify() {
    rnf_change_fn fn;
    void* ctx;
    {
      std::lock_guard<std::mutex> g(lock);
      fn = onChange;
      ctx = onChangeCtx;
    }
    if (fn) fn(ctx);
  }
  void releaseAll(const std::vector<void*>& v) {
    if (release) for (void* p : v) if (p) release(p);
  }
  void* retained(void* p) {
    if (p && retain) retain(p);
    return p;
  }
};

extern "C" {

rnf_thumb_cache* rnf_thumb_cache_new(rnf_payload_fn retain, rnf_payload_fn release) {
  try {
    auto* c = new rnf_thumb_cache;
    c->retain = retain;
    c->release = release;
    return c;
  } catch (...) { return nullptr; }
}

void rnf_thumb_cache_free(rnf_thumb_cache* c) {
  if (!c) return;
  std::vector<void*> drop;
  for (auto& e : c->images) drop.push_back(e.second);
  c->images.clear();
  c->releaseAll(drop);
  delete c;
}

void rnf_thumb_cache_set_on_change(rnf_thumb_cache* c, rnf_change_fn fn, void* ctx) {
  if (!c) return;
  std::lock_guard<std::mutex> g(c->lock);
  c->onChange = fn;
  c->onChangeCtx = ctx;
}

uint64_t rnf_thumb_cache_take(rnf_thumb_cache* c) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  return c->take;
}
uint64_t rnf_thumb_cache_generation(rnf_thumb_cache* c) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  return c->generation;
}
uint64_t rnf_thumb_cache_version(rnf_thumb_cache* c) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  return c->version;
}
size_t rnf_thumb_cache_count(rnf_thumb_cache* c) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  return c->images.size();
}
double rnf_thumb_cache_step(rnf_thumb_cache* c) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  return c->step;
}
size_t rnf_thumb_cache_capacity(rnf_thumb_cache* c) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  return c->capacity;
}
void rnf_thumb_cache_set_capacity(rnf_thumb_cache* c, size_t capacity) {
  if (!c) return;
  std::lock_guard<std::mutex> g(c->lock);
  c->capacity = capacity;
}

void rnf_thumb_cache_reset(rnf_thumb_cache* c, uint64_t take) {
  if (!c) return;
  std::vector<void*> drop;
  {
    std::lock_guard<std::mutex> g(c->lock);
    for (auto& e : c->images) drop.push_back(e.second);
    c->images.clear();
    c->take = take;
    c->generation += 1;
    c->version += 1;
  }
  c->releaseAll(drop);
  c->notify();
}

void rnf_thumb_cache_rebase(rnf_thumb_cache* c, uint64_t take, uint64_t keep_through) {
  if (!c) return;
  std::vector<void*> drop;
  {
    std::lock_guard<std::mutex> g(c->lock);
    if (take == c->take) return;
    for (auto it = c->images.begin(); it != c->images.end();) {
      if (it->first > keep_through) { drop.push_back(it->second); it = c->images.erase(it); }
      else ++it;
    }
    c->take = take;
    c->generation += 1;
    c->version += 1;
  }
  c->releaseAll(drop);
  c->notify();
}

void rnf_thumb_cache_set_step(rnf_thumb_cache* c, double q) {
  if (!c) return;
  std::vector<void*> drop;
  {
    std::lock_guard<std::mutex> g(c->lock);
    c->step = q;
    if (c->images.size() > c->capacity && q > 0) {
      uint64_t start = rnf::thumbStartPicture(q);
      for (auto it = c->images.begin(); it != c->images.end();) {
        if (!rnf::isGridFrame(it->first, q) && it->first != start) { drop.push_back(it->second); it = c->images.erase(it); }
        else ++it;
      }
    }
  }
  c->releaseAll(drop);
}

int rnf_thumb_cache_wants(rnf_thumb_cache* c, uint64_t f, uint64_t take) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  if (f == 0 || !(c->step > 0) || take != c->take || c->images.count(f)) return 0;
  return rnf::isGridFrame(f, c->step) || f == rnf::thumbStartPicture(c->step);
}

int rnf_thumb_cache_insert(rnf_thumb_cache* c, uint64_t f, uint64_t take, int has_generation, uint64_t generation,
                           void* payload) {
  if (!c) return 0;
  void* old = nullptr;
  {
    std::lock_guard<std::mutex> g(c->lock);
    if (f == 0 || take != c->take || (has_generation && generation != c->generation)) return 0;
    void*& slot = c->images[f];
    old = slot;
    slot = c->retained(payload);
    c->version += 1;
  }
  if (old && c->release) c->release(old);
  c->notify();
  return 1;
}

int rnf_thumb_cache_insert_batch(rnf_thumb_cache* c, const uint64_t* frames, void* const* payloads, size_t count,
                                 uint64_t take, uint64_t generation) {
  if (!c) return 0;
  std::vector<void*> drop;
  {
    std::lock_guard<std::mutex> g(c->lock);
    if (take != c->take || generation != c->generation || count == 0 || !frames || !payloads) return 0;
    for (size_t i = 0; i < count; ++i) {
      if (frames[i] == 0) continue;
      void*& slot = c->images[frames[i]];
      if (slot) drop.push_back(slot);
      slot = c->retained(payloads[i]);
    }
    c->version += 1;
  }
  c->releaseAll(drop);
  c->notify();
  return 1;
}

int rnf_thumb_cache_contains(rnf_thumb_cache* c, uint64_t f) {
  if (!c) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  return c->images.count(f) ? 1 : 0;
}

size_t rnf_thumb_cache_missing(rnf_thumb_cache* c, const uint64_t* frames, size_t count, uint64_t* out, size_t cap) {
  if (!c || !frames) return 0;
  std::lock_guard<std::mutex> g(c->lock);
  size_t n = 0;
  for (size_t i = 0; i < count; ++i) {
    if (c->images.count(frames[i])) continue;
    if (out && n < cap) out[n] = frames[i];
    ++n;
  }
  return n;
}

void* rnf_thumb_cache_image_at(rnf_thumb_cache* c, uint64_t f) {
  if (!c) return nullptr;
  std::lock_guard<std::mutex> g(c->lock);
  auto it = c->images.find(f);
  return it == c->images.end() ? nullptr : c->retained(it->second);
}

void* rnf_thumb_cache_image_before(rnf_thumb_cache* c, uint64_t f, uint64_t window) {
  if (!c) return nullptr;
  std::lock_guard<std::mutex> g(c->lock);
  auto it = c->images.lower_bound(f);  // first key >= f
  if (it == c->images.begin()) return nullptr;
  --it;
  return f - it->first <= window ? c->retained(it->second) : nullptr;
}

}  // extern "C"
