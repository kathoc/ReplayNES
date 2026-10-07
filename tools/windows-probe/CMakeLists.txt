# replaynes-winprobe: reports the graphics / audio / input APIs a Windows machine offers
# (D3D11/D3D12/DXGI, Vulkan loader, OpenGL, XInput/GameInput/raw HID, WASAPI). Diagnostic only;
# results for the development VM are recorded in docs/WINDOWS.md. Every API is loaded at run
# time, so the probe starts (and reports "missing") on machines without a given runtime.
add_executable(replaynes-winprobe probe.cpp)
target_link_libraries(replaynes-winprobe PRIVATE ole32 user32 gdi32)
target_compile_definitions(replaynes-winprobe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(replaynes-winprobe PRIVATE /W3 /utf-8)
endif()
