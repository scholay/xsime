import AppKit
import CryptoKit

/// Exports only isolated public records and the production workspace, without a provider or PTY.
@MainActor
enum MailboxLinuxFixtures {
    static func export(to output: URL) throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("xsime-mailbox-fixture-\(UUID())")
        let prior = getenv("RIMEBUFFER_USER_DIR").map { String(cString: $0) }
        let appearanceMode = getenv("RIMEBUFFER_APPEARANCE_MODE").map { String(cString: $0) }
        setenv("RIMEBUFFER_USER_DIR", root.path, 1)
        defer {
            try? FileManager.default.removeItem(at: root)
            if let prior { setenv("RIMEBUFFER_USER_DIR", prior, 1) } else { unsetenv("RIMEBUFFER_USER_DIR") }
            if let appearanceMode { setenv("RIMEBUFFER_APPEARANCE_MODE", appearanceMode, 1) } else { unsetenv("RIMEBUFFER_APPEARANCE_MODE") }
        }
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        defer { OfficialPluginSmokePreferences.clear() }
        for module in MailboxModuleID.allCases {
            _ = try PresetBufferPluginInstallationStore.shared.package(id: module.pluginKey.rawID, requireEnabled: false)
        }
        let store = try MailboxStore(storageRoot: root, dateProvider: { Date(timeIntervalSince1970: 1_700_000_000) })
        let chat = try store.beginAIConversation(source: .openAICompatible(identifier: "public-fixture", model: "public-model"), title: "公开测试对话", prompt: "你好，这是公开样例。")
        try store.completeGeneration(chat, response: "## 本地工作区\n\n会话、消息和草稿各自保存。", format: .markdown)
        let incoming = try store.createInboundThread(source: .http(source: "公开测试来源"), title: "收到一份新的反馈", body: "这是公开的本地测试消息。\n\n收件不会自动回复来源。")
        _ = try store.addLocalNote(threadID: incoming.id, body: "先在本机记录备注。")
        let archived = try store.createInboundThread(source: .mcp(client: "公开测试客户端"), title: "已归档的样例", body: "已保留的公开历史。")
        try store.setArchived(true, threadID: archived.id)
        try store.markRead(threadID: archived.id)
        let accepted = try store.createInboundThread(source: .http(source: "公开审核来源"), title: "已接受的收件", body: "已明确接受的公开消息。")
        _ = try store.resolveReview(threadID: accepted.id, decision: .accept)
        let rejected = try store.createInboundThread(source: .http(source: "公开审核来源"), title: "已忽略的收件", body: "原始消息仍然保留。")
        _ = try store.resolveReview(threadID: rejected.id, decision: .reject)
        let pixelURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("platforms/linux/mailbox/tests/fixtures/public-pixel.png")
        let picture = try store.importTerminalResult(sessionID: UUID(), sourceName: "公开图片来源", title: "独立保存图片", body: "图片可以单独保存，也可以存入 Capsule。", pngData: Data(contentsOf: pixelURL))
        let imageDirectory = output.appendingPathComponent("images")
        try FileManager.default.createDirectory(at: imageDirectory, withIntermediateDirectories: true)
        var imageHashes: [String: String] = [:]
        for message in picture.messages {
            if let name = message.imageFileName, let data = store.imageData(for: message) {
                try data.write(to: imageDirectory.appendingPathComponent(name), options: .atomic)
                imageHashes[name] = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
            }
        }
        var cases: [[String: Any]] = []
        for module in [MailboxModuleID.chat, .inbox, .terminal] {
            for filter in MailboxInboxFilter.allCases {
                for query in ["", "公开", "不会自动", "不存在的查询"] {
                    cases.append(["module": module.rawValue, "filter": ["all", "unread", "attention", "archived"][filter.rawValue], "query": query,
                                  "ids": MailboxWorkspaceRules.threads(store.snapshot.threads, module: module, query: query, filter: filter).map { $0.id.uuidString.lowercased() }])
                }
            }
        }
        let document = try JSONSerialization.jsonObject(with: Data(contentsOf: store.storageURL))
        var legacy = document as! [String: Any]
        var threads = legacy["threads"] as! [[String: Any]]
        for i in threads.indices {
            threads[i].removeValue(forKey: "workspace"); threads[i].removeValue(forKey: "terminalSessionID"); threads[i].removeValue(forKey: "archivedAt")
            var messages = threads[i]["messages"] as! [[String: Any]]
            for j in messages.indices where messages[j]["kind"] as? String == "content" {
                messages[j].removeValue(forKey: "kind"); messages[j].removeValue(forKey: "format")
            }
            threads[i]["messages"] = messages
        }
        legacy["threads"] = threads
        // Confirm the legacy variation with the actual native decoder/store.
        let legacyRoot = root.appendingPathComponent("legacy")
        try FileManager.default.createDirectory(at: legacyRoot.appendingPathComponent("mailbox"), withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let legacyFile = legacyRoot.appendingPathComponent("mailbox/mailbox.json")
        try JSONSerialization.data(withJSONObject: legacy).write(to: legacyFile)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: legacyFile.path)
        _ = try MailboxStore(storageRoot: legacyRoot)
        var palettes: [String: [String: String]] = [:]
        for theme in ["night", "day", "quiet", "rasta"] {
            setenv("RIMEBUFFER_APPEARANCE_MODE", theme, 1)
            let appearance = RimeUI.appKitAppearance ?? NSAppearance(named: .darkAqua)!
            var palette: [String: String] = [:]
            appearance.performAsCurrentDrawingAppearance {
                for (name, color) in ["background": NSColor.textBackgroundColor, "label": .labelColor, "secondary": .secondaryLabelColor,
                                      "tertiary": .tertiaryLabelColor, "control": .controlBackgroundColor, "separator": .separatorColor,
                                      "accent": MailboxUI.accent, "sidebar": MailboxUI.sidebar, "selection": MailboxUI.selection] {
                    let c = color.usingColorSpace(.sRGB)!
                    palette[name] = "rgba(\(Int((c.redComponent * 255).rounded())),\(Int((c.greenComponent * 255).rounded())),\(Int((c.blueComponent * 255).rounded())),\(c.alphaComponent))"
                }
            }
            palettes[theme] = palette
            let host = MailboxWorkspaceViewController(store: store, remembersModule: false, isEnabled: { _ in true })
            let window = NSWindow(contentRect: NSRect(origin: .zero, size: MailboxUI.defaultSize), styleMask: [.titled, .resizable], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false; window.appearance = appearance
            window.contentViewController = host
            for (name, module, selected) in [("inbox", MailboxModuleID.inbox, incoming.id), ("chat", .chat, chat.threadID),
                                               ("terminal", .terminal, UUID()), ("image", .inbox, picture.id),
                                               ("accepted", .inbox, accepted.id), ("rejected", .inbox, rejected.id)] {
                if module == .terminal { host.selectModule(.terminal) }
                else { host.show(selecting: selected) }
                for (suffix, size) in [("wide", MailboxUI.defaultSize), ("compact", MailboxUI.minimumSize)] {
                    window.setContentSize(size)
                    RunLoop.current.run(until: Date().addingTimeInterval(0.15))
                    host.view.layoutSubtreeIfNeeded(); host.view.displayIfNeeded()
                    guard let bitmap = host.view.bitmapImageRepForCachingDisplay(in: host.view.bounds) else { throw CocoaError(.coderInvalidValue) }
                    host.view.cacheDisplay(in: host.view.bounds, to: bitmap)
                    guard let png = bitmap.representation(using: .png, properties: [:]) else { throw CocoaError(.coderInvalidValue) }
                    try png.write(to: output.appendingPathComponent("\(theme)-\(name)-\(suffix).png"), options: .atomic)
                }
            }
            window.close()
        }
        let repo = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        var sources: [String: String] = [:]
        for name in ["MailboxModels.swift", "MailboxStore.swift", "MailboxModules.swift", "MailboxWorkspaceUI.swift", "MailboxWorkspaceViewController.swift", "MailboxMessagesWorkspace.swift", "MailboxAdaptiveSplitView.swift", "MailboxLinuxFixtures.swift"] {
            let path = "Sources/RimeBuffer/Mailbox/\(name)"
            sources[path] = SHA256.hash(data: try Data(contentsOf: repo.appendingPathComponent(path))).map { String(format: "%02x", $0) }.joined()
        }
        for path in ["Sources/RimeBuffer/RimeUI.swift"] + MailboxModuleID.allCases.map({ "OfficialPlugins/plugins/mailbox.\($0.rawValue)/plugin.json" }) {
            sources[path] = SHA256.hash(data: try Data(contentsOf: repo.appendingPathComponent(path))).map { String(format: "%02x", $0) }.joined()
        }
        let modules: [[String: Any]] = MailboxModuleID.allCases.map { module in
            ["id": module.rawValue, "title": module.title, "symbol": module.symbol,
             "filters": module.filters.map { ["all", "unread", "attention", "archived"][$0.rawValue] }]
        }
        let fixture: [String: Any] = ["purpose": "public-parity-fixture", "document": document, "legacy": legacy, "images": imageHashes,
            "modules": modules, "cases": cases, "palettes": palettes, "selectedInbox": incoming.id.uuidString.lowercased(), "selectedChat": chat.threadID.uuidString.lowercased(),
            "selectedImage": picture.id.uuidString.lowercased(), "selectedAccepted": accepted.id.uuidString.lowercased(), "selectedRejected": rejected.id.uuidString.lowercased(),
            "sources": sources, "metrics": ["defaultWidth": MailboxUI.defaultSize.width, "defaultHeight": MailboxUI.defaultSize.height,
                "minimumWidth": MailboxUI.minimumSize.width, "minimumHeight": MailboxUI.minimumSize.height,
                "railWidth": MailboxUI.railWidth, "chatListWidth": MailboxUI.chatListWidth, "inboxListWidth": MailboxUI.inboxListWidth,
                "sidebarBreakpoint": MailboxUI.sidebarBreakpoint, "navigationWidth": MailboxUI.navigationWidth,
                "navigationHeight": MailboxUI.navigationHeight, "rowHeight": MailboxUI.rowHeight, "contentInset": MailboxUI.contentInset,
                "actionHeight": MailboxUI.actionHeight, "actionIconSize": MailboxUI.actionIconSize, "actionIconGap": MailboxUI.actionIconGap,
                "messageSpacing": MailboxUI.messageSpacing, "actionSpacing": MailboxUI.actionSpacing,
                "bodyFontSize": MailboxUI.bodyFontSize, "bodyLineSpacing": MailboxUI.bodyLineSpacing, "bodyParagraphSpacing": MailboxUI.bodyParagraphSpacing,
                "inboxImageHeight": MailboxUI.inboxImageHeight, "chatImageHeight": MailboxUI.chatImageHeight]]
        try JSONSerialization.data(withJSONObject: fixture, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
            .write(to: output.appendingPathComponent("native-mailbox.json"), options: .atomic)
    }
    static func verifyLinux(at root: URL) throws {
        let store = try MailboxStore(storageRoot: root)
        let expected = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("linux-public-fixture.json"))) as! [String: Any]
        guard expected["purpose"] as? String == "public-parity-fixture", let threads = expected["threads"] as? [[String: Any]] else { throw CocoaError(.coderReadCorrupt) }
        func normalized(_ value: Any) -> Any {
            if let text = value as? String, let uuid = UUID(uuidString: text) { return uuid.uuidString.lowercased() }
            if let object = value as? [String: Any] { return object.filter { !($0.value is NSNull) }.mapValues(normalized) }
            if let array = value as? [Any] { return array.map(normalized) }
            return value
        }
        let encoded = try JSONEncoder().encode(store.snapshot.threads)
        let actual = try JSONSerialization.jsonObject(with: encoded) as! [[String: Any]]
        let keyed: ([[String: Any]]) -> [String: Any] = { rows in
            Dictionary(uniqueKeysWithValues: rows.map { ((($0["id"] as! String).lowercased()), normalized($0)) })
        }
        guard NSDictionary(dictionary: keyed(threads)).isEqual(to: keyed(actual)) else { throw CocoaError(.coderReadCorrupt) }
        let images = expected["images"] as? [String: String] ?? [:]
        for thread in store.snapshot.threads {
            for message in thread.messages {
                guard let name = message.imageFileName else { continue }
                guard let bytes = store.imageData(for: message), let digest = images[name],
                      SHA256.hash(data: bytes).map({ String(format: "%02x", $0) }).joined() == digest else { throw CocoaError(.coderReadCorrupt) }
            }
        }
        print("Linux mailbox schema-v1 verified by native store: \(threads.count) public threads, complete sources, messages, dates and review states")
    }
}
