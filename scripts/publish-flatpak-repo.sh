#!/bin/bash
# Publishes the Linux Flatpak (io.github.replaynes.ReplayNES) to its signed OSTree repository on
# GitHub Pages: https://kathoc.github.io/ReplayNES/flatpak/ (branch gh-pages, folder flatpak/).
# Installs from that repository (the .flatpakref, or the bundle built here with --repo-url) get
# updates: `flatpak update`, Discover, and the app's own update notice (Flatpak portal).
#
#   scripts/publish-flatpak-repo.sh                  # build on the Deck, sign, publish, push gh-pages
#   NO_PUSH=1 scripts/publish-flatpak-repo.sh        # ... but only commit gh-pages locally (build/gh-pages)
#   SKIP_BUILD=1 ...                                 # reuse the Deck's last build (~/ReplayNES-dev/repo)
#   TARGET=test scripts/publish-flatpak-repo.sh      # a local test repository on the Deck instead
#                                                    # (~/ReplayNES-dev/test-repo, file:// URL; gh-pages untouched)
#
# Steps: (1) scripts/build-linux-flatpak.sh (NO_INSTALL=1 NO_BUNDLE=1) builds into the Deck's
# unsigned build repo; (2) the published repository (gh-pages flatpak/, checked out in
# build/gh-pages) is copied to the Deck; (3) there `flatpak build-commit-from` adds the build as a
# new signed commit, `flatpak build-update-repo` signs the summary, generates static deltas and
# prunes everything older than KEEP commits; (4) the bundle dist/<id>-<version>-x86_64.flatpak is
# made from the signed commit with --repo-url (bundle installs register the remote, so they
# update too); (5) the repository comes back to build/gh-pages with ReplayNES.flatpakrepo,
# io.github.replaynes.ReplayNES.flatpakref (signing key embedded) and is committed as ONE orphan
# commit and force-pushed: gh-pages never accumulates old repository objects in git history.
#
# Signing key: GnuPG home ~/.local/share/replaynes-signing/gnupg on the Deck (no passphrase; see
# docs/RELEASE.md "Flatpak repository" for backup / restore). Its public key is
# apps/linux/flatpak/replaynes-repo.gpg. The private key is never copied anywhere by this script.
# Env: HOST (deck@steamdeck.local), REMOTE_DIR (ReplayNES-dev), KEEP (3), GPG_KEY, SIGN_HOME,
#      REPO_URL (TARGET=pages: the Pages URL; TARGET=test: file://<host home>/<REMOTE_DIR>/test-repo/).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${HOST:-deck@steamdeck.local}"
RDIR="${REMOTE_DIR:-ReplayNES-dev}"
TARGET="${TARGET:-pages}"
APP_ID=io.github.replaynes.ReplayNES
KEEP="${KEEP:-3}"
GPG_KEY="${GPG_KEY:-5267770BF84CC0DA2FB9B8D093BCD9083B5DF142}"
SIGN_HOME="${SIGN_HOME:-.local/share/replaynes-signing/gnupg}"  # relative to the host's home
PUBKEY="$ROOT/apps/linux/flatpak/replaynes-repo.gpg"
PAGES_URL=https://kathoc.github.io/ReplayNES/flatpak/
GH_REPO=kathoc/ReplayNES
PAGES_DIR="$ROOT/build/gh-pages"
VERSION="$(sed -n 's/^project(ReplayNES VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUNDLE="$APP_ID-$VERSION-x86_64.flatpak"
[ -f "$PUBKEY" ] || { echo "missing $PUBKEY" >&2; exit 1; }
[ "$KEEP" -ge 1 ] || { echo "KEEP must be >= 1" >&2; exit 1; }

HOST_HOME="$(ssh "$HOST" 'echo $HOME')"
case "$TARGET" in
  pages)
    REPO_URL="${REPO_URL:-$PAGES_URL}"
    DEST="$RDIR/publish/flatpak"
    ;;
  test)
    DEST="$RDIR/test-repo"
    REPO_URL="${REPO_URL:-file://$HOST_HOME/$DEST/}"
    ;;
  *) echo "TARGET must be pages or test" >&2; exit 2 ;;
esac
GPGKEY_B64="$(base64 < "$PUBKEY" | tr -d '\n')"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "==> build ($VERSION)"
  NO_INSTALL=1 NO_BUNDLE=1 HOST="$HOST" REMOTE_DIR="$RDIR" "$ROOT/scripts/build-linux-flatpak.sh"
fi

if [ "$TARGET" = pages ]; then
  echo "==> gh-pages checkout ($PAGES_DIR)"
  git -C "$ROOT" fetch -q origin gh-pages 2>/dev/null || true
  if [ ! -e "$PAGES_DIR/.git" ]; then
    git -C "$ROOT" worktree prune
    if git -C "$ROOT" rev-parse -q --verify origin/gh-pages >/dev/null; then
      git -C "$ROOT" worktree add -q --detach "$PAGES_DIR" origin/gh-pages
    else
      git -C "$ROOT" worktree add -q --orphan -b gh-pages-publish "$PAGES_DIR"
    fi
  elif git -C "$ROOT" rev-parse -q --verify origin/gh-pages >/dev/null; then
    git -C "$PAGES_DIR" reset -q --hard
    git -C "$PAGES_DIR" clean -qfdx
    git -C "$PAGES_DIR" checkout -q --detach origin/gh-pages
  fi
  mkdir -p "$PAGES_DIR/flatpak"
  echo "==> published repository -> $HOST:~/$DEST"
  ssh "$HOST" "mkdir -p ~/$DEST"
  rsync -a --delete --exclude=/tmp/ -e ssh "$PAGES_DIR/flatpak/" "$HOST:$DEST/"
fi

echo "==> sign + commit + summary + deltas (keep $KEEP commits)"
scp -q "$PUBKEY" "$HOST:$RDIR/replaynes-repo.gpg"
ssh "$HOST" "set -e; cd ~  # paths are relative to the home folder
G=\$HOME/$SIGN_HOME
[ -d \"\$G\" ] || { echo 'no signing key at '\$G' (docs/RELEASE.md: Flatpak repository)' >&2; exit 1; }
[ -f $DEST/config ] || ostree init --mode=archive-z2 --repo=$DEST
mkdir -p $DEST/refs/heads $DEST/refs/remotes $DEST/refs/mirrors  # git does not keep empty dirs; ostree needs them
flatpak build-commit-from --src-repo=$RDIR/repo --gpg-sign=$GPG_KEY --gpg-homedir=\"\$G\" --no-update-summary \
  --subject='ReplayNES $VERSION' $DEST app/$APP_ID/x86_64/master
flatpak build-update-repo --title=ReplayNES --homepage=https://github.com/$GH_REPO --default-branch=master \
  --gpg-sign=$GPG_KEY --gpg-homedir=\"\$G\" --generate-static-deltas --static-delta-ignore-ref='*.Debug' \
  --prune --prune-depth=$((KEEP - 1)) $DEST
ostree --repo=$DEST log app/$APP_ID/x86_64/master | grep -E '^commit|^ +ReplayNES' | head -$((KEEP * 2))
du -sh $DEST
cat > $DEST/ReplayNES.flatpakrepo <<EOF
[Flatpak Repo]
Title=ReplayNES
Url=$REPO_URL
Homepage=https://github.com/$GH_REPO
Comment=Record, rewind and re-record NES play; export one continuous video
GPGKey=$GPGKEY_B64
EOF
cat > $DEST/$APP_ID.flatpakref <<EOF
[Flatpak Ref]
Name=$APP_ID
Branch=master
Title=ReplayNES
Url=$REPO_URL
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
IsRuntime=false
SuggestRemoteName=replaynes
Homepage=https://github.com/$GH_REPO
GPGKey=$GPGKEY_B64
EOF
echo '==> bundle (--repo-url $REPO_URL)'
OUT=\$HOME/$RDIR/$([ "$TARGET" = test ] && echo test-)$BUNDLE
flatpak build-bundle --repo-url=$REPO_URL --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  --gpg-keys=$RDIR/replaynes-repo.gpg $DEST \"\$OUT\" $APP_ID master
echo \"    $HOST:\$OUT\""

if [ "$TARGET" = test ]; then
  echo "==> test repository: $HOST:~/$DEST ($REPO_URL)"
  exit 0
fi

mkdir -p "$ROOT/dist"
scp -q "$HOST:$RDIR/$BUNDLE" "$ROOT/dist/"
echo "    $ROOT/dist/$BUNDLE"

echo "==> repository -> $PAGES_DIR/flatpak"
rsync -a --delete --exclude=/tmp/ --exclude=/index.html -e ssh "$HOST:$DEST/" "$PAGES_DIR/flatpak/"
touch "$PAGES_DIR/.nojekyll"  # Jekyll would drop files starting with "_" (static delta names)
index_html() {  # $1: path of flatpak/ relative to the page
  cat <<EOF
<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>ReplayNES Flatpak repository</title>
<p><a href="https://github.com/$GH_REPO">ReplayNES</a> ${VERSION}: Flatpak repository for Linux / Steam Deck.</p>
<ul><li><a href="$1$APP_ID.flatpakref">$APP_ID.flatpakref</a>: <code>flatpak install --user $PAGES_URL$APP_ID.flatpakref</code></li>
<li><a href="$1ReplayNES.flatpakrepo">ReplayNES.flatpakrepo</a>: <code>flatpak remote-add --user replaynes ${PAGES_URL}ReplayNES.flatpakrepo</code></li></ul>
EOF
}
index_html flatpak/ > "$PAGES_DIR/index.html"
index_html "" > "$PAGES_DIR/flatpak/index.html"

echo "==> gh-pages commit (orphan, single commit)"
cd "$PAGES_DIR"
git checkout -q --orphan gh-pages-publish-new
git add -A
git commit -q -m "Flatpak repository: ReplayNES $VERSION" -m "Signed OSTree repository (summary + commits, key $GPG_KEY), static deltas, last $KEEP commits. Generated by scripts/publish-flatpak-repo.sh."
git branch -D gh-pages-publish >/dev/null 2>&1 || true
git branch -m gh-pages-publish
git log --oneline -1
if [ "${NO_PUSH:-0}" = "1" ]; then
  echo "==> NO_PUSH=1: not pushed (git -C $PAGES_DIR push -f origin gh-pages-publish:gh-pages)"
  exit 0
fi
git push -q -f origin gh-pages-publish:gh-pages
if ! gh api "repos/$GH_REPO/pages" >/dev/null 2>&1; then
  # GitHub may enable Pages by itself for a new gh-pages branch (then this answers 409).
  echo "==> enable GitHub Pages (gh-pages /)"
  gh api -X POST "repos/$GH_REPO/pages" -f 'source[branch]=gh-pages' -f 'source[path]=/' >/dev/null 2>&1 ||
    gh api "repos/$GH_REPO/pages" >/dev/null
fi
echo "==> pushed. $PAGES_URL (GitHub Pages deploys in a minute or two; the CDN caches ~10 min)"
