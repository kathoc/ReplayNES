// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_backdrop.h"

#include <algorithm>
#include <cmath>

namespace rnl {
namespace {
const uint32_t kBackdropFrag[] = {
#include "backdrop.frag.spv.inc"
};
}  // namespace

bool VkBackdrop::create(VkDevice device, VkRenderPass renderPass, const uint32_t* vert, size_t vertBytes,
                        FindMemory findMemory) {
  device_ = device;
  findMemory_ = std::move(findMemory);
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = VK_LOD_CLAMP_NONE;
  if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;
  VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 1;
  lci.pBindings = &b;
  if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &setLayout_) != VK_SUCCESS) return false;
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &ps;
  if (vkCreateDescriptorPool(device_, &dpi, nullptr, &pool_) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = pool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &setLayout_;
  if (vkAllocateDescriptorSets(device_, &dai, &set_) != VK_SUCCESS) return false;
  VkPushConstantRange pcr[2]{{VK_SHADER_STAGE_VERTEX_BIT, 0, 32}, {VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16}};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &setLayout_;
  pli.pushConstantRangeCount = 2;
  pli.pPushConstantRanges = pcr;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &layout_) != VK_SUCCESS) return false;

  auto module = [&](const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = bytes;
    mci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &mci, nullptr, &m);
    return m;
  };
  VkShaderModule vs = module(vert, vertBytes), fs = module(kBackdropFrag, sizeof(kBackdropFrag));
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
  vp.viewportCount = vp.scissorCount = 1;
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
  gp.layout = layout_;
  gp.renderPass = renderPass;
  VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, vs, nullptr);
  vkDestroyShaderModule(device_, fs, nullptr);
  return r == VK_SUCCESS;
}

void VkBackdrop::destroyImage() {
  if (view_) vkDestroyImageView(device_, view_, nullptr);
  if (image_) vkDestroyImage(device_, image_, nullptr);
  if (mem_) vkFreeMemory(device_, mem_, nullptr);
  view_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
  mem_ = VK_NULL_HANDLE;
  w_ = h_ = levels_ = 0;
  valid_ = false;
}

void VkBackdrop::release() {
  if (!device_) return;
  destroyImage();
  if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
  if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
  if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  setLayout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
  set_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

void VkBackdrop::update(VkCommandBuffer cmd, VkImage picture, uint32_t w, uint32_t h) {
  if (!pipeline_ || !w || !h) return;
  if (w != w_ || h != h_) {
    vkDeviceWaitIdle(device_);  // rare: a new picture size
    destroyImage();
    levels_ = uint32_t(std::floor(std::log2(double(std::max(w, h))))) + 1;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = levels_;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device_, &ici, nullptr, &image_) != VK_SUCCESS) return destroyImage();
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device_, image_, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemory_(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &mem_) != VK_SUCCESS) return destroyImage();
    vkBindImageMemory(device_, image_, mem_, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels_, 0, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &view_) != VK_SUCCESS) return destroyImage();
    VkDescriptorImageInfo ii{sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = set_;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
    w_ = w;
    h_ = h;
  }
  auto barrier = [&](VkImage img, uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  const VkPipelineStageFlags FS = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, TR = VK_PIPELINE_STAGE_TRANSFER_BIT;
  barrier(picture, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, FS, TR);
  barrier(image_, 0, levels_, valid_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, valid_ ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT, FS, TR);
  VkImageBlit bl{};
  bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bl.srcOffsets[1] = bl.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
  vkCmdBlitImage(cmd, picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                 VK_FILTER_NEAREST);
  barrier(picture, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, TR, FS);
  int32_t lw = int32_t(w), lh = int32_t(h);
  for (uint32_t i = 1; i < levels_; ++i) {
    barrier(image_, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, TR, TR);
    VkImageBlit m{};
    m.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
    m.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    m.srcOffsets[1] = {lw, lh, 1};
    lw = std::max(1, lw / 2);
    lh = std::max(1, lh / 2);
    m.dstOffsets[1] = {lw, lh, 1};
    vkCmdBlitImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &m,
                   VK_FILTER_LINEAR);
  }
  // Every level but the last is TRANSFER_SRC now, the last TRANSFER_DST.
  if (levels_ > 1)
    barrier(image_, 0, levels_ - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, TR, FS);
  barrier(image_, levels_ - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, TR, FS);
  valid_ = true;
}

void VkBackdrop::draw(VkCommandBuffer cmd, const BackdropPlan& plan, VkExtent2D screen, const float uv[8]) {
  if (!valid_ || !plan.show) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  VkViewport v{plan.x, plan.y, plan.w, plan.h, 0.f, 1.f};
  vkCmdSetViewport(cmd, 0, 1, &v);
  VkRect2D sc{{0, 0}, screen};
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, uv);
  const float level = std::min(plan.lod, float(levels_ - 1));
  const float texel = std::exp2(level);
  const float fp[4] = {level, plan.dim, texel * plan.spread / float(w_), texel * plan.spread / float(h_)};
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16, fp);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
  vkCmdDraw(cmd, 4, 1, 0, 0);
}

}  // namespace rnl
