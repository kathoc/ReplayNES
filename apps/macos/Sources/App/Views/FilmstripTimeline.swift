// Timeline like a video editor: a filmstrip of thumbnails across the take with the playhead,
// bookmarks and the A/B practice ranges drawn on top, plus a range lane above it where the
// A/B ranges are edited directly (drag = new range for the selected slot, drag a range's ends =
// move A/B, click a range = practice it).
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI

enum SlotColors {
    static let all: [Color] = [.orange, .blue, .green, .pink, .purple, .teal, .yellow, .red]
    static func color(_ slot: Int) -> Color { all[((slot % all.count) + all.count) % all.count] }
}

struct FilmstripTimeline: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject private var strip = FilmstripModel.shared

    static let laneHeight: CGFloat = 16
    static let stripHeight: CGFloat = 40
    static var tileWidth: CGFloat { stripHeight * CGFloat(RN_VIDEO_WIDTH) / CGFloat(RN_VIDEO_HEIGHT) }

    private enum DragKind {
        case scrub
        case select(slot: Int)
        case handle(slot: Int, TimelineHandle, a: UInt64, b: UInt64)
        case body(slot: Int)
        case ignore
    }

    @State private var dragKind: DragKind?
    @State private var scrubFrame: UInt64?
    @State private var preview: TimelineRange?   // range being edited (not committed yet)

    private struct LayoutKey: Equatable {
        var width: Double
        var length: UInt64
    }

    var body: some View {
        GeometryReader { geo in
            let w = Double(geo.size.width)
            let st = model.status
            let g = TimelineGeometry(width: w, length: st.takeLength)
            let ranges = TimelineEditing.visibleRanges(slots: model.practiceSlots, takes: model.takes,
                                                       activeTake: st.activeTake, takeLength: st.takeLength)
            let version = strip.version
            let fade = strip.fade
            TimelineView(.animation(minimumInterval: nil, paused: fade == nil)) { tl in
                Canvas { ctx, size in
                    _ = version
                    draw(&ctx, size: size, g: g, st: st, ranges: ranges, fade: fade, now: tl.date)
                }
            }
            .contentShape(Rectangle())
            .gesture(dragGesture(g: g, st: st, ranges: ranges))
            .onAppear { strip.layout(width: w, tileWidth: Double(Self.tileWidth), takeLength: st.takeLength) }
            .onChange(of: LayoutKey(width: w, length: st.takeLength)) { _, k in
                strip.layout(width: k.width, tileWidth: Double(Self.tileWidth), takeLength: k.length)
            }
        }
        .frame(height: Self.laneHeight + Self.stripHeight)
        .help("タイムライン: ドラッグで移動（無音・一時停止）。上の帯をドラッグ（またはShift+ドラッグ）で選択中の区間のA/Bを設定、区間の端をドラッグで調整、区間をクリックで練習")
    }

    // MARK: drawing

    private func draw(_ ctx: inout GraphicsContext, size: CGSize, g: TimelineGeometry, st: EmuStatus,
                      ranges: [TimelineRange], fade: (from: UInt64, start: Date)?, now: Date) {
        let lane = Self.laneHeight
        let stripRect = CGRect(x: 0, y: lane, width: size.width, height: Self.stripHeight)
        let stripPath = Path(roundedRect: stripRect, cornerRadius: 4)
        ctx.fill(stripPath, with: .color(Color.secondary.opacity(0.18)))

        // Thumbnails anchored to take time (ThumbnailGrid.tiles): they slide smoothly as the take
        // grows; the step change (x2) crossfades from the previous layout.
        var thumbs = ctx
        thumbs.clip(to: stripPath)
        let tw = Double(Self.tileWidth)
        let step = ThumbnailGrid.tileStep(width: g.width, tileWidth: tw, length: g.length,
                                          current: strip.tileStep == 0 ? nil : strip.tileStep)
        drawTiles(&thumbs, g: g, step: step)
        if let f = fade, f.from != step, f.from == step * 2 || f.from * 2 == step {
            let p = now.timeIntervalSince(f.start) / FilmstripModel.fadeDuration
            if p < 1 {
                var old = thumbs
                old.opacity = 1 - max(0, p)
                drawTiles(&old, g: g, step: f.from)
            }
        }

        let selected = strip.selectedSlot
        var shown = ranges
        if let p = preview {
            shown.removeAll { $0.slot == p.slot }
            shown.append(p)
        }
        // Selected slot on top.
        shown.sort { ($0.slot == selected ? 1 : 0, $0.slot) < ($1.slot == selected ? 1 : 0, $1.slot) }

        for r in shown {
            let c = SlotColors.color(r.slot)
            let xa = g.x(forFrame: r.a)
            let hl = r.slot == selected || preview?.slot == r.slot
            if let b = r.b {
                let xb = max(g.x(forFrame: b), xa + 2)
                // Over the filmstrip: tinted band with edges.
                thumbs.fill(Path(CGRect(x: xa, y: Double(lane), width: xb - xa, height: Double(Self.stripHeight))),
                            with: .color(c.opacity(hl ? 0.28 : 0.16)))
                // Lane bar with number.
                let bar = CGRect(x: xa, y: 2, width: xb - xa, height: Double(lane) - 4)
                ctx.fill(Path(roundedRect: bar, cornerRadius: 3), with: .color(c.opacity(hl ? 0.95 : 0.6)))
                if hl {
                    ctx.stroke(Path(roundedRect: bar, cornerRadius: 3), with: .color(.white.opacity(0.9)), lineWidth: 1)
                }
                for x in [xa, xb] {
                    ctx.fill(Path(CGRect(x: x - 1, y: 0, width: 2, height: Double(lane + Self.stripHeight))),
                             with: .color(c.opacity(hl ? 1 : 0.7)))
                }
                if xb - xa >= 12 {
                    ctx.draw(Text("\(r.slot + 1)").font(.system(size: 10, weight: .bold)).foregroundColor(.black),
                             at: CGPoint(x: xa + 3, y: Double(lane) / 2), anchor: .leading)
                }
            } else {
                // A only: flag at A.
                ctx.fill(Path(CGRect(x: xa - 1, y: 0, width: 2, height: Double(lane + Self.stripHeight))),
                         with: .color(c.opacity(hl ? 1 : 0.7)))
                let flag = CGRect(x: xa, y: 2, width: 22, height: Double(lane) - 4)
                ctx.fill(Path(roundedRect: flag, cornerRadius: 3), with: .color(c.opacity(hl ? 0.95 : 0.6)))
                ctx.draw(Text("\(r.slot + 1)A").font(.system(size: 9, weight: .bold)).foregroundColor(.black),
                         at: CGPoint(x: xa + 3, y: Double(lane) / 2), anchor: .leading)
            }
        }

        // Bookmarks.
        for b in model.bookmarks where b.onActiveTake {
            let x = g.x(forFrame: b.frame)
            ctx.fill(Path(CGRect(x: x - 1, y: Double(lane), width: 2, height: 10)), with: .color(.yellow))
        }

        // Playhead: take cursor, or the practice position inside the practiced range.
        var head: (x: Double, color: Color)?
        if st.practicing {
            if let r = ranges.first(where: { $0.slot == st.practiceSlot }) {
                let f = r.a + min(st.practiceFrame, (r.b ?? r.a) - r.a)
                head = (g.x(forFrame: f), .orange)
            } else if st.practiceLength > 0 {
                // Range not on this take (A set inside practice / on another take): A->B progress.
                let p = min(1, Double(st.practiceFrame) / Double(st.practiceLength))
                ctx.fill(Path(CGRect(x: 0, y: Double(lane + Self.stripHeight) - 4, width: g.width * p, height: 4)),
                         with: .color(.orange.opacity(0.9)))
            }
        } else if g.length > 0 {
            head = (g.x(forFrame: scrubFrame ?? st.frame), st.recording ? .red : .white)
        }
        if let h = head {
            let x = max(1, min(g.width - 1, h.x))
            ctx.fill(Path(CGRect(x: x - 1, y: Double(lane) - 2, width: 2, height: Double(Self.stripHeight) + 2)),
                     with: .color(h.color))
            var tri = Path()
            tri.move(to: CGPoint(x: x - 5, y: Double(lane) - 7))
            tri.addLine(to: CGPoint(x: x + 5, y: Double(lane) - 7))
            tri.addLine(to: CGPoint(x: x, y: Double(lane) - 1))
            tri.closeSubpath()
            ctx.fill(tri, with: .color(h.color))
        }
        ctx.stroke(stripPath, with: .color(.secondary.opacity(0.35)), lineWidth: 1)
    }

    /// Draws the filmstrip tiles for `step` frames per tile. A tile whose picture is not cached
    /// (yet) shows the nearest cached one: no blank tiles once anything is cached.
    private func drawTiles(_ ctx: inout GraphicsContext, g: TimelineGeometry, step: UInt64) {
        let tw = Double(Self.tileWidth)
        let y = Double(Self.laneHeight), h = Double(Self.stripHeight)
        let tiles = ThumbnailGrid.tiles(width: g.width, length: g.length, step: step)
        for (i, t) in tiles.enumerated() {
            let clip = CGRect(x: t.x, y: y, width: t.span, height: h)
            if let img = strip.cache.image(near: t.frame, tolerance: .max) {
                var c = ctx
                c.clip(to: Path(clip))
                let image = Image(decorative: img, scale: 1).interpolation(.none)
                var x = t.x
                repeat {  // natural aspect; repeated only when the take is too short to fill the strip
                    c.draw(image, in: CGRect(x: x, y: y, width: tw, height: h))
                    x += tw
                } while x < t.x + t.span
            }
            if i > 0 {  // subtle separator at the anchor
                ctx.fill(Path(CGRect(x: t.x - 0.5, y: y, width: 1, height: h)), with: .color(.black.opacity(0.35)))
            }
        }
    }

    // MARK: gestures

    private func dragGesture(g: TimelineGeometry, st: EmuStatus, ranges: [TimelineRange]) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { v in
                if dragKind == nil { dragKind = beginDrag(at: v.startLocation, g: g, st: st, ranges: ranges) }
                updateDrag(v, g: g, st: st)
            }
            .onEnded { v in
                endDrag(v, g: g, st: st)
                dragKind = nil
            }
    }

    private func beginDrag(at p: CGPoint, g: TimelineGeometry, st: EmuStatus, ranges: [TimelineRange]) -> DragKind {
        guard st.hasSession, g.length > 0 else { return .ignore }
        let inLane = p.y < Self.laneHeight
        let mods = NSEvent.modifierFlags
        let selectMod = mods.contains(.shift) || mods.contains(.option)
        if inLane || selectMod {
            let hit = TimelineEditing.hitTest(x: Double(p.x), ranges: ranges, in: g, preferred: strip.selectedSlot)
            if st.practicing {
                if case .body(let slot) = hit { return .body(slot: slot) }
                if case .handle(let slot, _) = hit { return .body(slot: slot) }
                model.flash(EmulationController.practiceBlockedText)
                return .ignore
            }
            if !selectMod || inLane {
                switch hit {
                case .handle(let slot, let h):
                    if let r = ranges.first(where: { $0.slot == slot }), let b = r.b {
                        strip.selectedSlot = slot
                        return .handle(slot: slot, h, a: r.a, b: b)
                    }
                case .body(let slot) where !selectMod:
                    return .body(slot: slot)
                default: break
                }
            }
            return .select(slot: strip.selectedSlot)
        }
        return st.practicing ? .ignore : .scrub
    }

    private func updateDrag(_ v: DragGesture.Value, g: TimelineGeometry, st: EmuStatus) {
        switch dragKind {
        case .scrub:
            let f = g.frame(atX: Double(v.location.x))
            if f != scrubFrame { scrubFrame = f; model.scrub(to: f) }
        case .select(let slot):
            if let r = TimelineEditing.range(fromX: Double(v.startLocation.x), toX: Double(v.location.x), in: g) {
                preview = TimelineRange(slot: slot, a: r.a, b: r.b)
            } else {
                preview = nil
            }
        case .handle(let slot, let h, let a, let b):
            let r = TimelineEditing.drag(h, of: (a, b), toX: Double(v.location.x), in: g)
            preview = TimelineRange(slot: slot, a: r.a, b: r.b)
        case .body(let slot):
            // Dragging from a range body (lane) selects a new range for the selected slot instead.
            if abs(v.translation.width) > 3 && !st.practicing {
                dragKind = .select(slot: strip.selectedSlot)
                _ = slot
                updateDrag(v, g: g, st: st)
            }
        case .ignore, .none:
            break
        }
    }

    private func endDrag(_ v: DragGesture.Value, g: TimelineGeometry, st: EmuStatus) {
        switch dragKind {
        case .scrub:
            // Keep showing the drag position until the emulation thread catches up.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { scrubFrame = nil }
        case .select(let slot), .handle(let slot, _, _, _):
            if let p = preview, p.slot == slot, let b = p.b {
                strip.selectedSlot = slot
                model.timelineSetRange(slot, a: p.a, b: b)
                // Keep the preview until the engine's slot list arrives (no flicker back).
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { if preview == p { preview = nil } }
            } else {
                preview = nil
                if case .select = dragKind, abs(v.translation.width) < 3, !st.practicing {
                    model.seek(to: g.frame(atX: Double(v.location.x))) // plain click on the empty lane: move there
                }
            }
        case .body(let slot):
            strip.selectedSlot = slot
            model.practiceStart(slot)
        case .ignore, .none:
            break
        }
    }
}

/// Picks the A/B slot edited on the timeline; shows its color and the I/O-style actions.
struct TimelineSlotPicker: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject private var strip = FilmstripModel.shared

    var body: some View {
        let sel = strip.selectedSlot
        let slots = model.practiceSlots
        Menu {
            Picker("区間", selection: $strip.selectedSlot) {
                ForEach(slots) { s in
                    Text("\(s.index + 1). " + (s.hasA ? s.displayName : "（未設定）")).tag(s.index)
                }
            }
            .pickerStyle(.inline)
            Divider()
            Button("Aをここに（再生位置）") { model.timelineMarkA() }.disabled(model.status.practicing)
            Button("Bをここに（再生位置）") { model.timelineMarkB() }.disabled(model.status.practicing)
            Button("この区間を練習") { model.practiceStart(sel) }
                .disabled(sel >= slots.count || !slots[sel].hasA)
            Button("この区間を消去", role: .destructive) { model.practiceClear(sel) }
                .disabled(sel >= slots.count || !slots[sel].hasA)
        } label: {
            HStack(spacing: 4) {
                RoundedRectangle(cornerRadius: 2).fill(SlotColors.color(sel)).frame(width: 10, height: 10)
                Text("A/B \(sel + 1)").font(.system(size: 11, weight: .semibold, design: .monospaced))
            }
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .help("タイムラインで編集する A/B 区間（1〜8）。上の帯をドラッグで範囲指定、⌥⌘I / ⌥⌘O で再生位置にA / B")
    }
}
