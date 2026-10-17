import AppKit
import CryptoKit

/// Production manager render with an isolated vault and public fixture content.
/// This does not claim key-window interaction acceptance or access a live vault.
@MainActor
enum CapsulePasswordVisualFixtures {
    static func export(to output: URL) throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("xsime-vault-visual-\(UUID())")
        let suite = "xsime.vault.visual.\(UUID())"
        let defaults = UserDefaults(suiteName: suite)!
        let previous = getenv("RIMEBUFFER_APPEARANCE_MODE").map { String(cString: $0) }
        defer {
            try? FileManager.default.removeItem(at: root)
            defaults.removePersistentDomain(forName: suite)
            if let previous { setenv("RIMEBUFFER_APPEARANCE_MODE", previous, 1) }
            else { unsetenv("RIMEBUFFER_APPEARANCE_MODE") }
        }
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        let passwords = CapsulePasswordStore(rootURL: root)
        let saved = try passwords.put(CapsulePasswordWriteRequest(title: "公开测试账号", body: "- 用户名：public-user\n- 密码：public-fixture-secret", summaryText: "仅供测试", category: .login))
        let repository = CapsuleWindowRepository(contentStore: CapsuleContentStore(rootURL: root), passwordStore: passwords)
        func descendants(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(descendants) }
        for theme in ["night", "day", "quiet", "rasta"] {
            setenv("RIMEBUFFER_APPEARANCE_MODE", theme, 1)
            let pane = CapsulePaneViewController(repository: repository, cloudSyncController: nil,
                                                 revealPasscodeStore: CapsuleRevealPasscodeStore(defaults: defaults))
            let size = CapsuleWindowGeometry.defaultContentSize
            let window = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                                  styleMask: [.titled, .fullSizeContentView], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false
            window.titleVisibility = .hidden
            window.titlebarAppearsTransparent = true
            window.appearance = RimeUI.appKitAppearance
            CapsuleWindowGeometry.install(contentController: pane, in: window)
            window.setContentSize(size)
            pane.reveal(kind: .password, id: saved.id)
            let deadline = Date().addingTimeInterval(1.5)
            while Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.02)) }
            let labels = descendants(pane.view).compactMap { ($0 as? NSTextField)?.stringValue }
            guard labels.contains("公开测试账号"), descendants(pane.view).contains(where: { $0 is CapsuleInlinePasscodeView }) else {
                throw CocoaError(.coderInvalidValue)
            }
            guard !descendants(pane.view).contains(where: { ($0 as? NSTextView)?.string.contains("public-fixture-secret") == true }) else {
                throw CocoaError(.coderInvalidValue)
            }
            pane.view.layoutSubtreeIfNeeded(); pane.view.displayIfNeeded()
            guard let bitmap = pane.view.bitmapImageRepForCachingDisplay(in: pane.view.bounds) else { throw CocoaError(.coderInvalidValue) }
            pane.view.cacheDisplay(in: pane.view.bounds, to: bitmap)
            guard let png = bitmap.representation(using: .png, properties: [:]) else { throw CocoaError(.coderInvalidValue) }
            try png.write(to: output.appendingPathComponent("\(theme)-locked.png"), options: .atomic)
            window.close()
        }
        var sources: [String: String] = [:]
        let repo = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        for name in ["Sources/RimeBuffer/CapsulePasswordVisualFixtures.swift", "Sources/RimeBuffer/CapsuleWindowController.swift", "Sources/RimeBuffer/CapsuleRevealPasscode.swift"] {
            sources[name] = SHA256.hash(data: try Data(contentsOf: repo.appendingPathComponent(name))).map { String(format: "%02x", $0) }.joined()
        }
        try JSONSerialization.data(withJSONObject: ["purpose": "public-parity-fixture", "state": "locked manager", "sources": sources], options: [.sortedKeys, .prettyPrinted])
            .write(to: output.appendingPathComponent("sources.json"), options: .atomic)
    }
}
