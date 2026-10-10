// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/crt_export.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "render/headless_vulkan.h"

namespace rnl {

namespace {
class CrtExportProcessor : public ExportVideoProcessor {
 public:
  explicit CrtExportProcessor(const CrtSettings& s) : settings_(s) {}
  ~CrtExportProcessor() override {
    renderer_.reset();
    if (target_.buffer) {
      vkUnmapMemory(ctx_.device, target_.memory);
      vkDestroyBuffer(ctx_.device, target_.buffer, nullptr);
      vkFreeMemory(ctx_.device, target_.memory, nullptr);
    }
    vk_.shutdown();
  }

  bool init(std::string* error) {
    if (!vk_.init(error)) {
      *error = "CRT effect: Vulkan isn't available (" + *error + ")";
      return false;
    }
    ctx_ = vk_.context();
    renderer_ = std::make_unique<CrtRenderer>();
    return renderer_->init(ctx_, VK_NULL_HANDLE, error);
  }

  bool begin(const rnf_export_settings& s, const rnf_export_geometry& g, std::string* error) override {
    w_ = g.canvas_width;
    h_ = g.canvas_height;
    crop_ = double(s.crop_top + s.crop_bottom) / 2 / 240;
    // Full 4:3 raster minus the cropped overscan rows, fitted into the canvas and centred.
    double aspect = (4.0 / 3.0) / (1 - 2 * crop_);
    double h = double(h_), w = std::round(h * aspect);
    if (w > double(w_)) {
      w = double(w_);
      h = std::round(w / aspect);
    }
    dst_ = CrtRect{float(std::round((double(w_) - w) / 2)), float(std::round((double(h_) - h) / 2)), float(w), float(h)};
    int tw = 0, th = 0;
    CrtRenderer::tubeSize(w, h, crop_, 1600, &tw, &th);
    renderer_->quality = CrtQuality::reference;  // offline: the 1:1 port (time is not critical)
    renderer_->configure(settings_, tw, th, true);
    // Host-visible BGRA canvas the show pass writes.
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize(w_) * h_ * 4;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(ctx_.device, &bci, nullptr, &target_.buffer) != VK_SUCCESS) return fail(error, "canvas buffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(ctx_.device, target_.buffer, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(ctx_.physicalDevice, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(ctx_.device, &mai, nullptr, &target_.memory) != VK_SUCCESS)
      return fail(error, "canvas memory");
    vkBindBufferMemory(ctx_.device, target_.buffer, target_.memory, 0);
    vkMapMemory(ctx_.device, target_.memory, 0, VK_WHOLE_SIZE, 0, &target_.map);
    return true;
  }

  // Same input rule as the live view: RF path from the PPU codes, or the (flash-filtered) RGB
  // picture when the filter altered the frame / the core has no codes.
  bool render(const ExportFrameSignal& sig, const uint32_t* pixels, uint8_t* canvas, size_t stride, std::string* error) override {
    CrtRenderer::Input in;
    if (sig.codes && !sig.flashAltered) {
      in.kind = CrtRenderer::InputKind::codes;
      in.codes = sig.codes;
      in.burstPhase = sig.burstPhase;
    } else {
      in.kind = CrtRenderer::InputKind::rgb;
      in.rgb = pixels;
    }
    bool encoded = false;
    bool ok = renderer_->runSync([&](VkCommandBuffer cmd) {
      encoded = renderer_->encode(cmd, in, sig.ordinal);
      if (encoded) renderer_->encodeShowToBuffer(cmd, target_.buffer, w_, h_, dst_, crop_);
    });
    if (!ok || !encoded) return fail(error, "CRT rendering failed");
    const uint8_t* src = static_cast<const uint8_t*>(target_.map);
    for (int y = 0; y < h_; ++y) std::memcpy(canvas + size_t(y) * stride, src + size_t(y) * size_t(w_) * 4, size_t(w_) * 4);
    return true;
  }

 private:
  static bool fail(std::string* error, const char* m) {
    if (error) *error = m;
    return false;
  }
  struct Target { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* map = nullptr; };
  CrtSettings settings_;
  HeadlessVulkan vk_;
  CrtVulkanContext ctx_;
  std::unique_ptr<CrtRenderer> renderer_;
  Target target_;
  int w_ = 0, h_ = 0;
  double crop_ = 0;
  CrtRect dst_;
};
}  // namespace

std::unique_ptr<ExportVideoProcessor> makeCrtExportProcessor(const CrtSettings& settings, std::string* error) {
  auto p = std::make_unique<CrtExportProcessor>(settings);
  std::string err;
  if (!p->init(&err)) {
    if (error) *error = err;
    return nullptr;
  }
  return p;
}

std::function<std::unique_ptr<ExportVideoProcessor>(std::string*)> crtExportProcessorFactory(const CrtSettings& settings) {
  return [settings](std::string* error) { return makeCrtExportProcessor(settings, error); };
}

}  // namespace rnl
