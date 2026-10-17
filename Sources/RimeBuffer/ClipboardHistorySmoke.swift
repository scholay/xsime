import Cocoa
import CryptoKit
import Carbon.HIToolbox

private final class ClipboardUnreadableDataProvider: NSObject,
    NSPasteboardItemDataProvider {
    private(set) var requestCount = 0

    func pasteboard(
        _ pasteboard: NSPasteboard?,
        item: NSPasteboardItem,
        provideDataForType type: NSPasteboard.PasteboardType
    ) {
        requestCount += 1
        // Intentionally leave the requested representation unreadable.
    }
}

/// Deterministic Clipboard model/window-view contract check. It never touches
/// the user's NSPasteboard or registers the global shortcut.
@MainActor
enum ClipboardHistorySmoke {
    /// Export behavior from the production model using an isolated pasteboard
    /// double. Linux consumes these semantic fixtures through native MIME names.
    static func exportLinuxParityFixtures(to path: String) -> Bool {
        struct Sample {
            let name: String
            let native: [[String: String]]
            let linux: [(String, String)]
        }
        let samples: [Sample] = [
            .init(name: "plain", native: [["public.utf8-plain-text": "中文\n第二行"]],
                  linux: [("text/plain", "中文\n第二行")]),
            .init(name: "plain-url", native: [["public.utf8-plain-text": "https://example.com"]],
                  linux: [("text/plain", "https://example.com")]),
            .init(name: "url", native: [["public.url": "https://example.com/path"]],
                  linux: [("text/uri-list", "https://example.com/path\r\n")]),
            .init(name: "image-with-caption", native: [["public.png": "image bytes", "public.utf8-plain-text": "caption"]],
                  linux: [("image/png", "image bytes"), ("text/plain", "caption")]),
            .init(name: "single-image-file", native: [["public.file-url": "file:///tmp/a%20b.PNG"]],
                  linux: [("text/uri-list", "file:///tmp/a%20b.PNG\r\n")]),
            .init(name: "multiple-files", native: [["public.file-url": "file:///tmp/a.png"], ["public.file-url": "file:///tmp/b.pdf"]],
                  linux: [("text/uri-list", "file:///tmp/a.png\r\nfile:///tmp/b.pdf\r\n")]),
            .init(name: "opaque", native: [["application/x-fixture": "opaque bytes"]],
                  linux: [("application/x-fixture", "opaque bytes")]),
        ]
        do {
            var rows: [[String: Any]] = []
            for sample in samples {
                let board = ClipboardHistoryPasteboardDouble()
                board.usesAsynchronousArchive = true
                let model = ClipboardHistoryModel(configuration: .init(), pasteboard: board,
                    sourceApplicationName: { "Fixture" }, schedulesAutomaticPolling: false)
                model.start()
                model.update(windowVisible: true, captureEnabled: true, protection: [])
                _ = model.pollNow()
                board.stubArchive = try ClipboardPasteboardArchive(items: sample.native.map { item in
                    .init(types: item.keys.sorted(), dataByType: item.mapValues { Data($0.utf8) })
                })
                board.stubChangeCount += 1
                _ = model.pollNow()
                board.completeAsynchronousRead()
                let deadline = Date(timeIntervalSinceNow: 3)
                while model.items.isEmpty, Date() < deadline {
                    RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
                }
                guard let item = model.items.first else { return false }
                rows.append([
                    "name": sample.name,
                    "representations": sample.linux.map { ["type": $0.0, "base64": Data($0.1.utf8).base64EncodedString()] },
                    "kind": item.kind.rawValue,
                    "text": item.canonicalText ?? "",
                    "filter": CapsuleModuleClassification.clipboard(item).rawValue,
                ])
                model.stop()
            }
            let paths = ["Sources/RimeBuffer/ClipboardHistoryModel.swift",
                         "Sources/RimeBuffer/ClipboardPasteboardArchive.swift",
                         "Sources/RimeBuffer/ClipboardHistorySmoke.swift",
                         "OfficialPlugins/native/macos/CapsuleModules.swift"]
            let sources = try Dictionary(uniqueKeysWithValues: paths.map { source in
                (source, SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: source)))
                    .map { String(format: "%02x", $0) }.joined())
            })
            let data = try JSONSerialization.data(withJSONObject: ["sources": sources, "cases": rows],
                                                 options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
            return true
        } catch { return false }
    }

    static func renderPreview(to path: String) -> Bool {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(
            "rimes-clipboard-preview-\(UUID().uuidString)",
            isDirectory: true
        )
        defer { try? FileManager.default.removeItem(at: root) }
        guard let store = try? ClipboardHistoryStore(rootDirectory: root) else {
            return false
        }
        let pasteboard = ClipboardHistoryPasteboardDouble()
        let model = ClipboardHistoryModel(
            configuration: .init(),
            pasteboard: pasteboard,
            clock: Date.init,
            sourceApplicationName: { "Safari" },
            sourceApplicationBundleIdentifier: { "com.apple.Safari" },
            store: store,
            schedulesAutomaticPolling: false
        )
        model.start()
        model.update(windowVisible: true, captureEnabled: true, protection: [])
        _ = model.ingest("https://docs.example.com/product/clipboard-history")
        _ = model.ingest("下周把 Capsule 的导入流程和 Obsidian 目录一起复查。")
        _ = model.ingest("RIMES keeps this clipboard timeline in its local private store.")
        _ = model.ingest("swift build -c debug")

        // Use a real project asset so the preview exercises the production
        // rich-image path instead of drawing a placeholder thumbnail.
        let iconURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            .appendingPathComponent("Logo/AppIcon.iconset/icon_256x256.png")
        if let iconData = (try? Data(contentsOf: iconURL)) ?? makeFixturePNG(),
           let archive = try? ClipboardPasteboardArchive(items: [
                .init(
                    types: [NSPasteboard.PasteboardType.png.rawValue],
                    dataByType: [
                        NSPasteboard.PasteboardType.png.rawValue: iconData,
                    ]
                ),
           ]) {
            pasteboard.usesAsynchronousArchive = true
            pasteboard.stubArchive = archive
            pasteboard.stubChangeCount += 1
            _ = model.pollNow()
            pasteboard.completeAsynchronousRead()
            let ingestionDeadline = Date(timeIntervalSinceNow: 1)
            while !model.items.contains(where: { $0.kind == .image }),
                  Date() < ingestionDeadline {
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
            }
        }

        let pane = ClipboardHistoryPaneView(model: model)
        pane.frame = NSRect(
            x: 0,
            y: 0,
            width: ClipboardHistoryWindowMetrics.preferredWidth,
            height: ClipboardHistoryWindowMetrics.preferredHeight
        )
        let thumbnailDeadline = Date(timeIntervalSinceNow: 2)
        while pane.snapshotForSmoke().renderedThumbnailCount == 0,
              Date() < thumbnailDeadline {
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
        }
        guard pane.snapshotForSmoke().renderedThumbnailCount > 0 else {
            return false
        }
        // Host the pane in a real (offscreen) window before capturing. The
        // search caret and the focused search border exist only when the rail
        // has a window, so a windowless preview silently dropped both and
        // could not have shown that the search box looked dead.
        let host = NSWindow(
            contentRect: pane.frame,
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        host.contentView = pane
        pane.layoutSubtreeIfNeeded()
        pane.showSearchCaretForCapture()
        pane.displayIfNeeded()
        guard let bitmap = pane.bitmapImageRepForCachingDisplay(in: pane.bounds) else {
            return false
        }
        pane.cacheDisplay(in: pane.bounds, to: bitmap)
        guard let data = bitmap.representation(using: .png, properties: [:]) else {
            return false
        }
        do {
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
            return true
        } catch {
            return false
        }
    }

    /// Under RIMES the search box is a label, so AppKit draws no insertion
    /// point: the box accepted text while looking dead. Assert the caret
    /// exists once the rail has a window, and that it advances as the query
    /// grows — a caret pinned at the left edge would look equally broken.
    @MainActor
    static func searchCaretProbe() -> Bool {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(
            "rimes-caret-probe-\(UUID().uuidString)",
            isDirectory: true
        )
        defer { try? FileManager.default.removeItem(at: root) }
        guard let store = try? ClipboardHistoryStore(rootDirectory: root) else {
            return false
        }
        let model = ClipboardHistoryModel(
            configuration: .init(),
            pasteboard: ClipboardHistoryPasteboardDouble(),
            clock: Date.init,
            sourceApplicationName: { "Safari" },
            sourceApplicationBundleIdentifier: { "com.apple.Safari" },
            store: store,
            schedulesAutomaticPolling: false
        )
        model.start()
        let pane = ClipboardHistoryPaneView(model: model)
        pane.frame = NSRect(
            x: 0,
            y: 0,
            width: ClipboardHistoryWindowMetrics.preferredWidth,
            height: ClipboardHistoryWindowMetrics.preferredHeight
        )
        // No window: nothing is focused, so there must be no caret.
        pane.layoutSubtreeIfNeeded()
        guard !pane.snapshotForSmoke().searchCaretVisible else { return false }

        let host = NSWindow(
            contentRect: pane.frame,
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        host.contentView = pane
        pane.layoutSubtreeIfNeeded()
        // In a window but never clicked: an unfocused field shows no caret.
        guard !pane.snapshotForSmoke().searchCaretVisible else { return false }

        pane.setSearchFocused(true)
        pane.layoutSubtreeIfNeeded()
        let empty = pane.snapshotForSmoke()
        guard empty.searchCaretVisible else { return false }

        _ = pane.appendSearchText("clipboard")
        pane.layoutSubtreeIfNeeded()
        pane.showSearchCaretForCapture()
        let typed = pane.snapshotForSmoke()
        guard typed.searchCaretVisible,
              typed.queryCharacterCount == 9,
              typed.searchCaretX > empty.searchCaretX else { return false }

        // Clicking a card or the background takes the focus away again.
        pane.setSearchFocused(false)
        pane.layoutSubtreeIfNeeded()
        return !pane.snapshotForSmoke().searchCaretVisible
    }

    /// Both bands were positioned by hand and drifted apart.
    @MainActor
    static func capturesBandProbe() -> Bool {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(
            "rimes-band-probe-\(UUID().uuidString)",
            isDirectory: true
        )
        defer { try? FileManager.default.removeItem(at: root) }
        guard let store = try? ClipboardHistoryStore(rootDirectory: root) else {
            return false
        }
        let model = ClipboardHistoryModel(
            configuration: .init(),
            pasteboard: ClipboardHistoryPasteboardDouble(),
            clock: Date.init,
            sourceApplicationName: { "Safari" },
            sourceApplicationBundleIdentifier: { "com.apple.Safari" },
            store: store,
            schedulesAutomaticPolling: false
        )
        model.start()
        let pane = ClipboardHistoryPaneView(model: model)
        pane.frame = NSRect(
            x: 0,
            y: 0,
            width: ClipboardHistoryWindowMetrics.preferredWidth,
            height: ClipboardHistoryWindowMetrics.preferredHeight
        )
        pane.layoutSubtreeIfNeeded()
        return pane.capturesBandMatchesRecentForSmoke
    }

    @MainActor
    static func makeThemeProbePane() -> ClipboardHistoryPaneView {
        ClipboardHistoryPaneView(model: ClipboardHistoryModel(
            configuration: .init(),
            pasteboard: ClipboardHistoryPasteboardDouble(),
            schedulesAutomaticPolling: false
        ))
    }

    static func run() -> Bool {
        var ok = true
        func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
            guard !condition() else { return }
            FileHandle.standardError.write(Data("FAILED: \(message)\n".utf8))
            ok = false
        }

        expect(
            ClipboardHistoryWindowVisibilityRules.isVisibleOnActiveSpace(
                isOrdered: true,
                isOnActiveSpace: true
            ),
            "active-space visibility"
        )
        expect(
            !ClipboardHistoryWindowVisibilityRules.isVisibleOnActiveSpace(
                isOrdered: true,
                isOnActiveSpace: false
            ),
            "other-space window reported visible"
        )
        expect(
            clipboardHistoryStandalonePanelKeyboardProbe(),
            "standalone Clipboard panel cannot own native search input"
        )
        expect(
            capturesBandProbe(),
            "the captures tab must occupy the same band as Recent"
        )
        expect(
            searchCaretProbe(),
            "borrowed-Rime search box must show a caret that follows the query"
        )
        expect(
            ClipboardHistoryPresentationMode.resolve(
                currentSourceIsOwn: true
            ) == .borrowedRime,
            "RIMES clipboard presentation mode"
        )
        expect(
            ClipboardHistoryPresentationMode.resolve(
                currentSourceIsOwn: false
            ) == .standalonePasteboard,
            "external clipboard presentation mode"
        )
        expect(
            ClipboardHistoryActivationRules.shouldAttemptDirectTextDelivery(
                mode: .borrowedRime,
                currentSourceIsOwn: true,
                allItemsAreCompletePlainText: true
            ),
            "borrowed RIMES text delivery policy"
        )
        expect(
            !ClipboardHistoryActivationRules.shouldAttemptDirectTextDelivery(
                mode: .standalonePasteboard,
                currentSourceIsOwn: false,
                allItemsAreCompletePlainText: true
            ),
            "external input method attempted direct text delivery"
        )
        expect(
            ClipboardHistoryStandaloneEditingRules.shouldDeleteSelectedCards(
                queryIsEmpty: true,
                selectedCount: 1,
                hasMarkedText: false
            ),
            "standalone empty-query Delete did not target the selected card"
        )
        expect(
            ClipboardHistoryStandaloneEditingRules.shouldDeleteSelectedCards(
                queryIsEmpty: false,
                selectedCount: 2,
                hasMarkedText: false
            ),
            "standalone multi-selection Delete did not target cards"
        )
        expect(
            !ClipboardHistoryStandaloneEditingRules.shouldDeleteSelectedCards(
                queryIsEmpty: false,
                selectedCount: 1,
                hasMarkedText: false
            ),
            "standalone query Delete bypassed native text editing"
        )
        expect(
            !ClipboardHistoryStandaloneEditingRules.shouldDeleteSelectedCards(
                queryIsEmpty: true,
                selectedCount: 1,
                hasMarkedText: true
            ),
            "standalone Delete interrupted an active external-IME composition"
        )

        let eligible = ClipboardHistoryWindowLifecycleRules.captureState(
            windowVisibleOnActiveSpace: true,
            captureEnabled: true,
            secureInput: false,
            screenLocked: false,
            sessionInactive: false,
            sleeping: false
        )
        let secure = ClipboardHistoryWindowLifecycleRules.captureState(
            windowVisibleOnActiveSpace: true,
            captureEnabled: true,
            secureInput: true,
            screenLocked: false,
            sessionInactive: false,
            sleeping: false
        )
        let stacked = ClipboardHistoryWindowLifecycleRules.captureState(
            windowVisibleOnActiveSpace: true,
            captureEnabled: true,
            secureInput: true,
            screenLocked: true,
            sessionInactive: true,
            sleeping: true
        )
        expect(eligible.allowsClipboardObservation, "eligible window did not capture")
        expect(eligible.allowsContentPresentation, "eligible window did not present")
        expect(!secure.allowsClipboardObservation, "secure input left capture open")
        expect(
            stacked.protection == [.secureInput, .screenLocked, .sessionInactive],
            "stacked protection projection"
        )

        expect(
            ClipboardHistoryPasteboardPolicy.allowsPlainTextRead(
                typeNames: ["public.utf8-plain-text"]
            ),
            "ordinary plain text marker rejected"
        )
        expect(
            !ClipboardHistoryPasteboardPolicy.allowsPlainTextRead(
                typeNames: ["public.utf8-plain-text", "org.nspasteboard.ConcealedType"]
            ),
            "concealed pasteboard marker accepted"
        )
        expect(
            !ClipboardHistoryPasteboardPolicy.allowsPlainTextRead(
                typeNames: ["org.nspasteboard.TransientType"]
            ),
            "transient pasteboard marker accepted"
        )
        do {
            let isolatedPasteboard = NSPasteboard(
                name: .init("com.rimes.clipboard-smoke.\(UUID().uuidString)")
            )
            defer { isolatedPasteboard.releaseGlobally() }
            let item = NSPasteboardItem()
            let fixture = "RIMES rich clipboard fixture"
            let rich = NSAttributedString(
                string: fixture,
                attributes: [.font: NSFont.boldSystemFont(ofSize: 16)]
            )
            let richData = try rich.data(
                from: NSRange(location: 0, length: rich.length),
                documentAttributes: [
                    .documentType: NSAttributedString.DocumentType.rtf,
                ]
            )
            expect(item.setString(fixture, forType: .string), "rich fixture text")
            expect(item.setData(richData, forType: .rtf), "rich fixture RTF")
            isolatedPasteboard.clearContents()
            expect(
                isolatedPasteboard.writeObjects([item]),
                "rich fixture pasteboard write"
            )
            let synthesizedUTF16 = isolatedPasteboard.pasteboardItems?.first?
                .data(forType: .init("public.utf16-external-plain-text"))
            let archive = try ClipboardPasteboardArchive.capture(
                from: isolatedPasteboard
            )
            expect(archive.items.count == 1, "rich fixture archive item count")
            expect(
                archive.items[0].dataByType[
                    NSPasteboard.PasteboardType.string.rawValue
                ] != nil,
                "rich fixture lost readable plain text"
            )
            expect(
                archive.items[0].dataByType[
                    NSPasteboard.PasteboardType.rtf.rawValue
                ] != nil,
                "rich fixture lost RTF"
            )
            expect(
                archive.items[0].dataByType[
                    "public.utf16-external-plain-text"
                ] == synthesizedUTF16,
                "rich fixture did not mirror readable synthesized UTF-16"
            )
            let expectedChangeCount = isolatedPasteboard.changeCount
            isolatedPasteboard.clearContents()
            isolatedPasteboard.setString(
                "newer-user-clipboard",
                forType: .string
            )
            do {
                _ = try archive.write(
                    to: isolatedPasteboard,
                    expectedChangeCount: expectedChangeCount
                )
                expect(false, "stale archive overwrote newer pasteboard")
            } catch ClipboardPasteboardArchive.ArchiveError.pasteboardChanged {
                // Expected: asynchronous restoration is compare-before-write.
            }
            expect(
                isolatedPasteboard.string(forType: .string)
                    == "newer-user-clipboard",
                "stale archive cleared newer pasteboard"
            )
        } catch {
            expect(false, "rich fixture archive: \(error.localizedDescription)")
        }
        runCapturePolicyChecks(expect: expect)
        for command in [
            "insertNewline:", "cancelOperation:", "moveLeft:", "moveRight:",
            "moveUp:", "moveDown:", "deleteBackward:", "deleteForward:",
        ] {
            expect(
                !ClipboardHistoryStandaloneEditingRules
                    .permitsSurfaceCommand(hasMarkedText: true),
                "standalone marked composition leaked command \(command)"
            )
        }
        expect(
            ClipboardHistoryStandaloneEditingRules
                .permitsSurfaceCommand(hasMarkedText: false),
            "standalone settled search rejected surface commands"
        )
        expect(
            ClipboardHistoryScrollRules.horizontalDelta(
                deltaX: 0,
                deltaY: 1,
                precise: false,
                shiftHeld: true
            ) == -48,
            "Shift discrete-wheel reversed acceleration"
        )
        expect(
            ClipboardHistoryScrollRules.horizontalDelta(
                deltaX: 0,
                deltaY: 4,
                precise: true,
                shiftHeld: true
            ) == 10,
            "Shift precise-wheel acceleration follows the trackpad"
        )
        expect(
            ClipboardHistoryScrollRules.horizontalDelta(
                deltaX: 0,
                deltaY: -10,
                precise: false,
                shiftHeld: true
            ) == 240,
            "Shift reversed wheel clamp"
        )
        expect(
            ClipboardHistoryScrollRules.horizontalDelta(
                deltaX: 5,
                deltaY: 100,
                precise: true,
                shiftHeld: false
            ) == 5,
            "trackpad horizontal delta follows the fingers"
        )
        expect(
            ClipboardHistoryScrollRules.horizontalDelta(
                deltaX: 0,
                deltaY: 1,
                precise: false,
                shiftHeld: false
            ) == -32,
            "unmodified vertical timeline direction"
        )
        // macOS inverts scroll direction per device, so the wheel and the
        // trackpad must take opposite signs to feel identical in the hand.
        // Every trackpad path agrees with itself; the wheel opposes it.
        expect(
            ClipboardHistoryScrollRules.horizontalDelta(
                deltaX: 0, deltaY: 3, precise: true, shiftHeld: false
            ) > 0
                && ClipboardHistoryScrollRules.horizontalDelta(
                    deltaX: 0, deltaY: 3, precise: true, shiftHeld: true
                ) > 0
                && ClipboardHistoryScrollRules.horizontalDelta(
                    deltaX: 3, deltaY: 0, precise: true, shiftHeld: false
                ) > 0
                && ClipboardHistoryScrollRules.horizontalDelta(
                    deltaX: 0, deltaY: 3, precise: false, shiftHeld: true
                ) < 0,
            "trackpad must oppose the wheel, not match it"
        )

        let pasteboard = ClipboardHistoryPasteboardDouble()
        let configuration = ClipboardHistoryConfiguration(
            maximumItems: 3,
            maximumItemBytes: 32,
            maximumTotalBytes: 64,
            pollingInterval: 1
        )
        var now = Date(timeIntervalSince1970: 1_700_000_000)
        var dynamicProtection: ClipboardHistoryProtection = []
        let model = ClipboardHistoryModel(
            configuration: configuration,
            pasteboard: pasteboard,
            protectionProbe: { dynamicProtection },
            clock: {
                defer { now.addTimeInterval(1) }
                return now
            },
            sourceApplicationName: { "Fixture App" },
            schedulesAutomaticPolling: false
        )

        pasteboard.stubChangeCount = 7
        model.start()
        expect(!model.pollNow(), "hidden poll reported capture")
        expect(pasteboard.changeCountReadCount == 0, "hidden poll read change count")
        expect(pasteboard.plainTextReadCount == 0, "hidden poll read text")

        model.update(windowVisible: true, captureEnabled: false, protection: [])
        expect(!model.pollNow(), "disabled poll reported capture")
        expect(pasteboard.changeCountReadCount == 0, "disabled poll touched pasteboard")

        model.update(windowVisible: true, captureEnabled: true, protection: [])
        expect(pasteboard.changeCountReadCount == 1, "visible start missed baseline")
        expect(pasteboard.plainTextReadCount == 0, "baseline read text")
        pasteboard.stubChangeCount = 8
        pasteboard.stubPlainText = "first"
        expect(model.pollNow(), "changed pasteboard was not captured")
        expect(model.items.first?.sourceApplicationName == "Fixture App", "source app missing")
        expect(model.items.first?.text == "first", "captured wrong item")

        let firstID = model.items.first?.id
        pasteboard.stubChangeCount = 9
        pasteboard.stubPlainText = "first"
        expect(model.pollNow(), "duplicate was not accepted")
        expect(model.itemCount == 1, "duplicate created a second item")
        expect(model.items.first?.id == firstID, "duplicate identity changed")

        pasteboard.stubChangeCount = 10
        expect(
            model.baselineAfterOwnPasteboardWrite(expectedChangeCount: 10),
            "exact own-write baseline rejected"
        )
        let readsBeforeOwnWritePoll = pasteboard.plainTextReadCount
        expect(!model.pollNow(), "own write baseline recaptured content")
        expect(
            pasteboard.plainTextReadCount == readsBeforeOwnWritePoll,
            "own write baseline read text"
        )
        pasteboard.stubChangeCount = 11
        pasteboard.stubPlainText = "concurrent external write"
        expect(
            !model.baselineAfterOwnPasteboardWrite(expectedChangeCount: 10),
            "stale own-write baseline hid a concurrent external copy"
        )
        expect(model.pollNow(), "concurrent external copy was not captured")
        expect(
            model.items.first?.text == "concurrent external write",
            "concurrent external copy captured the wrong value"
        )

        expect(!model.ingest(""), "empty item accepted")
        expect(!model.ingest(" \n\t "), "whitespace item accepted")
        expect(!model.ingest("bad\0value"), "NUL item accepted")
        expect(!model.ingest(String(repeating: "x", count: 33)), "oversized item accepted")
        expect(model.ingest("second"), "second item rejected")
        expect(model.ingest("third"), "third item rejected")
        expect(model.moveSelection(delta: 1), "selection movement failed")
        let selectedBeforeDelete = model.selectedID
        expect(model.deleteSelected(), "selected delete failed")
        expect(
            !model.items.contains(where: { $0.id == selectedBeforeDelete }),
            "selected delete retained item"
        )

        model.update(
            windowVisible: true,
            captureEnabled: true,
            protection: [.screenLocked]
        )
        let protectedCountReads = pasteboard.changeCountReadCount
        let protectedTextReads = pasteboard.plainTextReadCount
        pasteboard.stubChangeCount = 11
        pasteboard.stubPlainText = "protected"
        expect(!model.pollNow(), "protected poll reported capture")
        expect(model.items.isEmpty, "protected items projection leaked")
        expect(model.itemCount > 0, "protected history was discarded")
        expect(pasteboard.changeCountReadCount == protectedCountReads, "protected count read")
        expect(pasteboard.plainTextReadCount == protectedTextReads, "protected text read")

        model.update(windowVisible: true, captureEnabled: true, protection: [])
        expect(!model.pollNow(), "protected interval was backfilled")
        expect(pasteboard.plainTextReadCount == protectedTextReads, "resume read text")
        dynamicProtection = [.secureInput]
        pasteboard.stubChangeCount = 12
        expect(!model.pollNow(), "dynamic secure input reported capture")
        dynamicProtection = []
        expect(!model.pollNow(), "dynamic protection resume backfilled")

        let explicitPasteboard = ClipboardHistoryPasteboardDouble()
        explicitPasteboard.stubChangeCount = 40
        explicitPasteboard.stubPlainText = "already copied"
        let explicitModel = ClipboardHistoryModel(
            configuration: configuration,
            pasteboard: explicitPasteboard,
            schedulesAutomaticPolling: false
        )
        explicitModel.start()
        expect(!explicitModel.captureCurrentIfEligible(), "hidden explicit capture")
        explicitModel.update(windowVisible: true, captureEnabled: true, protection: [])
        expect(explicitModel.captureCurrentIfEligible(), "explicit current capture failed")
        expect(explicitModel.items.map(\.text) == ["already copied"], "explicit item mismatch")

        do {
            let asyncPasteboard = ClipboardHistoryPasteboardDouble()
            asyncPasteboard.usesAsynchronousArchive = true
            asyncPasteboard.stubChangeCount = 50
            let asyncModel = ClipboardHistoryModel(
                configuration: .init(
                    maximumItems: 10,
                    maximumItemBytes: 1_024 * 1_024,
                    maximumTotalBytes: 4 * 1_024 * 1_024,
                    pollingInterval: 1
                ),
                pasteboard: asyncPasteboard,
                schedulesAutomaticPolling: false
            )
            asyncModel.start()
            asyncModel.update(
                windowVisible: true,
                captureEnabled: true,
                protection: []
            )
            asyncPasteboard.stubChangeCount = 51
            asyncPasteboard.stubArchive = try ClipboardPasteboardArchive(items: [
                .init(
                    types: [NSPasteboard.PasteboardType.string.rawValue],
                    dataByType: [
                        NSPasteboard.PasteboardType.string.rawValue:
                            Data("discarded async".utf8),
                    ]
                ),
            ])
            expect(asyncModel.pollNow(), "async capture was not scheduled")
            asyncModel.update(
                windowVisible: true,
                captureEnabled: true,
                protection: [.secureInput]
            )
            asyncPasteboard.completeAsynchronousRead()
            RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.05))
            expect(
                asyncModel.itemCount == 0,
                "protected transition accepted in-flight async capture"
            )

            asyncModel.update(
                windowVisible: true,
                captureEnabled: true,
                protection: []
            )
            asyncPasteboard.stubChangeCount = 52
            let utf16Type = "public.utf16-plain-text"
            asyncPasteboard.stubArchive = try ClipboardPasteboardArchive(items: [
                .init(
                    types: [utf16Type],
                    dataByType: [
                        utf16Type: "accepted async".data(
                            using: .utf16LittleEndian
                        )!,
                    ]
                ),
            ])
            expect(asyncModel.pollNow(), "eligible async capture was not scheduled")
            asyncPasteboard.completeAsynchronousRead()
            let deadline = Date(timeIntervalSinceNow: 2)
            while asyncModel.itemCount == 0, Date() < deadline {
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            expect(
                asyncModel.items.first?.text == "accepted async",
                "eligible async capture was not applied"
            )
        } catch {
            expect(false, "async archive fixture threw: \(error.localizedDescription)")
        }

        let boundedModel = ClipboardHistoryModel(
            configuration: .init(
                maximumItems: 2,
                maximumItemBytes: 8,
                maximumTotalBytes: 10,
                pollingInterval: 1
            ),
            pasteboard: ClipboardHistoryPasteboardDouble(),
            schedulesAutomaticPolling: false
        )
        _ = boundedModel.ingest("aaaa")
        _ = boundedModel.ingest("bbbb")
        _ = boundedModel.ingest("cccc")
        expect(boundedModel.itemCount == 2, "item bound failed")
        expect(boundedModel.storedByteCount == 8, "byte bound failed")
        expect(!boundedModel.ingest("123456789"), "per-item byte cap failed")

        do {
            let promotionPasteboard = ClipboardHistoryPasteboardDouble()
            promotionPasteboard.usesAsynchronousArchive = true
            promotionPasteboard.stubChangeCount = 70
            let promotionModel = ClipboardHistoryModel(
                configuration: .init(
                    maximumItems: 10,
                    maximumItemBytes: 1_024 * 1_024,
                    maximumTotalBytes: 4 * 1_024 * 1_024,
                    pollingInterval: 1
                ),
                pasteboard: promotionPasteboard,
                schedulesAutomaticPolling: false
            )
            promotionModel.start()
            promotionModel.update(
                windowVisible: true,
                captureEnabled: true,
                protection: []
            )
            expect(
                promotionModel.ingest("older selected companion"),
                "manual-paste promotion companion fixture"
            )
            let companionID = promotionModel.items.first?.id
            guard let fixturePNG = makeFixturePNG() else {
                expect(false, "manual-paste promotion PNG fixture")
                throw ClipboardHistoryStoreError.corruptRecord
            }
            promotionPasteboard.stubArchive = try ClipboardPasteboardArchive(items: [
                .init(
                    types: [NSPasteboard.PasteboardType.png.rawValue],
                    dataByType: [
                        NSPasteboard.PasteboardType.png.rawValue: fixturePNG,
                    ]
                ),
            ])
            promotionPasteboard.stubChangeCount = 71
            expect(
                promotionModel.pollNow(),
                "manual-paste promotion image capture was not scheduled"
            )
            promotionPasteboard.completeAsynchronousRead()
            let promotionCaptureDeadline = Date(timeIntervalSinceNow: 2)
            while !promotionModel.items.contains(where: { $0.kind == .image }),
                  Date() < promotionCaptureDeadline {
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            let richID = promotionModel.items.first(where: { $0.kind == .image })?.id
            expect(
                promotionModel.ingest("newer unselected sentinel"),
                "manual-paste promotion sentinel fixture"
            )
            let sentinelID = promotionModel.items.first?.id
            if let richID, let companionID, let sentinelID {
                expect(
                    promotionModel.items.first?.id == sentinelID
                        && promotionModel.items.firstIndex(where: { $0.id == richID }) != 0,
                    "manual-paste rich fixture was already first"
                )
                let activationOrder = [richID, companionID]
                expect(
                    promotionModel.select(
                        ids: activationOrder,
                        focusedID: activationOrder.first
                    ),
                    "manual-paste activation selection fixture"
                )
                expect(
                    promotionModel.promote(ids: activationOrder),
                    "manual-paste activation promotion"
                )
                expect(
                    Array(promotionModel.items.prefix(2).map(\.id))
                        == activationOrder,
                    "manual-paste activation changed selection order"
                )
                expect(
                    promotionModel.items.map(\.id)
                        == activationOrder + [sentinelID],
                    "manual-paste activation changed unselected order"
                )
                expect(
                    promotionModel.selectedIDs == Set(activationOrder),
                    "manual-paste activation lost selected items"
                )
                expect(
                    promotionModel.selectedID == activationOrder.first,
                    "manual-paste activation lost focused item"
                )
            } else {
                expect(false, "manual-paste promotion fixture IDs")
            }
        } catch {
            expect(
                false,
                "manual-paste promotion fixture threw: \(error.localizedDescription)"
            )
        }

        let imageSearchItem = ClipboardHistoryItem(
            id: UUID(),
            kind: .image,
            displayText: "图像",
            searchText: nil,
            canonicalText: nil,
            textCompleteness: .unavailable,
            byteCount: 1,
            payloadByteCount: 1,
            capturedAt: Date(),
            sourceApplicationName: "Fixture",
            sourceApplicationBundleIdentifier: "fixture.image",
            sourceNamespace: "rimes.clipboard.smoke",
            sourceID: "image-search"
        )
        let searchableItems = explicitModel.items + model.items + [imageSearchItem]
        expect(
            ClipboardHistorySearchRules.filter(searchableItems, query: "ALREADY copied")
                .map(\.text) == ["already copied"],
            "case-insensitive multi-term search failed"
        )
        expect(
            ClipboardHistorySearchRules.filter(searchableItems, query: "missing").isEmpty,
            "no-result search failed"
        )
        expect(
            ClipboardHistorySearchRules.filter(searchableItems, query: "图片")
                .map(\.id) == [imageSearchItem.id],
            "localized image-kind search failed"
        )

        runArchiveAndStoreChecks(expect: expect)
        runViewChecks(expect: expect)
        return ok
    }

    private static func runCapturePolicyChecks(
        expect: (_ condition: @autoclosure () -> Bool, _ message: String) -> Void
    ) {
        let externalUTF16 = NSPasteboard.PasteboardType(
            "public.utf16-external-plain-text"
        )

        do {
            let pasteboard = NSPasteboard(
                name: .init("com.rimes.clipboard-readable-text.\(UUID().uuidString)")
            )
            defer { pasteboard.releaseGlobally() }
            let provider = ClipboardUnreadableDataProvider()
            let item = NSPasteboardItem()
            let textData = Data("readable UTF-8 fallback".utf8)
            expect(item.setData(textData, forType: .string), "fallback text data")
            expect(
                item.setDataProvider(provider, forTypes: [externalUTF16]),
                "external UTF-16 provider"
            )
            pasteboard.clearContents()
            expect(pasteboard.writeObjects([item]), "fallback fixture write")
            expect(
                pasteboard.pasteboardItems?.first?.types.contains(externalUTF16) == true,
                "fallback fixture did not advertise external UTF-16"
            )
            expect(
                pasteboard.pasteboardItems?.first?.data(forType: externalUTF16) == nil,
                "fallback fixture external UTF-16 was unexpectedly readable"
            )
            let archive = try ClipboardPasteboardArchive.capture(from: pasteboard)
            expect(archive.items.count == 1, "fallback fixture archive count")
            expect(
                archive.items[0].dataByType[NSPasteboard.PasteboardType.string.rawValue]
                    == textData,
                "fallback fixture lost decodable plain text"
            )
            expect(
                archive.items[0].dataByType[externalUTF16.rawValue] == nil,
                "fallback fixture retained unreadable synthesized text"
            )
        } catch {
            expect(false, "readable fallback fixture: \(error.localizedDescription)")
        }

        do {
            let pasteboard = NSPasteboard(
                name: .init("com.rimes.clipboard-no-fallback.\(UUID().uuidString)")
            )
            defer { pasteboard.releaseGlobally() }
            let provider = ClipboardUnreadableDataProvider()
            let item = NSPasteboardItem()
            expect(
                item.setDataProvider(provider, forTypes: [externalUTF16]),
                "no-fallback external UTF-16 provider"
            )
            pasteboard.clearContents()
            expect(pasteboard.writeObjects([item]), "no-fallback fixture write")
            var rejected = false
            do {
                _ = try ClipboardPasteboardArchive.capture(from: pasteboard)
            } catch ClipboardPasteboardArchive.ArchiveError
                .unreadablePasteboardType {
                rejected = true
            } catch {}
            expect(rejected, "unreadable external UTF-16 without fallback accepted")
        }

        do {
            let pasteboard = NSPasteboard(
                name: .init("com.rimes.clipboard-unknown-type.\(UUID().uuidString)")
            )
            defer { pasteboard.releaseGlobally() }
            let provider = ClipboardUnreadableDataProvider()
            let item = NSPasteboardItem()
            let unknownType = NSPasteboard.PasteboardType(
                "com.rimes.smoke.unreadable"
            )
            expect(
                item.setData(Data("known text".utf8), forType: .string),
                "unknown fixture fallback text"
            )
            expect(
                item.setDataProvider(provider, forTypes: [unknownType]),
                "unknown fixture provider"
            )
            pasteboard.clearContents()
            expect(pasteboard.writeObjects([item]), "unknown fixture write")
            var rejected = false
            do {
                _ = try ClipboardPasteboardArchive.capture(from: pasteboard)
            } catch ClipboardPasteboardArchive.ArchiveError
                .unreadablePasteboardType {
                rejected = true
            } catch {}
            expect(rejected, "unknown unreadable pasteboard type accepted")
        }

        do {
            let pasteboard = NSPasteboard(
                name: .init("com.rimes.clipboard-malformed-text.\(UUID().uuidString)")
            )
            defer { pasteboard.releaseGlobally() }
            let provider = ClipboardUnreadableDataProvider()
            let item = NSPasteboardItem()
            expect(
                item.setData(Data([0xFF]), forType: .string),
                "malformed fixture text data"
            )
            expect(
                item.setDataProvider(provider, forTypes: [externalUTF16]),
                "malformed fixture external UTF-16 provider"
            )
            pasteboard.clearContents()
            expect(pasteboard.writeObjects([item]), "malformed fixture write")
            var rejected = false
            do {
                _ = try ClipboardPasteboardArchive.capture(from: pasteboard)
            } catch ClipboardPasteboardArchive.ArchiveError
                .unreadablePasteboardType {
                rejected = true
            } catch {}
            expect(rejected, "malformed plain text unlocked unreadable UTF-16")
        }

        do {
            let pasteboard = NSPasteboard(
                name: .init("com.rimes.clipboard-confidential.\(UUID().uuidString)")
            )
            defer { pasteboard.releaseGlobally() }
            let provider = ClipboardUnreadableDataProvider()
            let first = NSPasteboardItem()
            expect(
                first.setDataProvider(provider, forTypes: [.string]),
                "confidential fixture lazy provider"
            )
            let second = NSPasteboardItem()
            expect(
                second.setData(
                    Data(),
                    forType: .init("org.nspasteboard.ConcealedType")
                ),
                "confidential fixture marker"
            )
            pasteboard.clearContents()
            expect(
                pasteboard.writeObjects([first, second]),
                "confidential fixture write"
            )
            let requestsBeforeCapture = provider.requestCount
            var rejected = false
            do {
                _ = try ClipboardPasteboardArchive.capture(from: pasteboard)
            } catch ClipboardPasteboardArchive.ArchiveError.confidentialContent {
                rejected = true
            } catch {}
            expect(rejected, "later confidential marker was accepted")
            expect(
                provider.requestCount == requestsBeforeCapture,
                "payload read occurred before global confidential preflight"
            )
        }
    }

    private static func runArchiveAndStoreChecks(
        expect: (_ condition: @autoclosure () -> Bool, _ message: String) -> Void
    ) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(
            "rimes-clipboard-history-smoke-\(UUID().uuidString)",
            isDirectory: true
        )
        defer { try? FileManager.default.removeItem(at: root) }

        do {
            let textType = NSPasteboard.PasteboardType.string.rawValue
            let htmlType = NSPasteboard.PasteboardType.html.rawValue
            let archive = try ClipboardPasteboardArchive(items: [
                .init(
                    types: [textType, htmlType],
                    dataByType: [
                        textType: Data("durable fixture".utf8),
                        htmlType: Data("<b>durable fixture</b>".utf8),
                    ]
                ),
            ])
            let compressed = try archive.encodeRawDeflate()
            let decoded = try ClipboardPasteboardArchive.decodeRawDeflate(
                compressed
            )
            expect(
                decoded == archive,
                "lossless archive round trip"
            )
            expect(
                decoded.requiresPasteboardRestorationForTextInsertion,
                "HTML archive must require restoration for lossless copy"
            )
            let reconstructed = try decoded.makePasteboardItems()
            expect(
                reconstructed.count == 1
                    && reconstructed[0].data(
                        forType: NSPasteboard.PasteboardType(htmlType)
                    ) == Data("<b>durable fixture</b>".utf8),
                "archive did not reconstruct exact pasteboard representations"
            )
            let plainArchive = try ClipboardPasteboardArchive(items: [
                .init(
                    types: [textType],
                    dataByType: [textType: Data("plain fixture".utf8)]
                ),
            ])
            expect(
                !plainArchive.requiresPasteboardRestorationForTextInsertion,
                "plain text archive unexpectedly requires pasteboard restore"
            )
            guard let fixturePNG = makeFixturePNG() else {
                expect(false, "fixture PNG creation")
                return
            }
            let imageArchive = try ClipboardPasteboardArchive(items: [
                .init(
                    types: [NSPasteboard.PasteboardType.png.rawValue],
                    dataByType: [
                        NSPasteboard.PasteboardType.png.rawValue: fixturePNG,
                    ]
                ),
            ])
            expect(
                imageArchive.containsImageRepresentation,
                "image archive representation detection"
            )
            for identifier in [
                "public.png",
                "public.jpeg",
                "public.heic",
                "com.compuserve.gif",
                "org.webmproject.webp",
            ] {
                expect(
                    ClipboardPasteboardArchive.isImageRepresentationType(
                        identifier
                    ),
                    "image UTI detection"
                )
            }
            expect(
                !ClipboardPasteboardArchive.isImageRepresentationType(
                    NSPasteboard.PasteboardType.fileURL.rawValue
                ),
                "file URL misclassified as image representation"
            )
            let fileURLType = NSPasteboard.PasteboardType.fileURL.rawValue
            let imageFileArchive = try ClipboardPasteboardArchive(items: [
                .init(
                    types: [fileURLType, NSPasteboard.PasteboardType.png.rawValue],
                    dataByType: [
                        fileURLType: Data("file:///tmp/rimes-image.png".utf8),
                        NSPasteboard.PasteboardType.png.rawValue: fixturePNG,
                    ]
                ),
            ])
            expect(
                imageFileArchive.containsImageRepresentation,
                "image-bearing file archive representation detection"
            )
            expect(
                imageFileArchive.makeImageThumbnail(maximumPixelSize: 3) != nil,
                "image-bearing file archive thumbnail decode"
            )
            let thumbnail = imageArchive.makeImageThumbnail(maximumPixelSize: 3)
            expect(thumbnail != nil, "image thumbnail decode")
            expect(
                max(thumbnail?.width ?? 0, thumbnail?.height ?? 0) <= 3,
                "image thumbnail was not downsampled"
            )
            let mergedArchive = try ClipboardPasteboardArchive.merging([
                plainArchive, imageArchive, imageFileArchive,
            ])
            expect(mergedArchive.items.count == 3, "ordered archive merge")
            let directTextItem = ClipboardHistoryItem(
                id: UUID(),
                kind: .text,
                displayText: "plain fixture",
                searchText: "plain fixture",
                canonicalText: "plain fixture",
                textCompleteness: .complete,
                byteCount: 13,
                payloadByteCount: 13,
                capturedAt: Date(),
                sourceApplicationName: "Fixture",
                sourceApplicationBundleIdentifier: "fixture.app",
                sourceNamespace: "rimes.clipboard.smoke",
                sourceID: "direct-text"
            )
            let richImageItem = ClipboardHistoryItem(
                id: UUID(),
                kind: .image,
                displayText: "Image 8 × 6",
                searchText: "image",
                canonicalText: nil,
                textCompleteness: .unavailable,
                byteCount: fixturePNG.count,
                payloadByteCount: fixturePNG.count,
                capturedAt: Date(),
                sourceApplicationName: "Fixture",
                sourceApplicationBundleIdentifier: "fixture.app",
                sourceNamespace: "rimes.clipboard.smoke",
                sourceID: "rich-image"
            )
            expect(
                ClipboardHistoryActivationRules.canInsertEveryItemAsPlainText(
                    items: [directTextItem],
                    archives: [plainArchive]
                ),
                "plain-text activation classification"
            )
            expect(
                !ClipboardHistoryActivationRules.canInsertEveryItemAsPlainText(
                    items: [directTextItem],
                    archives: [decoded]
                ),
                "rich text activation flattened its archived representations"
            )
            expect(
                !ClipboardHistoryActivationRules.canInsertEveryItemAsPlainText(
                    items: [directTextItem, richImageItem],
                    archives: [plainArchive, imageArchive]
                ),
                "mixed rich activation classification"
            )
            let record = ClipboardHistoryImportRecord(
                kind: .text,
                displayText: "durable fixture",
                searchText: "durable fixture fixture.app",
                canonicalText: "durable fixture",
                textCompleteness: .complete,
                capturedAt: Date(timeIntervalSince1970: 1_700_000_100),
                sourceApplicationName: "Fixture",
                sourceApplicationBundleIdentifier: "fixture.app",
                sourceNamespace: "rimes.clipboard.smoke",
                sourceID: "record-1",
                opaquePayload: compressed
            )
            let firstStore = try ClipboardHistoryStore(rootDirectory: root)
            try firstStore.importSourceApplicationIcons([
                .init(
                    bundleIdentifier: "fixture.app",
                    applicationName: "Fixture",
                    pngData: fixturePNG
                ),
            ])
            let persistedFixtureIcon = try firstStore.sourceApplicationIcon(
                bundleIdentifier: "fixture.app"
            )
            expect(
                persistedFixtureIcon == fixturePNG,
                "source application icon persistence"
            )
            let first = try firstStore.importBatch([record])
            expect(first.inserted == 1 && first.unchanged == 0, "store first import")
            let second = try firstStore.importBatch([record])
            expect(second.inserted == 0 && second.unchanged == 1, "store idempotent import")
            let storedPayload = try firstStore.payload(for: record.id)
            expect(
                storedPayload?.opaquePayload == compressed,
                "store lazy payload mismatch"
            )
            let audit = try firstStore.audit()
            expect(audit.isConsistent, "store audit")
            expect(audit.totalItemCount == 1 && audit.payloadItemCount == 1,
                   "store audit counts")

            let reopened = try ClipboardHistoryStore(rootDirectory: root)
            let reopenedCount = try reopened.count()
            expect(reopenedCount == 1, "store did not survive reopen")
            let secondRecord = ClipboardHistoryImportRecord(
                kind: .text,
                displayText: "second fixture",
                searchText: "second fixture",
                canonicalText: "second fixture",
                textCompleteness: .complete,
                capturedAt: Date(timeIntervalSince1970: 1_700_000_101),
                sourceApplicationName: "Fixture",
                sourceApplicationBundleIdentifier: "fixture.app",
                sourceNamespace: "rimes.clipboard.smoke",
                sourceID: "record-2",
                opaquePayload: try plainArchive.encodeRawDeflate()
            )
            _ = try reopened.importBatch([secondRecord])
            _ = try reopened.promote(
                ids: [record.id, secondRecord.id],
                at: Date(timeIntervalSince1970: 1_700_000_300)
            )
            let promotedIDs = Array(
                try reopened.loadAllMetadata().prefix(2).map(\.id)
            )
            expect(
                promotedIDs == [record.id, secondRecord.id],
                "store batch promotion order"
            )
            let deleted = try reopened.delete(
                ids: [record.id, secondRecord.id]
            )
            expect(deleted == 2, "store persistent batch delete")
            let countAfterDelete = try reopened.count()
            expect(countAfterDelete == 0, "store delete retained record")

            let persistedModel = ClipboardHistoryModel(
                configuration: .init(),
                pasteboard: ClipboardHistoryPasteboardDouble(),
                store: reopened,
                schedulesAutomaticPolling: false
            )
            persistedModel.start()
            persistedModel.update(
                windowVisible: true,
                captureEnabled: true,
                protection: []
            )
            expect(
                persistedModel.ingest("termination flush fixture"),
                "model persistence fixture ingest"
            )
            expect(
                persistedModel.flushPersistence(timeout: 5),
                "model persistence flush timed out"
            )
            let countAfterFlush = try reopened.count()
            expect(countAfterFlush == 1, "flush returned before durable upsert")
            persistedModel.clear()
            expect(
                persistedModel.flushPersistence(timeout: 5),
                "model persistence clear flush timed out"
            )
            let countAfterClearFlush = try reopened.count()
            expect(
                countAfterClearFlush == 0,
                "flush returned before durable clear"
            )

            let imageRecord = ClipboardHistoryImportRecord(
                kind: .image,
                displayText: "Image 8 × 6",
                searchText: "image fixture",
                canonicalText: nil,
                textCompleteness: .unavailable,
                capturedAt: Date(timeIntervalSince1970: 1_700_000_200),
                sourceApplicationName: "Fixture",
                sourceApplicationBundleIdentifier: "fixture.app",
                sourceNamespace: "rimes.clipboard.smoke",
                sourceID: "image-record",
                opaquePayload: try imageArchive.encodeRawDeflate()
            )
            let imageFileRecord = ClipboardHistoryImportRecord(
                kind: .files,
                displayText: "rimes-image.png",
                searchText: "image file fixture",
                canonicalText: "file:///tmp/rimes-image.png",
                textCompleteness: .complete,
                capturedAt: Date(timeIntervalSince1970: 1_700_000_201),
                sourceApplicationName: "Fixture",
                sourceApplicationBundleIdentifier: "fixture.app",
                sourceNamespace: "rimes.clipboard.smoke",
                sourceID: "image-file-record",
                opaquePayload: try imageFileArchive.encodeRawDeflate()
            )
            _ = try reopened.importBatch([imageRecord, imageFileRecord])
            let imageReopenedStore = try ClipboardHistoryStore(
                rootDirectory: root
            )
            let reopenedImageCount = try imageReopenedStore.count()
            expect(
                reopenedImageCount == 2,
                "image fixtures did not survive store reopen"
            )
            let imageModel = ClipboardHistoryModel(
                configuration: .init(),
                pasteboard: ClipboardHistoryPasteboardDouble(),
                store: imageReopenedStore,
                schedulesAutomaticPolling: false
            )
            imageModel.start()
            imageModel.update(
                windowVisible: true,
                captureEnabled: true,
                protection: []
            )
            let metadataDeadline = Date(timeIntervalSinceNow: 2)
            while imageModel.itemCount < 2, Date() < metadataDeadline {
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            expect(imageModel.itemCount == 2, "reopened image metadata load")
            var asynchronouslyLoadedThumbnail: CGImage?
            var asynchronouslyLoadedFileThumbnail: CGImage?
            imageModel.loadImageThumbnail(
                id: imageRecord.id,
                maximumPixelSize: 4
            ) { asynchronouslyLoadedThumbnail = $0 }
            imageModel.loadImageThumbnail(
                id: imageFileRecord.id,
                maximumPixelSize: 4
            ) { asynchronouslyLoadedFileThumbnail = $0 }
            let thumbnailDeadline = Date(timeIntervalSinceNow: 2)
            while (asynchronouslyLoadedThumbnail == nil
                    || asynchronouslyLoadedFileThumbnail == nil),
                  Date() < thumbnailDeadline {
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            expect(
                max(
                    asynchronouslyLoadedThumbnail?.width ?? 0,
                    asynchronouslyLoadedThumbnail?.height ?? 0
                ) <= 4 && asynchronouslyLoadedThumbnail != nil,
                "reopened image thumbnail path"
            )
            expect(
                max(
                    asynchronouslyLoadedFileThumbnail?.width ?? 0,
                    asynchronouslyLoadedFileThumbnail?.height ?? 0
                ) <= 4 && asynchronouslyLoadedFileThumbnail != nil,
                "image-bearing file thumbnail path"
            )

            let imagePane = ClipboardHistoryPaneView(model: imageModel)
            imagePane.frame = NSRect(
                x: 0,
                y: 0,
                width: ClipboardHistoryWindowMetrics.preferredWidth,
                height: ClipboardHistoryWindowMetrics.preferredHeight
            )
            imagePane.layoutSubtreeIfNeeded()
            let renderedThumbnailDeadline = Date(timeIntervalSinceNow: 2)
            while imagePane.snapshotForSmoke().renderedThumbnailCount < 2,
                  Date() < renderedThumbnailDeadline {
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            expect(
                imagePane.snapshotForSmoke().renderedThumbnailCount == 2,
                "visible cards did not render image thumbnails"
            )
            var asynchronouslyLoadedIcon: Data?
            imageModel.loadSourceApplicationIcon(
                bundleIdentifier: "fixture.app"
            ) { asynchronouslyLoadedIcon = $0 }
            let iconDeadline = Date(timeIntervalSinceNow: 2)
            while asynchronouslyLoadedIcon == nil, Date() < iconDeadline {
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.01))
            }
            expect(
                asynchronouslyLoadedIcon == fixturePNG,
                "persisted source icon lookup"
            )
            imageModel.clear()
            expect(
                imageModel.flushPersistence(timeout: 5),
                "image fixture clear flush"
            )

            let directoryMode = try FileManager.default.attributesOfItem(
                atPath: root.path
            )[.posixPermissions] as? NSNumber
            let databaseMode = try FileManager.default.attributesOfItem(
                atPath: reopened.databaseURL.path
            )[.posixPermissions] as? NSNumber
            expect(directoryMode?.intValue == 0o700, "store directory permissions")
            expect(databaseMode?.intValue == 0o600, "store database permissions")
        } catch {
            expect(false, "archive/store smoke threw: \(error.localizedDescription)")
        }
    }

    private static func runViewChecks(
        expect: (_ condition: @autoclosure () -> Bool, _ message: String) -> Void
    ) {
        let model = ClipboardHistoryModel(
            configuration: .init(),
            pasteboard: ClipboardHistoryPasteboardDouble(),
            schedulesAutomaticPolling: false
        )
        model.start()
        model.update(windowVisible: true, captureEnabled: true, protection: [])
        _ = model.ingest("alpha one")
        _ = model.ingest("beta two")

        let pane = ClipboardHistoryPaneView(model: model)
        pane.frame = NSRect(
            x: 0,
            y: 0,
            width: ClipboardHistoryWindowMetrics.preferredWidth,
            height: ClipboardHistoryWindowMetrics.preferredHeight
        )
        pane.layoutSubtreeIfNeeded()
        let snapshot = pane.snapshotForSmoke()
        expect(snapshot.cardCount == 2, "timeline card count")
        expect(snapshot.cardWidth == 206, "timeline card width")
        expect(snapshot.cardHeight == 126, "timeline card height")
        expect(snapshot.selectedCardCount == 1, "timeline single selection")
        expect(snapshot.selectedCardBorderWidth == 2, "selected timeline border")
        pane.setStandaloneSearchEnabled(true)
        expect(
            pane.standaloneSearchEnabled,
            "standalone native search did not enable"
        )
        pane.setStandaloneSearchTextForSmoke("beta")
        expect(
            pane.query == "beta"
                && pane.snapshotForSmoke().queryCharacterCount == 4,
            "standalone native search did not update the model query"
        )
        pane.resetSearch()
        pane.setStandaloneSearchEnabled(false)
        expect(
            !pane.standaloneSearchEnabled,
            "borrowed RIMES search did not restore"
        )

        guard let searchA = keyEvent(keyCode: 0, characters: "a") else {
            expect(false, "search event creation")
            return
        }
        expect(!pane.handleKeyDown(searchA), "printable search bypassed Rime routing")
        expect(pane.appendSearchText("a"), "Rime search commit was rejected")
        expect(pane.query == "a", "logical search query mismatch")
        expect(pane.snapshotForSmoke().queryCharacterCount == 1, "search snapshot mismatch")

        guard let delete = keyEvent(keyCode: 51) else {
            expect(false, "delete event creation")
            return
        }
        expect(pane.handleKeyDown(delete), "query delete was not consumed")
        expect(pane.query.isEmpty, "query delete removed an item instead")
        expect(model.itemCount == 2, "query delete mutated history")

        pane.updateComposingText("中")
        expect(pane.composingText == "中", "Rime search preedit projection mismatch")
        pane.updateComposingText("")

        var activatedID: UUID?
        pane.onActivate = { items in
            activatedID = items.first?.id
            return model.promote(ids: items.map(\.id))
        }
        guard let left = keyEvent(keyCode: 123),
              let enter = keyEvent(keyCode: UInt16(kVK_Return)),
              let keypadEnter = keyEvent(keyCode: UInt16(kVK_ANSI_KeypadEnter)) else {
            expect(false, "navigation event creation")
            return
        }
        expect(
            !ClipboardHistoryWindowController.shouldRouteSearchCompositionEventToRime(
                enter,
                compositionActive: true
            ),
            "active search composition retained plain Return"
        )
        expect(
            !ClipboardHistoryWindowController.shouldRouteSearchCompositionEventToRime(
                keypadEnter,
                compositionActive: true
            ),
            "active search composition retained keypad Enter"
        )
        expect(
            ClipboardHistoryWindowController.shouldRouteSearchCompositionEventToRime(
                left,
                compositionActive: true
            ),
            "active search composition lost editing arrow"
        )
        expect(
            !ClipboardHistoryWindowController.shouldRouteSearchCompositionEventToRime(
                left,
                compositionActive: false
            ),
            "inactive search composition claimed editing arrow"
        )
        expect(pane.handleKeyDown(left), "left navigation not consumed")
        let expectedID = model.selectedID
        expect(pane.handleKeyDown(enter), "activation not consumed")
        expect(activatedID == expectedID, "activation returned wrong item")
        expect(model.items.first?.id == expectedID, "activation did not promote")

        var rejectedActivationCount = 0
        pane.onActivate = { _ in
            rejectedActivationCount += 1
            return false
        }
        expect(
            pane.handleKeyDown(enter),
            "failed activation leaked Return to the host"
        )
        expect(
            pane.handleKeyDown(keypadEnter),
            "failed activation leaked keypad Enter to the host"
        )
        expect(rejectedActivationCount == 2, "failed activation was not attempted")

        var copiedID: UUID?
        pane.onCopy = { items in
            copiedID = items.first?.id
            return true
        }
        guard let commandCopy = keyEvent(
            keyCode: 8,
            characters: "c",
            modifiers: [.command]
        ), let capsLockCommandCopy = keyEvent(
            keyCode: 8,
            characters: "c",
            modifiers: [.command, .capsLock]
        ), let commandOne = keyEvent(
            keyCode: 18,
            characters: "1",
            modifiers: [.command]
        ), let commandRight = keyEvent(
            keyCode: 124,
            modifiers: [.command]
        ) else {
            expect(false, "command event creation")
            return
        }
        pane.setStandaloneSearchEnabled(true)
        copiedID = nil
        let activationCountBeforeMarkedCommands = rejectedActivationCount
        expect(
            !pane.handleStandaloneKeyEquivalent(
                commandCopy,
                hasMarkedText: true
            ),
            "marked standalone Command-C was consumed"
        )
        expect(copiedID == nil, "marked standalone Command-C copied a card")
        expect(
            !pane.handleStandaloneKeyEquivalent(
                commandOne,
                hasMarkedText: true
            ),
            "marked standalone Command-1 was consumed"
        )
        expect(
            rejectedActivationCount == activationCountBeforeMarkedCommands,
            "marked standalone Command-1 activated a card"
        )
        expect(
            pane.handleStandaloneKeyEquivalent(
                commandCopy,
                hasMarkedText: false
            ),
            "settled standalone Command-C was not consumed"
        )
        expect(copiedID == model.selectedID, "Command-C copied wrong item")
        pane.setStandaloneSearchEnabled(false)
        expect(pane.handleKeyDown(capsLockCommandCopy), "Caps-Lock Command-C was not consumed")
        expect(
            ClipboardHistoryWindowController.hardwareKeyCodes(
                for: NSSelectorFromString("copy:")
            ) == [UInt16(kVK_ANSI_C)],
            "Command-C callback ownership mapping"
        )
        let upFunctionCharacter = String(
            UnicodeScalar(NSUpArrowFunctionKey)!
        )
        guard let upFunction = keyEvent(
            keyCode: UInt16(kVK_UpArrow),
            characters: upFunctionCharacter,
            modifiers: [.function]
        ) else {
            expect(false, "function-key event creation")
            return
        }
        expect(
            !ClipboardHistoryWindowController.isPlainSearchInputEvent(upFunction),
            "Cocoa function character was accepted as search text"
        )
        expect(
            ClipboardHistoryWindowController.isRimeCompositionEditingEvent(upFunction),
            "active Rime composition did not retain Up-arrow navigation"
        )
        expect(
            !ClipboardHistoryWindowController.isLiteralSearchText(upFunctionCharacter),
            "private-use function scalar was accepted as search text"
        )
        expect(
            ClipboardHistoryWindowController.hardwareKeyCodes(
                for: NSSelectorFromString("moveUp:")
            ) == [UInt16(kVK_UpArrow)],
            "Up-arrow callback ownership mapping"
        )
        expect(
            ClipboardHistoryWindowController.hardwareKeyCodes(
                for: NSSelectorFromString("insertTab:")
            ) == [UInt16(kVK_Tab)],
            "Tab callback ownership mapping"
        )
        for selectorName in [
            "insertNewline:", "insertLineBreak:",
            "insertNewlineIgnoringFieldEditor:",
            "insertParagraphSeparator:",
        ] {
            expect(
                ClipboardHistoryWindowController.hardwareKeyCodes(
                    for: NSSelectorFromString(selectorName)
                ) == [UInt16(kVK_Return), UInt16(kVK_ANSI_KeypadEnter)],
                "Return callback ownership mapping \(selectorName)"
            )
        }
        expect(pane.handleKeyDown(commandOne), "Command-1 was not consumed")
        expect(!pane.handleKeyDown(commandRight), "modified arrow was consumed")

        let multiIDs = model.items.map(\.id)
        guard multiIDs.count == 2,
              let shiftRight = keyEvent(
                keyCode: UInt16(kVK_RightArrow),
                modifiers: [.shift]
              ) else {
            expect(false, "multi-selection fixture")
            return
        }
        _ = model.select(id: multiIDs[0])
        expect(
            pane.handleKeyDown(shiftRight),
            "Shift-Right multi-selection was not consumed"
        )
        expect(model.selectedIDs == Set(multiIDs), "Shift range selection")
        expect(
            pane.snapshotForSmoke().selectedCardCount == 2,
            "multi-selection rendering"
        )

        guard let doubleClickEvent = NSEvent.mouseEvent(
            with: .leftMouseDown,
            location: .zero,
            modifierFlags: [.command],
            timestamp: 0,
            windowNumber: 0,
            context: nil,
            eventNumber: 0,
            clickCount: 2,
            pressure: 1
        ) else {
            expect(false, "double-click event creation")
            return
        }
        let doubleClickContext = ClipboardHistoryCardActionContext(
            event: doubleClickEvent
        )
        expect(doubleClickContext.clickCount == 2, "button double-click capture")
        expect(
            doubleClickContext.modifiers.contains(.command),
            "button modifier capture"
        )
        expect(
            ClipboardHistoryCardActionContext(
                modifiers: [],
                clickCount: 0
            ).clickCount == 1,
            "button click-count normalization"
        )

        var plainClickActivationCount = 0
        pane.onActivate = { _ in
            plainClickActivationCount += 1
            return true
        }
        _ = model.select(id: multiIDs[0])
        expect(
            pane.handleCardInteraction(
                itemID: multiIDs[1],
                modifiers: [],
                clickCount: 1
            ),
            "plain single-click selection"
        )
        expect(
            model.selectedID == multiIDs[1]
                && model.selectedIDs == Set([multiIDs[1]]),
            "plain single-click selected the wrong item"
        )
        expect(
            plainClickActivationCount == 0,
            "plain single-click activated an item"
        )

        // A click selects where the pointer already is, so the rail must not
        // scroll underneath it — otherwise the second click of a double click
        // lands on a different card. Keyboard selection still scrolls, because
        // nothing is under the pointer to keep still.
        do {
            let scrollModel = ClipboardHistoryModel(
                configuration: .init(),
                pasteboard: ClipboardHistoryPasteboardDouble(),
                schedulesAutomaticPolling: false
            )
            scrollModel.start()
            scrollModel.update(windowVisible: true, captureEnabled: true,
                               protection: [])
            for index in 0..<12 { _ = scrollModel.ingest("overflow card \(index)") }
            let scrollPane = ClipboardHistoryPaneView(model: scrollModel)
            scrollPane.frame = NSRect(
                x: 0,
                y: 0,
                width: ClipboardHistoryWindowMetrics.preferredWidth,
                height: ClipboardHistoryWindowMetrics.preferredHeight
            )
            scrollPane.layoutSubtreeIfNeeded()
            let overflowIDs = scrollModel.items.map(\.id)
            guard overflowIDs.count == 12, let lastID = overflowIDs.last else {
                expect(false, "click-scroll fixture")
                return
            }
            _ = scrollModel.select(id: overflowIDs[0])
            scrollPane.layoutSubtreeIfNeeded()
            let restingOrigin = scrollPane.scrollOriginXForSmoke
            _ = scrollPane.handleCardInteraction(
                itemID: lastID,
                modifiers: [],
                clickCount: 1
            )
            scrollPane.layoutSubtreeIfNeeded()
            expect(
                abs(scrollPane.scrollOriginXForSmoke - restingOrigin) < 0.5,
                "a click scrolled the rail out from under the pointer"
            )
            expect(
                scrollModel.selectedID == lastID,
                "click on an off-screen card did not select it"
            )
            _ = scrollModel.select(id: overflowIDs[0])
            scrollPane.layoutSubtreeIfNeeded()
            _ = scrollModel.select(id: lastID)
            scrollPane.layoutSubtreeIfNeeded()
            expect(
                scrollPane.scrollOriginXForSmoke > restingOrigin + 0.5,
                "keyboard selection stopped scrolling into view"
            )
        }

        _ = model.select(id: multiIDs[0])
        expect(
            pane.handleCardInteraction(
                itemID: multiIDs[1],
                modifiers: [.command],
                clickCount: 1
            ),
            "Command-click multi-selection"
        )
        expect(model.selectedIDs == Set(multiIDs), "Command-click selection set")
        var doubleClickedIDs: [UUID] = []
        pane.onActivate = { items in
            doubleClickedIDs = items.map(\.id)
            return true
        }
        expect(
            pane.handleCardInteraction(
                itemID: multiIDs[1],
                modifiers: [.command],
                clickCount: 2
            ),
            "double-click activation"
        )
        expect(doubleClickedIDs == [multiIDs[1]], "double-click was not single-item")
        _ = model.select(ids: multiIDs, focusedID: multiIDs[1])
        var activatedIDs: [UUID] = []
        pane.onActivate = { items in
            activatedIDs = items.map(\.id)
            return true
        }
        expect(pane.handleKeyDown(enter), "multi-selection Enter")
        expect(activatedIDs == multiIDs, "multi-selection activation order")
        expect(pane.handleKeyDown(delete), "multi-selection Delete")
        expect(model.itemCount == 0, "multi-selection delete retained items")

        var closeCount = 0
        pane.onClose = { closeCount += 1 }
        _ = pane.appendSearchText("a")
        guard let escape = keyEvent(keyCode: 53) else {
            expect(false, "escape event creation")
            return
        }
        expect(pane.handleKeyDown(escape), "query escape was not consumed")
        expect(pane.query.isEmpty && closeCount == 0, "first escape did not clear query")
        expect(pane.handleKeyDown(escape), "close escape was not consumed")
        expect(closeCount == 1, "second escape did not close")

        model.update(
            windowVisible: true,
            captureEnabled: true,
            protection: [.secureInput]
        )
        let protected = pane.snapshotForSmoke()
        expect(protected.contentIsProtected, "protected view flag missing")
        expect(protected.cardCount == 0, "protected view retained cards")
        expect(protected.stateIsVisible, "protected view missing state")
        expect(!pane.activateSelectedItems(), "protected view activated item")
    }

    private static func keyEvent(
        keyCode: UInt16,
        characters: String = "",
        modifiers: NSEvent.ModifierFlags = []
    ) -> NSEvent? {
        NSEvent.keyEvent(
            with: .keyDown,
            location: .zero,
            modifierFlags: modifiers,
            timestamp: 0,
            windowNumber: 0,
            context: nil,
            characters: characters,
            charactersIgnoringModifiers: characters,
            isARepeat: false,
            keyCode: keyCode
        )
    }

    private static func makeFixturePNG() -> Data? {
        guard let bitmap = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: 8,
            pixelsHigh: 6,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0
        ) else { return nil }
        for x in 0..<8 {
            for y in 0..<6 {
                bitmap.setColor(
                    (x + y).isMultiple(of: 2)
                        ? NSColor(deviceRed: 0.1, green: 0.8, blue: 0.3, alpha: 1)
                        : NSColor(deviceRed: 0.95, green: 0.8, blue: 0.1, alpha: 1),
                    atX: x,
                    y: y
                )
            }
        }
        return bitmap.representation(using: .png, properties: [:])
    }
}

private final class ClipboardHistoryPasteboardDouble: ClipboardHistoryPasteboardReading {
    var stubChangeCount = 0
    var stubPlainText: String?
    var stubArchive: ClipboardPasteboardArchive?
    var usesAsynchronousArchive = false
    private(set) var changeCountReadCount = 0
    private(set) var plainTextReadCount = 0
    private var pendingArchiveCompletion:
        ((Result<ClipboardPasteboardArchive?, Error>) -> Void)?

    var usesTextOnlyCompatibilityArchive: Bool {
        !usesAsynchronousArchive
    }

    var changeCount: Int {
        changeCountReadCount += 1
        return stubChangeCount
    }

    func readPlainText() -> String? {
        plainTextReadCount += 1
        return stubPlainText
    }

    func readArchiveAsynchronously(
        expectedChangeCount: Int,
        completion: @escaping (Result<ClipboardPasteboardArchive?, Error>) -> Void
    ) {
        guard usesAsynchronousArchive,
              expectedChangeCount == stubChangeCount else {
            completion(.success(nil))
            return
        }
        pendingArchiveCompletion = completion
    }

    func completeAsynchronousRead() {
        let completion = pendingArchiveCompletion
        pendingArchiveCompletion = nil
        completion?(.success(stubArchive))
    }
}

/// Activation policy and its feedback. A silent fallback to the pasteboard is
/// what made this feel unreliable next to a dedicated paste utility: the
/// gesture looked like it had failed when the content was ready and only the
/// synthetic key press was blocked.
func runClipboardActivationPolicySmokeTest() -> Bool {
    print("== RIMES clipboard activation policy smoke ==")
    let suite = "clipboard-policy-smoke-\(UUID().uuidString)"
    guard let defaults = UserDefaults(suiteName: suite) else {
        print("FAILED: could not create a defaults suite")
        return false
    }
    defer { defaults.removeSuite(named: suite) }

    guard ClipboardActivationPolicy(rawValue: "pasteIntoApp") == .pasteIntoApp,
          ClipboardActivationPolicy(rawValue: "clipboardOnly") == .clipboardOnly,
          ClipboardActivationPolicy(rawValue: "nonsense") == nil,
          ClipboardActivationPolicy.allCases.count == 2 else {
        print("FAILED: activation policy cases")
        return false
    }

    // Every outcome says what happened. A blocked paste must never be
    // reported the same way as a successful one, and must name its cause:
    // the grant is the one thing the user can act on.
    let outcomes: [ClipboardAutoPasteOutcome] = [
        .pasted, .clipboardOnlyByChoice, .blockedWithoutAccessibility,
        .blockedBySecureInput, .targetUnavailable,
    ]
    let messages = outcomes.map(ClipboardActivationFeedback.message)
    guard Set(messages).count == messages.count,
          messages.allSatisfy({ !$0.isEmpty }) else {
        print("FAILED: every outcome needs its own message")
        return false
    }
    guard ClipboardActivationFeedback.message(for: .pasted) == "已粘贴到目标应用",
          ClipboardActivationFeedback.message(for: .clipboardOnlyByChoice)
            == "已复制到剪贴板",
          ClipboardActivationFeedback.message(for: .blockedWithoutAccessibility)
            .contains("辅助功能") else {
        print("FAILED: outcome wording")
        return false
    }
    // A paste that did not happen must not claim it did.
    for outcome in outcomes where outcome != .pasted {
        guard !ClipboardActivationFeedback.message(for: outcome)
            .contains("已粘贴") else {
            print("FAILED: \(outcome) reported itself as a paste")
            return false
        }
    }

    // Everything the capture module requests has to be listed here, or a
    // refusal shows up as a broken feature with no row explaining it.
    let captureGrants: [SystemPermission] = [
        .screenRecording, .inputMonitoring, .camera, .microphone,
    ]
    guard captureGrants.allSatisfy(SystemPermission.allCases.contains) else {
        return clipboardPermissionFail("capture permissions missing from the panel")
    }

    // The permission inventory. Anything listed must name a real feature and
    // a real consequence, and must reach a settings pane — a row that cannot
    // be acted on is worse than no row.
    // Six: accessibility and local network, plus the four screen-capture
    // grants. The count is pinned so adding a permission is a decision
    // rather than a side effect — a new row has to earn its wording here.
    guard SystemPermission.allCases.count == 6,
          SystemPermission.allCases.allSatisfy({
              !$0.title.isEmpty && !$0.enables.isEmpty
                  && !$0.whenMissing.isEmpty && $0.settingsURL != nil
          }) else {
        return clipboardPermissionFail("permission inventory completeness")
    }
    // Listing a permission nothing requests teaches the user to grant things
    // blindly, so the inventory is pinned to an explicit set. Input
    // Monitoring used to be banned outright here because the global monitors
    // watched mouse buttons only; the capture recorder's keystroke overlay
    // calls CGRequestListenEventAccess, so the rule moved from "never
    // listed" to "listed only with a caller" — which is what the scan below
    // checks.
    let expectedInventory: Set<SystemPermission> = [
        .accessibility, .screenRecording, .inputMonitoring,
        .camera, .microphone, .localNetwork,
    ]
    guard Set(SystemPermission.allCases) == expectedInventory else {
        return clipboardPermissionFail("unused permissions must not be listed")
    }
    // Actual optional-input routing and asynchronous permission lifetimes
    // are exercised by capture-permission-smoke, not source-text matching of
    // the old synchronous ensure() call (which raced system dialogs).

    let reports = SystemPermissionAudit.reportAll()
    guard reports.count == SystemPermission.allCases.count,
          reports.allSatisfy({ !$0.actionTitle.isEmpty }) else {
        return clipboardPermissionFail("every permission needs a live report")
    }
    // Local network has no read API; claiming to know its state would be a
    // lie, and a lie here sends the user to check the wrong thing.
    guard SystemPermissionAudit.status(for: .localNetwork) == .undeterminable else {
        return clipboardPermissionFail("local network cannot be queried")
    }
    // A denied grant that will not prompt must offer the pane, not a request
    // that produces no dialog — the failure the user actually hit.
    let silentDenied = SystemPermissionReport(permission: .accessibility,
                                              status: .denied,
                                              promptWouldBeSilent: true)
    let promptableDenied = SystemPermissionReport(permission: .accessibility,
                                                  status: .denied,
                                                  promptWouldBeSilent: false)
    guard silentDenied.actionTitle == "前往系统设置",
          promptableDenied.actionTitle == "请求权限" else {
        return clipboardPermissionFail("denied grants must offer the right action")
    }

    // Identity. The grant is recorded against the identifier, and this bundle
    // carries three different names — folder/executable ETInput, display
    // RIMES, identifier RimeBuffer — so the mismatch must be detectable
    // rather than left for the user to notice in a system list.
    let agreeing = SystemPermissionAudit.Identity(
        bundleIdentifier: "com.example.Widget",
        bundleName: "Widget",
        executableName: "Widget",
        bundlePath: "/Applications/Widget.app",
        isAdHocSigned: false
    )
    let mismatched = SystemPermissionAudit.Identity(
        bundleIdentifier: RimesIdentity.bundleIdentifier,
        bundleName: "RIMES",
        executableName: "ETInput",
        bundlePath: "/Users/x/Library/Input Methods/ETInput.app",
        isAdHocSigned: true
    )
    guard agreeing.namesAgree, !mismatched.namesAgree else {
        return clipboardPermissionFail("identity name agreement")
    }
    // Reading the live identity must never produce an empty field, including
    // from this bare test executable, which has no bundle at all — a blank
    // line in the settings page would be worse than an honest "unknown".
    let live = SystemPermissionAudit.identity()
    guard !live.bundleIdentifier.isEmpty,
          !live.bundleName.isEmpty,
          !live.executableName.isEmpty,
          !live.bundlePath.isEmpty else {
        return clipboardPermissionFail("live identity must have no blank fields")
    }
    // Resetting a record needs a real identifier; an empty one must not
    // shell out at all.
    guard !SystemPermissionAudit.resetAccessibilityRecord(
        bundleIdentifier: ""
    ) else {
        return clipboardPermissionFail("empty identifier must not be reset")
    }

    // Signing decides whether a grant outlives a rebuild: an ad-hoc
    // requirement is a cdhash, which changes every build, while an identity
    // requirement names a certificate and does not. The two readings must
    // never both be true, or the page would claim both at once.
    let adHoc = SystemPermissionAudit.isAdHocSigned()
    let authority = SystemPermissionAudit.signingAuthority()
    guard !(adHoc && authority != nil) else {
        return clipboardPermissionFail(
            "a build cannot be both ad-hoc and identity-signed"
        )
    }
    if let authority {
        guard !authority.isEmpty else {
            return clipboardPermissionFail("a signing authority must be named")
        }
    }

    print("clipboard activation policy smoke: OK")
    return true
}

private func clipboardPermissionFail(_ message: String) -> Bool {
    print("FAILED: \(message)")
    return false
}
