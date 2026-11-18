#!/usr/bin/env bash
# Replaces third_party/gamecontrollerdb with SDL_GameControllerDB at a commit (default: the newest on master):
# the table (gamecontrollerdb.txt), its licence (LICENSE) and the commit (VERSION). The app builds the target
# platform's lines in (apps/desktop/gamepad_db.cmake; docs/design/CONTROLLERS.md). Run it before a release.
#   scripts/update-gamecontrollerdb.sh [commit]
# Needs gh (logged in): the files come through the GitHub API, not a web page.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO=mdqinc/SDL_GameControllerDB
DIR="$ROOT/third_party/gamecontrollerdb"
COMMIT="${1:-$(gh api "repos/$REPO/commits/master" --jq .sha)}"
DATE="$(gh api "repos/$REPO/commits/$COMMIT" --jq .commit.committer.date)"
mkdir -p "$DIR"
raw() { gh api "repos/$REPO/contents/$1?ref=$COMMIT" -H "Accept: application/vnd.github.raw"; }
raw gamecontrollerdb.txt > "$DIR/gamecontrollerdb.txt.new"
raw LICENSE > "$DIR/LICENSE.new"
# Sanity: mapping lines (GUID,name,...,platform:X,) and nothing that looks like a web page.
lines="$(grep -cE '^[0-9a-f]{32},.*platform:' "$DIR/gamecontrollerdb.txt.new" || true)"
[ "$lines" -gt 1000 ] || { echo "only $lines mapping lines: not replaced" >&2; rm -f "$DIR"/*.new; exit 1; }
mv "$DIR/gamecontrollerdb.txt.new" "$DIR/gamecontrollerdb.txt"
mv "$DIR/LICENSE.new" "$DIR/LICENSE"
printf '%s %s https://github.com/%s\n' "$COMMIT" "$DATE" "$REPO" > "$DIR/VERSION"
for p in Windows Linux Android "Mac OS X"; do
  printf '%-9s %s\n' "$p" "$(grep -c "platform:$p," "$DIR/gamecontrollerdb.txt")"
done
echo "==> $DIR at ${COMMIT:0:12} ($DATE), $lines mappings"
