# Bezel (withdrawn)

A product bezel around horizontal games (ReplayArcade: an arcade cabinet's screen surround) was
built on 2026-11-01 for the desktop frontend and the iPhone app, then withdrawn the same day by user
decision ("ベゼルは廃止"). The code is in the history (desktop 8bec9f1, 28a9eed; iPhone a6ef438 on
ios-generalize) and was removed afterwards; nothing of it remains in the product.

What stays from that work: apps/desktop/src/qoi.{h,cpp}, a self-contained QOI codec (written because
the Flatpak runtime's SDL, older than 3.4, has no PNG loader), now used for the seek bar thumbnails
kept across sessions.
