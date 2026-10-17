import Cocoa

/// Narrow windows keep a usable reading/terminal canvas. Side panels become
/// dismissible drawers instead of imposing their intrinsic widths on it.
final class MailboxAdaptiveSplitView: NSView {
    let main: NSView
    let sidebar: NSView
    let inspector: NSView?
    private let sidebarWidth: CGFloat
    private let inspectorWidth: CGFloat = 240
    private let sidebarBreakpoint: CGFloat
    private let inspectorBreakpoint: CGFloat = 920
    private var sidebarPreference: Bool?
    private var inspectorPreference: Bool?
    private let scrim = MailboxDrawerScrim()
    private(set) var sidebarVisible = false
    private(set) var inspectorVisible = false
    private(set) var sidebarOverlay = false
    private(set) var inspectorOverlay = false
    var visibilityChanged: ((Bool, Bool) -> Void)?

    init(sidebar: NSView, main: NSView, inspector: NSView? = nil,
         sidebarWidth: CGFloat = MailboxUI.chatListWidth, sidebarBreakpoint: CGFloat = MailboxUI.sidebarBreakpoint) {
        self.sidebar = sidebar; self.main = main; self.inspector = inspector
        self.sidebarWidth = sidebarWidth; self.sidebarBreakpoint = sidebarBreakpoint
        super.init(frame: .zero)
        // Frames are owned only by this container; descendants still use
        // Auto Layout. Hidden drawers never contribute a minimum width.
        for child in [main, scrim, sidebar] + (inspector.map { [$0] } ?? []) {
            child.translatesAutoresizingMaskIntoConstraints = true
            child.autoresizingMask = []
            addSubview(child)
        }
        main.wantsLayer = true; main.layer?.masksToBounds = true
        scrim.dismiss = { [weak self] in self?.dismissOverlays() }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }

    func toggleSidebar() {
        sidebarPreference = sidebarVisible ? (sidebarOverlay ? nil : false) : true
        if inspectorOverlay { inspectorPreference = nil }
        needsLayout = true
    }
    func toggleInspector() {
        guard inspector != nil else { return }
        inspectorPreference = inspectorVisible ? (inspectorOverlay ? nil : false) : true
        if sidebarOverlay { sidebarPreference = nil }
        needsLayout = true
    }
    func dismissOverlays() {
        if sidebarOverlay { sidebarPreference = nil }
        if inspectorOverlay { inspectorPreference = nil }
        needsLayout = true
    }
    override func cancelOperation(_ sender: Any?) { dismissOverlays() }

    override func layout() {
        let previous = (sidebarVisible, inspectorVisible)
        sidebarVisible = sidebarPreference ?? (bounds.width >= sidebarBreakpoint)
        inspectorVisible = inspector != nil && (inspectorPreference ?? (bounds.width >= inspectorBreakpoint))
        sidebarOverlay = sidebarVisible && bounds.width < sidebarBreakpoint
        inspectorOverlay = inspectorVisible && bounds.width < inspectorBreakpoint
        // Even when both panels were explicitly opened in a wide window,
        // narrowing it shows at most one drawer over the canvas.
        if sidebarOverlay && inspectorOverlay { sidebarVisible = false; sidebarOverlay = false }
        let left = sidebarVisible && !sidebarOverlay ? sidebarWidth : 0
        let right = inspectorVisible && !inspectorOverlay ? inspectorWidth : 0
        main.frame = NSRect(x: left, y: 0, width: max(0, bounds.width - left - right), height: bounds.height)
        sidebar.frame = NSRect(x: 0, y: 0, width: sidebarWidth, height: bounds.height)
        inspector?.frame = NSRect(x: bounds.width - inspectorWidth, y: 0, width: inspectorWidth, height: bounds.height)
        setPanel(sidebar, visible: sidebarVisible, overlay: sidebarOverlay)
        if let inspector { setPanel(inspector, visible: inspectorVisible, overlay: inspectorOverlay) }
        scrim.frame = main.frame
        scrim.isHidden = !sidebarOverlay && !inspectorOverlay
        super.layout()
        if previous.0 != sidebarVisible || previous.1 != inspectorVisible {
            visibilityChanged?(sidebarVisible, inspectorVisible)
        }
    }
    private func setPanel(_ panel: NSView, visible: Bool, overlay: Bool) {
        if !visible, !panel.isHidden, let responder = window?.firstResponder as? NSView,
           responder.isDescendant(of: panel) { window?.makeFirstResponder(nil) }
        panel.isHidden = !visible
        panel.wantsLayer = true
        panel.layer?.shadowOpacity = overlay ? 0.16 : 0
        panel.layer?.shadowRadius = 12
        panel.layer?.shadowOffset = .zero
    }
}

private final class MailboxDrawerScrim: NSButton {
    var dismiss: (() -> Void)?
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        title = ""; isBordered = false; target = self; action = #selector(closeDrawer)
        setAccessibilityLabel("收起侧栏")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
    @objc private func closeDrawer() { dismiss?() }
    override func draw(_ dirtyRect: NSRect) { NSColor.black.withAlphaComponent(0.08).setFill(); bounds.fill() }
}
