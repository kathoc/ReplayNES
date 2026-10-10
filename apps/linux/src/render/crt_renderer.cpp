// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
// Vulkan host side of the CRT pipeline: a 1:1 port of CRTRenderer.swift (see crt_renderer.h).
#include "render/crt_renderer.h"

#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rnl {

namespace {
namespace spv {
const uint32_t rf_forward_tg[] = {
#include "crt/rf_forward_tg.comp.spv.inc"
};
const uint32_t rf_inverse_tg[] = {
#include "crt/rf_inverse_tg.comp.spv.inc"
};
const uint32_t rf_init_codes[] = {
#include "crt/rf_init_codes.comp.spv.inc"
};
const uint32_t rf_butterfly2[] = {
#include "crt/rf_butterfly2.comp.spv.inc"
};
const uint32_t rf_butterfly[] = {
#include "crt/rf_butterfly.comp.spv.inc"
};
const uint32_t rf_multiply[] = {
#include "crt/rf_multiply.comp.spv.inc"
};
const uint32_t rf_extract[] = {
#include "crt/rf_extract.comp.spv.inc"
};
const uint32_t rx_reduce[] = {
#include "crt/rx_reduce.comp.spv.inc"
};
const uint32_t rx_agc[] = {
#include "crt/rx_agc.comp.spv.inc"
};
const uint32_t rx_prepare[] = {
#include "crt/rx_prepare.comp.spv.inc"
};
const uint32_t rx_decode[] = {
#include "crt/rx_decode.comp.spv.inc"
};
const uint32_t raster_area[] = {
#include "crt/raster_area.comp.spv.inc"
};
const uint32_t supply_mean[] = {
#include "crt/supply_mean.comp.spv.inc"
};
const uint32_t supply_row[] = {
#include "crt/supply_row.comp.spv.inc"
};
const uint32_t supply_state[] = {
#include "crt/supply_state.comp.spv.inc"
};
const uint32_t supply_resample[] = {
#include "crt/supply_resample.comp.spv.inc"
};
const uint32_t spot_h[] = {
#include "crt/spot_h.comp.spv.inc"
};
const uint32_t tube_h[] = {
#include "crt/tube_h.comp.spv.inc"
};
const uint32_t tube_v[] = {
#include "crt/tube_v.comp.spv.inc"
};
const uint32_t tube_v_growth[] = {
#include "crt/tube_v_growth.comp.spv.inc"
};
const uint32_t tube_scatter[] = {
#include "crt/tube_scatter.comp.spv.inc"
};
const uint32_t tube_scatter_x16[] = {
#include "crt/tube_scatter_x16.comp.spv.inc"
};
const uint32_t tube_scatter_y16[] = {
#include "crt/tube_scatter_y16.comp.spv.inc"
};
const uint32_t tube_mix[] = {
#include "crt/tube_mix.comp.spv.inc"
};
const uint32_t tube_lit[] = {
#include "crt/tube_lit.comp.spv.inc"
};
const uint32_t tube_persist[] = {
#include "crt/tube_persist.comp.spv.inc"
};
const uint32_t show_kernel[] = {
#include "crt/show_kernel.comp.spv.inc"
};
const uint32_t tube_scatter_tx[] = {
#include "crt/tube_scatter_tx.comp.spv.inc"
};
const uint32_t tube_h_rows[] = {
#include "crt/tube_h_rows.comp.spv.inc"
};
const uint32_t tube_lit_persist[] = {
#include "crt/tube_lit_persist.comp.spv.inc"
};
const uint32_t show_vert[] = {
#include "crt/show.vert.spv.inc"
};
// Fast path (CrtQuality::fast).
const uint32_t rf_fast[] = {
#include "crt/rf_fast.comp.spv.inc"
};
const uint32_t rx_stats_fast[] = {
#include "crt/rx_stats_fast.comp.spv.inc"
};
const uint32_t rx_agc_fast[] = {
#include "crt/rx_agc_fast.comp.spv.inc"
};
const uint32_t rx_decode_fast[] = {
#include "crt/rx_decode_fast.comp.spv.inc"
};
const uint32_t row_mean_fast[] = {
#include "crt/row_mean_fast.comp.spv.inc"
};
const uint32_t supply_fast[] = {
#include "crt/supply_fast.comp.spv.inc"
};
const uint32_t drive_post_fast[] = {
#include "crt/drive_post_fast.comp.spv.inc"
};
const uint32_t scatter_drive[] = {
#include "crt/scatter_drive.comp.spv.inc"
};
const uint32_t tube_h_fast[] = {
#include "crt/tube_h_fast.comp.spv.inc"
};
const uint32_t tube_v_fast[] = {
#include "crt/tube_v_fast.comp.spv.inc"
};
const uint32_t show_kernel_h[] = {
#include "crt/show_kernel_h.comp.spv.inc"
};
const uint32_t show_h_frag[] = {
#include "crt/show_h.frag.spv.inc"
};
const uint32_t show_frag[] = {
#include "crt/show.frag.spv.inc"
};
}  // namespace spv

struct KernelSource {
  const char* name;
  const uint32_t* code;
  size_t bytes;
  uint32_t lx, ly;  // local size (matches the shader's layout(local_size_*))
};
#define RN_K(n, lx, ly) KernelSource{#n, spv::n, sizeof(spv::n), lx, ly}
const KernelSource kKernels[] = {
    RN_K(rf_forward_tg, 512, 1),  RN_K(rf_inverse_tg, 512, 1),  RN_K(rf_init_codes, 64, 4), RN_K(rf_butterfly2, 32, 8),
    RN_K(rf_butterfly, 32, 8),    RN_K(rf_multiply, 32, 8),     RN_K(rf_extract, 32, 8),    RN_K(rx_reduce, 64, 1),
    RN_K(rx_agc, 1, 1),           RN_K(rx_prepare, 32, 8),      RN_K(rx_decode, 32, 8),     RN_K(raster_area, 32, 8),
    RN_K(supply_mean, 64, 1),     RN_K(supply_row, 64, 1),      RN_K(supply_state, 1, 1),   RN_K(supply_resample, 32, 8),
    RN_K(spot_h, 32, 8),          RN_K(tube_h, 32, 8),          RN_K(tube_v, 32, 8),        RN_K(tube_v_growth, 32, 8),
    RN_K(tube_scatter, 32, 8),    RN_K(tube_scatter_x16, 32, 8), RN_K(tube_scatter_y16, 32, 8), RN_K(tube_mix, 32, 8),
    RN_K(tube_lit, 32, 8),        RN_K(tube_persist, 32, 8),    RN_K(show_kernel, 16, 16),  RN_K(tube_scatter_tx, 256, 1),
    RN_K(tube_h_rows, 64, 1),       RN_K(tube_lit_persist, 32, 8),
    // Fast path.
    RN_K(rf_fast, 512, 1),        RN_K(rx_stats_fast, 64, 1),   RN_K(rx_agc_fast, 256, 1),  RN_K(rx_decode_fast, 256, 1),
    RN_K(row_mean_fast, 128, 1),  RN_K(supply_fast, 256, 1),    RN_K(drive_post_fast, 512, 1), RN_K(scatter_drive, 32, 8),
    RN_K(tube_h_fast, 384, 1),    RN_K(tube_v_fast, 32, 8),     RN_K(show_kernel_h, 16, 16),
};
#undef RN_K

constexpr uint32_t kPushBytes = 64;
constexpr uint32_t kBindings = 8;
const char* const kDeviceExtensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};

// Push-constant blocks (same layout as the GLSL declarations).
struct RFParams { int32_t hop, overlap, delay, blocks; float scale, signalLevel, noiseSigma; int32_t noiseKey; };
struct FFTParams { int32_t span; float signValue; int32_t blocks; int32_t pad; };
struct AGCParams { float lineSeconds, attack, release, minGain, maxGain; int32_t enabled, delay, pad; };
struct RasterParams { int32_t lines, decode, rgbSource, pad; };
struct SupplyParams {
  int32_t width, rows;
  float decay, v0, reff, imax, n, relax, reqScale, ablFraction, ilim, prime;  // prime: supply_state only
  float share[4];
};
struct SpotParams { int32_t width, rows, radius; float k1; };
struct TubeParams {
  int32_t width, height, ow, oh, xCount, yCount, gCount, levels;
  float gain, scatterFraction, keep;
  int32_t radius, depth, extra, pad1, pad2;
};
struct ShowParams { float dst[4]; float src[4]; int32_t ow, oh, tw, th; float ndc[4]; };
// Fast path.
struct PostParams { int32_t supply, spot; float k1; int32_t persistence, newSlot, depth, rows, rx; };
struct ScatterParams { int32_t rows, ry, pad0, pad1; float kappa[4]; };
struct FastTubeParams {
  int32_t height, ow, oh, xCount, gCount, hStride;
  float gain, keep;
  float amb[4], light[4];
};
static_assert(sizeof(SupplyParams) == 64 && sizeof(TubeParams) == 64 && sizeof(ShowParams) == 64 && sizeof(FastTubeParams) == 64,
              "push constant layout");

uint32_t groups(int n, uint32_t local) { return uint32_t((std::max(0, n) + int(local) - 1) / int(local)); }
}  // namespace

// ------------------------------------------------------------------ setup
const char* const* CrtRenderer::requiredDeviceExtensions(uint32_t* count) {
  *count = uint32_t(sizeof(kDeviceExtensions) / sizeof(kDeviceExtensions[0]));
  return kDeviceExtensions;
}

bool CrtRenderer::supported(VkPhysicalDevice phys, std::string* why) {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
  for (const char* want : kDeviceExtensions) {
    bool found = false;
    for (const auto& e : exts) found |= std::strcmp(e.extensionName, want) == 0;
    if (!found) {
      if (why) *why = std::string("missing ") + want;
      return false;
    }
  }
  return true;
}

CrtRenderer::~CrtRenderer() { shutdown(); }

uint32_t CrtRenderer::findMemory(uint32_t typeBits, VkMemoryPropertyFlags props, VkMemoryPropertyFlags avoid) {
  for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
    if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props &&
        !(memProps_.memoryTypes[i].propertyFlags & avoid))
      return i;
  return UINT32_MAX;
}

bool CrtRenderer::createBuffer(Buffer* b, VkDeviceSize size, bool hostVisible, bool preferDeviceLocal) {
  size = std::max<VkDeviceSize>(16, size);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(ctx_.device, &bci, nullptr, &b->buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(ctx_.device, b->buffer, &mr);
  uint32_t type = UINT32_MAX;
  const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  if (hostVisible) {
    if (preferDeviceLocal) type = findMemory(mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = findMemory(mr.memoryTypeBits, hv);
  } else {
    type = findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == UINT32_MAX) type = findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = findMemory(mr.memoryTypeBits, 0);
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = type;
  VkResult r = type == UINT32_MAX ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkAllocateMemory(ctx_.device, &mai, nullptr, &b->memory);
  if (r != VK_SUCCESS && hostVisible && preferDeviceLocal) {
    // The small host-visible VRAM window (e.g. 256 MB BAR) is full: system memory instead.
    type = findMemory(mr.memoryTypeBits, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type != UINT32_MAX) {
      mai.memoryTypeIndex = type;
      r = vkAllocateMemory(ctx_.device, &mai, nullptr, &b->memory);
    }
  }
  if (r != VK_SUCCESS) {
    vkDestroyBuffer(ctx_.device, b->buffer, nullptr);
    b->buffer = VK_NULL_HANDLE;
    return false;
  }
  vkBindBufferMemory(ctx_.device, b->buffer, b->memory, 0);
  b->size = size;
  if (hostVisible) vkMapMemory(ctx_.device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map);
  return true;
}

void CrtRenderer::destroyBuffer(Buffer* b) {
  if (!ctx_.device) return;
  if (b->map) vkUnmapMemory(ctx_.device, b->memory);
  if (b->buffer) vkDestroyBuffer(ctx_.device, b->buffer, nullptr);
  if (b->memory) vkFreeMemory(ctx_.device, b->memory, nullptr);
  *b = Buffer();
}

bool CrtRenderer::uploadConst(Buffer* b, const void* data, size_t bytes) {
  if (!createBuffer(b, bytes, true)) return false;
  if (bytes) std::memcpy(b->map, data, bytes);
  return true;
}

bool CrtRenderer::init(const CrtVulkanContext& ctx, VkRenderPass showPass, std::string* error) {
  ctx_ = ctx;
  auto fail = [&](const std::string& m) {
    if (error) *error = "CRT (Vulkan): " + m;
    shutdown();
    return false;
  };
  if (!supported(ctx.physicalDevice, error)) return fail(error ? *error : "unsupported device");
  vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &memProps_);
  pushDescriptor_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(ctx.device, "vkCmdPushDescriptorSetKHR"));
  if (!pushDescriptor_) return fail("vkCmdPushDescriptorSetKHR not enabled on the device");
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
  sharedFFT_ = props.limits.maxComputeSharedMemorySize >= 4096 * 8 && props.limits.maxComputeWorkGroupInvocations >= 512;
  // tube_h_fast stages up to 30.9 KB in shared memory; rf_fast / drive_post_fast use 512 invocations.
  fastSupported_ = props.limits.maxComputeSharedMemorySize >= 32768 && props.limits.maxComputeWorkGroupInvocations >= 512 &&
                   props.limits.maxComputeWorkGroupSize[0] >= 512;
  timestampPeriod_ = props.limits.timestampPeriod;

  // Layouts: one push-descriptor set of storage buffers + 64 bytes of push constants.
  VkDescriptorSetLayoutBinding bind[kBindings]{};
  for (uint32_t i = 0; i < kBindings; ++i) {
    bind[i].binding = i;
    bind[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bind[i].descriptorCount = 1;
    bind[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  lci.bindingCount = kBindings;
  lci.pBindings = bind;
  if (vkCreateDescriptorSetLayout(ctx.device, &lci, nullptr, &setLayout_) != VK_SUCCESS) return fail("set layout");
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &setLayout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(ctx.device, &pli, nullptr, &layout_) != VK_SUCCESS) return fail("pipeline layout");

  auto module = [&](const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = bytes;
    mci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx.device, &mci, nullptr, &m);
    return m;
  };
  for (const KernelSource& k : kKernels) {
    VkShaderModule m = module(k.code, k.bytes);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = m;
    cpi.stage.pName = "main";
    cpi.layout = layout_;
    Kernel kern;
    kern.lx = k.lx;
    kern.ly = k.ly;
    VkResult r = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &kern.pipeline);
    vkDestroyShaderModule(ctx.device, m, nullptr);
    if (r != VK_SUCCESS) return fail(std::string("kernel ") + k.name);
    kernels_[k.name] = kern;
  }

  if (showPass) {
    VkDescriptorSetLayoutBinding sb{};
    sb.binding = 0;
    sb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sb.descriptorCount = 1;
    sb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sci.bindingCount = 1;
    sci.pBindings = &sb;
    if (vkCreateDescriptorSetLayout(ctx.device, &sci, nullptr, &showSetLayout_) != VK_SUCCESS) return fail("show set layout");
    VkPushConstantRange spr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShowParams)};
    VkPipelineLayoutCreateInfo spl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    spl.setLayoutCount = 1;
    spl.pSetLayouts = &showSetLayout_;
    spl.pushConstantRangeCount = 1;
    spl.pPushConstantRanges = &spr;
    if (vkCreatePipelineLayout(ctx.device, &spl, nullptr, &showLayout_) != VK_SUCCESS) return fail("show layout");
    VkShaderModule vs = module(spv::show_vert, sizeof(spv::show_vert)), fs = module(spv::show_frag, sizeof(spv::show_frag));
    VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = showLayout_;
    gp.renderPass = showPass;
    VkResult r = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &gp, nullptr, &showPipeline_);
    // The fast path's half-float output.
    VkShaderModule fsH = module(spv::show_h_frag, sizeof(spv::show_h_frag));
    st[1].module = fsH;
    if (r == VK_SUCCESS) r = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &gp, nullptr, &showPipelineH_);
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);
    vkDestroyShaderModule(ctx.device, fsH, nullptr);
    if (r != VK_SUCCESS) return fail("show pipeline");
  }

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = ctx.queueFamily;
  vkCreateCommandPool(ctx.device, &cpi, nullptr, &syncPool_);

  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &qn, qf.data());
  if (ctx.queueFamily < qn && qf[ctx.queueFamily].timestampValidBits > 0 && timestampPeriod_ > 0) {
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = kQueryPairs * 2;
    vkCreateQueryPool(ctx.device, &qci, nullptr, &queries_);
  }

  // Receiver (fixed size) + constant tables.
  const int n = crt::rf::kFFTSize, blocks = crt::rf::kBlocks;
  bool ok = createBuffer(&ping_, VkDeviceSize(n) * blocks * 8, false) && createBuffer(&pong_, VkDeviceSize(n) * blocks * 8, false) &&
            uploadConst(&kernelSpec_, crt::rf::kernelSpectrum().data(), crt::rf::kernelSpectrum().size() * 4) &&
            uploadConst(&twiddles_, crt::rf::twiddles().data(), crt::rf::twiddles().size() * 4) &&
            uploadConst(&volts_, crt::composite::voltageLUT().data(), crt::composite::voltageLUT().size() * 4);
  // receiver-webgl.mjs basis: (cos, sin)(2*pi*n/12) for n < 2728, float32.
  std::vector<float> basis(2728 * 2);
  for (int i = 0; i < 2728; ++i) {
    basis[size_t(i) * 2] = float(std::cos(2 * 3.141592653589793 * double(i) / 12));
    basis[size_t(i) * 2 + 1] = float(std::sin(2 * 3.141592653589793 * double(i) / 12));
  }
  ok = ok && createBuffer(&carrierReal_, VkDeviceSize(crt::rf::kMaxSamples) * 4, false) &&
       uploadConst(&kernelSpecReal_, crt::rf::kernelSpectrumReal().data(), crt::rf::kernelSpectrumReal().size() * 4) &&
       createBuffer(&agcRows_, 240 * 16, false);
  ok = ok && uploadConst(&basis_, basis.data(), basis.size() * 4) && createBuffer(&carrier_, 2728 * 240 * 8, false) &&
       createBuffer(&stats_, 240 * 16, false) && createBuffer(&gains_, 240 * 4, false) && createBuffer(&agcState_, 16, false) &&
       createBuffer(&prepared_, 682 * 240 * 16, false) && createBuffer(&rxOut_, 512 * 240 * 16, false) &&
       createBuffer(&driveIn_, 512 * 240 * 16, true, false);
  for (int i = 0; i < kInputSlots && ok; ++i)
    ok = createBuffer(&codeBufs_[i], 256 * 240 * 2, true, false) && createBuffer(&phaseBufs_[i], 240 * 4, true, false) &&
         createBuffer(&rgbBufs_[i], 256 * 240 * 4, true, false) &&
         createBuffer(&weightBufs_[i], VkDeviceSize(crt::phosphor::depth()) * 16, true, false);
  if (!ok) return fail("buffers");
  resetReceiverPending_ = resetSupplyPending_ = true;
  ready_ = true;
  return true;
}

void CrtRenderer::shutdown() {
  {
    std::lock_guard<std::mutex> lk(lock_);
    stopBuilder_ = true;
    buildCv_.notify_all();
  }
  if (builder_.joinable()) builder_.join();
  if (!ctx_.device) return;
  vkDeviceWaitIdle(ctx_.device);
  if (builtTube_) destroyTube(std::move(builtTube_));
  if (tube_) destroyTube(std::move(tube_));
  for (auto& r : retired_) destroyTube(std::move(r.tube));
  retired_.clear();
  for (Buffer* b : {&ping_, &pong_, &kernelSpec_, &twiddles_, &volts_, &basis_, &carrier_, &stats_, &gains_, &agcState_, &prepared_,
                    &rxOut_, &driveIn_, &rasterOut_, &supplyMeans_, &supplyRow_, &supplyOut_, &spotOut_, &supplyState_[0],
                    &supplyState_[1], &carrierReal_, &kernelSpecReal_, &agcRows_})
    destroyBuffer(b);
  for (int i = 0; i < kInputSlots; ++i)
    for (Buffer* b : {&codeBufs_[i], &phaseBufs_[i], &rgbBufs_[i], &weightBufs_[i]}) destroyBuffer(b);
  for (auto& [name, k] : kernels_) vkDestroyPipeline(ctx_.device, k.pipeline, nullptr);
  kernels_.clear();
  if (showPipeline_) vkDestroyPipeline(ctx_.device, showPipeline_, nullptr);
  if (showPipelineH_) vkDestroyPipeline(ctx_.device, showPipelineH_, nullptr);
  showPipelineH_ = VK_NULL_HANDLE;
  if (layout_) vkDestroyPipelineLayout(ctx_.device, layout_, nullptr);
  if (showLayout_) vkDestroyPipelineLayout(ctx_.device, showLayout_, nullptr);
  if (setLayout_) vkDestroyDescriptorSetLayout(ctx_.device, setLayout_, nullptr);
  if (showSetLayout_) vkDestroyDescriptorSetLayout(ctx_.device, showSetLayout_, nullptr);
  if (syncPool_) vkDestroyCommandPool(ctx_.device, syncPool_, nullptr);
  if (queries_) vkDestroyQueryPool(ctx_.device, queries_, nullptr);
  if (profileQueries_) vkDestroyQueryPool(ctx_.device, profileQueries_, nullptr);
  profileQueries_ = VK_NULL_HANDLE;
  showPipeline_ = VK_NULL_HANDLE;
  layout_ = showLayout_ = VK_NULL_HANDLE;
  setLayout_ = showSetLayout_ = VK_NULL_HANDLE;
  syncPool_ = VK_NULL_HANDLE;
  queries_ = VK_NULL_HANDLE;
  ctx_ = CrtVulkanContext();
  ready_ = false;
}

// ------------------------------------------------------------------ configuration
void CrtRenderer::configure(const CrtSettings& newSettings, int outputWidth, int outputHeight, bool synchronous) {
  CrtSettings s = newSettings.sanitized();
  if (s.lines != settings_.lines) stageLines_ = 0;  // raster/supply/spot rebuilt (nesterm: rasterChanged)
  settings_ = s;
  TubeKey key{std::max(64, outputWidth), std::max(48, outputHeight), s.lines, s.beamGrowth, s.ambientLux,
              quality == CrtQuality::fast && fastSupported_ ? CrtQuality::fast : CrtQuality::reference};
  if (synchronous) {
    if (!tube_ || !(tube_->key == key)) {
      auto t = makeTube(key);
      if (t) {
        retire(std::move(tube_));
        tube_ = std::move(t);
        appliedPersistence_ = -1;
        hasOutput_ = false;
      }
    }
    return;
  }
  std::lock_guard<std::mutex> lk(lock_);
  // Latest wanted plan: queued > being built > built but not adopted > in use.
  const TubeKey* target = hasPending_ ? &pendingKey_ : hasInFlight_ ? &inFlightKey_ : builtTube_ ? &builtTube_->key
                                                                                   : tube_ ? &tube_->key : nullptr;
  if (target && *target == key) return;
  pendingKey_ = key;
  hasPending_ = true;
  if (!builder_.joinable()) builder_ = std::thread([this] { buildLoop(); });
  buildCv_.notify_all();
}

bool CrtRenderer::planPending() {
  std::lock_guard<std::mutex> lk(lock_);
  return hasPending_ || hasInFlight_ || builtTube_ != nullptr;
}

void CrtRenderer::buildLoop() {
  std::unique_lock<std::mutex> lk(lock_);
  for (;;) {
    buildCv_.wait(lk, [&] { return stopBuilder_ || hasPending_; });
    if (stopBuilder_) return;
    TubeKey key = pendingKey_;
    hasPending_ = false;
    inFlightKey_ = key;
    hasInFlight_ = true;
    lk.unlock();
    auto t = makeTube(key);
    lk.lock();
    hasInFlight_ = false;
    if (t && !hasPending_) {
      if (builtTube_) {
        auto old = std::move(builtTube_);
        lk.unlock();
        destroyTube(std::move(old));  // never adopted: no GPU work references it
        lk.lock();
      }
      builtTube_ = std::move(t);
    } else if (t) {
      lk.unlock();
      destroyTube(std::move(t));
      lk.lock();
    }
  }
}

std::unique_ptr<CrtRenderer::Tube> CrtRenderer::makeTube(const TubeKey& k) {
  crt::tube::Plan plan = crt::tube::makePlan(k.lines, k.ow, k.oh, k.ambient, k.growth ? 1 : 0);
  auto t = std::make_unique<Tube>();
  t->key = k;
  t->width = plan.width;
  t->height = plan.height;
  t->ow = plan.outputWidth;
  t->oh = plan.outputHeight;
  t->xCount = plan.xCount;
  t->yCount = plan.yCount;
  t->gCount = plan.gCount;
  t->levels = plan.growthLevels;
  t->radius[0] = plan.radius[0];
  t->radius[1] = plan.radius[1];
  t->gain = plan.gain;
  t->scatterFraction = plan.scatterFraction;
  t->growth = plan.growth;
  const std::vector<float>& vmap = plan.growth ? plan.gmap : plan.ymap;
  const VkDeviceSize img = VkDeviceSize(t->ow) * t->oh * 16;
  if (k.quality == CrtQuality::fast) {
    crt::tube::FastPlan fp = crt::tube::fastPlan(plan);
    if (plan.xCount <= 24 && fp.maxSpan <= 160) {
      crt::tube::DriveScatter sc = crt::tube::driveScatter(plan);
      t->fast = true;
      t->fastTaps = fp.taps;
      t->hStride = fp.maxSpan;
      t->srx = sc.rx;
      t->sry = sc.ry;
      for (int c = 0; c < 3; ++c) t->kappa[c] = sc.kappa[c];
      const VkDeviceSize drive = VkDeviceSize(512) * plan.height * 8;
      bool ok = uploadConst(&t->xmapFast, fp.xmap.data(), fp.xmap.size() * 4) && uploadConst(&t->tiles, fp.tiles.data(), fp.tiles.size() * 4) &&
                uploadConst(&t->vrows, fp.vrows.data(), fp.vrows.size() * 4) && uploadConst(&t->vcoef, fp.vcoef.data(), fp.vcoef.size() * 4) &&
                uploadConst(&t->colinv, fp.colinv.data(), fp.colinv.size() * 4) && uploadConst(&t->swx, sc.wx.data(), sc.wx.size() * 4) &&
                uploadConst(&t->swy, sc.wy.data(), sc.wy.size() * 4) &&
                createBuffer(&t->hplanes, VkDeviceSize(t->ow) * t->height * 2 * 16, false) &&
                createBuffer(&t->outH, VkDeviceSize(t->ow) * t->oh * 8, false) && createBuffer(&t->scatterTmp, drive, false) &&
                createBuffer(&t->scatterSrc, drive, false);
      if (!ok) {
        destroyTube(std::move(t));
        return nullptr;
      }
      return t;
    }
  }
  bool ok = uploadConst(&t->xmap, plan.xmap.data(), plan.xmap.size() * 4) && uploadConst(&t->vmap, vmap.data(), vmap.size() * 4) &&
            uploadConst(&t->colsum, plan.colsum.data(), plan.colsum.size() * 4) &&
            uploadConst(&t->kx, plan.kernel[0].data(), plan.kernel[0].size() * 4) &&
            uploadConst(&t->ky, plan.kernel[1].data(), plan.kernel[1].size() * 4) &&
            uploadConst(&t->ambient, plan.ambient.data(), plan.ambient.size() * 4) &&
            createBuffer(&t->horizontal, VkDeviceSize(t->ow) * t->height * 2 * 16, false) && createBuffer(&t->emission, img, false) &&
            createBuffer(&t->scatterX, img, false) && createBuffer(&t->scatterY, img, false);
  if (!ok) {
    destroyTube(std::move(t));
    return nullptr;
  }
  return t;
}

void CrtRenderer::destroyTube(std::unique_ptr<Tube> t) {
  if (!t) return;
  for (Buffer* b : {&t->xmap, &t->vmap, &t->colsum, &t->kx, &t->ky, &t->ambient, &t->horizontal, &t->emission, &t->scatterX,
                    &t->scatterY, &t->ring, &t->xmapFast, &t->tiles, &t->vrows, &t->vcoef, &t->colinv, &t->swx, &t->swy, &t->hplanes,
                    &t->outH, &t->scatterTmp, &t->scatterSrc})
    destroyBuffer(b);
}

void CrtRenderer::retire(std::unique_ptr<Tube> t) {
  if (!t) return;
  // In-flight command buffers may still read it: freed after a few more encodes (the caller has
  // at most kInputSlots-1 frames in flight) or at shutdown.
  retired_.push_back(Retired{std::move(t), encodes_ + kInputSlots});
}

void CrtRenderer::collectRetired() {
  for (size_t i = 0; i < retired_.size();) {
    if (encodes_ >= retired_[i].after) {
      destroyTube(std::move(retired_[i].tube));
      retired_.erase(retired_.begin() + long(i));
    } else {
      ++i;
    }
  }
}

bool CrtRenderer::ensureStages() {
  int rows = settings_.lines;
  if (stageLines_ == rows) return true;
  if (stageLines_ != 0 || rasterOut_.buffer) {
    // Line count changed: the old stage buffers may be referenced by frames in flight.
    vkDeviceWaitIdle(ctx_.device);
    for (Buffer* b : {&rasterOut_, &supplyMeans_, &supplyRow_, &supplyOut_, &spotOut_, &supplyState_[0], &supplyState_[1]}) destroyBuffer(b);
  }
  const VkDeviceSize img = VkDeviceSize(512) * rows * 16;
  bool ok = createBuffer(&rasterOut_, img, false) && createBuffer(&supplyOut_, img, false) && createBuffer(&spotOut_, img, false) &&
            createBuffer(&supplyMeans_, VkDeviceSize(rows) * 16, false) && createBuffer(&supplyRow_, VkDeviceSize(rows) * 16, false) &&
            createBuffer(&supplyState_[0], 16, false) && createBuffer(&supplyState_[1], 16, false);
  if (!ok) return false;
  stageLines_ = rows;
  resetSupplyPending_ = true;
  return true;
}

std::string CrtRenderer::planInfo() const {
  if (!tube_) return "";
  char b[160];
  std::snprintf(b, sizeof b, "xCount=%d yCount=%d gCount=%d levels=%d radius=%d,%d", tube_->xCount, tube_->yCount, tube_->gCount,
                tube_->levels, tube_->radius[0], tube_->radius[1]);
  return b;
}

int CrtRenderer::outputWidth() const { return tube_ ? tube_->ow : 0; }
bool CrtRenderer::fastActive() const { return tube_ && tube_->fast; }
int CrtRenderer::outputHeight() const { return tube_ ? tube_->oh : 0; }

// ------------------------------------------------------------------ recording helpers
void CrtRenderer::barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void CrtRenderer::dispatch(VkCommandBuffer cmd, const char* name, std::initializer_list<const Buffer*> buffers, const void* pc,
                           uint32_t pcSize, uint32_t gx, uint32_t gy) {
  const Kernel& k = kernels_.at(name);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
  VkDescriptorBufferInfo infos[kBindings]{};
  VkWriteDescriptorSet writes[kBindings]{};
  uint32_t n = 0;
  for (const Buffer* b : buffers) {
    infos[n] = VkDescriptorBufferInfo{b->buffer, 0, VK_WHOLE_SIZE};
    writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[n].dstBinding = n;
    writes[n].descriptorCount = 1;
    writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[n].pBufferInfo = &infos[n];
    ++n;
  }
  pushDescriptor_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, n, writes);
  if (pc && pcSize) vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize, pc);
  vkCmdDispatch(cmd, gx, gy, 1);
  barrier(cmd);
  if (profiling_ && profileNames_.size() < 63) {
    profileNames_.push_back(name);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profileQueries_, uint32_t(profileNames_.size()));
  }
}

std::vector<std::pair<std::string, double>> CrtRenderer::profileTimes() {
  std::vector<std::pair<std::string, double>> out;
  if (!profileQueries_ || profileNames_.empty()) return out;
  std::vector<uint64_t> ts(profileNames_.size() + 1);
  if (vkGetQueryPoolResults(ctx_.device, profileQueries_, 0, uint32_t(ts.size()), ts.size() * 8, ts.data(), 8,
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
    return out;
  for (size_t i = 0; i < profileNames_.size(); ++i)
    out.push_back({profileNames_[i], double(ts[i + 1] - ts[i]) * timestampPeriod_ * 1e-9});
  return out;
}

// ------------------------------------------------------------------ per-frame encoding
bool CrtRenderer::encode(VkCommandBuffer cmd, const Input& input, uint64_t ordinal) {
  if (!ready_) return false;
  {
    std::lock_guard<std::mutex> lk(lock_);
    if (builtTube_) {
      retire(std::move(tube_));
      tube_ = std::move(builtTube_);
      appliedPersistence_ = -1;
      hasOutput_ = false;
    }
  }
  if (!tube_ || !ensureStages()) return false;
  if (input.kind == InputKind::codes && !input.codes) return false;
  if (input.kind == InputKind::rgb && !input.rgb) return false;
  if (input.kind == InputKind::drive && !input.drive) return false;
  encodes_ += 1;
  collectRetired();
  Tube& t = *tube_;
  int64_t ord = int64_t(std::min<uint64_t>(ordinal, uint64_t(INT64_MAX)));
  if (hasLastOrdinal_ && ord <= lastOrdinal_) {
    resetReceiverPending_ = true;
    resetSupplyPending_ = true;
    slots_.clear();
  }
  lastOrdinal_ = ord;
  hasLastOrdinal_ = true;
  bool ringClearPending = false;
  int wantPersistence = settings_.persistence ? 1 : 0;
  if (wantPersistence != appliedPersistence_) {
    // tube.setPersistence(): allocate the ring on demand; always restarts history.
    // Reference: half4 tube-output slots. Fast: half4 drive slots (512 x lines).
    if (wantPersistence && !t.ring.buffer &&
        !createBuffer(&t.ring, VkDeviceSize(crt::phosphor::depth()) * (t.fast ? VkDeviceSize(512) * t.height : VkDeviceSize(t.ow) * t.oh) * 8,
                      false))
      return false;
    // Zero like a fresh WebGL texture: unused slots have weight 0, and 0 * garbage could be NaN.
    if (wantPersistence && !t.ringCleared) ringClearPending = true;
    slots_.clear();
    appliedPersistence_ = wantPersistence;
  }
  if (settings_.persistence && t.ring.buffer && !t.ringCleared) ringClearPending = true;
  const int slot = inputSlot_;
  inputSlot_ = (inputSlot_ + 1) % kInputSlots;
  const int rows = settings_.lines;

  // Timestamps (GPU time of the CRT passes) + ordering against the previous frame's show pass.
  int pair = -1;
  if (queries_) {
    pair = queryNext_;
    queryNext_ = (queryNext_ + 1) % kQueryPairs;
    queryPending_.erase(std::remove(queryPending_.begin(), queryPending_.end(), pair), queryPending_.end());
    vkCmdResetQueryPool(cmd, queries_, uint32_t(pair * 2), 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, uint32_t(pair * 2));
  }
  profiling_ = profile && queries_;
  if (profiling_) {
    if (!profileQueries_) {
      VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qci.queryCount = 64;
      vkCreateQueryPool(ctx_.device, &qci, nullptr, &profileQueries_);
    }
    profileNames_.clear();
    vkCmdResetQueryPool(cmd, profileQueries_, 0, 64);
  }
  {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  }
  // State resets (physical-worker 'reset': receiver.reset(), supply.reset()), in command order.
  if (resetReceiverPending_) {
    const float st[4] = {float(crt::agc::kInitialGain), 0, 0, 0};
    vkCmdUpdateBuffer(cmd, agcState_.buffer, 0, sizeof(st), st);
    resetReceiverPending_ = false;
  }
  if (resetSupplyPending_) {
    const float st[4] = {float(crt::supply::kV0), 0, 1, 1};
    vkCmdUpdateBuffer(cmd, supplyState_[0].buffer, 0, sizeof(st), st);
    vkCmdUpdateBuffer(cmd, supplyState_[1].buffer, 0, sizeof(st), st);
    supplyCurrent_ = 0;
    resetSupplyPending_ = false;
    primeSupply_ = true;
  }
  if (ringClearPending) {
    vkCmdFillBuffer(cmd, t.ring.buffer, 0, VK_WHOLE_SIZE, 0);
    t.ringCleared = true;
  }
  barrier(cmd);
  if (profiling_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profileQueries_, 0);
  auto finish = [&] {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         1, &mb, 0, nullptr, 0, nullptr);
    if (pair >= 0) {
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, uint32_t(pair * 2 + 1));
      queryPending_.push_back(pair);
    }
    hasOutput_ = true;
    return true;
  };
  usedCodes_ = input.kind == InputKind::codes;
  if (t.fast) {
    encodeFast(cmd, input, ordinal, ord, slot);
    return finish();
  }

  const Buffer* drive = nullptr;
  if (input.kind == InputKind::codes) {
    std::memcpy(codeBufs_[slot].map, input.codes, 256 * 240 * 2);
    auto ph = crt::composite::rowPhasesForBurst(input.burstPhase);
    std::memcpy(phaseBufs_[slot].map, ph.data(), sizeof(ph));
    // M3-NOISE: receiver thermal noise from the antenna level; seed 1, keyed by frame ordinal.
    double sigma = crt::rf::noiseSigma(crt::rf::carrierToNoiseDb(settings_.antennaDbuv), 1);
    RFParams rp{crt::rf::kHop, crt::rf::kOverlap, crt::rf::filter().delaySamples, crt::rf::kBlocks,
                float(crt::rf::kModulationDepth / (crt::composite::kWhite - crt::composite::kSync)), 1.0f,
                noiseEnabled ? float(sigma) : 0.0f, crt::rf::noiseKey(1, ordinal)};
    const uint32_t blocks = uint32_t(crt::rf::kBlocks);
    if (useFastFFT && sharedFFT_) {
      dispatch(cmd, "rf_forward_tg", {&ping_, &codeBufs_[slot], &phaseBufs_[slot], &volts_, &twiddles_}, &rp, sizeof rp, blocks);
      dispatch(cmd, "rf_inverse_tg", {&ping_, &carrier_, &kernelSpec_, &twiddles_}, &rp, sizeof rp, blocks);
    } else {
      const Kernel& ki = kernels_.at("rf_init_codes");
      dispatch(cmd, "rf_init_codes", {&ping_, &codeBufs_[slot], &phaseBufs_[slot], &volts_}, &rp, sizeof rp, groups(4096, ki.lx),
               groups(int(blocks), ki.ly));
      const Buffer* a = &ping_;
      const Buffer* b = &pong_;
      auto fft = [&](float sign) {
        for (int span = 2; span <= 4096; span *= 4) {
          FFTParams fp{span, sign, int32_t(blocks), 0};
          const char* name = span * 2 <= 4096 ? "rf_butterfly2" : "rf_butterfly";
          dispatch(cmd, name, {a, b, &twiddles_}, &fp, sizeof fp, groups(4096, 32), groups(int(blocks), 8));
          std::swap(a, b);
        }
      };
      fft(-1);
      FFTParams mp{0, 0, int32_t(blocks), 0};
      dispatch(cmd, "rf_multiply", {a, b, &kernelSpec_}, &mp, sizeof mp, groups(4096, 32), groups(int(blocks), 8));
      std::swap(a, b);
      fft(1);
      dispatch(cmd, "rf_extract", {a, &carrier_}, &rp, sizeof rp, groups(2728, 32), groups(240, 8));
    }
    dispatch(cmd, "rx_reduce", {&carrier_, &basis_, &stats_}, nullptr, 0, groups(240, 64));
    AGCParams ap{float(double(crt::composite::kLineSamples) / crt::rf::sampleRateHz()), float(crt::agc::kAttackSeconds),
                 float(crt::agc::kReleaseSeconds), float(crt::agc::kMinGain), float(crt::agc::kMaxGain), 1,
                 crt::rf::filter().delaySamples, 0};
    dispatch(cmd, "rx_agc", {&stats_, &gains_, &agcState_}, &ap, sizeof ap, 1);
    dispatch(cmd, "rx_prepare", {&carrier_, &basis_, &stats_, &prepared_}, nullptr, 0, groups(682, 32), groups(240, 8));
    dispatch(cmd, "rx_decode", {&prepared_, &stats_, &gains_, &rxOut_}, nullptr, 0, groups(512, 32), groups(240, 8));
    drive = &rxOut_;
  } else if (input.kind == InputKind::rgb) {
    std::memcpy(rgbBufs_[slot].map, input.rgb, 256 * 240 * 4);
    RasterParams rp2{rows, 1, 1, 0};
    dispatch(cmd, "raster_area", {&rxOut_, &rgbBufs_[slot], &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  } else {
    std::memcpy(driveIn_.map, input.drive, 512 * 240 * 16);
    drive = &driveIn_;
  }
  if (input.kind != InputKind::rgb && rows != 240) {
    // RasterWebGL.process(texture): the native 240-line RF path passes through untouched.
    RasterParams rp2{rows, 0, 0, 0};
    dispatch(cmd, "raster_area", {drive, drive, &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  }

  // M4c: anode-voltage load and ABL act on the drive before the tube.
  if (settings_.supply) {
    namespace f = crt::supply;
    double line = f::kLineS * f::kVisibleLines / double(rows);
    SupplyParams sp{512,
                    rows,
                    float(std::exp(-line / f::kTauS)),
                    float(f::kV0),
                    float(f::kReff),
                    float(f::kImax),
                    float(f::kN),
                    float(std::exp(-(f::kTotalLines - f::kVisibleLines) * f::kLineS / f::kTauS)),
                    float(f::kVisibleLines / f::kTotalLines / double(rows)),
                    float(-std::expm1(-f::kFrameS / f::kTauAblS)),
                    float(f::kIlim),
                    0,
                    {float(f::kShare[0]), float(f::kShare[1]), float(f::kShare[2]), 0}};
    const Buffer* now = &supplyState_[supplyCurrent_];
    const Buffer* next = &supplyState_[1 - supplyCurrent_];
    dispatch(cmd, "supply_mean", {drive, &supplyMeans_}, &sp, sizeof sp, groups(rows, 64));
    if (primeSupply_) {
      // Start of history: this frame's steady state into `next`, which then is the state in force.
      SupplyParams pp = sp;
      pp.prime = 1;
      dispatch(cmd, "supply_state", {&supplyMeans_, now, next}, &pp, sizeof pp, 1);
      std::swap(now, next);
      supplyCurrent_ = 1 - supplyCurrent_;
      primeSupply_ = false;
    }
    dispatch(cmd, "supply_row", {&supplyMeans_, now, &supplyRow_}, &sp, sizeof sp, groups(rows, 64));
    dispatch(cmd, "supply_state", {&supplyMeans_, now, next}, &sp, sizeof sp, 1);
    dispatch(cmd, "supply_resample", {drive, &supplyRow_, &supplyOut_}, &sp, sizeof sp, groups(512, 32), groups(rows, 8));
    supplyCurrent_ = 1 - supplyCurrent_;
    drive = &supplyOut_;
  }
  // M4C-SPOT-H: horizontal counterpart of the beam-current spot growth (same toggle).
  if (settings_.beamGrowth && !disableSpotH) {
    SpotParams spp{512, rows, crt::tube::kSpotHRadius, float(crt::tube::extraSigmaSamples(1, 1, 512))};
    dispatch(cmd, "spot_h", {drive, &spotOut_}, &spp, sizeof spp, groups(512, 32), groups(rows, 8));
    drive = &spotOut_;
  }
  lastDrive_ = drive;

  // Tube.
  const int ow = t.ow, oh = t.oh;
  TubeParams tp{t.width, t.height, ow, oh, t.xCount, t.yCount, t.gCount, t.levels, float(t.gain), float(t.scatterFraction),
                float(1 - t.scatterFraction), 0, crt::phosphor::depth(), 0, 0, 0};
  if (useFastScatter && t.xCount <= 16)  // taps in registers over 8 rows (same arithmetic as tube_h)
    dispatch(cmd, "tube_h_rows", {drive, &t.xmap, &t.horizontal}, &tp, sizeof tp, groups(ow, 64), 2 * uint32_t(groups(t.height, 8)));
  else
    dispatch(cmd, "tube_h", {drive, &t.xmap, &t.horizontal}, &tp, sizeof tp, groups(ow, 32), groups(t.height * 2, 8));
  if (t.growth)
    dispatch(cmd, "tube_v_growth", {&t.horizontal, &t.vmap, &t.colsum, &t.emission}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
  else
    dispatch(cmd, "tube_v", {&t.horizontal, &t.vmap, &t.emission}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
  for (int axis = 0; axis < 2; ++axis) {
    tp.radius = t.radius[axis];
    tp.extra = axis;
    const Buffer* from = axis == 0 ? &t.emission : &t.scatterX;
    const Buffer* to = axis == 0 ? &t.scatterX : &t.scatterY;
    const Buffer* w = axis == 0 ? &t.kx : &t.ky;
    if (useFastScatter && axis == 0 && tp.radius <= 64) {
      // Shared-memory row tile (same taps, order and weights as tube_scatter).
      dispatch(cmd, "tube_scatter_tx", {from, w, to}, &tp, sizeof tp, groups(ow, 256), uint32_t(oh));
    } else if (useFastScatter) {
      if (axis == 0)
        dispatch(cmd, "tube_scatter_x16", {from, w, to}, &tp, sizeof tp, groups((ow + 15) / 16, 32), groups(oh, 8));
      else
        dispatch(cmd, "tube_scatter_y16", {from, w, to}, &tp, sizeof tp, groups(ow, 32), groups((oh + 15) / 16, 8));
    } else {
      dispatch(cmd, "tube_scatter", {from, w, to}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
    }
  }
  tp.extra = 0;
  // The x-scatter buffer is free again: it doubles as the output image.
  const Buffer* out = &t.scatterX;
  if (settings_.persistence && t.ring.buffer) {
    const int depth = crt::phosphor::depth();
    const int64_t next = ord;
    if (!slots_.empty() && next <= slots_.front().ordinal) slots_.clear();
    std::vector<Slot> kept;
    for (const Slot& s : slots_) {
      int64_t newer = kept.empty() ? next : kept.back().ordinal;
      if (next - newer + 1 <= int64_t(depth - 1)) kept.push_back(s);
      else break;
    }
    slots_ = kept;
    int r = 0;
    for (;;) {
      bool used = false;
      for (const Slot& s : kept) used |= s.ring == r;
      if (!used) break;
      ++r;
    }
    slots_.insert(slots_.begin(), Slot{r, next});
    std::vector<float> w = persistenceWeights();
    std::memcpy(weightBufs_[slot].map, w.data(), w.size() * 4);
    tp.extra = r;
    if (useFastScatter) {
      // litprog + persistprog fused (same values: the new slot is read as the half it stores).
      dispatch(cmd, "tube_lit_persist", {&t.emission, &t.scatterY, &t.ring, &t.ambient, out, &weightBufs_[slot]}, &tp, sizeof tp,
               groups(ow, 32), groups(oh, 8));
    } else {
      dispatch(cmd, "tube_lit", {&t.emission, &t.scatterY, &t.ring}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
      tp.extra = 0;
      dispatch(cmd, "tube_persist", {&t.ring, &t.ambient, out, &weightBufs_[slot]}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
    }
    tp.extra = 0;
  } else {
    dispatch(cmd, "tube_mix", {&t.emission, &t.scatterY, &t.ambient, out}, &tp, sizeof tp, groups(ow, 32), groups(oh, 8));
  }
  return finish();
}

void CrtRenderer::encodeFast(VkCommandBuffer cmd, const Input& input, uint64_t ordinal, int64_t ord, int slot) {
  Tube& t = *tube_;
  const int rows = settings_.lines;
  const Buffer* drive = nullptr;
  bool meansReady = false;  // supplyMeans_ already holds this frame's row means (rx_decode_fast)
  if (input.kind == InputKind::codes) {
    std::memcpy(codeBufs_[slot].map, input.codes, 256 * 240 * 2);
    auto ph = crt::composite::rowPhasesForBurst(input.burstPhase);
    std::memcpy(phaseBufs_[slot].map, ph.data(), sizeof(ph));
    double sigma = crt::rf::noiseSigma(crt::rf::carrierToNoiseDb(settings_.antennaDbuv), 1);
    RFParams rp{crt::rf::kHop, crt::rf::kOverlap, crt::rf::filter().delaySamples, crt::rf::kBlocks,
                float(crt::rf::kModulationDepth / (crt::composite::kWhite - crt::composite::kSync)), 1.0f,
                noiseEnabled ? float(sigma) : 0.0f, crt::rf::noiseKey(1, ordinal)};
    dispatch(cmd, "rf_fast", {&carrierReal_, &codeBufs_[slot], &phaseBufs_[slot], &volts_, &twiddles_, &kernelSpecReal_}, &rp, sizeof rp,
             uint32_t((crt::rf::kBlocks + 1) / 2));
    AGCParams ap{float(double(crt::composite::kLineSamples) / crt::rf::sampleRateHz()), float(crt::agc::kAttackSeconds),
                 float(crt::agc::kReleaseSeconds), float(crt::agc::kMinGain), float(crt::agc::kMaxGain), 1,
                 crt::rf::filter().delaySamples, 0};
    dispatch(cmd, "rx_stats_fast", {&carrierReal_, &basis_, &stats_, &agcRows_}, &ap, sizeof ap, 240);
    dispatch(cmd, "rx_agc_fast", {&agcRows_, &gains_, &agcState_}, &ap, sizeof ap, 1);
    dispatch(cmd, "rx_decode_fast", {&carrierReal_, &basis_, &stats_, &gains_, &rxOut_, &supplyMeans_}, nullptr, 0, 240);
    drive = &rxOut_;
    meansReady = rows == 240;
  } else if (input.kind == InputKind::rgb) {
    std::memcpy(rgbBufs_[slot].map, input.rgb, 256 * 240 * 4);
    RasterParams rp2{rows, 1, 1, 0};
    dispatch(cmd, "raster_area", {&rxOut_, &rgbBufs_[slot], &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  } else {
    std::memcpy(driveIn_.map, input.drive, 512 * 240 * 16);
    drive = &driveIn_;
  }
  if (input.kind != InputKind::rgb && rows != 240) {
    RasterParams rp2{rows, 0, 0, 0};
    dispatch(cmd, "raster_area", {drive, drive, &rasterOut_}, &rp2, sizeof rp2, groups(512, 32), groups(rows, 8));
    drive = &rasterOut_;
  }
  const Buffer* src = drive;
  const bool spot = settings_.beamGrowth && !disableSpotH;
  if (settings_.supply) {
    namespace f = crt::supply;
    double line = f::kLineS * f::kVisibleLines / double(rows);
    SupplyParams sp{512,
                    rows,
                    float(std::exp(-line / f::kTauS)),
                    float(f::kV0),
                    float(f::kReff),
                    float(f::kImax),
                    float(f::kN),
                    float(std::exp(-(f::kTotalLines - f::kVisibleLines) * f::kLineS / f::kTauS)),
                    float(f::kVisibleLines / f::kTotalLines / double(rows)),
                    float(-std::expm1(-f::kFrameS / f::kTauAblS)),
                    float(f::kIlim),
                    primeSupply_ ? 1.0f : 0.0f,
                    {float(f::kShare[0]), float(f::kShare[1]), float(f::kShare[2]), 0}};
    if (!meansReady) dispatch(cmd, "row_mean_fast", {src, &supplyMeans_}, nullptr, 0, uint32_t(rows));
    dispatch(cmd, "supply_fast", {&supplyMeans_, &supplyState_[supplyCurrent_], &supplyState_[1 - supplyCurrent_], &supplyRow_}, &sp,
             sizeof sp, 1);
    supplyCurrent_ = 1 - supplyCurrent_;
    primeSupply_ = false;
  }
  const bool persist = settings_.persistence && t.ring.buffer;
  PostParams pp{settings_.supply ? 1 : 0, spot ? 1 : 0, float(crt::tube::extraSigmaSamples(1, 1, 512)), 0, 0,
                crt::phosphor::depth(), rows, t.srx};
  std::vector<float> w(size_t(crt::phosphor::depth()) * 4, 0.0f);
  if (persist) {
    const int depth = crt::phosphor::depth();
    if (!slots_.empty() && ord <= slots_.front().ordinal) slots_.clear();
    std::vector<Slot> kept;
    for (const Slot& s : slots_) {
      int64_t newer = kept.empty() ? ord : kept.back().ordinal;
      if (ord - newer + 1 <= int64_t(depth - 1)) kept.push_back(s);
      else break;
    }
    slots_ = kept;
    int r = 0;
    for (;;) {
      bool used = false;
      for (const Slot& s : kept) used |= s.ring == r;
      if (!used) break;
      ++r;
    }
    slots_.insert(slots_.begin(), Slot{r, ord});
    w = persistenceWeights();
    pp.persistence = 1;
    pp.newSlot = r;
  }
  std::memcpy(weightBufs_[slot].map, w.data(), w.size() * 4);
  const Buffer* ring = persist ? &t.ring : &t.scatterSrc;  // unused without persistence
  dispatch(cmd, "drive_post_fast", {src, &supplyRow_, &spotOut_, ring, &weightBufs_[slot], &t.swx, &t.scatterTmp}, &pp, sizeof pp,
           uint32_t(rows));
  drive = &spotOut_;
  lastDrive_ = drive;

  // Tube.
  const double lightNorm = std::sqrt(crt::tube::kLightX * crt::tube::kLightX + crt::tube::kLightY * crt::tube::kLightY +
                                     crt::tube::kLightZ * crt::tube::kLightZ);
  FastTubeParams tp{t.height, t.ow, t.oh, t.xCount, t.fastTaps, t.hStride, float(t.gain), float(1 - t.scatterFraction),
                    {float(crt::tube::kAlbedo * t.key.ambient / (3.141592653589793 * crt::tube::kReferenceWhiteCdM2)),
                     float(crt::tube::kDiffuseFraction), float(crt::tube::kNormalSlopeX), float(crt::tube::kNormalSlopeY)},
                    {float(crt::tube::kLightX / lightNorm), float(crt::tube::kLightY / lightNorm), float(crt::tube::kLightZ / lightNorm), 0}};
  ScatterParams scp{rows, t.sry, 0, 0, {t.kappa[0], t.kappa[1], t.kappa[2], 0}};
  dispatch(cmd, "scatter_drive", {&t.scatterTmp, &t.swy, &t.scatterSrc}, &scp, sizeof scp, groups(512, 32), groups(rows, 8));
  dispatch(cmd, "tube_h_fast", {drive, &t.xmapFast, &t.hplanes, &t.tiles}, &tp, sizeof tp, groups(t.ow, 64), groups(t.height, 16));
  dispatch(cmd, "tube_v_fast", {&t.hplanes, &t.vcoef, &t.colinv, &t.vrows, &t.scatterSrc, &t.outH}, &tp, sizeof tp, groups(t.ow, 32),
           groups(t.oh, 8));
}

/// tube-webgl.mjs persistenceWeights(): slot i (newest first) also stands for missing ordinals up
/// to the next newer slot (M4B-GAP), the oldest one for all older ordinals. Float32 accumulation
/// like the JS Float32Array.
std::vector<float> CrtRenderer::persistenceWeights() const {
  const int depth = crt::phosphor::depth();
  std::vector<float> w(size_t(depth) * 4, 0.0f);
  if (slots_.empty()) return w;
  const int64_t k = slots_.front().ordinal;
  const auto& fr = crt::phosphor::fractions();
  for (size_t i = 0; i < slots_.size(); ++i) {
    const Slot& slot = slots_[i];
    int64_t newest = i == 0 ? 0 : k - slots_[i - 1].ordinal + 1, oldest = k - slot.ordinal;
    // ReplayNES: the oldest slot also stands for the ordinals before it (history start after a
    // seek / rewind / load, or the first frame): the picture is taken as held, so a still shows
    // every phosphor's full steady-state light, not just the first frame's share (purple tint).
    int64_t hi = i + 1 == slots_.size() ? int64_t(depth - 1) : std::min<int64_t>(oldest, depth - 1);
    if (newest > hi) continue;
    for (int64_t m = newest; m <= hi; ++m)
      for (int c = 0; c < 3; ++c) {
        float& v = w[size_t(slot.ring) * 4 + size_t(c)];
        v = float(double(v) + fr[size_t(c)][size_t(m)]);
      }
  }
  return w;
}

std::vector<double> CrtRenderer::takeGpuTimes() {
  std::vector<double> out;
  if (!queries_) return out;
  while (!queryPending_.empty()) {
    int pair = queryPending_.front();
    uint64_t ts[2] = {0, 0};
    VkResult r = vkGetQueryPoolResults(ctx_.device, queries_, uint32_t(pair * 2), 2, sizeof ts, ts, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) break;
    queryPending_.erase(queryPending_.begin());
    if (ts[1] > ts[0]) {
      double s = double(ts[1] - ts[0]) * timestampPeriod_ * 1e-9;
      out.push_back(s);
      lastGpu_ = s;
    }
  }
  return out;
}

// ------------------------------------------------------------------ presentation
namespace {
ShowParams showParams(int ow, int oh, int tw, int th, const CrtRect& dst, double cropFraction) {
  double cy = double(oh) * cropFraction;
  ShowParams sp{};
  sp.dst[0] = dst.x;
  sp.dst[1] = dst.y;
  sp.dst[2] = dst.w;
  sp.dst[3] = dst.h;
  sp.src[0] = 0;
  sp.src[1] = float(cy);
  sp.src[2] = float(ow);
  sp.src[3] = float(double(oh) - 2 * cy);
  sp.ow = ow;
  sp.oh = oh;
  sp.tw = tw;
  sp.th = th;
  if (tw > 0 && th > 0) {
    sp.ndc[0] = dst.x / float(tw) * 2 - 1;
    sp.ndc[1] = dst.y / float(th) * 2 - 1;
    sp.ndc[2] = dst.w / float(tw) * 2;
    sp.ndc[3] = dst.h / float(th) * 2;
  }
  return sp;
}
}  // namespace

void CrtRenderer::encodeShow(VkCommandBuffer cmd, int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction) {
  if (!showPipeline_ || !tube_ || !hasOutput_ || targetWidth <= 0 || targetHeight <= 0) return;
  ShowParams sp = showParams(tube_->ow, tube_->oh, targetWidth, targetHeight, dst, cropFraction);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tube_->fast ? showPipelineH_ : showPipeline_);
  VkViewport v{0, 0, float(targetWidth), float(targetHeight), 0.f, 1.f};
  vkCmdSetViewport(cmd, 0, 1, &v);
  int x0 = std::max(0, int(std::floor(dst.x))), y0 = std::max(0, int(std::floor(dst.y)));
  int x1 = std::min(targetWidth, int(std::ceil(dst.x + dst.w))), y1 = std::min(targetHeight, int(std::ceil(dst.y + dst.h)));
  VkRect2D sc{{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
  vkCmdSetScissor(cmd, 0, 1, &sc);
  VkDescriptorBufferInfo info{tube_->fast ? tube_->outH.buffer : tube_->scatterX.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &info;
  pushDescriptor_(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, showLayout_, 0, 1, &w);
  vkCmdPushConstants(cmd, showLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof sp, &sp);
  vkCmdDraw(cmd, 4, 1, 0, 0);
}

void CrtRenderer::encodeShowToBuffer(VkCommandBuffer cmd, VkBuffer target, int tw, int th, const CrtRect& dst, double cropFraction) {
  if (!tube_ || !hasOutput_) return;
  ShowParams sp = showParams(tube_->ow, tube_->oh, tw, th, dst, cropFraction);
  Buffer tb;
  tb.buffer = target;
  if (tube_->fast) dispatch(cmd, "show_kernel_h", {&tube_->outH, &tb}, &sp, sizeof sp, groups(tw, 16), groups(th, 16));
  else dispatch(cmd, "show_kernel", {&tube_->scatterX, &tb}, &sp, sizeof sp, groups(tw, 16), groups(th, 16));
}

// ------------------------------------------------------------------ synchronous helpers (tests / export)
bool CrtRenderer::runSync(const std::function<void(VkCommandBuffer)>& fn) {
  if (!syncPool_) return false;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = syncPool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(ctx_.device, &ai, &cmd) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  fn(cmd);
  vkEndCommandBuffer(cmd);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(ctx_.device, &fci, nullptr, &fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VkResult r = vkQueueSubmit(ctx_.queue, 1, &si, fence);
  if (r == VK_SUCCESS) r = vkWaitForFences(ctx_.device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(ctx_.device, fence, nullptr);
  vkFreeCommandBuffers(ctx_.device, syncPool_, 1, &cmd);
  return r == VK_SUCCESS;
}

std::vector<float> CrtRenderer::read(Stage stage) {
  const Buffer* src = nullptr;
  const bool fast = tube_ && tube_->fast;
  switch (stage) {
    case Stage::receiver: src = &rxOut_; break;
    case Stage::tubeInput: src = lastDrive_; break;
    case Stage::emission: src = tube_ && !fast ? &tube_->emission : nullptr; break;  // fast: not kept
    case Stage::output: src = hasOutput_ && tube_ ? (fast ? &tube_->outH : &tube_->scatterX) : nullptr; break;
  }
  if (!src || !src->buffer) return {};
  Buffer staging;
  if (!createBuffer(&staging, src->size, true, false)) return {};
  bool ok = runSync([&](VkCommandBuffer cmd) {
    barrier(cmd);
    VkBufferCopy c{0, 0, src->size};
    vkCmdCopyBuffer(cmd, src->buffer, staging.buffer, 1, &c);
  });
  std::vector<float> out;
  if (ok && fast && stage == Stage::output) {
    // half4 -> float4
    const uint16_t* h = static_cast<const uint16_t*>(staging.map);
    out.resize(size_t(src->size / 2));
    for (size_t i = 0; i < out.size(); ++i) {
      uint32_t v = h[i], sign = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023;
      float f = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
      out[i] = sign ? -f : f;
    }
  } else if (ok) {
    out.resize(size_t(src->size / 4));
    std::memcpy(out.data(), staging.map, size_t(src->size));
  }
  destroyBuffer(&staging);
  return out;
}

}  // namespace rnl
