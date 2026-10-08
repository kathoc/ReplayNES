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
    @ObservedObject private var clock = AppModel.shared.clock   // playhead / take length (AppModel.status)
    @ObservedObject private var markers = AppModel.shared.markers   // the controller's A/B flags
    @Environment(\.displayScale) private var displayScale

    static let laneHeight: CGFloat = 16
    static let stripHeight: CGFloat = 40
    static var tileWidth: CGFloat { stripHeight * CGFloat(RN_VIDEO_WIDTH) / CGFloat(RN_VIDEO_HEIGHT) }
    /// Margin on both sides of the strip: the playhead's head, the A/B edges and flags stay whole
    /// at frame 0 and at the take end (where the playhead sits while recording).
    static let inset: CGFloat = 6

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
            let w = max(0, Double(geo.size.width - 2 * Self.inset))
            let st = clock.status
            // Fixed scale (ThumbnailGrid): the step FilmstripModel.layout committed (it also starts
            // the crossfade when the scale changes); the recorded part spans 0 ... extent.
            let tw = Double(Self.tileWidth)
            let step = strip.tileStep != 0 ? strip.tileStep : ThumbnailGrid.tileStep(width: w, tileWidth: tw, length: st.takeLength)
            let g = TimelineGeometry(width: ThumbnailGrid.extent(length: st.takeLength, step: step, tileWidth: tw), length: st.takeLength)
            let ranges = TimelineEditing.visibleRanges(slots: model.practiceSlots, takes: model.takes,
                                                       activeTake: st.activeTake, takeLength: st.takeLength)
            let version = strip.version
            let fade = strip.fade
            let flags = markers.active && preview == nil && dragKind == nil
                ? (frames: markers.frames, focus: markers.focus, editing: markers.editing) : nil
            TimelineView(.animation(minimumInterval: nil, paused: fade == nil)) { tl in
                Canvas { ctx, size in
                    _ = version
                    var c = ctx
                    c.translateBy(x: Self.inset, y: 0)
                    draw(&c, size: CGSize(width: w, height: size.height), g: g, step: step, st: st, ranges: ranges, flags: flags,
                         fade: fade, now: tl.date)
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
        .help("Timeline: drag to move (muted, paused). Drag the top band (or Shift-drag) to set A/B of the selected section, drag a section edge to adjust it, click a section to practice it")
    }

    // MARK: drawing

    private func draw(_ ctx: inout GraphicsContext, size: CGSize, g: TimelineGeometry, step: Double, st: EmuStatus,
                      ranges: [TimelineRange], flags: (frames: [UInt64], focus: Int, editing: Bool)?,
                      fade: (from: Double, start: Date)?, now: Date) {
        let lane = Self.laneHeight
        let stripRect = CGRect(x: 0, y: lane, width: size.width, height: Self.stripHeight)
        let stripPath = Path(roundedRect: stripRect, cornerRadius: 4)
        ctx.fill(stripPath, with: .color(Color.secondary.opacity(0.10)))  // nothing recorded there yet
        if g.width > 0 {  // the recorded part
            var rec = ctx
            rec.clip(to: stripPath)
            rec.fill(Path(CGRect(x: 0, y: lane, width: min(g.width, size.width), height: Self.stripHeight)),
                     with: .color(Color.secondary.opacity(0.12)))
        }

        // Thumbnails at the fixed scale (ThumbnailGrid.tiles), clipped at the recorded extent; a
        // scale change (x2) crossfades from the previous layout.
        var thumbs = ctx
        thumbs.clip(to: stripPath)
        drawTiles(&thumbs, length: g.length, step: step)
        if let f = fade, f.from != step, f.from == step * 2 || f.from * 2 == step {
            let p = now.timeIntervalSince(f.start) / FilmstripModel.fadeDuration
            if p < 1 {
                var old = thumbs
                old.opacity = 1 - max(0, p)
                drawTiles(&old, length: g.length, step: f.from)
            }
        }

        let selected = strip.selectedSlot
        var shown = ranges
        if let p = preview {
            shown.removeAll { $0.slot == p.slot }
            shown.append(p)
        }
        // The controller's markers stand for the selected slot (they may be mid-edit): its range from them.
        if let fl = flags {
            shown.removeAll { $0.slot == selected }
            if fl.frames.count == 2 { shown.append(TimelineRange(slot: selected, a: fl.frames[0], b: fl.frames[1])) }
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
                // A only: flag at A (to the left of A near the take end, so it stays visible).
                ctx.fill(Path(CGRect(x: xa - 1, y: 0, width: 2, height: Double(lane + Self.stripHeight))),
                         with: .color(c.opacity(hl ? 1 : 0.7)))
                let fx = xa + 22 > Double(size.width + Self.inset) ? xa - 22 : xa
                let flag = CGRect(x: fx, y: 2, width: 22, height: Double(lane) - 4)
                ctx.fill(Path(roundedRect: flag, cornerRadius: 3), with: .color(c.opacity(hl ? 0.95 : 0.6)))
                ctx.draw(Text("\(r.slot + 1)A").font(.system(size: 9, weight: .bold)).foregroundColor(.black),
                         at: CGPoint(x: fx + 3, y: Double(lane) / 2), anchor: .leading)
            }
        }

        // Marker flags: a pole through the filmstrip and a small flag in the lane (A to the left of
        // its pole, B to the right; letters once both exist), the focused one ringed, the edited one lit.
        if let fl = flags {
            let n = fl.frames.count
            for (i, f) in fl.frames.enumerated() {
                let x = g.x(forFrame: f)
                let focused = i == fl.focus, lit = focused && fl.editing
                let c = lit ? QMStyle.brand : SlotColors.color(selected)
                let pw: Double = lit ? 3 : 2
                ctx.fill(Path(CGRect(x: x - pw / 2, y: 0, width: pw, height: Double(lane + Self.stripHeight))), with: .color(c))
                let fw = Double(lane) * 1.15, fh = Double(lane) - 4
                var fx = n == 2 && i == 0 ? x - fw : x
                if fx < -Double(Self.inset) { fx = x }
                if fx + fw > Double(size.width) + Double(Self.inset) { fx = x - fw }
                let flag = CGRect(x: fx, y: 2, width: fw, height: fh)
                ctx.fill(Path(roundedRect: flag, cornerRadius: 3), with: .color(c))
                if n == 2 {
                    ctx.draw(Text(verbatim: i == 0 ? "A" : "B").font(.system(size: 9, weight: .heavy)).foregroundColor(.black),
                             at: CGPoint(x: flag.midX, y: flag.midY), anchor: .center)
                }
                if focused {
                    ctx.stroke(Path(roundedRect: flag.insetBy(dx: -2, dy: -2), cornerRadius: 4),
                               with: .color(fl.editing ? .white : QMStyle.brand), lineWidth: fl.editing ? 2 : 1.5)
                }
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
                ctx.fill(Path(CGRect(x: 0, y: Double(lane + Self.stripHeight) - 4, width: Double(size.width) * p, height: 4)),
                         with: .color(.orange.opacity(0.9)))
            }
        } else if g.length > 0 {
            head = (g.x(forFrame: scrubFrame ?? st.frame), st.recording ? .red : .white)
        }
        if let h = head {
            // Whole backing pixels: while recording the playhead advances in clean 1-dot steps.
            let x = ThumbnailGrid.snapToPixel(max(0, min(g.width, h.x)), scale: Double(displayScale))
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

    /// Draws the filmstrip tiles at the scale `step` (ThumbnailGrid.tiles): each tile shows its
    /// own picture at its natural size and position, clipped at the recorded extent (the newest
    /// one is revealed as the take grows), never stretched, repeated or misdated: while it is being
    /// made, the latest earlier picture within one step is shown dimmed
    /// (ThumbnailGrid.fallbackWindow), otherwise the tile is an empty placeholder.
    private func drawTiles(_ ctx: inout GraphicsContext, length: UInt64, step: Double) {
        let tw = Double(Self.tileWidth)
        let y = Double(Self.laneHeight), h = Double(Self.stripHeight)
        let base = ctx.opacity
        for t in ThumbnailGrid.tiles(length: length, step: step, tileWidth: tw) {
            let own = strip.cache.image(at: t.picture)
            if let img = own ?? strip.cache.image(before: t.picture, within: ThumbnailGrid.fallbackWindow(step: step)) {
                var c = ctx
                c.clip(to: Path(CGRect(x: t.x, y: y, width: t.visible, height: h)))
                c.opacity = own != nil ? base : base * ThumbnailGrid.fallbackOpacity
                c.draw(Image(decorative: img, scale: 1).interpolation(.none), in: CGRect(x: t.x, y: y, width: tw, height: h))
            }
            if t.index > 0 {  // subtle separator at the anchor
                ctx.fill(Path(CGRect(x: t.x - 0.5, y: y, width: 1, height: h)), with: .color(.black.opacity(0.35)))
            }
        }
    }

    // MARK: gestures

    private func dragGesture(g: TimelineGeometry, st: EmuStatus, ranges: [TimelineRange]) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { v in
                if dragKind == nil {
                    dragKind = beginDrag(at: CGPoint(x: v.startLocation.x - Self.inset, y: v.startLocation.y), g: g, st: st, ranges: ranges)
                }
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
            let f = g.frame(atX: Double(v.location.x - Self.inset))
            if f != scrubFrame { scrubFrame = f; model.scrub(to: f) }
        case .select(let slot):
            if let r = TimelineEditing.range(fromX: Double(v.startLocation.x - Self.inset), toX: Double(v.location.x - Self.inset), in: g) {
                preview = TimelineRange(slot: slot, a: r.a, b: r.b)
            } else {
                preview = nil
            }
        case .handle(let slot, let h, let a, let b):
            let r = TimelineEditing.drag(h, of: (a, b), toX: Double(v.location.x - Self.inset), in: g)
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
                    model.seek(to: g.frame(atX: Double(v.location.x - Self.inset))) // plain click on the empty lane: move there
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
            Picker("Section", selection: $strip.selectedSlot) {
                ForEach(slots) { s in
                    Text(verbatim: "\(s.index + 1). " + (s.hasA ? s.displayName : String(localized: "(not set)"))).tag(s.index)
                }
            }
            .pickerStyle(.inline)
            Divider()
            Button("Set A Here (Playhead)") { model.timelineMarkA() }.disabled(model.status.practicing)
            Button("Set B Here (Playhead)") { model.timelineMarkB() }.disabled(model.status.practicing)
            Button("Practice This Section") { model.practiceStart(sel) }
                .disabled(sel >= slots.count || !slots[sel].hasA)
            Button("Clear This Section", role: .destructive) { model.practiceClear(sel) }
                .disabled(sel >= slots.count || !slots[sel].hasA)
        } label: {
            HStack(spacing: 4) {
                RoundedRectangle(cornerRadius: 2).fill(SlotColors.color(sel)).frame(width: 10, height: 10)
                Text("A/B \(sel + 1)").font(.system(size: 11, weight: .semibold, design: .monospaced))
            }
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .help("A/B section (1–8) edited on the timeline. Drag the top band to set its range; ⌥⌘I / ⌥⌘O put A / B at the playhead")
    }
}
