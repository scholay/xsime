import Cocoa

// Layout fixtures must not be resized to a CI runner's small visible screen.
// Production windows continue to use AppKit's normal screen constraints.
private final class MailboxSmokeWindow: NSWindow {
    override func constrainFrameRect(_ frameRect: NSRect, to screen: NSScreen?) -> NSRect { frameRect }
}

/// Isolated native UI + PTY test. Fixtures never enter the user's Mailbox and
/// no provider is called. Optional PNGs are real AppKit renders of the module.
func runMailboxWorkspaceSmoke(output: URL? = nil) -> Bool {
    defer { OfficialPluginSmokePreferences.clear() }
    struct Failure: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }
    func require(_ condition: @autoclosure () throws -> Bool, _ message: String) throws {
        if try !condition() { throw Failure(message: message) }
    }
    func pump(_ seconds: TimeInterval = 0.12) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }
    func descendants(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(descendants) }
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("mailbox-workspace-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: root) }
    do {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let store = try MailboxStore(storageRoot: root)
        let chat = try store.beginAIConversation(source: .codexCLI(), title: "Mailbox 插件工作区", prompt: "我们把 Mailbox 拆成三种插件，工作区该怎样组织？")
        try store.completeGeneration(chat, response: "可以保留同一套导航，让每个插件有自己的工作方式。\n\n**终端**\n直接运行 Codex、Claude Code 或 Shell；右侧预览文件、收取产物。\n\n**对话**\n用干净的阅读区持续交流，模型和上下文跟随会话。\n\n**收件**\n接收通知、图片和任务成果，需要时再回到原来的工作。", format: .markdown)
        let inbound = try store.createInboundThread(source: .http(source: "连接的来源"), title: "收到一份新的反馈", body: "这是一条用于验证单向收件的本地测试消息。")
        let image = NSImage(size: NSSize(width: 800, height: 450), flipped: false) { rect in
            NSColor(srgbRed: 0.89, green: 0.91, blue: 0.86, alpha: 1).setFill(); rect.fill()
            NSColor(srgbRed: 0.25, green: 0.40, blue: 0.33, alpha: 1).setFill()
            NSBezierPath(roundedRect: NSRect(x: 55, y: 55, width: 690, height: 340), xRadius: 30, yRadius: 30).fill()
            ("MAILBOX" as NSString).draw(at: NSPoint(x: 100, y: 235), withAttributes: [.font: NSFont.systemFont(ofSize: 62, weight: .bold), .foregroundColor: NSColor.white])
            ("A place for work to arrive." as NSString).draw(at: NSPoint(x: 104, y: 173), withAttributes: [.font: NSFont.systemFont(ofSize: 25), .foregroundColor: NSColor.white.withAlphaComponent(0.75)])
            return true
        }
        guard let tiff = image.tiffRepresentation, let bitmap = NSBitmapImageRep(data: tiff),
              let png = bitmap.representation(using: .png, properties: [:]) else { throw Failure(message: "fixture image") }

        // Missing optional keys from old schema-v1 threads remain readable.
        let original = store.thread(id: chat.threadID)!
        let encoded = try JSONEncoder().encode(original)
        var legacy = try JSONSerialization.jsonObject(with: encoded) as! [String: Any]
        for key in ["workspace", "terminalSessionID", "archivedAt"] { legacy.removeValue(forKey: key) }
        let decoded = try JSONDecoder().decode(MailboxThread.self, from: JSONSerialization.data(withJSONObject: legacy))
        try require(decoded.contentWorkspace == .chat, "legacy AI routing")
        try require(inbound.contentWorkspace == .inbox, "inbound routing")
        try require(MailboxWorkspaceRules.threads(store.snapshot.threads, module: .inbox, query: "反馈", filter: .attention).map(\.id) == [inbound.id], "inbox search and review filter")

        let app = NSApplication.shared
        app.setActivationPolicy(.accessory); app.finishLaunching()
        var enabled = Set(MailboxModuleID.allCases)
        let host = MailboxWorkspaceViewController(store: store, remembersModule: false, isEnabled: { enabled.contains($0) })
        let window = MailboxSmokeWindow(contentRect: NSRect(origin: .zero, size: MailboxUI.defaultSize),
            styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.appearance = NSAppearance(named: .aqua)
        window.contentViewController = host
        window.contentMinSize = MailboxUI.minimumSize
        window.setContentSize(MailboxUI.defaultSize)
        defer { window.orderOut(nil) }
        window.orderFront(nil); host.view.layoutSubtreeIfNeeded()
        host.selectModule(.terminal)
        let session = host.terminal.launch(kind: .shell, workspace: root, executable: URL(fileURLWithPath: "/bin/zsh"), arguments: ["-f", "-c", "printf 'Mailbox · terminal workspace\n\nPTY ready.\nType a line to finish this local test.\n'; read result; printf 'completed\\n'"])
        defer { session.stop() }
        pump()
        try require(session.running, "real PTY started")
        func terminalText() -> String { String(decoding: session.terminal.getTerminal().getBufferAsData(), as: UTF8.self) }
        let outputDeadline = Date().addingTimeInterval(3)
        while !terminalText().contains("PTY ready.") && Date() < outputDeadline { pump(0.05) }
        try require(terminalText().contains("PTY ready."), "real PTY output received")
        let receipt = try store.importTerminalResult(sessionID: session.id, sourceName: "Shell · 本地测试", title: "Mailbox 概念封面.png", body: "终端收取的图片成果\n800 × 450 · PNG", pngData: png)
        try require(receipt.terminalSessionID == session.id && receipt.contentWorkspace == .inbox, "receipt association")
        try require(store.imageData(for: receipt.messages[0]) == png, "owned PNG persisted")
        try store.setArchived(true, threadID: inbound.id)
        try require(store.snapshot.unreadCount == 2, "archive removed from unread count")
        let reloaded = try MailboxStore(storageRoot: root)
        try require(reloaded.thread(id: inbound.id)?.archivedAt != nil, "archive survives reload")
        try require(reloaded.thread(id: receipt.id)?.terminalSessionID == session.id, "association survives reload")
        let beforeCount = store.snapshot.threads.count
        do {
            _ = try store.importTerminalResult(sessionID: session.id, sourceName: "", title: "invalid", body: "body", pngData: png)
            throw Failure(message: "invalid import accepted")
        } catch MailboxStoreError.corruptDocument {} catch MailboxStoreError.invalidSource {}
        let images = try FileManager.default.contentsOfDirectory(at: store.storageDirectoryURL.appendingPathComponent("images"), includingPropertiesForKeys: nil)
        try require(images.count == 1 && store.snapshot.threads.count == beforeCount, "failed import atomic cleanup")

        if let output { try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true) }
        func render(_ name: String, size: NSSize = MailboxUI.defaultSize) throws {
            window.setContentSize(size)
            pump(); host.view.layoutSubtreeIfNeeded(); host.view.displayIfNeeded()
            try require(host.view.bounds.size == size, "host dimensions \(host.view.bounds)")
            let visible = descendants(host.view).filter { !$0.isHiddenOrHasHiddenAncestor }
            let composers = visible.compactMap { $0 as? MailboxComposerTextView }
            for composer in composers { try require(composer.bounds.width > 400 && composer.bounds.height > 20, "composer layout at \(size)") }
            for button in visible.compactMap({ $0 as? MailboxActionButton }) {
                try require(button.bounds.height == 28 && button.bounds.width >= 28, "button hit area: \(button.toolTip ?? button.title) \(button.frame)")
                if let parent = button.superview {
                    try require(button.frame.minX >= -1 && button.frame.maxX <= parent.bounds.width + 1,
                                "button outside toolbar: \(button.title)")
                }
            }
            guard let output else { return }
            guard let bitmap = host.view.bitmapImageRepForCachingDisplay(in: host.view.bounds) else { throw Failure(message: "render allocation") }
            host.view.cacheDisplay(in: host.view.bounds, to: bitmap)
            guard let data = bitmap.representation(using: .png, properties: [:]) else { throw Failure(message: "render PNG") }
            try data.write(to: output.appendingPathComponent("\(name).png"))
        }
        host.terminal.refresh(); try render("terminal")
        try render("terminal-compact", size: MailboxUI.minimumSize)
        let terminalSplit = descendants(host.view).compactMap { $0 as? MailboxAdaptiveSplitView }.first!
        try require(!terminalSplit.sidebarVisible && !terminalSplit.inspectorVisible, "terminal panels collapse")
        let terminalWidth = session.terminal.bounds.width
        try require(terminalText().contains("PTY ready."), "PTY content survives compact resize")
        if ProcessInfo.processInfo.environment["RIMES_MAILBOX_LAYOUT_REVIEW"] == "1" {
            window.title = "Mailbox Layout Preview"
            window.center(); window.makeKeyAndOrderFront(nil); app.activate(ignoringOtherApps: true)
            pump(45)
        }
        descendants(host.view).compactMap { $0 as? MailboxActionButton }.first { $0.toolTip == "显示或隐藏扩展区" }!.performClick(nil)
        try render("terminal-compact-extension", size: MailboxUI.minimumSize)
        try require(terminalSplit.inspectorOverlay && session.terminal.bounds.width == terminalWidth, "extension drawer does not squeeze PTY")
        descendants(host.view).compactMap { $0 as? MailboxActionButton }.first { $0.toolTip == "收起扩展区" }!.performClick(nil)
        pump(); try require(!terminalSplit.inspectorVisible && session.running, "extension closes without stopping PTY")
        host.show(selecting: chat.threadID); try require(session.running, "switching plugins preserves PTY")
        try render("chat")
        let composer = descendants(host.view).compactMap { $0 as? MailboxComposerTextView }.first!
        composer.string = "尚未发送的草稿"; composer.didChangeText()
        try render("chat-compact", size: MailboxUI.minimumSize)
        let chatSplit = descendants(host.view).compactMap { $0 as? MailboxAdaptiveSplitView }.first!
        try require(!chatSplit.sidebarVisible, "chat list collapses at minimum width")
        descendants(host.view).compactMap { $0 as? MailboxActionButton }.first { $0.toolTip == "显示或隐藏列表" }!.performClick(nil)
        try render("chat-compact-list", size: MailboxUI.minimumSize)
        try require(chatSplit.sidebarOverlay, "chat list remains accessible as drawer")
        host.show(selecting: chat.threadID); pump()
        try require(!chatSplit.sidebarVisible && composer.string == "尚未发送的草稿", "selection closes drawer and preserves draft")
        try store.renameThread(chat.threadID, title: "这是一个很长的会话标题，用来验证窄窗口不会被标题和工具栏撑开，也不会覆盖右侧操作按钮")
        try render("chat-medium-long-title", size: NSSize(width: 840, height: 600))
        try require(chatSplit.sidebarVisible && !chatSplit.sidebarOverlay, "list restores above breakpoint")
        try store.renameThread(chat.threadID, title: "Mailbox 插件工作区")
        host.show(selecting: receipt.id); try render("inbox")
        try render("inbox-compact", size: MailboxUI.minimumSize)
        host.show(selecting: chat.threadID)
        let restored = descendants(host.view).compactMap { $0 as? MailboxComposerTextView }.first!
        try require(restored.string == "尚未发送的草稿", "draft survives module switch")
        let reply = try store.beginAIReply(threadID: chat.threadID, body: "验证流式显示")
        try store.updateGenerationPreview(reply, response: "第一段预览")
        pump()
        let firstPreview = descendants(host.view).compactMap { $0 as? MailboxMessageTextView }.first { $0.string == "第一段预览" }
        try require(firstPreview != nil, "streaming preview visible")
        try store.updateGenerationPreview(reply, response: "第一段预览，继续更新")
        pump()
        let nextPreview = descendants(host.view).compactMap { $0 as? MailboxMessageTextView }.first { $0.string == "第一段预览，继续更新" }
        try require(firstPreview === nextPreview, "streaming updates the same native row")
        try require(store.thread(id: chat.threadID)?.messages.count == 3, "preview stays outside durable messages")
        try store.completeGeneration(reply, response: "流式请求完成。")
        window.appearance = NSAppearance(named: .darkAqua)
        host.applyAppearance(); try render("chat-dark")
        try render("chat-compact-dark", size: MailboxUI.minimumSize)
        descendants(host.view).compactMap { $0 as? MailboxActionButton }.first { $0.toolTip == "新对话" }!.performClick(nil)
        try render("chat-new-compact-dark", size: MailboxUI.minimumSize)
        window.orderOut(nil); try require(session.running, "closing window preserves PTY")
        enabled.remove(.terminal); host.selectModule(.terminal)
        try require(host.selectedModule != .terminal && session.running, "disabled module hidden without implicit kill")
        session.terminal.send(txt: "done\n")
        let deadline = Date().addingTimeInterval(3)
        while session.running && Date() < deadline { pump(0.05) }
        try require(!session.running && session.exitCode == 0, "PTY input and successful exit")
        MailboxInteractionBridge.shared.clearComposerDraft(threadID: chat.threadID)
        print("mailbox-workspace-smoke: PASS (routing, migration, archive, atomic PNG receipt, native layouts, draft, real PTY lifetime)")
        return true
    } catch {
        print("mailbox-workspace-smoke: FAIL \(error.localizedDescription)")
        return false
    }
}
