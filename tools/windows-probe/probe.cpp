// replaynes-winprobe: prints which graphics / audio / input APIs this Windows machine offers.
// Usage: replaynes-winprobe [--wait-input SECONDS]   (polls XInput for that long, e.g. to check
// that a USB controller passed through to a VM reaches the guest).
// Everything is loaded dynamically (LoadLibrary / CoCreateInstance) so a missing runtime is
// reported instead of preventing the probe from starting. Output: "section.key: value" lines.
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <xinput.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string utf8(const wchar_t* w) {
  if (!w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

void out(const char* key, const std::string& v) { std::printf("%s: %s\n", key, v.c_str()); }
void outf(const char* key, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  out(key, buf);
}

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

template <typename F>
F sym(HMODULE m, const char* name) {
  return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

const char* flName(D3D_FEATURE_LEVEL fl) {
  switch (fl) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    default: return "?";
  }
}

const char* machineName(USHORT m) {
  switch (m) {
    case IMAGE_FILE_MACHINE_AMD64: return "x64";
    case IMAGE_FILE_MACHINE_ARM64: return "arm64";
    case IMAGE_FILE_MACHINE_I386: return "x86";
    case IMAGE_FILE_MACHINE_UNKNOWN: return "native";
    default: return "other";
  }
}

void probeSystem() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof v;
  if (auto f = sym<RtlGetVersionFn>(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
  outf("system.windows", "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
  using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
  USHORT proc = 0, native = 0;
  if (auto f = sym<IsWow64Process2Fn>(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"))
    f(GetCurrentProcess(), &proc, &native);
#if defined(_M_ARM64) || defined(__aarch64__)
  const char* self = "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  const char* self = "x64";
#else
  const char* self = "other";
#endif
  // An x64 process on an arm64 host is emulated (IsWow64Process2 reports "native" for it).
  outf("system.cpu", "native=%s probe=%s logical_processors=%lu", machineName(native), self,
       (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  GlobalMemoryStatusEx(&m);
  outf("system.ram_mb", "%llu", (unsigned long long)(m.ullTotalPhys >> 20));
}

void probeDxgi() {
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
  auto create = sym<CreateFactory1Fn>(dxgi, "CreateDXGIFactory1");
  if (!create) return out("dxgi", "missing");
  IDXGIFactory1* f = nullptr;
  if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return out("dxgi", "CreateDXGIFactory1 failed");
  IDXGIFactory5* f5 = nullptr;
  if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
    BOOL tearing = FALSE;
    f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
    out("dxgi.allow_tearing", tearing ? "yes" : "no");
    release(f5);
  }
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    char key[64];
    std::snprintf(key, sizeof key, "dxgi.adapter%u", i);
    outf(key, "%s vendor=0x%04x device=0x%04x vram_mb=%llu shared_mb=%llu%s", utf8(d.Description).c_str(),
         d.VendorId, d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory >> 20),
         (unsigned long long)(d.SharedSystemMemory >> 20), (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
    IDXGIOutput* o = nullptr;
    for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
      DXGI_OUTPUT_DESC od{};
      o->GetDesc(&od);
      DEVMODEW dm{};
      dm.dmSize = sizeof dm;
      EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
      std::snprintf(key, sizeof key, "dxgi.adapter%u.output%u", i, j);
      outf(key, "%s %ldx%ld @ %lu Hz", utf8(od.DeviceName).c_str(), od.DesktopCoordinates.right - od.DesktopCoordinates.left,
           od.DesktopCoordinates.bottom - od.DesktopCoordinates.top, dm.dmDisplayFrequency);
      release(o);
    }
    release(a);
  }
  release(f);
}

void probeD3D11() {
  using CreateFn = decltype(&D3D11CreateDevice);
  auto create = sym<CreateFn>(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDevice");
  if (!create) return out("d3d11", "missing");
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                    D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
  for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = create(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, UINT(sizeof want / sizeof want[0]),
                        D3D11_SDK_VERSION, &dev, &got, nullptr);
    const char* key = type == D3D_DRIVER_TYPE_HARDWARE ? "d3d11.hardware" : "d3d11.warp";
    if (FAILED(hr)) {
      outf(key, "failed hr=0x%08lx", (unsigned long)hr);
      continue;
    }
    std::string adapter;
    IDXGIDevice* dd = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dd)))) {
      IDXGIAdapter* a = nullptr;
      if (SUCCEEDED(dd->GetAdapter(&a))) {
        DXGI_ADAPTER_DESC d{};
        a->GetDesc(&d);
        adapter = utf8(d.Description);
        release(a);
      }
      release(dd);
    }
    outf(key, "feature_level=%s adapter=%s", flName(got), adapter.c_str());
    release(dev);
  }
}

void probeD3D12() {
  using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  auto create = sym<CreateFn>(LoadLibraryW(L"d3d12.dll"), "D3D12CreateDevice");
  if (!create) return out("d3d12", "missing");
  ID3D12Device* dev = nullptr;
  HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
  if (FAILED(hr)) return outf("d3d12", "D3D12CreateDevice(FL 11_0) failed hr=0x%08lx", (unsigned long)hr);
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
  D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
  fl.NumFeatureLevels = UINT(sizeof levels / sizeof levels[0]);
  fl.pFeatureLevelsRequested = levels;
  dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
  // Highest shader model the runtime + driver accept (newest first; unknown values fail).
  D3D12_FEATURE_DATA_SHADER_MODEL sm{};
  for (int m : {0x69, 0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61, 0x60, 0x51}) {
    sm.HighestShaderModel = D3D_SHADER_MODEL(m);
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) break;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
  dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof opt);
  LUID luid = dev->GetAdapterLuid();
  outf("d3d12", "max_feature_level=%s shader_model=%d.%d resource_binding_tier=%d adapter_luid=%08lx:%08lx",
       flName(fl.MaxSupportedFeatureLevel), int(sm.HighestShaderModel) >> 4, int(sm.HighestShaderModel) & 0xf,
       int(opt.ResourceBindingTier), (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);
  release(dev);
}

// Minimal Vulkan declarations (no SDK headers needed).
struct VkAppInfo {
  int sType;  // VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
  const void* pNext;
  const char* pApplicationName;
  uint32_t applicationVersion;
  const char* pEngineName;
  uint32_t engineVersion;
  uint32_t apiVersion;
};
struct VkInstInfo {
  int sType;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
  const void* pNext;
  uint32_t flags;
  const VkAppInfo* pApplicationInfo;
  uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
};
using VkInstance = void*;
using VkPhysicalDevice = void*;
using PFN_vkVoid = void(WINAPI*)();
using PFN_GetInstanceProcAddr = PFN_vkVoid(WINAPI*)(VkInstance, const char*);
using PFN_EnumerateInstanceVersion = int(WINAPI*)(uint32_t*);
using PFN_CreateInstance = int(WINAPI*)(const VkInstInfo*, const void*, VkInstance*);
using PFN_DestroyInstance = void(WINAPI*)(VkInstance, const void*);
using PFN_EnumeratePhysicalDevices = int(WINAPI*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_GetPhysicalDeviceProperties = void(WINAPI*)(VkPhysicalDevice, void*);

std::string vkVer(uint32_t v) {
  char b[32];
  std::snprintf(b, sizeof b, "%u.%u.%u", (v >> 22) & 0x7f, (v >> 12) & 0x3ff, v & 0xfff);
  return b;
}

void probeVulkan() {
  HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
  if (!vk) return out("vulkan", "missing (no vulkan-1.dll loader)");
  auto gipa = sym<PFN_GetInstanceProcAddr>(vk, "vkGetInstanceProcAddr");
  if (!gipa) return out("vulkan", "loader without vkGetInstanceProcAddr");
  auto enumVer = reinterpret_cast<PFN_EnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
  uint32_t ver = (1u << 22);  // 1.0 when the entry point is absent
  if (enumVer) enumVer(&ver);
  out("vulkan.loader_instance_version", vkVer(ver));
  auto createInst = reinterpret_cast<PFN_CreateInstance>(gipa(nullptr, "vkCreateInstance"));
  VkAppInfo app{0, nullptr, "replaynes-winprobe", 1, "ReplayNES", 1, (1u << 22) | (1u << 12)};  // 1.1
  VkInstInfo ci{1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
  VkInstance inst = nullptr;
  int r = createInst ? createInst(&ci, nullptr, &inst) : -1;
  if (r != 0) return outf("vulkan.instance", "vkCreateInstance failed (VkResult %d)", r);
  auto destroy = reinterpret_cast<PFN_DestroyInstance>(gipa(inst, "vkDestroyInstance"));
  auto enumDev = reinterpret_cast<PFN_EnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
  auto props = reinterpret_cast<PFN_GetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
  uint32_t n = 0;
  enumDev(inst, &n, nullptr);
  outf("vulkan.physical_devices", "%u", n);
  std::vector<VkPhysicalDevice> devs(n);
  if (n) enumDev(inst, &n, devs.data());
  static const char* kTypes[] = {"other", "integrated", "discrete", "virtual", "cpu"};
  for (uint32_t i = 0; i < n; ++i) {
    // VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID, deviceID, deviceType,
    // deviceName[256], ... (limits etc. follow; the buffer is larger than the whole struct).
    alignas(8) unsigned char p[4096] = {};
    props(devs[i], p);
    uint32_t f[5];
    std::memcpy(f, p, sizeof f);
    char key[64];
    std::snprintf(key, sizeof key, "vulkan.device%u", i);
    outf(key, "%s api=%s vendor=0x%04x type=%s", reinterpret_cast<const char*>(p + 20), vkVer(f[0]).c_str(), f[2],
         f[4] < 5 ? kTypes[f[4]] : "?");
  }
  if (destroy) destroy(inst, nullptr);
}

void probeOpenGL() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"rn_winprobe_gl";
  RegisterClassW(&wc);
  HWND w = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
  HDC dc = w ? GetDC(w) : nullptr;
  if (!dc) return out("opengl", "no window/DC (non-interactive session?)");
  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof pfd;
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  int pf = ChoosePixelFormat(dc, &pfd);
  HMODULE gl = LoadLibraryW(L"opengl32.dll");
  using CreateCtx = HGLRC(WINAPI*)(HDC);
  using MakeCurrent = BOOL(WINAPI*)(HDC, HGLRC);
  using DeleteCtx = BOOL(WINAPI*)(HGLRC);
  using GetString = const unsigned char*(WINAPI*)(unsigned);
  auto create = sym<CreateCtx>(gl, "wglCreateContext");
  auto make = sym<MakeCurrent>(gl, "wglMakeCurrent");
  auto del = sym<DeleteCtx>(gl, "wglDeleteContext");
  auto str = sym<GetString>(gl, "glGetString");
  HGLRC ctx = (pf && SetPixelFormat(dc, pf, &pfd) && create) ? create(dc) : nullptr;
  if (ctx && make(dc, ctx)) {
    auto s = [&](unsigned e) { const unsigned char* p = str(e); return p ? std::string(reinterpret_cast<const char*>(p)) : "?"; };
    outf("opengl", "version=%s renderer=%s vendor=%s", s(0x1F02).c_str(), s(0x1F01).c_str(), s(0x1F00).c_str());
    make(nullptr, nullptr);
    del(ctx);
  } else {
    out("opengl", "no context");
  }
  ReleaseDC(w, dc);
  DestroyWindow(w);
}

void probeWasapi() {
  IMMDeviceEnumerator* en = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en));
  if (FAILED(hr)) return outf("wasapi", "MMDeviceEnumerator failed hr=0x%08lx", (unsigned long)hr);
  IMMDeviceCollection* col = nullptr;
  UINT count = 0;
  if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) col->GetCount(&count);
  release(col);
  outf("wasapi.render_endpoints", "%u", count);
  IMMDevice* dev = nullptr;
  hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  if (FAILED(hr)) {
    outf("wasapi.default", "none hr=0x%08lx", (unsigned long)hr);
    return release(en);
  }
  IAudioClient* ac = nullptr;
  if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac)))) {
    WAVEFORMATEX* fmt = nullptr;
    REFERENCE_TIME def = 0, mn = 0;
    ac->GetDevicePeriod(&def, &mn);
    if (SUCCEEDED(ac->GetMixFormat(&fmt))) {
      outf("wasapi.default", "mix=%lu Hz %u ch %u bit period default=%.2f ms min=%.2f ms", (unsigned long)fmt->nSamplesPerSec,
           fmt->nChannels, fmt->wBitsPerSample, def / 1e4, mn / 1e4);
      IAudioClient3* ac3 = nullptr;
      if (SUCCEEDED(ac->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(&ac3)))) {
        UINT32 d = 0, fund = 0, lo = 0, hi = 0;
        if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(fmt, &d, &fund, &lo, &hi)))
          outf("wasapi.engine_period_frames", "default=%u min=%u max=%u fundamental=%u (IAudioClient3 low latency)", d, lo, hi,
               fund);
        release(ac3);
      }
      CoTaskMemFree(fmt);
    }
    release(ac);
  }
  release(dev);
  release(en);
}

void probeInput(int waitSeconds) {
  HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
  if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
  using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
  using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
  auto getState = sym<GetStateFn>(xi, "XInputGetState");
  auto getCaps = sym<GetCapsFn>(xi, "XInputGetCapabilities");
  if (!getState) {
    out("xinput", "missing");
  } else {
    std::string slots;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
      XINPUT_STATE st{};
      bool on = getState(i, &st) == ERROR_SUCCESS;
      XINPUT_CAPABILITIES c{};
      if (on && getCaps) getCaps(i, 0, &c);
      char b[48];
      std::snprintf(b, sizeof b, "%s%lu=%s", i ? " " : "", (unsigned long)i, on ? (c.SubType == XINPUT_DEVSUBTYPE_GAMEPAD ? "gamepad" : "connected") : "-");
      slots += b;
    }
    out("xinput.slots", slots);
    for (int t = 0; t < waitSeconds * 20; ++t) {
      for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE st{};
        if (getState(i, &st) == ERROR_SUCCESS && st.Gamepad.wButtons) {
          outf("xinput.input", "slot %lu buttons=0x%04x", (unsigned long)i, st.Gamepad.wButtons);
          t = waitSeconds * 20;
          break;
        }
      }
      Sleep(50);
    }
  }
  HMODULE gi = LoadLibraryW(L"GameInput.dll");
  out("gameinput", gi ? (GetProcAddress(gi, "GameInputCreate") ? "GameInput.dll present (GameInputCreate exported)" : "GameInput.dll present")
                      : "missing (GameInput redistributable not installed)");
  // Raw input: HID game controllers (usage page 1: 4 joystick, 5 gamepad, 8 multi-axis).
  UINT n = 0;
  GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
  std::vector<RAWINPUTDEVICELIST> list(n);
  if (n) n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
  int kb = 0, mouse = 0, pads = 0;
  for (UINT i = 0; i < n && n != UINT(-1); ++i) {
    if (list[i].dwType == RIM_TYPEKEYBOARD) ++kb;
    if (list[i].dwType == RIM_TYPEMOUSE) ++mouse;
    if (list[i].dwType != RIM_TYPEHID) continue;
    RID_DEVICE_INFO info{};
    info.cbSize = sizeof info;
    UINT sz = sizeof info;
    if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == UINT(-1)) continue;
    USHORT page = info.hid.usUsagePage, usage = info.hid.usUsage;
    if (page != 1 || (usage != 4 && usage != 5 && usage != 8)) continue;
    wchar_t name[512] = {};
    UINT nl = 512;
    GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name, &nl);
    char key[48];
    std::snprintf(key, sizeof key, "rawinput.gamecontroller%d", pads++);
    outf(key, "vid=0x%04lx pid=0x%04lx usage=%u %s", info.hid.dwVendorId, info.hid.dwProductId, usage, utf8(name).c_str());
  }
  outf("rawinput.devices", "keyboards=%d mice=%d game_controllers=%d", kb, mouse, pads);
}

}  // namespace

int main(int argc, char** argv) {
  int wait = 0;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--wait-input") && i + 1 < argc) wait = std::atoi(argv[++i]);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  probeSystem();
  probeDxgi();
  probeD3D11();
  probeD3D12();
  probeVulkan();
  probeOpenGL();
  probeWasapi();
  probeInput(wait);
  CoUninitialize();
  return 0;
}
