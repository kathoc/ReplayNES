#!/bin/sh
# Prints the directory for build outputs of the repository $1 (default: this checkout).
#   RN_BUILD_ROOT set               -> that
#   the repository is inside a File Provider domain (iCloud Drive's "Desktop & Documents",
#   a parent carries the com.apple.file-provider-domain-id attribute; macOS only)
#                                   -> ~/Library/Developer/ReplayNES-build/<repository path with / -> _>
#   otherwise                       -> <repository>/build
# Why: in a File Provider domain every new file gets com.apple.fileprovider.* / FinderInfo
# attributes and codesign refuses the app ("resource fork, Finder information, or similar detritus
# not allowed"); a symlinked build/ does not help (iCloud renamed the link to "build 2"). 2026-10-23.
set -eu
REPO="$(cd "${1:-$(dirname "$0")/../..}" && pwd)"
if [ -n "${RN_BUILD_ROOT:-}" ]; then echo "$RN_BUILD_ROOT"; exit 0; fi
if command -v xattr >/dev/null 2>&1 && [ "$(uname)" = Darwin ]; then
  d="$REPO"
  while [ "$d" != "/" ]; do
    if xattr "$d" 2>/dev/null | grep -q '^com\.apple\.file-provider-domain-id$'; then
      echo "$HOME/Library/Developer/ReplayNES-build/$(printf '%s' "$REPO" | sed 's#^/##; s#/#_#g')"
      exit 0
    fi
    d="$(dirname "$d")"
  done
fi
echo "$REPO/build"
