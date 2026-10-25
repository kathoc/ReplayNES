"""Offline product defaults/custom-profile regression and Windows build inspection."""
import argparse
import html
import pathlib
import re
import subprocess
import tempfile

DEFAULT_FEED = "https://github.com/kathoc/ReplayNES/releases/latest/download/appcast-windows.xml"
DEFAULT_KEY = "epSlR4TX073wn6yhEigPygjpRMr2K39iu9ZbQg3OPgQ="
CUSTOM_FEED = "https://example.invalid/l1/appcast.xml"
CUSTOM_KEY = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="


def contains(path, expected):
    text = path.read_text()
    for value in expected:
        assert value in text, f"{path}: missing {value!r}"


def check_header(path, version, feed, key):
    contains(path, [f'#define RN_PRODUCT_{name} "{value}"' for name, value in
                    (("VERSION", version), ("WINDOWS_APPCAST_URL", feed),
                     ("UPDATE_PUBLIC_KEY", key))])


def check_windows(build, version, feed, key, app_version, app_feed, app_key):
    check_header(build / "generated/replaynes/product.h", version, feed, key)
    numeric = re.match(r"\d+\.\d+\.\d+", app_version).group().replace(".", ",")
    contains(build / "apps/windows/replaynes.rc", [
        f"FILEVERSION {numeric},0", f"PRODUCTVERSION {numeric},0",
        f'VALUE "FileVersion", "{app_version}"',
        f'VALUE "ProductVersion", "{app_version}"'])
    # Inspect the actual compiler inputs, not a second reconstruction of the settings.
    inputs = [*build.glob("build.ninja"), *build.glob("CMakeFiles/impl-*.ninja"),
              *build.rglob("flags.make"), *build.rglob("*.vcxproj")]
    flags = html.unescape("\n".join(p.read_text() for p in inputs))
    flags = flags.replace('\\"', '"').replace("$", "")
    for name, value in (("RNL_APP_VERSION", app_version), ("RNW_APPCAST_URL", app_feed),
                        ("RNW_UPDATE_PUBLIC_KEY", app_key)):
        assert f'{name}="{value}"' in flags, f"Windows compiler input missing {name}={value}"


def check_profiles(source, cmake):
    root_version = re.search(r"project\(ReplayNES VERSION ([0-9.]+)",
                             (source / "CMakeLists.txt").read_text()).group(1)
    with tempfile.TemporaryDirectory(prefix="replaynes-product-test-") as tmp:
        tmp = pathlib.Path(tmp)
        profile = tmp / "product.cmake"
        profile.write_text(f'include("{source}/cmake/products/replaynes.cmake")\n'
                           'set(RN_PRODUCT_VERSION "0.1.0")\n'
                           f'set(RN_PRODUCT_WINDOWS_APPCAST_URL "{CUSTOM_FEED}")\n'
                           f'set(RN_PRODUCT_UPDATE_PUBLIC_KEY "{CUSTOM_KEY}")\n')
        # A different enclosing project catches accidental use of PROJECT_VERSION.
        (tmp / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.16)\nproject(Outer VERSION 9.8.7 LANGUAGES NONE)\n'
            f'include("{source}/cmake/Product.cmake")\n'
            f'configure_file("{source}/engine/include/replaynes/product.h.in" '
            '"${CMAKE_BINARY_DIR}/product.h" @ONLY)\n')
        for custom in (False, True):
            build = tmp / ("custom" if custom else "default")
            cmd = [cmake, "-S", str(tmp), "-B", str(build)]
            if custom:
                cmd.append(f"-DRN_PRODUCT_FILE={profile}")
            subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=60)
            check_header(build / "product.h", "0.1.0" if custom else root_version,
                         CUSTOM_FEED if custom else DEFAULT_FEED,
                         CUSTOM_KEY if custom else DEFAULT_KEY)
        # The non-CMake frontend fallback must retain the same ReplayNES defaults.
        check_header(source / "frontend/include/replaynes/frontend.h", root_version,
                     DEFAULT_FEED, DEFAULT_KEY)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--windows-build", type=pathlib.Path)
    parser.add_argument("--version", default="0.6.4")
    parser.add_argument("--feed", default=DEFAULT_FEED)
    parser.add_argument("--key", default=DEFAULT_KEY)
    parser.add_argument("--app-version")
    parser.add_argument("--app-feed")
    parser.add_argument("--app-key")
    args = parser.parse_args()
    check_profiles(args.source.resolve(), args.cmake)
    if args.windows_build:
        check_windows(args.windows_build, args.version, args.feed, args.key,
                      args.app_version or args.version, args.app_feed or args.feed,
                      args.app_key or args.key)
    print("PASS: product release identity")


if __name__ == "__main__":
    main()
