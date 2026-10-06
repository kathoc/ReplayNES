// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/headless_vulkan.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace rnl {

namespace {
bool hasExt(const std::vector<VkExtensionProperties>& list, const char* name) {
  for (const auto& e : list)
    if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}
}  // namespace

bool HeadlessVulkan::init(std::string* error) {
#ifdef __APPLE__
  setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
#endif
  uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> iexts(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
  std::vector<const char*> enable;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  if (hasExt(iexts, "VK_KHR_portability_enumeration")) {
    enable.push_back("VK_KHR_portability_enumeration");
    ici.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
  }
  if (hasExt(iexts, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    enable.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "ReplayNES (offscreen)";
  app.apiVersion = VK_API_VERSION_1_1;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(enable.size());
  ici.ppEnabledExtensionNames = enable.data();
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
  if (const char* v = std::getenv("REPLAYNES_VK_VALIDATION"); v && *v == '1') {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
  }
  if (VkResult r = vkCreateInstance(&ici, nullptr, &instance_); r != VK_SUCCESS) {
    *error = "vkCreateInstance failed (" + std::to_string(int(r)) + ")";
    return false;
  }
  vkEnumeratePhysicalDevices(instance_, &n, nullptr);
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(instance_, &n, devs.data());
  const char* want = std::getenv("REPLAYNES_VK_DEVICE");
  int best = -1;
  for (VkPhysicalDevice d : devs) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    if (!CrtRenderer::supported(d, nullptr)) continue;
    if (want && *want && !std::strstr(p.deviceName, want)) continue;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
    for (uint32_t i = 0; i < qn; ++i) {
      if ((qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) continue;
      int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
      if (score > best) {
        best = score;
        phys_ = d;
        queueFamily_ = i;
      }
      break;
    }
  }
  if (!phys_) {
    *error = "no Vulkan device with VK_KHR_push_descriptor";
    return false;
  }
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> dexts(n);
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, dexts.data());
  uint32_t cn = 0;
  const char* const* crtExts = CrtRenderer::requiredDeviceExtensions(&cn);
  std::vector<const char*> exts(crtExts, crtExts + cn);
  if (hasExt(dexts, "VK_KHR_portability_subset")) exts.push_back("VK_KHR_portability_subset");
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(exts.size());
  dci.ppEnabledExtensionNames = exts.data();
  if (VkResult r = vkCreateDevice(phys_, &dci, nullptr, &device_); r != VK_SUCCESS) {
    *error = "vkCreateDevice failed (" + std::to_string(int(r)) + ")";
    return false;
  }
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
  VkPhysicalDeviceProperties p;
  vkGetPhysicalDeviceProperties(phys_, &p);
  description_ = p.deviceName;
  return true;
}

void HeadlessVulkan::shutdown() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    vkDestroyDevice(device_, nullptr);
  }
  if (instance_) vkDestroyInstance(instance_, nullptr);
  device_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
  phys_ = VK_NULL_HANDLE;
}

}  // namespace rnl
