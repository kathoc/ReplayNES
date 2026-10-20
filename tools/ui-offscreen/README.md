# Offscreen desktop UI check

Runs the production `UI::build`/ImGui menus and dialogs with SDL3's software renderer. It tests keyboard selection, preference persistence and the Live session, then writes actual draw-data screenshots. The synthetic cartridge intentionally never enables rendering; its uniform gray picture is not a game title. An optional owned ROM is run for at most 120 frames, stopping on the first core error.

This validates shared Linux/Windows UI code, not Vulkan/D3D swapchains, platform audio, CRT processing or the independent macOS SwiftUI implementation. Export/CRT features are explicitly unavailable in this test executable. No emulator functionality is stubbed.

Build SDL3 in a scratch directory (no system installation needed):

```sh
cmake -S third_party/SDL -B /tmp/replaynes-ui-sdl \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF \
  -DSDL_PIPEWIRE=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
  -DSDL_VULKAN=OFF -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_SHARED=OFF \
  -DSDL_STATIC=ON -DSDL_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/replaynes-ui-sdl -j4
cmake -S tools/ui-offscreen -B /tmp/replaynes-ui-offscreen \
  -DSDL3_DIR=/tmp/replaynes-ui-sdl -DCMAKE_BUILD_TYPE=Debug \
  -DREPLAYNES_ENABLE_REPLAYNESCORE=ON -DREPLAYNESCORE_SOURCE_DIR=/absolute/path/ReplayNEScore
cmake --build /tmp/replaynes-ui-offscreen --target replaynes-ui-offscreen test_desktop_frontend -j4
/tmp/replaynes-ui-offscreen/replaynes-ui-offscreen /tmp/replaynes-ui-evidence [owned-rom.nes]
```

The screenshot directory also holds scratch settings, bindings and the generated test ROM. Use a dedicated directory. The test uses `DejaVuSans.ttf` and `NotoSansCJK-Regular.ttc` at the standard Linux font paths, and writes English and Japanese menu screenshots. Keep an external wall/CPU deadline on build/run commands; the UI run has no event loop and its emulation loop is capped internally.
