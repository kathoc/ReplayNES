// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_renderer.h"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host_clock.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "replaynes/replaynes.h"

namespace rnl {

namespace {
const uint32_t kBlitVert[] = {
#include "blit.vert.spv.inc"
};
const uint32_t kBlitFrag[] = {
#include "blit.frag.spv.inc"
};
constexpr VkDeviceSize kPictureBytes = VkDeviceSize(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT * 4;

PFN_vkWaitForPresentKHR pfnWaitForPresent = nullptr;

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
  for (const auto& e : list)
    if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}
}  // namespace

bool VkRenderer::init(SDL_Window* window, std::string* error) {
  window_ = window;
  Uint32 n = 0;
  const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&n);
  if (!sdlExts) { *error = std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError(); return false; }
  std::vector<const char*> exts(sdlExts, sdlExts + n);

  uint32_t loaderVersion = VK_API_VERSION_1_0;
  vkEnumerateInstanceVersion(&loaderVersion);
  apiVersion_ = loaderVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "ReplayNES";
  app.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
  app.pEngineName = "ReplayNES";
  app.apiVersion = apiVersion_;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(exts.size());
  ici.ppEnabledExtensionNames = exts.data();
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
  if (const char* v = std::getenv("REPLAYNES_VK_VALIDATION"); v && *v == '1') {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
  }
  if (VkResult r = vkCreateInstance(&ici, nullptr, &instance_); r != VK_SUCCESS) {
    *error = "vkCreateInstance failed (" + std::to_string(r) + ")";
    return false;
  }
  if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
    *error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
    return false;
  }
  if (!createDevice(error)) return false;

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queueFamily_;
  vkCreateCommandPool(device_, &pci, nullptr, &pool_);
  for (Slot& s : slots_) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &ai, &s.cmd);
    vkAllocateCommandBuffers(device_, &ai, &buildCmds_[&s - slots_]);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fci, nullptr, &s.fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &sci, nullptr, &s.acquired);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    // The game picture, then up to kAtlasUploadsPerFrame thumbnails (vk_ui_resources.cpp).
    bci.size = kPictureBytes + kAtlasUploadsPerFrame * kThumbBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device_, &bci, nullptr, &s.staging);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device_, s.staging, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &mai, nullptr, &s.stagingMem);
    vkBindBufferMemory(device_, s.staging, s.stagingMem, 0);
    vkMapMemory(device_, s.stagingMem, 0, bci.size, 0, &s.stagingPtr);
  }
  if (presentWait_) waiter_ = std::thread([this] { waiterLoop(); });
  if (!createGameTexture()) { *error = "game texture"; return false; }
  if (!createAtlas()) { *error = "thumbnail atlas"; return false; }
  if (!createSwapchain()) { *error = "swapchain"; return false; }
  if (!createPipeline()) { *error = "pipeline"; return false; }
  return true;
}

bool VkRenderer::createDevice(std::string* error) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devs(count);
  vkEnumeratePhysicalDevices(instance_, &count, devs.data());
  int bestScore = -1;
  for (VkPhysicalDevice d : devs) {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
    for (uint32_t i = 0; i < qn; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
      if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
      if (score > bestScore) { bestScore = score; phys_ = d; queueFamily_ = i; }
      break;
    }
  }
  if (!phys_) { *error = "no Vulkan device can present to this window"; return false; }

  uint32_t en = 0;
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, nullptr);
  std::vector<VkExtensionProperties> avail(en);
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, avail.data());
  std::vector<const char*> exts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

  VkPhysicalDevicePresentIdFeaturesKHR idF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  VkPhysicalDevicePresentWaitFeaturesKHR waitF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  idF.pNext = &waitF;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &idF;
  vkGetPhysicalDeviceFeatures2(phys_, &f2);
  bool disable = std::getenv("REPLAYNES_NO_PRESENT_WAIT") != nullptr;
  presentWait_ = !disable && hasExtension(avail, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                 hasExtension(avail, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) && idF.presentId && waitF.presentWait;
  if (presentWait_) {
    exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
  }
  // CRT Display (render/crt_renderer.h) needs push descriptors.
  std::string crtWhy;
  crtSupported_ = CrtRenderer::supported(phys_, &crtWhy);
  status_.crtAvailable = crtSupported_;
  if (crtSupported_) {
    uint32_t cn = 0;
    const char* const* ce = CrtRenderer::requiredDeviceExtensions(&cn);
    for (uint32_t i = 0; i < cn; ++i) exts.push_back(ce[i]);
  } else {
    status_.crtError = crtWhy;
  }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDevicePresentIdFeaturesKHR idOn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  VkPhysicalDevicePresentWaitFeaturesKHR waitOn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  idOn.presentId = VK_TRUE;
  waitOn.presentWait = VK_TRUE;
  idOn.pNext = &waitOn;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = presentWait_ ? &idOn : nullptr;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(exts.size());
  dci.ppEnabledExtensionNames = exts.data();
  if (VkResult r = vkCreateDevice(phys_, &dci, nullptr, &device_); r != VK_SUCCESS) {
    *error = "vkCreateDevice failed (" + std::to_string(r) + ")";
    return false;
  }
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
  if (presentWait_) {
    pfnWaitForPresent = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device_, "vkWaitForPresentKHR"));
    presentWait_ = pfnWaitForPresent != nullptr;
  }

  VkPhysicalDeviceProperties p;
  vkGetPhysicalDeviceProperties(phys_, &p);
  VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &drv;
  vkGetPhysicalDeviceProperties2(phys_, &p2);
  description_ = std::string(p.deviceName) + " / " + drv.driverName + " " + drv.driverInfo +
                 " / present_wait=" + (presentWait_ ? "yes" : "no") +
                 " present_timing(ext)=" + (hasExtension(avail, "VK_EXT_present_timing") ? "yes" : "no") +
                 " display_timing(ext)=" + (hasExtension(avail, "VK_GOOGLE_display_timing") ? "yes" : "no");
  return true;
}

uint32_t VkRenderer::findMemory(uint32_t typeBits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  return 0;
}

bool VkRenderer::createGameTexture() {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_B8G8R8A8_UNORM;  // rn_video is BGRA8: bytes copied unchanged
  ici.extent = {RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &gameImage_) != VK_SUCCESS) return false;
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device_, gameImage_, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device_, &mai, nullptr, &gameMem_);
  vkBindImageMemory(device_, gameImage_, gameMem_, 0);
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = gameImage_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = ici.format;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device_, &vci, nullptr, &gameView_);
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_NEAREST;
  sci.minFilter = VK_FILTER_NEAREST;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = 0;
  vkCreateSampler(device_, &sci, nullptr, &sampler_);
  return true;
}

bool VkRenderer::createSwapchain() {
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window_, &w, &h);
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) {
    ext.width = uint32_t(std::clamp(w, int(caps.minImageExtent.width), int(caps.maxImageExtent.width)));
    ext.height = uint32_t(std::clamp(h, int(caps.minImageExtent.height), int(caps.maxImageExtent.height)));
  }
  if (ext.width == 0 || ext.height == 0) { extent_ = {0, 0}; return true; }  // minimised

  if (renderPass_ == VK_NULL_HANDLE) {
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, fmts.data());
    format_ = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0].format;
    for (auto f : fmts)
      if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
          f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { format_ = f.format; break; }
    VkAttachmentDescription att{};
    att.format = format_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1;
    rci.pAttachments = &att;
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    if (vkCreateRenderPass(device_, &rci, nullptr, &renderPass_) != VK_SUCCESS) return false;
  }

  uint32_t count = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);
  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & alpha)) {
    for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
      if (caps.supportedCompositeAlpha & b) { alpha = VkCompositeAlphaFlagBitsKHR(b); break; }
  }
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = count;
  sci.imageFormat = format_;
  sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  sci.imageExtent = ext;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  // Screenshots (requestScreenshot) copy the presented image out.
  swapchainCopyable_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  if (swapchainCopyable_) sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = alpha;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // display-locked: one picture per vblank, never torn
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = swapchain_;
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  if (vkCreateSwapchainKHR(device_, &sci, nullptr, &sc) != VK_SUCCESS) return false;
  {
    std::lock_guard<std::mutex> lk(swapMutex_);
    destroySwapchain();
    swapchain_ = sc;
    swapGen_ += 1;
  }
  extent_ = ext;

  uint32_t n = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  images_.resize(n);
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
  for (VkImage img : images_) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v;
    vkCreateImageView(device_, &vci, nullptr, &v);
    views_.push_back(v);
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = renderPass_;
    fci.attachmentCount = 1;
    fci.pAttachments = &v;
    fci.width = ext.width;
    fci.height = ext.height;
    fci.layers = 1;
    VkFramebuffer fb;
    vkCreateFramebuffer(device_, &fci, nullptr, &fb);
    framebuffers_.push_back(fb);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    vkCreateSemaphore(device_, &semi, nullptr, &s);
    renderDone_.push_back(s);
  }
  if (imguiReady_) ImGui_ImplVulkan_SetMinImageCount(std::max(2u, count));
  return true;
}

void VkRenderer::destroySwapchain() {
  for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
  for (auto v : views_) vkDestroyImageView(device_, v, nullptr);
  for (auto s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
  framebuffers_.clear();
  views_.clear();
  renderDone_.clear();
  images_.clear();
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
}

bool VkRenderer::createPipeline() {
  VkDescriptorSetLayoutBinding b{};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 1;
  lci.pBindings = &b;
  vkCreateDescriptorSetLayout(device_, &lci, nullptr, &setLayout_);
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &ps;
  vkCreateDescriptorPool(device_, &dpi, nullptr, &descPool_);
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descPool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &setLayout_;
  vkAllocateDescriptorSets(device_, &dai, &set_);
  VkDescriptorImageInfo ii{sampler_, gameView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set_;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo = &ii;
  vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

  VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &setLayout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  vkCreatePipelineLayout(device_, &pli, nullptr, &pipeLayout_);

  auto module = [&](const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = bytes;
    mci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &mci, nullptr, &m);
    return m;
  };
  VkShaderModule vs = module(kBlitVert, sizeof(kBlitVert)), fs = module(kBlitFrag, sizeof(kBlitFrag));
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
  gp.layout = pipeLayout_;
  gp.renderPass = renderPass_;
  VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, vs, nullptr);
  vkDestroyShaderModule(device_, fs, nullptr);
  return r == VK_SUCCESS;
}

bool VkRenderer::initImGui() {
  ImGui_ImplSDL3_InitForVulkan(window_);
  ImGui_ImplVulkan_InitInfo ii{};
  ii.ApiVersion = apiVersion_;
  ii.Instance = instance_;
  ii.PhysicalDevice = phys_;
  ii.Device = device_;
  ii.QueueFamily = queueFamily_;
  ii.Queue = queue_;
  ii.DescriptorPoolSize = 16;
  ii.MinImageCount = 2;
  ii.ImageCount = std::max(2u, imageCount());
  ii.PipelineInfoMain.RenderPass = renderPass_;
  ii.PipelineInfoMain.Subpass = 0;
  ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  imguiReady_ = ImGui_ImplVulkan_Init(&ii);
  if (imguiReady_) registerAtlasWithImGui();
  return imguiReady_;
}

void VkRenderer::newImGuiFrame() { ImGui_ImplVulkan_NewFrame(); }

uint64_t VkRenderer::drawAndPresent(const uint32_t* newPicture, const GameRect& rect, ImDrawData* ui, const FrameSignal* signal) {
  // CRT Display on / off (created on first use, freed when switched off).
  bool crtOn = post_.crt && crtSupported_;
  if (crtOn && !crt_.active()) {
    std::string e;
    if (!crt_.ensure(CrtVulkanContext{phys_, device_, queueFamily_, queue_}, renderPass_, &e)) {
      status_.crtError = e;
      std::fprintf(stderr, "CRT: %s\n", e.c_str());
      post_.crt = false;
      crtOn = false;
    }
  }
  if (!crtOn && crt_.active()) crt_.release();  // waits for the device (the toggle, not the frame loop)
  status_.crtShown = false;
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window_, &w, &h);
  if (needRecreate_ || swapchain_ == VK_NULL_HANDLE || extent_.width == 0 ||
      (w > 0 && h > 0 && (uint32_t(w) != extent_.width || uint32_t(h) != extent_.height))) {
    vkDeviceWaitIdle(device_);
    needRecreate_ = false;
    if (!createSwapchain()) return 0;
  }
  if (extent_.width == 0 || swapchain_ == VK_NULL_HANDLE) return 0;

  Slot& s = slots_[slot_];
  double t0 = nowSeconds();
  vkWaitForFences(device_, 1, &s.fence, VK_TRUE, UINT64_MAX);
  uint32_t idx = 0;
  VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &idx);
  lastAcquireWait_ = nowSeconds() - t0;
  if (ar == VK_ERROR_OUT_OF_DATE_KHR) { needRecreate_ = true; return 0; }
  if (ar == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
  else if (ar != VK_SUCCESS) return 0;
  vkResetFences(device_, 1, &s.fence);

  VkCommandBuffer cmd = s.cmd;
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  if (newPicture) {
    std::memcpy(s.stagingPtr, newPicture, size_t(kPictureBytes));
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.srcAccessMask = hasPicture_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = hasPicture_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = gameImage_;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toDst);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, 1};
    vkCmdCopyBufferToImage(cmd, s.staging, gameImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toRead = toDst;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toRead);
    hasPicture_ = true;
  }
  recordAtlasUploads(cmd, slot_);
  // CRT: a new picture (or a changed size / setting / tube plan) is built by its compute passes,
  // here before the show pass (or, built ahead, after this present: the next one shows it).
  bool crtBuildAfter = false;
  if (crtOn) {
    crt_.update(gpuBudget_, post_.allowBuildAhead, post_.adaptiveResolution);
    if (newPicture) crt_.store(newPicture, signal);
    if (rect.visible) crt_.configure(post_, rect.crt, rect.crtCrop);
    bool changed = shownPost_ != post_ || shownCrt_.w != rect.crt.w || shownCrt_.h != rect.crt.h;
    bool needBuild = crt_.hasFrame() && (newPicture || changed || !crt_.canShow() || crt_.planPending());
    if (needBuild) {
      if (crt_.pipelined() && crt_.canShow()) crtBuildAfter = true;
      else crt_.build(cmd);
    }
    shownPost_ = post_;
    shownCrt_ = rect.crt;
  }
  VkClearValue clear{};
  clear.color = {{0.f, 0.f, 0.f, 1.f}};
  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = renderPass_;
  rbi.framebuffer = framebuffers_[idx];
  rbi.renderArea = {{0, 0}, extent_};
  rbi.clearValueCount = 1;
  rbi.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  if (crtOn && rect.visible && crt_.canShow()) {
    crt_.show(cmd, int(extent_.width), int(extent_.height), rect.crt, rect.crtCrop);
    status_.crtShown = true;
  } else if (rect.visible && hasPicture_) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport v{rect.x, rect.y, rect.w, rect.h, 0.f, 1.f};
    vkCmdSetViewport(cmd, 0, 1, &v);
    int x0 = std::max(0, int(rect.x)), y0 = std::max(0, int(rect.y));
    int x1 = std::min(int(extent_.width), int(rect.x + rect.w)), y1 = std::min(int(extent_.height), int(rect.y + rect.h));
    VkRect2D sc{{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    float uv[4] = {float(rect.crop) / RN_VIDEO_WIDTH, float(rect.crop) / RN_VIDEO_HEIGHT,
                   float(RN_VIDEO_WIDTH - rect.crop) / RN_VIDEO_WIDTH, float(RN_VIDEO_HEIGHT - rect.crop) / RN_VIDEO_HEIGHT};
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uv), uv);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &set_, 0, nullptr);
    vkCmdDraw(cmd, 4, 1, 0, 0);
  }
  if (ui && imguiReady_ && ui->CmdListsCount > 0) ImGui_ImplVulkan_RenderDrawData(ui, cmd);
  vkCmdEndRenderPass(cmd);
  recordScreenshot(cmd, idx);
  vkEndCommandBuffer(cmd);

  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
  si[0].waitSemaphoreCount = 1;
  si[0].pWaitSemaphores = &s.acquired;
  si[0].pWaitDstStageMask = &waitStage;
  si[0].commandBufferCount = 1;
  si[0].pCommandBuffers = &cmd;
  si[0].signalSemaphoreCount = 1;
  si[0].pSignalSemaphores = &renderDone_[idx];
  uint32_t submits = 1;
  VkCommandBuffer bcmd = buildCmds_[slot_];
  if (crtBuildAfter) {
    // Built ahead: the present above only waits for its own commands; the new picture's passes
    // run after it (same queue, ordered by the CRT's barriers) and the next present shows it.
    vkResetCommandBuffer(bcmd, 0);
    vkBeginCommandBuffer(bcmd, &bi);
    crt_.build(bcmd);
    vkEndCommandBuffer(bcmd);
    si[1].commandBufferCount = 1;
    si[1].pCommandBuffers = &bcmd;
    submits = 2;
  }
  vkQueueSubmit(queue_, submits, si, s.fence);
  finishScreenshot(slot_);
  if (crtOn) crt_.fillStatus(&status_);

  uint64_t id = ++presentId_;
  VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
  pid.swapchainCount = 1;
  pid.pPresentIds = &id;
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.pNext = presentWait_ ? &pid : nullptr;
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &renderDone_[idx];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &idx;
  VkResult pr = vkQueuePresentKHR(queue_, &pi);
  slot_ = (slot_ + 1) % kSlots;
  if (pr == VK_ERROR_OUT_OF_DATE_KHR) { needRecreate_ = true; return 0; }
  if (pr == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
  else if (pr != VK_SUCCESS) return 0;
  if (presentWait_) {
    std::lock_guard<std::mutex> lk(queueMutex_);
    toWait_.push_back({id, swapGen_});
    queueCv_.notify_one();
  }
  return id;
}

void VkRenderer::waiterLoop() {
  for (;;) {
    std::pair<uint64_t, uint64_t> item;
    {
      std::unique_lock<std::mutex> lk(queueMutex_);
      queueCv_.wait(lk, [&] { return stopWaiter_.load() || !toWait_.empty(); });
      if (stopWaiter_) return;
      item = toWait_.front();
      toWait_.pop_front();
    }
    PresentDone d;
    d.id = item.first;
    {
      // One wait per present, holding the swapchain lock: a recreation (resize) waits for it.
      std::lock_guard<std::mutex> lk(swapMutex_);
      VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
      if (item.second == swapGen_ && swapchain_ != VK_NULL_HANDLE)
        r = pfnWaitForPresent(device_, swapchain_, item.first, 200'000'000ull);
      d.result = int(r);
      d.time = nowSeconds();
      d.ok = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
    }
    std::lock_guard<std::mutex> lk(queueMutex_);
    done_.push_back(d);
  }
}

std::vector<PresentDone> VkRenderer::takeCompleted() {
  std::lock_guard<std::mutex> lk(queueMutex_);
  std::vector<PresentDone> out;
  out.swap(done_);
  return out;
}

void VkRenderer::waitLastSubmit() {
  const Slot& last = slots_[(slot_ + kSlots - 1) % kSlots];
  vkWaitForFences(device_, 1, &last.fence, VK_TRUE, UINT64_MAX);
}

void VkRenderer::shutdown() {
  if (!device_) return;
  {
    std::lock_guard<std::mutex> lk(queueMutex_);
    stopWaiter_ = true;
    queueCv_.notify_all();
  }
  if (waiter_.joinable()) waiter_.join();
  vkDeviceWaitIdle(device_);
  if (imguiReady_) ImGui_ImplVulkan_Shutdown();
  imguiReady_ = false;
  crt_.release();
  destroyAtlas();
  destroyScreenshotBuffer();
  destroySwapchain();
  if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
  if (pipeLayout_) vkDestroyPipelineLayout(device_, pipeLayout_, nullptr);
  if (descPool_) vkDestroyDescriptorPool(device_, descPool_, nullptr);
  if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
  if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
  if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
  if (gameView_) vkDestroyImageView(device_, gameView_, nullptr);
  if (gameImage_) vkDestroyImage(device_, gameImage_, nullptr);
  if (gameMem_) vkFreeMemory(device_, gameMem_, nullptr);
  for (Slot& s : slots_) {
    if (s.stagingMem) { vkUnmapMemory(device_, s.stagingMem); vkFreeMemory(device_, s.stagingMem, nullptr); }
    if (s.staging) vkDestroyBuffer(device_, s.staging, nullptr);
    if (s.fence) vkDestroyFence(device_, s.fence, nullptr);
    if (s.acquired) vkDestroySemaphore(device_, s.acquired, nullptr);
  }
  if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  vkDestroyDevice(device_, nullptr);
  device_ = VK_NULL_HANDLE;
  if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
  instance_ = VK_NULL_HANDLE;
}

}  // namespace rnl
