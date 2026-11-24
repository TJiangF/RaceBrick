import SwiftUI
import RaceBrickCore

struct SessionDetailView: View {
    let session: SessionEnvelope
    @State private var selected: Set<Int> = []

    private var laps: [LapSummary] { session.summary?.laps ?? [] }
    private var bestLap: Int? { session.bestLap ?? session.summary?.bestLap }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(session.trackName.isEmpty ? "Race Log" : session.trackName).font(.title2.bold())
                    Text("\(laps.count) 圈 · \(DeviceDetailView.duration(session.durationMs))")
                        .font(.caption).foregroundStyle(.secondary)
                    if let bestLap, let bestTime = session.bestTime {
                        Text("最快 L\(bestLap) · \(DeviceDetailView.lap(bestTime))")
                            .font(.callout.bold()).foregroundStyle(.purple)
                    }
                }
                SessionMapCanvas(laps: laps, selected: selected, bestLap: bestLap)
                    .frame(height: 280)
                    .background(Color(red: 0.035, green: 0.07, blue: 0.1), in: RoundedRectangle(cornerRadius: 12))
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack {
                        ForEach(laps) { lap in
                            LapChip(lap: lap, best: lap.lap == bestLap,
                                    color: lapColor(lap.lap), on: selected.contains(lap.lap)) {
                                if selected.contains(lap.lap) { selected.remove(lap.lap) }
                                else { selected.insert(lap.lap) }
                            }
                        }
                    }
                }
                ForEach(laps) { lap in
                    HStack {
                        Text("L\(lap.lap)").font(.headline)
                        if lap.lap == bestLap { Text("BEST").font(.caption2.bold()).foregroundStyle(.purple) }
                        Spacer()
                        Text(DeviceDetailView.lap(lap.time)).font(.body.monospaced().bold())
                    }
                    .padding(10)
                    .background(lap.lap == bestLap ? Color.purple.opacity(0.16) : Color.white.opacity(0.04),
                                in: RoundedRectangle(cornerRadius: 9))
                    Text(String(format: "距离 %.0fm · 均速 %.1f · 最高 %.1f km/h · %d 点",
                                lap.distanceM, lap.avgKmh, lap.maxKmh, lap.samples))
                        .font(.caption2).foregroundStyle(.secondary)
                        .padding(.horizontal, 10).padding(.bottom, 4)
                }
            }
            .padding(16)
        }
        .navigationTitle("Race Log")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { selected = Set(laps.map(\.lap)) }
    }

    private func lapColor(_ lap: Int) -> Color {
        let order = selected.sorted()
        guard let idx = order.firstIndex(of: lap) else { return .gray }
        let palette: [Color] = order.count == 2
            ? [.red, .blue]
            : order.count == 3 ? [.red, .yellow, .blue]
            : [.red, .blue, .yellow, .green, .pink, .cyan, .orange, .purple]
        return palette[idx % palette.count]
    }
}

private struct LapChip: View {
    let lap: LapSummary
    let best: Bool
    let color: Color
    let on: Bool
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 5) {
                Image(systemName: on ? "checkmark.square.fill" : "square")
                Text("L\(lap.lap)")
                Text(DeviceDetailView.lap(lap.time)).font(.caption.monospaced())
            }
            .font(.caption.bold())
            .foregroundStyle(on ? .white : .secondary)
            .padding(.horizontal, 9).padding(.vertical, 7)
            .background(on ? color.opacity(0.72) : Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(best ? Color.purple : .clear, lineWidth: 2))
        }
        .buttonStyle(.plain)
    }
}

struct SessionMapCanvas: View {
    let laps: [LapSummary]
    let selected: Set<Int>
    let bestLap: Int?

    private let palette: [Color] = [.red, .blue, .yellow, .green, .pink, .cyan, .orange, .purple]

    var body: some View {
        Canvas { context, size in
            let active = laps.filter { selected.contains($0.lap) }
            let all = active.flatMap(\.points)
            guard let first = all.first, first.count >= 2 else { return }
            var minX = first[0], maxX = first[0], minY = first[1], maxY = first[1]
            for p in all where p.count >= 2 {
                minX = min(minX, p[0]); maxX = max(maxX, p[0])
                minY = min(minY, p[1]); maxY = max(maxY, p[1])
            }
            guard maxX > minX, maxY > minY else { return }
            let w = Double(size.width), h = Double(size.height)
            let pad = 16.0
            let s = min((w - pad * 2) / (maxX - minX),
                        (h - pad * 2) / (maxY - minY))
            let ox = (w - (maxX - minX) * s) / 2
            let oy = (h - (maxY - minY) * s) / 2
            func map(_ p: [Double]) -> CGPoint {
                CGPoint(x: CGFloat(ox + (p[0] - minX) * s),
                        y: CGFloat(h - oy - (p[1] - minY) * s))
            }
            let sorted = selected.sorted()
            for lap in active {
                guard let idx = sorted.firstIndex(of: lap.lap) else { continue }
                let color = selected.count == 2
                    ? [Color.red, .blue][idx % 2]
                    : selected.count == 3 ? [Color.red, .yellow, .blue][idx % 3]
                    : palette[idx % palette.count]
                if selected.count == 1 {
                    for i in 1..<lap.points.count {
                        let a = lap.points[i - 1], b = lap.points[i]
                        guard a.count >= 4, b.count >= 4 else { continue }
                        let dt = max(b[3] - a[3], 1e-3)
                        let dv = b[2] - a[2]
                        let accel = dv / dt
                        let c: Color = accel > 0.7 ? .green : accel < -0.7 ? .red : .gray
                        var path = Path(); path.move(to: map(a)); path.addLine(to: map(b))
                        context.stroke(path, with: .color(c), lineWidth: 3)
                    }
                    if let mx = lap.points.max(by: { $0[2] < $1[2] }) {
                        let p = map(mx)
                        context.fill(Path(ellipseIn: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10)), with: .color(.orange))
                        context.draw(Text(String(format: "MAX %.0f", mx[2])).font(.caption2.bold()).foregroundColor(.white),
                                     at: CGPoint(x: p.x + 30, y: p.y - 12))
                    }
                } else {
                    var path = Path()
                    if let p = lap.points.first { path.move(to: map(p)) }
                    for p in lap.points.dropFirst() { path.addLine(to: map(p)) }
                    context.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: lap.lap == bestLap ? 3.5 : 2.2, lineCap: .round, lineJoin: .round))
                }
            }
        }
    }
}
