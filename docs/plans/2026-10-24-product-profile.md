# Product profile: build ReplayNES's engine, frontend and Linux app as another product (2026-10-24)

## Goal

A second product (ReplayArcade, an arcade build of the same app with another emulation core) is built
from these sources with its own name, app ID, folders, file extensions and texts, chosen at configure
time. **ReplayNES itself must not change at all**: same names, paths, texts, files and test results.
This step only moves product identity into one place; cores, video sizes, timing and input are later
steps.

## Design

1. **Profile file.** `cmake/products/replaynes.cmake` sets the values below. The top-level
   `CMakeLists.txt` includes `${RN_PRODUCT_FILE}` (cache variable, default that file), so another
   repository can pass its own profile with `-DRN_PRODUCT_FILE=/path/to/product.cmake`.

   | Variable | ReplayNES value | Used for |
   |---|---|---|
   | `RN_PRODUCT_NAME` | `ReplayNES` | window title, `--version`, logs, Vulkan app name, settings header, virtual pad name, Steam shortcut name, texts |
   | `RN_PRODUCT_APP_ID` | `io.github.replaynes.ReplayNES` | SDL metadata, Flatpak, Steam launch options, update hints |
   | `RN_PRODUCT_EXECUTABLE` | `replaynes-linux` | Linux executable, update respawn, `--help` |
   | `RN_PRODUCT_DATA_DIR` | `ReplayNES` | `~/Documents/<dir>`, `$XDG_DATA_HOME/<dir>/Session`, `$XDG_CONFIG_HOME/<dir>` |
   | `RN_PRODUCT_ENV_PREFIX` | `REPLAYNES_` | every `getenv("REPLAYNES_*")` |
   | `RN_PRODUCT_PROJECT_EXT` | `.nesrec` | project folders, session temp project (`current.nesrec`), dialogs, library scan |
   | `RN_PRODUCT_PROJECT_FORMAT` | `replaynes-project` | `manifest.json` `"format"` (a product must not open the other's projects) |
   | `RN_PRODUCT_ROM_EXTS` | `nes` | library scan, open dialogs (`;`-separated list) |
   | `RN_PRODUCT_TEMP_PREFIX` | `replaynes` | `replaynes-backup-`, `.replaynes-tmp`, `.replaynes-backup` |
   | `RN_PRODUCT_L10N_OVERRIDES` | empty | optional JSON of extra/replaced strings (see 4) |
   | `RN_PRODUCT_LINUX_DESKTOP_FILE`, `RN_PRODUCT_LINUX_METAINFO_FILE`, `RN_PRODUCT_STEAM_ARTWORK_DIR` | today's files | install rules in `apps/linux` |

2. **Generated header.** `configure_file(engine/include/replaynes/product.h.in ${CMAKE_BINARY_DIR}/generated/replaynes/product.h)`
   with C string macros (`RN_PRODUCT_NAME "ReplayNES"` …), included by the engine, the frontend and the
   apps (a PUBLIC include directory of `replaynes_engine`). C macros, not `constexpr`, so the C API and
   C++ can both use them.

3. **Replace the literals** listed in the research maps with the macros: engine (`ProjectStore.cpp`
   format string and messages), frontend (`paths.cpp`, `library.cpp` extension filters, the temp
   project name), `apps/desktop` and `apps/linux` (the ~40 places: `app.cpp`, `app_model.cpp`,
   `settings.cpp`, `platform_xdg.cpp`, `audio_out.cpp`, `crt_display_policy.cpp`, `vk_renderer.cpp`,
   `headless_vulkan.cpp`, `steam_shortcut.{h,cpp}`, `update_model.cpp`, `update_service_flatpak.cpp`,
   `ui_update.cpp`, `ui_export.cpp`, `ui_dialogs.cpp`, `dialogs.h`, `main_linux.cpp`, `CMakeLists.txt`
   names and `APP_ID`). Environment variables go through one helper
   (`std::string productEnvName(const char* suffix)` → `REPLAYNES_` + suffix). `apps/macos`,
   `apps/windows` and `apps/ios` are not touched (they keep their own literals; ReplayNES only).

4. **Texts.** The translation keys stay exactly as they are (the macOS app uses the same table). At
   lookup time, when `RN_PRODUCT_NAME` differs from `"ReplayNES"`, the frontend replaces `ReplayNES`
   in the looked-up text with the product name (`rnf_tr` / the `TR` path). Texts that are NES-specific
   (".nes files", "Choose the NES ROM", "8:7", the Test Cartridge) can be replaced by the product's
   override JSON (`{"<key>": {"en": "...", "ja": "..."}}`), merged by `gen_l10n.py` at build time.
   ReplayNES has no override file, so its generated table is byte-identical.

5. **Linux packaging.** `apps/linux/CMakeLists.txt` takes the executable name, `APP_ID`, desktop file,
   metainfo and Steam artwork directory from the profile. The Flatpak manifest stays a per-product file
   (ReplayNES keeps `apps/linux/flatpak/io.github.replaynes.ReplayNES.yml` as it is).

## Done when

- Configuring with no options builds exactly today's ReplayNES: all ctest tests pass, including
  `test_golden` (per-frame digests and the frozen project fixture) and `test_desktop_frontend`.
- `grep -rn "ReplayNES\|replaynes\|REPLAYNES_\|\.nesrec" engine/src frontend/src apps/desktop/src apps/linux/src`
  finds only: include paths and type/function prefixes (`replaynes/…`, `rn_`, `rnf_`), comments, and
  the profile defaults. Every remaining hit is listed with a reason in the commit message.
- A smoke configure with a throw-away profile (`-DRN_PRODUCT_FILE=…` naming the product `TestArcade`,
  data dir `TestArcade`, extension `.arcrec`) compiles, and `test_desktop_frontend` passes there too
  (its expectations read the macros instead of literal "ReplayNES").

## Implementation decisions and validation

The specified configure-time profile is the implementation contract. Work is split into
profile/build plumbing, engine/frontend localization, desktop/Linux consumers, and test
expectations. Runtime configuration and translated-key renaming are rejected because they
would change persistence or shared localization behavior. Existing test and build tooling
provides the reusable validation; no separate automation framework is needed.

Profile strings are escaped before generating C-compatible macros. The Linux standalone
project loads the same profile in its scope. Default generated localization is compared
byte-for-byte, and engine golden tests protect rendering and persisted output. Validation
uses Unix Makefiles, a separate custom-profile build, and the Flatpak SDK when available.

### Completed validation

- Unix Makefiles Release build: all 34 tests passed, including test_golden and
  test_desktop_frontend. The final desktop changes were rebuilt and retested.
- TestArcade smoke build: desktop and localization tests passed; JSON overrides
  were verified in both English and Japanese at runtime.
- Linux app compiled with Freedesktop SDK 25.08, using a direct SDK mount because
  Flatpak startup could not create its NETLINK_ROUTE socket in this sandbox.
  Steam tests, --version, and installation into a build-local prefix passed.
- The default localization table and JSON are byte-identical to the old generator.
- Stable translation keys and emulator identities remain by design. Auxiliary
  tool executable names remain unchanged because this profile selects the main app.
- The requested external report directory is read-only in this session. The full
  Japanese report is retained at docs/reviews/s1-product-profile-codex.md.
