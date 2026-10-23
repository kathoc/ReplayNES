#!/bin/bash
# Publishes the Flatpak repository commit that scripts/publish-flatpak-repo.sh prepared:
# force-pushes build/gh-pages's branch gh-pages-publish to GitHub's gh-pages. Nothing else.
#   scripts/push-gh-pages.sh            (scripts/publish-flatpak-repo.sh calls it unless NO_PUSH=1)
# Why a script of its own: gh-pages is ONE orphan commit replaced on every release (the OSTree
# history never accumulates in git), so the push is always forced. As a fixed, checked step it can
# be allowed in Claude Code's settings (docs/RELEASE.md) instead of a free-form `git push -f`.
# Checks before pushing: the commit is a single orphan commit titled "Flatpak repository: ...",
# whose flatpak/ holds a summary and ReplayNES.flatpakrepo. Pushes over https with gh's login
# (rtxsrv's ssh key is a read-only deploy key).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PAGES_DIR="$ROOT/build/gh-pages"
GH_REPO=kathoc/ReplayNES
cd "$PAGES_DIR"
git rev-parse -q --verify gh-pages-publish >/dev/null || { echo "no branch gh-pages-publish in $PAGES_DIR (run scripts/publish-flatpak-repo.sh)" >&2; exit 1; }
[ "$(git rev-list --count gh-pages-publish)" = 1 ] || { echo "gh-pages-publish is not a single orphan commit" >&2; exit 1; }
subject="$(git log -1 --format=%s gh-pages-publish)"
case "$subject" in "Flatpak repository: ReplayNES "*) ;; *) echo "unexpected commit: $subject" >&2; exit 1 ;; esac
for f in flatpak/summary flatpak/ReplayNES.flatpakrepo; do
  git cat-file -e "gh-pages-publish:$f" 2>/dev/null || { echo "$f missing in the commit" >&2; exit 1; }
done
echo "==> push $subject ($(git rev-parse --short gh-pages-publish)) -> $GH_REPO gh-pages"
git -c credential.helper= -c credential.helper='!gh auth git-credential' \
  push -q -f "https://github.com/$GH_REPO.git" gh-pages-publish:gh-pages
git ls-remote "https://github.com/$GH_REPO.git" refs/heads/gh-pages
