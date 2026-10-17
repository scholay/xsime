import Cocoa

/// The host owns navigation; each module owns its layout and runtime state.
/// Removing a module view never terminates its terminal or background request.
final class MailboxWorkspaceViewController: NSViewController {
    let store: MailboxStore
    private let isEnabled: (MailboxModuleID) -> Bool
    private let remembersModule: Bool
    private let rail = MailboxSurface(MailboxUI.sidebar)
    private let content = NSView()
    private let navigation = NSStackView()
    private var observation: MailboxStoreObservation?
    private var registryObserver: NSObjectProtocol?
    private(set) var selectedModule: MailboxModuleID
    private var buttons: [MailboxModuleID: MailboxNavigationButton] = [:]
    private lazy var chat = MailboxMessagesWorkspace(module: .chat, store: store)
    private lazy var inbox = MailboxMessagesWorkspace(module: .inbox, store: store)
    private(set) lazy var terminal = MailboxTerminalWorkspace(store: store)

    init(store: MailboxStore = .shared, remembersModule: Bool = true,
         isEnabled: @escaping (MailboxModuleID) -> Bool = { PluginRegistry.shared.allowsHostModuleAction(.open, for: $0.pluginKey) }) {
        self.store = store; self.isEnabled = isEnabled; self.remembersModule = remembersModule
        selectedModule = remembersModule
            ? MailboxModuleID(rawValue: UserDefaults.standard.string(forKey: "mailbox.workspace") ?? "inbox") ?? .inbox
            : .inbox
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
    deinit { if let registryObserver { NotificationCenter.default.removeObserver(registryObserver) } }

    override func loadView() {
        view = MailboxSurface()
        let layout = MailboxUI.stack([rail, content], vertical: false, spacing: 0)
        layout.alignment = .height
        MailboxUI.pin(layout, to: view)
        rail.translatesAutoresizingMaskIntoConstraints = false
        rail.widthAnchor.constraint(equalToConstant: MailboxUI.railWidth).isActive = true
        let brand = MailboxUI.label("M", size: 22, weight: .semibold)
        brand.textColor = MailboxUI.accent
        brand.alignment = .center
        navigation.orientation = .vertical; navigation.alignment = .centerX; navigation.spacing = 8
        let settings = MailboxActionButton("", symbol: "slider.horizontal.3") {
            SettingsWindowController.shared.showMailboxPlugins()
        }
        settings.toolTip = "管理 Mailbox 插件"
        let settingsRow = NSView()
        settingsRow.addSubview(settings)
        NSLayoutConstraint.activate([
            settingsRow.heightAnchor.constraint(equalToConstant: 28),
            settings.centerXAnchor.constraint(equalTo: settingsRow.centerXAnchor),
            settings.centerYAnchor.constraint(equalTo: settingsRow.centerYAnchor),
        ])
        let railStack = MailboxUI.stack([brand, navigation, MailboxUI.spacer(), settingsRow], spacing: 20)
        railStack.alignment = .centerX
        railStack.edgeInsets = NSEdgeInsets(top: 8, left: 0, bottom: 4, right: 0)
        MailboxUI.pin(railStack, to: rail, inset: 8)
        inbox.returnToTerminal = { [weak self] id in
            guard let self, self.isEnabled(.terminal), self.terminal.selectSession(id) else { return false }
            self.selectModule(.terminal); return true
        }
        terminal.openReceipt = { [weak self] id in self?.show(selecting: id) }
        rebuildNavigation()
        selectModule(selectedModule)
        observation = store.observe(deliverInitial: false) { [weak self] _ in self?.updateBadges() }
        registryObserver = NotificationCenter.default.addObserver(forName: .pluginRegistryDidChange,
            object: nil, queue: .main) { [weak self] _ in
                self?.rebuildNavigation(); if let self { self.selectModule(self.selectedModule) }
            }
    }

    private func rebuildNavigation() {
        MailboxUI.clear(navigation); buttons.removeAll()
        for module in MailboxModuleID.allCases where isEnabled(module) {
            let button = MailboxNavigationButton(module) { [weak self] in
                self?.selectModule(module)
            }
            navigation.addArrangedSubview(button); buttons[module] = button
        }
        updateBadges()
    }
    private func updateBadges() {
        let count = store.snapshot.threads.filter { $0.contentWorkspace == .inbox && $0.unread && $0.archivedAt == nil }.count
        buttons[.inbox]?.unreadCount = count
        buttons[.inbox]?.setAccessibilityLabel(count > 0 ? "收件，\(count) 条未读" : "收件")
    }
    func selectModule(_ requested: MailboxModuleID) {
        _ = view
        content.subviews.forEach { $0.removeFromSuperview() }
        guard let module = isEnabled(requested) ? requested : MailboxModuleID.allCases.first(where: isEnabled) else {
            let empty = MailboxUI.paragraph("Mailbox 插件已停用。打开左下角的插件管理，启用工作区。")
            MailboxUI.pin(MailboxUI.padded(empty, inset: 40), to: content); return
        }
        selectedModule = module
        if remembersModule { UserDefaults.standard.set(module.rawValue, forKey: "mailbox.workspace") }
        for (id, button) in buttons {
            button.selected = id == module
        }
        let controller: NSViewController
        switch module { case .terminal: controller = terminal; case .chat: controller = chat; case .inbox: controller = inbox }
        if controller.parent == nil { addChild(controller) }
        MailboxUI.pin(controller.view, to: content)
        reloadFromStore()
        windowBecameKey()
    }
    func show(selecting threadID: UUID?) {
        _ = view
        guard let threadID, let thread = store.thread(id: threadID) else { reloadFromStore(); return }
        let module: MailboxModuleID = thread.contentWorkspace == .chat ? .chat : .inbox
        selectModule(module)
        if selectedModule == module { (module == .chat ? chat : inbox).selectThread(threadID) }
    }
    func reloadFromStore() {
        guard isViewLoaded else { return }
        updateBadges()
        switch selectedModule { case .terminal: terminal.refresh(); case .chat: chat.reloadFromStore(); case .inbox: inbox.reloadFromStore() }
    }
    func windowBecameKey() {
        switch selectedModule { case .terminal: break; case .chat: chat.markVisibleRead(); case .inbox: inbox.markVisibleRead() }
    }
    func applyAppearance() { guard isViewLoaded else { return }; view.needsDisplay = true; rebuildNavigation(); selectModule(selectedModule) }
}
