#!/bin/bash
# Sends trunk to kathoc/ReplayArcade's main with ReplayArcade's README on top.
#
# Both pages get the same tree (the monorepo's trunk), and GitHub shows the README at the top of a
# repository, which is ReplayNES's. GitHub reads .github/README.md before the top one, so the
# ReplayArcade page's main is trunk plus one file, .github/README.md, made here from
# products/ReplayArcade/README.md (2026-11-19, user decision). trunk itself never has that file, so the
# ReplayNES page is unchanged.
#
# The page's main moves forward only: the first commit has trunk as its parent, every later one the
# page's previous main and trunk (a merge), so `git push` never rewrites the page. The script stops when
# the page's main has anything other than trunk and this README (someone pushed there by hand).
#
#   scripts/push-arcade-page.sh             build the commit and push it (outside the sandbox: gh auth)
#   scripts/push-arcade-page.sh --dry-run   build the commit, show what would be pushed, push nothing
#
# Environment: ARCADE_REMOTE (default gh-arcade), TRUNK (default trunk).
set -euo pipefail

ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
REMOTE="${ARCADE_REMOTE:-gh-arcade}"
TRUNK_REF="${TRUNK:-trunk}"
OVERLAY=.github/README.md
PAGE_URL=https://github.com/kathoc/ReplayArcade/blob/main/products/ReplayArcade
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

git_gh() { git -c credential.helper= -c 'credential.helper=!gh auth git-credential' -C "$ROOT" "$@"; }
die() { echo "push-arcade-page: $*" >&2; exit 1; }

TRUNK_SHA="$(git -C "$ROOT" rev-parse --verify "$TRUNK_REF^{commit}")"
git -C "$ROOT" cat-file -e "$TRUNK_SHA:$OVERLAY" 2>/dev/null && die "$TRUNK_REF has $OVERLAY itself (it would show on the ReplayNES page too)"

git_gh fetch -q "$REMOTE" main
PAGE="$(git -C "$ROOT" rev-parse FETCH_HEAD)"

# The page's main: trunk (or an older trunk), or a commit of this script (trunk + the README).
if git -C "$ROOT" merge-base --is-ancestor "$PAGE" "$TRUNK_SHA"; then
  PARENTS=(-p "$TRUNK_SHA")
else
  n="$(git -C "$ROOT" rev-list --parents -n 1 "$PAGE" | wc -w)"
  base="$(git -C "$ROOT" rev-parse "$PAGE^$((n - 1))")"   # the last parent: the trunk commit it was made from
  git -C "$ROOT" merge-base --is-ancestor "$base" "$TRUNK_SHA" || die "the page's main ($PAGE) is not trunk + README: $base is not in $TRUNK_REF"
  other="$(git -C "$ROOT" diff --name-only "$base" "$PAGE")"
  [ "$other" = "$OVERLAY" ] || die "the page's main differs from $base by more than $OVERLAY: $(echo $other)"
  PARENTS=(-p "$PAGE" -p "$TRUNK_SHA")
fi

# The README: ReplayArcade's, its link to the Japanese one made absolute (a link in .github/ would be
# read from .github/).
readme="$(git -C "$ROOT" show "$TRUNK_SHA:products/ReplayArcade/README.md")"
readme="${readme//](README.ja.md)/]($PAGE_URL/README.ja.md)}"
blob="$(printf '%s\n' "$readme" | git -C "$ROOT" hash-object -w --stdin)"

index="$(mktemp "${TMPDIR:-/tmp}/arcade-page-index.XXXXXX")"
trap 'rm -f "$index"' EXIT
GIT_INDEX_FILE="$index" git -C "$ROOT" read-tree "$TRUNK_SHA"
GIT_INDEX_FILE="$index" git -C "$ROOT" update-index --add --cacheinfo "100644,$blob,$OVERLAY"
tree="$(GIT_INDEX_FILE="$index" git -C "$ROOT" write-tree)"

if [ "$(git -C "$ROOT" rev-parse "$PAGE^{tree}")" = "$tree" ]; then
  echo "push-arcade-page: the page's main already is $TRUNK_REF ($(git -C "$ROOT" rev-parse --short "$TRUNK_SHA")) + README"
  exit 0
fi

msg="ReplayArcade page: $TRUNK_REF $(git -C "$ROOT" rev-parse --short "$TRUNK_SHA") with ReplayArcade's README ($OVERLAY)

Made by scripts/push-arcade-page.sh: the same tree as $TRUNK_REF plus $OVERLAY (products/ReplayArcade/README.md),
which GitHub shows on this page instead of ReplayNES's README."
commit="$(printf '%s\n' "$msg" | git -C "$ROOT" commit-tree "$tree" "${PARENTS[@]}")"

echo "push-arcade-page: page main $(git -C "$ROOT" rev-parse --short "$PAGE") -> $(git -C "$ROOT" rev-parse --short "$commit") ($TRUNK_REF $(git -C "$ROOT" rev-parse --short "$TRUNK_SHA") + $OVERLAY)"
git -C "$ROOT" diff --stat "$TRUNK_SHA" "$commit" | tail -2
if [ "$DRY" = 1 ]; then
  echo "push-arcade-page: --dry-run, nothing pushed (commit $commit)"
  exit 0
fi
git_gh push "$REMOTE" "$commit:refs/heads/main"
