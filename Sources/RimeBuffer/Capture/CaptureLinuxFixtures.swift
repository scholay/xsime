import AppKit
import CryptoKit

/// Isolated exports of production Capture cards and duration rules for Linux.
@MainActor
enum CaptureLinuxFixtures {
    static func export(to output: URL) throws {
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        let file = output.appendingPathComponent("fixture.png")
        try CaptureImageIO.write(CaptureSmoke.fixture(width: 480, height: 240), to: file)
        let previous = getenv("RIMEBUFFER_APPEARANCE_MODE").map { String(cString: $0) }
        defer {
            if let previous { setenv("RIMEBUFFER_APPEARANCE_MODE", previous, 1) }
            else { unsetenv("RIMEBUFFER_APPEARANCE_MODE") }
        }
        func descendants(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(descendants) }
        var records: [[String: Any]] = []
        for (index, kind) in [CaptureKind.image, .video].enumerated() {
            let record = CaptureRecord(id: UUID(), title: index == 0 ? "截图示例" : "录屏示例 👨‍👩‍👧‍👦", kind: kind,
                createdAt: Date(timeIntervalSince1970: 1000), source: index == 0 ? "桌面捕获" : "文件导入",
                original: "fixture.png", output: "fixture.png", project: nil, text: "", width: 480, height: 240,
                duration: 125.9, collectionID: index == 0 ? nil : UUID(), incomplete: index != 0, thumbnail: "fixture.png")
            for theme in ["night", "day", "quiet", "rasta"] {
                setenv("RIMEBUFFER_APPEARANCE_MODE", theme, 1)
                let card = CaptureHistoryCard(record: record, file: file, previewFile: file)
                card.frame = NSRect(x: 0, y: 0, width: 206, height: 126)
                card.selected = true
                let window = NSWindow(contentRect: card.frame, styleMask: [.borderless], backing: .buffered, defer: false)
                window.appearance = RimeUI.appKitAppearance
                window.contentView = card
                card.showPreview(true)
                let deadline = Date().addingTimeInterval(4)
                while Date() < deadline {
                    if descendants(card).compactMap({ $0 as? CaptureDragImageView }).first?.image != nil { break }
                    RunLoop.current.run(until: Date().addingTimeInterval(0.02))
                }
                guard descendants(card).compactMap({ $0 as? CaptureDragImageView }).first?.image != nil else {
                    throw CaptureError.message("capture fixture image did not load")
                }
                card.layoutSubtreeIfNeeded();card.displayIfNeeded()
                if theme == "night" {
                    let labels = descendants(card).compactMap { ($0 as? NSTextField)?.stringValue }
                    records.append(["title": record.title, "kind": kind.rawValue, "source": record.source,
                        "duration": record.duration, "favorite": record.collectionID != nil,
                        "incomplete": record.incomplete, "labels": labels])
                }
                guard let bitmap = card.bitmapImageRepForCachingDisplay(in: card.bounds) else { throw CaptureError.message("bitmap unavailable") }
                card.cacheDisplay(in: card.bounds, to: bitmap)
                guard let png = bitmap.representation(using: .png, properties: [:]) else { throw CaptureError.message("PNG unavailable") }
                try png.write(to: output.appendingPathComponent("\(theme)-\(kind.rawValue).png"), options: .atomic)
                window.close()
            }
        }
        let durations: [Double] = [0, 59.9, 60, 125.9, -1, Double(Int32.max), .infinity, .nan]
        let cases = durations.map { ["input": String($0), "label": CaptureDuration.label($0)] }
        let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        var sources: [String: String] = [:]
        for source in ["Sources/RimeBuffer/Capture/CaptureStore.swift", "Sources/RimeBuffer/Capture/CaptureHistoryView.swift",
                       "Sources/RimeBuffer/Capture/CaptureLinuxFixtures.swift"] {
            sources[source] = SHA256.hash(data: try Data(contentsOf: root.appendingPathComponent(source)))
                .map { String(format: "%02x", $0) }.joined()
        }
        let json = try JSONSerialization.data(withJSONObject: ["version": 1, "sources": sources, "cards": records, "durations": cases],
            options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
        try json.write(to: output.appendingPathComponent("native-capture.json"), options: .atomic)
    }
}
