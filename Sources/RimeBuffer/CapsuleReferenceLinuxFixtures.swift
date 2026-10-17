import AppKit
import Foundation
import CryptoKit

/// Exports the production formatter's results, never a reimplementation.
enum CapsuleReferenceLinuxFixtures {
    static func export(to path: String) throws {
        var cases: [[String: Any]] = []
        for kind in CapsuleReference.Kind.allCases {
            var reference = CapsuleReference()
            reference.kind = kind
            reference.authors = "  张三 ; Alice Example; ; 李四  "
            reference.year = " 2024 "
            reference.container = "Journal of Local Examples"
            reference.publisher = "示例出版社"
            reference.volume = "12"
            reference.issue = "3"
            reference.pages = "12–25"
            reference.doi = "https://doi.org/10.1000/example"
            reference.url = "https://example.org/ignored-when-doi-present"
            reference.isbn = "9780000000000"
            for style in ScholayCitationStyle.allCases {
                cases.append(sample(title: " 示例文献 👨‍👩‍👧‍👦 ", reference: reference, style: style))
            }
        }
        var empty = CapsuleReference()
        cases.append(sample(title: "旧 PDF", reference: empty, style: .gbT7714))
        empty.authors = " ; ; "
        empty.year = "2020"
        empty.publisher = "Publisher"
        cases.append(sample(title: "No authors", reference: empty, style: .apa7))
        empty.authors = " Alice \u{2003}; Bob "
        empty.url = "https://example.org/resource"
        cases.append(sample(title: " URL only ", reference: empty, style: .mla9))
        cases.append(sample(title: " \n\t", reference: empty, style: .ieee))
        let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        var sources: [String: String] = [:]
        for source in ["Sources/RimeBuffer/CapsuleReference.swift",
                       "Sources/RimeBuffer/ScholayPlugin.swift",
                       "Sources/RimeBuffer/CapsuleReferenceLinuxFixtures.swift"] {
            sources[source] = SHA256.hash(data: try Data(contentsOf: root.appendingPathComponent(source)))
                .map { String(format: "%02x", $0) }.joined()
        }
        let data = try JSONSerialization.data(withJSONObject: ["version": 1, "sources": sources, "cases": cases], options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
        try data.write(to: URL(fileURLWithPath: path), options: .atomic)
    }
    @MainActor
    static func renderManager(to path: String) -> Bool {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("xsime-reference-preview-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let store = CapsuleContentStore(rootURL: root)
        var reference = CapsuleReference()
        reference.kind = .journalArticle
        reference.authors = "Ada Lovelace; Alan Turing"
        reference.year = "2024"
        reference.container = "Journal of Test Fixtures"
        reference.volume = "12"
        reference.pages = "10–20"
        reference.doi = "10.1000/example"
        // The portable bookmark deliberately points at a missing file. Both
        // platforms must still show and edit the existing metadata unchanged.
        do {
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            let pdf = root.appendingPathComponent("reference.pdf")
            try Data("%PDF-1.4\n".utf8).write(to: pdf)
            let saved = try store.put(CapsuleContentWriteRequest(type: .pdf, title: "参考文献示例",
                content: pdf.path, reference: reference))
            let source = try String(contentsOf: saved.fileURL, encoding: .utf8)
            try source.replacingOccurrences(of: pdf.path, with: "/home/example/Documents/reference.pdf")
                .write(to: saved.fileURL, atomically: true, encoding: .utf8)
            try FileManager.default.removeItem(at: pdf)
        } catch { return false }
        let pane = CapsulePaneViewController(repository: CapsuleWindowRepository(contentStore: store,
            passwordStore: CapsulePasswordStore(rootURL: root)), cloudSyncController: nil)
        let size = CapsuleWindowGeometry.defaultContentSize
        let window = NSWindow(contentRect: NSRect(origin: .zero, size: size),
            styleMask: [.titled, .fullSizeContentView], backing: .buffered, defer: false)
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        CapsuleWindowGeometry.install(contentController: pane, in: window)
        window.setContentSize(size)
        pane.selectKind(.pdf)
        pane.reloadFromStore()
        let deadline = Date().addingTimeInterval(1.5)
        while Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) }
        pane.view.layoutSubtreeIfNeeded()
        pane.view.displayIfNeeded()
        guard let bitmap = pane.view.bitmapImageRepForCachingDisplay(in: pane.view.bounds) else { return false }
        pane.view.cacheDisplay(in: pane.view.bounds, to: bitmap)
        guard let data = bitmap.representation(using: .png, properties: [:]) else { return false }
        return (try? data.write(to: URL(fileURLWithPath: path), options: .atomic)) != nil
    }
    private static func sample(title: String, reference: CapsuleReference, style: ScholayCitationStyle) -> [String: Any] {
        ["title": title, "kind": reference.kind.rawValue, "headers": Dictionary(uniqueKeysWithValues: reference.headerValues),
         "style": style.rawValue, "missing": reference.missingFields,
         "citation": CapsuleReferenceFormatter.bibliography(title: title, reference: reference, style: style) as Any? ?? NSNull()]
    }
}
