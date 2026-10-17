import Cocoa
import UniformTypeIdentifiers

/// Chat and Inbox share persistence and reading primitives, while keeping their
/// own selection, filters and composer semantics. Inbox never sends a reply.
final class MailboxMessagesWorkspace: NSViewController, NSSearchFieldDelegate {
    let module: MailboxModuleID
    private let store: MailboxStore
    private let bridge = MailboxInteractionBridge.shared
    // Hydrate inbound review routing before this view installs notification observers.
    // Its first initialization may synchronously publish recovered associations.
    private let reviewAdapter = MailboxBufferReviewAdapter.shared
    private var snapshot: MailboxStoreSnapshot
    private var observation: MailboxStoreObservation?
    private var associationObserver: NSObjectProtocol?
    private(set) var selectedID: UUID?
    private var creating = false
    private var submitting = false
    private var filter: MailboxInboxFilter = .all
    private let index = MailboxFlowView()
    private let transcript = MailboxFlowView()
    private let search = NSSearchField()
    private let filterControl = NSSegmentedControl(labels: ["全部", "未读", "待处理", "归档"], trackingMode: .selectOne, target: nil, action: nil)
    private let heading = MailboxUI.label("", size: 16, weight: .semibold)
    private let source = MailboxUI.label("", size: 11, secondary: true)
    private let toolbar = NSStackView()
    private let composer = MailboxComposerTextView()
    private let composerHint = MailboxUI.label("", size: 11, secondary: true)
    private let feedback = MailboxFeedbackLabel()
    private let modelPicker = NSPopUpButton()
    private var modelOptions: [MailboxNewConversationModelOption] = []
    private lazy var send = MailboxActionButton("发送", symbol: "arrow.up") { [weak self] in self?.submit() }
    private var transcriptScroll: NSScrollView!
    private var liveBody: MailboxMessageTextView?
    private var renderedThread: MailboxThread?
    private var split: MailboxAdaptiveSplitView!
    private lazy var toggleList = MailboxActionButton("", symbol: "sidebar.left") { [weak self] in self?.split.toggleSidebar() }
    var returnToTerminal: ((UUID) -> Bool)?

    init(module: MailboxModuleID, store: MailboxStore) {
        self.module = module; self.store = store; snapshot = store.snapshot
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
    deinit { if let associationObserver { NotificationCenter.default.removeObserver(associationObserver) } }

    override func loadView() {
        view = MailboxSurface(.textBackgroundColor)
        let listTitle = MailboxUI.label(module == .chat ? "对话" : "收件箱", size: 19, weight: .semibold)
        let titleRow = MailboxUI.stack([listTitle, MailboxUI.spacer()], vertical: false)
        if module == .chat {
            let new = MailboxActionButton("", symbol: "square.and.pencil") { [weak self] in self?.newConversation() }
            new.toolTip = "新对话"; titleRow.addArrangedSubview(new)
        }
        let closeList = MailboxActionButton("", symbol: "chevron.left") { [weak self] in self?.split.toggleSidebar() }
        closeList.isBordered = false; closeList.toolTip = "收起列表"
        closeList.setAccessibilityLabel("收起列表")
        titleRow.addArrangedSubview(closeList)
        search.placeholderString = module == .chat ? "搜索对话" : "搜索收件"
        search.delegate = self; search.font = .systemFont(ofSize: 12)
        filterControl.selectedSegment = 0; filterControl.segmentStyle = .rounded
        filterControl.target = self; filterControl.action = #selector(filterChanged)
        filterControl.font = .systemFont(ofSize: 10)
        filterControl.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        for segment in 0..<filterControl.segmentCount { filterControl.setWidth(0, forSegment: segment) }
        let listHeader = MailboxUI.stack([titleRow, search, filterControl], spacing: 12)
        let list = MailboxUI.stack([MailboxUI.padded(listHeader, inset: 16), MailboxUI.scroll(index)], spacing: 0)
        let sidebar = MailboxSurface(MailboxUI.sidebar)
        MailboxUI.pin(list, to: sidebar)
        toolbar.orientation = .horizontal; toolbar.alignment = .centerY; toolbar.spacing = 4
        toggleList.toolTip = "显示或隐藏列表"; toggleList.setAccessibilityLabel("显示或隐藏列表")
        toggleList.isBordered = false
        let titles = MailboxUI.stack([heading, source], spacing: 4)
        titles.setContentHuggingPriority(.defaultLow, for: .horizontal)
        titles.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        titles.widthAnchor.constraint(lessThanOrEqualToConstant: 520).isActive = true
        let top = MailboxUI.stack([titles, MailboxUI.spacer(), toolbar], vertical: false, spacing: 12)
        let topPanel = MailboxUI.padded(top, inset: MailboxUI.contentInset)
        transcriptScroll = MailboxUI.scroll(transcript)

        composer.isRichText = false; composer.font = .systemFont(ofSize: 14)
        composer.drawsBackground = false; composer.isAutomaticQuoteSubstitutionEnabled = false
        composer.textContainerInset = NSSize(width: 0, height: 6)
        composer.textContainer?.lineFragmentPadding = 0
        composer.isVerticallyResizable = true; composer.isHorizontallyResizable = false
        composer.autoresizingMask = [.width]; composer.textContainer?.widthTracksTextView = true
        composer.minSize = .zero; composer.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        composer.submit = { [weak self] in self?.submit() }
        composer.placeholder = module == .inbox ? "写下本地备注…" : "输入消息…"
        composer.setAccessibilityLabel(module == .inbox ? "本地备注" : "消息输入框")
        composer.changed = { [weak self] text in self?.saveDraft(text); self?.updateComposerState() }
        let inputScroll = NSScrollView()
        inputScroll.documentView = composer; inputScroll.hasVerticalScroller = true
        inputScroll.drawsBackground = false
        inputScroll.translatesAutoresizingMaskIntoConstraints = false
        inputScroll.heightAnchor.constraint(equalToConstant: 60).isActive = true
        modelPicker.font = .systemFont(ofSize: 11)
        modelPicker.target = self; modelPicker.action = #selector(modelChanged)
        modelPicker.translatesAutoresizingMaskIntoConstraints = false
        modelPicker.controlSize = .small
        modelPicker.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        modelPicker.cell?.lineBreakMode = .byTruncatingMiddle
        modelPicker.widthAnchor.constraint(lessThanOrEqualToConstant: 280).isActive = true
        composerHint.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        send.emphasized = true
        let composerActions = MailboxUI.stack([modelPicker, composerHint, MailboxUI.spacer(), send], vertical: false)
        let input = MailboxUI.padded(MailboxUI.stack([inputScroll, composerActions], spacing: 5), inset: 10, color: MailboxUI.sidebar)
        input.wantsLayer = true; input.layer?.cornerRadius = 12
        let footer = MailboxUI.padded(MailboxUI.stack([input, feedback], spacing: 7), inset: MailboxUI.contentInset)
        let main = MailboxUI.stack([topPanel, MailboxUI.line(), transcriptScroll, footer], spacing: 0)
        split = MailboxAdaptiveSplitView(sidebar: sidebar, main: main, sidebarWidth: module == .chat ? MailboxUI.chatListWidth : MailboxUI.inboxListWidth)
        split.visibilityChanged = { [weak self] visible, _ in
            self?.toggleList.contentTintColor = visible ? MailboxUI.accent : .secondaryLabelColor
        }
        MailboxUI.pin(split, to: view)
        loadModelOptions()
        observation = store.observe(deliverInitial: false) { [weak self] event in self?.apply(event) }
        associationObserver = NotificationCenter.default.addObserver(forName: .mailboxInboundReviewAssociationDidChange,
            object: nil, queue: .main) { [weak self] _ in self?.renderedThread = nil; self?.reloadFromStore() }
        reloadFromStore()
    }

    private func visibleThreads() -> [MailboxThread] {
        let searchable = PluginRegistry.shared.allowsHostModuleAction(.search, for: module.pluginKey)
        search.isEnabled = searchable
        let filters = module.filters
        for item in MailboxInboxFilter.allCases {
            filterControl.setEnabled(filters.contains(item), forSegment: item.rawValue)
        }
        if !filters.contains(filter) {
            filter = filters.first ?? .all
            filterControl.selectedSegment = filter.rawValue
        }
        return MailboxWorkspaceRules.threads(snapshot.threads, module: module,
            query: searchable ? search.stringValue : "", filter: filter)
    }
    func reloadFromStore() {
        guard isViewLoaded else { return }
        apply(MailboxStoreEvent(change: .contentChanged, snapshot: store.snapshot))
    }
    private func apply(_ event: MailboxStoreEvent) {
        guard event.snapshot.revision >= snapshot.revision else { return }
        let oldThreads = snapshot.threads
        snapshot = event.snapshot
        if case .generationProgressed = event.change, oldThreads == snapshot.threads {
            updateLiveBody(); return
        }
        if let selectedID, snapshot.thread(id: selectedID) == nil { self.selectedID = nil; renderedThread = nil }
        if selectedID == nil && !creating {
            selectedID = visibleThreads().first?.id
            restoreDraft()
        }
        renderIndex()
        let thread = selectedID.flatMap { snapshot.thread(id: $0) }
        if renderedThread != thread || transcript.arrangedSubviews.isEmpty {
            renderedThread = thread
            renderDetail(thread)
        }
        updateComposerState()
        if case let .unavailable(reason) = snapshot.persistence { feedback.stringValue = reason }
        markVisibleRead()
    }

    func selectThread(_ id: UUID) {
        guard !submitting, let thread = store.thread(id: id),
              thread.contentWorkspace == (module == .chat ? .chat : .inbox) else { return }
        _ = view
        creating = false; selectedID = id; renderedThread = nil
        if thread.archivedAt != nil { filter = .archived; filterControl.selectedSegment = 3 }
        else if filter == .archived { filter = .all; filterControl.selectedSegment = 0 }
        feedback.stringValue = ""; restoreDraft(); reloadFromStore()
        split.dismissOverlays()
    }
    private func newConversation() {
        guard !submitting else { return }
        creating = true; selectedID = nil; renderedThread = nil
        feedback.stringValue = ""; loadModelOptions(); restoreDraft()
        renderIndex(); renderDetail(nil); updateComposerState()
        split.dismissOverlays()
        view.window?.makeFirstResponder(composer)
    }
    func controlTextDidChange(_ obj: Notification) { renderIndex() }
    @objc private func filterChanged() {
        filter = MailboxInboxFilter(rawValue: filterControl.selectedSegment) ?? .all
        renderIndex()
    }
    private func renderIndex() {
        MailboxUI.clear(index)
        let threads = visibleThreads()
        if threads.isEmpty {
            index.addArrangedSubview(MailboxUI.padded(MailboxUI.paragraph(search.stringValue.isEmpty
                ? (module == .chat ? "在这里开始新的对话。" : "通知、任务产物和外部来信\n会汇集在这里。")
                : "没有找到匹配的内容。", size: 12), inset: 20))
        }
        for thread in threads {
            let row = MailboxListButton(title: thread.displayTitle,
                subtitle: thread.preview ?? "", metadata: "\(thread.source.displayName) · \(MailboxUI.time(thread.updatedAt)) · \(thread.workspaceStatus)",
                selected: selectedID == thread.id, unread: thread.unread) { [weak self] in self?.selectThread(thread.id) }
            index.addArrangedSubview(row)
        }
    }
    private func renderDetail(_ thread: MailboxThread?) {
        MailboxUI.clear(transcript); MailboxUI.clear(toolbar); liveBody = nil
        toolbar.addArrangedSubview(toggleList)
        guard let thread else {
            heading.stringValue = module == .chat ? "新对话" : "收件箱"
            source.stringValue = module == .chat ? "选择连接器，开始一个独立会话" : "让完成的工作，有一个落点"
            let welcome = MailboxUI.stack([
                MailboxUI.label(module == .chat ? "今天想一起做什么？" : "等待下一份收件", size: 23, weight: .medium),
                MailboxUI.paragraph(module == .chat ? "继续思考、整理文字，或开始一项新任务。\n每个会话独立保留模型、上下文和草稿。" : "从终端收取产物，接收 Buffer 生成的图片，\n或查看已连接来源推送的消息。", size: 14)
            ], spacing: 18)
            transcript.addArrangedSubview(MailboxUI.padded(welcome, inset: MailboxUI.contentInset))
            return
        }
        heading.stringValue = thread.displayTitle
        heading.toolTip = thread.displayTitle
        source.stringValue = [thread.source.displayName, thread.source.model, thread.workspaceStatus].compactMap { $0 }.joined(separator: " · ")
        source.toolTip = source.stringValue
        let rename = MailboxActionButton("", symbol: "pencil") { [weak self] in self?.rename(thread.id) }
        rename.toolTip = "重命名"; rename.setAccessibilityLabel("重命名"); rename.isBordered = false
        let archive = MailboxActionButton("", symbol: thread.archivedAt == nil ? "archivebox" : "tray.and.arrow.up") { [weak self] in
            self?.perform { try self?.store.setArchived(thread.archivedAt == nil, threadID: thread.id) }
        }
        archive.toolTip = thread.archivedAt == nil ? "归档" : "移回收件"
        archive.setAccessibilityLabel(archive.toolTip); archive.isBordered = false
        toolbar.addArrangedSubview(rename); toolbar.addArrangedSubview(archive)
        if module == .inbox, let sessionID = thread.terminalSessionID {
            let back = MailboxActionButton("", symbol: "terminal") { [weak self] in
                guard let self else { return }
                if self.returnToTerminal?(sessionID) != true { self.feedback.stringValue = "原终端已结束或插件已停用；收件内容仍已保存。" }
            }
            back.toolTip = "返回终端"; back.setAccessibilityLabel("返回终端"); back.isBordered = false
            toolbar.addArrangedSubview(back)
        }
        for message in thread.messages { appendMessage(message, thread: thread) }
        if thread.generation?.phase == .generating {
            let preview = snapshot.generationPreview(threadID: thread.id)?.message
                ?? MailboxMessage(role: .inbound, body: "正在生成…")
            liveBody = appendMessage(preview, thread: thread, streaming: true)
        }
        if let failure = thread.generation?.failureMessage {
            transcript.addArrangedSubview(MailboxUI.padded(MailboxUI.paragraph(failure), inset: MailboxUI.contentInset))
        }
        if module == .chat { scrollToEnd() }
    }

    @discardableResult private func appendMessage(_ message: MailboxMessage, thread: MailboxThread,
                                                  streaming: Bool = false) -> MailboxMessageTextView {
        let local = message.kind == .localNote
        let name = local ? "本地备注" : message.role == .user ? "你" : message.author ?? thread.source.displayName
        let identity = MailboxUI.label(name, size: 12, weight: .semibold, secondary: local)
        let timestamp = MailboxUI.label(streaming ? "生成中" : MailboxUI.time(message.createdAt), size: 10, secondary: true)
        let row = MailboxUI.stack([identity, MailboxUI.spacer(), timestamp], vertical: false)
        let body = MailboxUI.body(message)
        let block = MailboxUI.stack([row, body], spacing: 12)
        if let data = store.imageData(for: message), let image = NSImage(data: data) {
            let imageView = NSImageView()
            imageView.image = image; imageView.imageScaling = .scaleProportionallyUpOrDown
            imageView.translatesAutoresizingMaskIntoConstraints = false
            imageView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            imageView.setContentCompressionResistancePriority(.defaultLow, for: .vertical)
            imageView.heightAnchor.constraint(lessThanOrEqualToConstant: module == .inbox ? 300 : 240).isActive = true
            let aspect = imageView.heightAnchor.constraint(equalTo: imageView.widthAnchor, multiplier: image.size.height / max(1, image.size.width))
            aspect.priority = .defaultHigh; aspect.isActive = true
            block.addArrangedSubview(imageView)
        }
        if !streaming && message.role == .inbound && message.kind == .content {
            let actions = MailboxUI.stack([], vertical: false, spacing: 8)
            if message.imageFileName != nil {
                actions.addArrangedSubview(MailboxActionButton("保存图片", symbol: "square.and.arrow.down") { [weak self] in self?.saveImage(message) })
            } else {
                actions.addArrangedSubview(MailboxActionButton("复制", symbol: "doc.on.doc") {
                    NSPasteboard.general.clearContents(); NSPasteboard.general.setString(message.body, forType: .string)
                })
                let stage = MailboxActionButton("加入 Buffer", symbol: "text.badge.plus") { [weak self] in self?.stage(message, threadID: thread.id) }
                if thread.review != nil {
                    let adapter = MailboxBufferReviewAdapter.shared
                    stage.isEnabled = adapter.review(threadID: thread.id).map { adapter.canSendToBuffer(reviewID: $0.id) } ?? false
                }
                actions.addArrangedSubview(stage)
            }
            actions.addArrangedSubview(MailboxActionButton("存入 Capsule", symbol: "archivebox") { [weak self] in self?.saveToCapsule(message, title: thread.displayTitle) })
            if thread.review?.state == .pending {
                actions.addArrangedSubview(MailboxActionButton("忽略", symbol: "xmark") { [weak self] in
                    let adapter = MailboxBufferReviewAdapter.shared
                    if let review = adapter.review(threadID: thread.id), adapter.reject(reviewID: review.id) {
                        self?.feedback.stringValue = "已忽略这条外部消息"
                    } else { self?.feedback.stringValue = "这条消息当前无法处理" }
                })
            }
            actions.addArrangedSubview(MailboxUI.spacer())
            block.addArrangedSubview(actions)
        }
        let panel = MailboxUI.padded(block, inset: MailboxUI.contentInset, color: message.role == .user ? MailboxUI.sidebar : .textBackgroundColor)
        transcript.addArrangedSubview(panel)
        return body
    }
    private func updateLiveBody() {
        guard let selectedID, let message = snapshot.generationPreview(threadID: selectedID)?.message else { return }
        guard let liveBody else { renderedThread = nil; reloadFromStore(); return }
        let atBottom = transcript.bounds.maxY - transcriptScroll.contentView.bounds.maxY < 90
        let formatted = MailboxUI.body(message)
        liveBody.textStorage?.setAttributedString(formatted.attributedString())
        liveBody.invalidateIntrinsicContentSize(); transcript.needsLayout = true
        if atBottom { scrollToEnd() }
    }
    private func scrollToEnd() {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.view.layoutSubtreeIfNeeded()
            self.transcript.scroll(NSPoint(x: 0, y: max(0, self.transcript.bounds.height - self.transcriptScroll.contentView.bounds.height)))
        }
    }
    func markVisibleRead() {
        guard isViewLoaded, view.window?.isKeyWindow == true, view.window?.isVisible == true,
              let id = selectedID, snapshot.thread(id: id)?.unread == true else { return }
        let revision = snapshot.revision
        DispatchQueue.main.async { [weak self] in
            guard let self, self.selectedID == id, self.view.window?.isKeyWindow == true,
                  self.view.window?.isVisible == true, self.store.snapshot.revision == revision else { return }
            do { try self.store.markRead(threadID: id) }
            catch { self.feedback.stringValue = error.localizedDescription }
        }
    }

    private func loadModelOptions() {
        modelOptions = MailboxNewConversationModelCatalog.liveOptions()
        modelPicker.removeAllItems()
        modelPicker.addItems(withTitles: modelOptions.map(\.title))
        modelPicker.autoenablesItems = false
        for (index, option) in modelOptions.enumerated() { modelPicker.item(at: index)?.isEnabled = option.isAvailable }
        let selected = modelOptions.firstIndex { $0.selection == bridge.selectionForNewConversation() && $0.isAvailable }
            ?? modelOptions.firstIndex { $0.isPreferred && $0.isAvailable } ?? modelOptions.firstIndex { $0.isAvailable }
        if let selected { modelPicker.selectItem(at: selected); modelChanged() }
    }
    @objc private func modelChanged() {
        let index = modelPicker.indexOfSelectedItem
        guard modelOptions.indices.contains(index) else { return }
        bridge.setSelectionForNewConversation(modelOptions[index].selection)
        updateComposerState()
    }
    private func saveDraft(_ text: String) {
        if let selectedID { bridge.setComposerDraft(text, threadID: selectedID) }
        else if module == .chat { bridge.setDraftForNewConversation(text) }
    }
    private func restoreDraft() {
        composer.string = selectedID.flatMap { bridge.composerDraft(threadID: $0) }
            ?? (module == .chat && selectedID == nil ? bridge.draftForNewConversation() : "")
        composer.needsDisplay = true
    }
    private func updateComposerState() {
        let thread = selectedID.flatMap { snapshot.thread(id: $0) }
        let newChat = module == .chat && selectedID == nil
        modelPicker.isHidden = !newChat
        modelPicker.isEnabled = !submitting
        composerHint.stringValue = module == .inbox ? "本地备注 · 不会发送给来源" : newChat ? "" : "Enter 发送 · Shift Enter 换行"
        send.title = module == .inbox ? "添加备注" : "发送"
        composer.isEditable = !submitting && (module == .chat || selectedID != nil)
        let available: Bool
        if newChat {
            let index = modelPicker.indexOfSelectedItem
            available = modelOptions.indices.contains(index) && modelOptions[index].isAvailable && bridge.aiReplyCoordinator != nil
        } else { available = thread != nil && (module == .inbox || thread?.generation?.phase != .generating) }
        send.isEnabled = !submitting && available && !composer.string.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        if thread?.generation?.phase == .generating && module == .chat { composerHint.stringValue = "正在生成 · 可先写下下一条消息" }
        composerHint.toolTip = composerHint.stringValue
    }
    private func submit() {
        guard send.isEnabled, !submitting else { return }
        let body = composer.string
        let submittedID = selectedID
        feedback.stringValue = ""; submitting = true; updateComposerState()
        let finish: (Result<UUID?, Error>) -> Void = { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                self.submitting = false
                switch result {
                case let .success(createdID):
                    if let submittedID { self.bridge.clearComposerDraft(threadID: submittedID) }
                    else { self.bridge.clearNewConversationDraft() }
                    if let createdID { self.creating = false; self.selectedID = createdID }
                    self.restoreDraft(); self.reloadFromStore()
                case let .failure(error): self.feedback.stringValue = error.localizedDescription
                }
                self.updateComposerState()
            }
        }
        if let submittedID {
            if module == .inbox {
                do { _ = try store.addLocalNote(threadID: submittedID, body: body); finish(.success(nil)) }
                catch { finish(.failure(error)) }
            } else if let coordinator = bridge.aiReplyCoordinator {
                coordinator.sendMailboxReply(threadID: submittedID, body: body) { finish($0.map { nil }) }
            } else { finish(.failure(MailboxStoreError.continuationUnavailable)) }
        } else if let coordinator = bridge.aiReplyCoordinator,
                  modelOptions.indices.contains(modelPicker.indexOfSelectedItem) {
            coordinator.startMailboxConversation(selection: modelOptions[modelPicker.indexOfSelectedItem].selection,
                body: body) { finish($0.map { $0.threadID }) }
        } else { finish(.failure(MailboxStoreError.continuationUnavailable)) }
    }

    private func perform(_ action: () throws -> Void) {
        do { try action() } catch { feedback.stringValue = error.localizedDescription }
    }
    private func rename(_ id: UUID) {
        guard let window = view.window, let thread = store.thread(id: id) else { return }
        let alert = NSAlert(); alert.messageText = "重命名"; alert.addButton(withTitle: "保存"); alert.addButton(withTitle: "取消")
        let field = NSTextField(string: thread.displayTitle); field.frame = NSRect(x: 0, y: 0, width: 320, height: 26)
        alert.accessoryView = field
        alert.beginSheetModal(for: window) { [weak self] result in
            if result == .alertFirstButtonReturn { self?.perform { try self?.store.renameThread(id, title: field.stringValue) } }
        }
    }
    private func stage(_ message: MailboxMessage, threadID: UUID) {
        guard let thread = store.thread(id: threadID) else { return }
        if thread.review != nil {
            let adapter = MailboxBufferReviewAdapter.shared
            guard let review = adapter.review(threadID: threadID), adapter.canSendToBuffer(reviewID: review.id),
                  adapter.sendToBuffer(reviewID: review.id) else { feedback.stringValue = "这条外部消息当前无法加入 Buffer"; return }
        } else {
            BufferModel.shared.stageExternalSemantic(message.body, origin: .plugin(id: module.pluginKey.rawID))
        }
        feedback.stringValue = "已加入 Buffer，打开 Buffer 后可继续编辑"
    }
    private func saveImage(_ message: MailboxMessage) {
        guard let data = store.imageData(for: message), let window = view.window else { return }
        let panel = NSSavePanel(); panel.allowedContentTypes = [.png]; panel.nameFieldStringValue = "mailbox-image.png"
        panel.beginSheetModal(for: window) { [weak self] result in
            guard result == .OK, let url = panel.url else { return }
            self?.perform { try data.write(to: url, options: .atomic); self?.feedback.stringValue = "图片已保存" }
        }
    }
    private func saveToCapsule(_ message: MailboxMessage, title: String) {
        let plan: CapsuleRailSavePlan
        if let data = store.imageData(for: message) { plan = .imageData(title: title, data: data, fileExtension: "png") }
        else { plan = .note(title: title, text: message.body) }
        switch CapsuleRailSaver.live().save([plan]).first {
        case .saved?: feedback.stringValue = "已存入 Capsule"
        case .alreadySaved?: feedback.stringValue = "Capsule 中已有这份内容"
        default: feedback.stringValue = "未能存入 Capsule，请检查存储状态"
        }
    }
}
