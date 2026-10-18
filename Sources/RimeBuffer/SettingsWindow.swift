import Cocoa
import CryptoKit
import CRimeBridge
import UniformTypeIdentifiers

/// Native counterparts of the React SettingsSurface tokens. The Settings
/// background intentionally sits outside `RimeThemePalette`: candidate and
/// workbench surfaces use the product surfaces, while Settings uses the
/// quieter macOS-like chrome defined by the design system.
private enum SettingsVisualStyle {
    static var cardRadius: CGFloat { RimeUI.isLiquidGlass ? 16 : 8 }
    static var panelRadius: CGFloat { RimeUI.isLiquidGlass ? 18 : 10 }

    static var background: NSColor {
        RimeUI.usesLiquidGlassTransparency ? .clear
            : RimeUI.color(RimeUI.isDark ? 0x323232 : 0xECECEC)
    }

    static var separator: NSColor {
        RimeUI.color(RimeUI.isDark ? 0x464646 : 0xD5D5D5)
    }

    static var selectedNavigation: NSColor {
        if RimeUI.isLiquidGlass { return NSColor.controlAccentColor.withAlphaComponent(0.14) }
        return SettingsVisualStyle.background.blended(
            withFraction: 0.16,
            of: RimeUI.accentGreen
        ) ?? RimeUI.accentGreen.withAlphaComponent(0.16)
    }

    static var selectedChoice: NSColor {
        RimeUI.surface2.blended(withFraction: 0.10, of: RimeUI.accentGreen)
            ?? RimeUI.surface2
    }

    static func hairline(backingScale: CGFloat?) -> CGFloat {
        1 / max(backingScale ?? NSScreen.main?.backingScaleFactor ?? 2, 1)
    }
}

private enum SettingsPluginSwitchMode {
    case enablement
    case bufferEnablement
}

/// Keep the Settings-local names for the existing hierarchy while sharing the
/// exact same cursor implementation with views embedded by other modules.
private typealias SettingsPointingButton = RimePointingHandButton
private typealias SettingsPointingSegmentedControl =
    RimePointingHandSegmentedControl

private final class SettingsPluginSwitch: RimeFixedAccentSwitch {
    var pluginKey = PluginKey(domain: .builtIn, rawID: "")
    var mode: SettingsPluginSwitchMode = .enablement
}

private final class SettingsPluginConfigurationButton: SettingsPointingButton {
    var pluginKey = PluginKey(domain: .builtIn, rawID: "")
}

private final class SettingsPluginDetailsButton: SettingsPointingButton {
    var pluginKey = PluginKey(domain: .builtIn, rawID: "")
}

private final class SettingsPluginDownloadButton: SettingsPointingButton {
    var pluginKey = PluginKey(domain: .builtIn, rawID: "")
}

private final class SettingsBrandLinkButton: SettingsPointingButton {
    var destination: URL?
}

private final class SettingsThemeDetailsButton: SettingsPointingButton {
    var mode: RimeAppearanceMode = .night
}

private final class SettingsLexiconButton: SettingsPointingButton {
    var lexiconKind: UserLexiconKind = .chinese
}

private final class SettingsRouteButton: SettingsPointingButton {
    var routeID = SettingsCoreRoute.inputMethod.id
    var isRouteSelected = false {
        didSet { updateVisualState() }
    }
    private var pointerInside = false
    private let rowContent = NSView()
    private let routeIcon = NSImageView()
    private let routeLabel = NSTextField(labelWithString: "")

    func configure(title: String, symbolName: String) {
        // The button owns interaction and accessibility; its decorative views
        // use fixed slots instead of NSButtonCell's variable symbol metrics.
        self.title = ""
        image = nil
        isBordered = false
        bezelStyle = .regularSquare
        setAccessibilityLabel(title)
        toolTip = title
        routeIcon.image = NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)?
            .withSymbolConfiguration(.init(pointSize: 17, weight: .regular))
        routeIcon.imageScaling = .scaleProportionallyDown
        routeLabel.stringValue = title
        routeLabel.font = .systemFont(ofSize: 12, weight: .medium)
        routeLabel.lineBreakMode = .byTruncatingTail
        rowContent.translatesAutoresizingMaskIntoConstraints = false
        rowContent.setAccessibilityElement(false)
        addSubview(rowContent)
        for view in [routeIcon, routeLabel] {
            view.translatesAutoresizingMaskIntoConstraints = false
            view.setAccessibilityElement(false)
            rowContent.addSubview(view)
        }
        NSLayoutConstraint.activate([
            rowContent.leadingAnchor.constraint(equalTo: leadingAnchor),
            rowContent.trailingAnchor.constraint(equalTo: trailingAnchor),
            rowContent.topAnchor.constraint(equalTo: topAnchor),
            rowContent.bottomAnchor.constraint(equalTo: bottomAnchor),
            routeIcon.leadingAnchor.constraint(equalTo: rowContent.leadingAnchor, constant: 10),
            routeIcon.centerYAnchor.constraint(equalTo: rowContent.centerYAnchor),
            routeIcon.widthAnchor.constraint(equalToConstant: 20),
            routeIcon.heightAnchor.constraint(equalToConstant: 20),
            routeLabel.leadingAnchor.constraint(equalTo: routeIcon.trailingAnchor, constant: 8),
            routeLabel.trailingAnchor.constraint(equalTo: rowContent.trailingAnchor, constant: -10),
            routeLabel.centerYAnchor.constraint(equalTo: rowContent.centerYAnchor),
        ])
        updateVisualState()
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        super.hitTest(point) == nil ? nil : self
    }

    override func mouseDown(with event: NSEvent) {
        // Move keyboard focus with mouse navigation, so a previously focused
        // route does not keep a second highlight after another row is chosen.
        window?.makeFirstResponder(self)
        super.mouseDown(with: event)
    }

    override var focusRingMaskBounds: NSRect { bounds.insetBy(dx: 2, dy: 2) }

    override func drawFocusRingMask() {
        NSBezierPath(roundedRect: focusRingMaskBounds,
                     xRadius: RimeUI.isLiquidGlass ? 9 : 5,
                     yRadius: RimeUI.isLiquidGlass ? 9 : 5).fill()
    }

    var hasAlignedContent: Bool {
        layoutSubtreeIfNeeded()
        // SF Symbols and text fields have optical alignment insets; their
        // frame can extend outside the slot while the alignment rect is exact.
        let icon = routeIcon.alignmentRect(forFrame: routeIcon.frame)
        let label = routeLabel.alignmentRect(forFrame: routeLabel.frame)
        let aligned = abs(icon.minX - 10) < 0.5
            && abs(icon.midY - bounds.midY) < 0.5
            && abs(icon.width - 20) < 0.5 && abs(icon.height - 20) < 0.5
            && abs(label.minX - 38) < 0.5
            && abs(label.midY - bounds.midY) <= 0.5
            && label.maxX <= bounds.maxX - 9.5
            && focusRingMaskBounds.width > icon.width
            && accessibilityLabel() == routeLabel.stringValue
        return aligned
    }

    override func mouseEntered(with event: NSEvent) {
        super.mouseEntered(with: event)
        pointerInside = true
        updateVisualState()
    }

    override func mouseExited(with event: NSEvent) {
        pointerInside = false
        updateVisualState()
        super.mouseExited(with: event)
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateVisualState()
    }

    func updateVisualState() {
        wantsLayer = true
        layer?.cornerRadius = RimeUI.isLiquidGlass ? 11 : 7
        layer?.cornerCurve = .continuous
        layer?.backgroundColor = (isRouteSelected
            ? SettingsVisualStyle.selectedNavigation
            : (pointerInside ? RimeUI.surface3 : .clear)).cgColor
        let foreground = isRouteSelected ? RimeUI.textPrimary : RimeUI.textSecondary
        routeIcon.contentTintColor = foreground
        routeLabel.textColor = foreground
    }
}

private final class SettingsPageDocumentView: NSView {
    override var isFlipped: Bool { true }
}

private final class SettingsBackgroundView: NSView {
    private let glass = RimeGlassBackgroundView()

    override init(frame: NSRect) {
        super.init(frame: frame)
        glass.frame = bounds
        glass.autoresizingMask = [.width, .height]
        addSubview(glass)
    }

    required init?(coder: NSCoder) { nil }

    override var isOpaque: Bool { !RimeUI.usesLiquidGlassTransparency }

    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.saveGraphicsState()
        defer { NSGraphicsContext.restoreGraphicsState() }
        NSBezierPath(rect: bounds).addClip()
        SettingsVisualStyle.background.setFill()
        bounds.fill()
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
}

private final class SettingsChromeView: NSView {
    enum Fill: Equatable {
        case settings
        case surface
    }

    enum Border: Equatable {
        case none
        case top
        case bottom
    }

    private let fill: Fill
    private let border: Border

    init(fill: Fill, border: Border) {
        self.fill = fill
        self.border = border
        super.init(frame: .zero)
    }

    required init?(coder: NSCoder) { nil }

    override var isOpaque: Bool { !RimeUI.usesLiquidGlassTransparency }

    override func draw(_ dirtyRect: NSRect) {
        // `dirtyRect` is not guaranteed to be clipped to this arranged
        // subview's bounds while AppKit is snapshotting a layer-backed stack.
        // Filling it directly lets the last chrome band (the status bar) paint
        // over the complete window, although accessibility still sees every
        // control at its correct frame. Establish our own clip so live windows
        // and off-screen settings renders share the same compositing contract.
        NSGraphicsContext.saveGraphicsState()
        defer { NSGraphicsContext.restoreGraphicsState() }
        NSBezierPath(rect: bounds).addClip()
        let fillColor = fill == .settings ? SettingsVisualStyle.background
            : RimeUI.surface2.withAlphaComponent(RimeUI.usesLiquidGlassTransparency ? 0.35 : 1)
        fillColor.setFill()
        bounds.fill()
        guard border != .none else { return }
        (fill == .settings ? SettingsVisualStyle.separator : RimeUI.border).setStroke()
        let y = border == .top
            ? bounds.maxY - SettingsVisualStyle.hairline(backingScale: window?.backingScaleFactor) / 2
            : bounds.minY + SettingsVisualStyle.hairline(backingScale: window?.backingScaleFactor) / 2
        let line = NSBezierPath()
        line.move(to: NSPoint(x: bounds.minX, y: y))
        line.line(to: NSPoint(x: bounds.maxX, y: y))
        line.lineWidth = SettingsVisualStyle.hairline(backingScale: window?.backingScaleFactor)
        line.stroke()
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
}

private final class SettingsSeparatorView: NSView {
    override var intrinsicContentSize: NSSize {
        NSSize(width: SettingsVisualStyle.hairline(backingScale: window?.backingScaleFactor),
               height: NSView.noIntrinsicMetric)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSGraphicsContext.saveGraphicsState()
        defer { NSGraphicsContext.restoreGraphicsState() }
        NSBezierPath(rect: bounds).addClip()
        SettingsVisualStyle.separator.setFill()
        bounds.fill()
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
}

private final class SettingsIconTileView: NSView {
    private let imageView = NSImageView()
    private let explicitPalette: RimeThemePalette?

    init(symbolName: String,
         accessibilityDescription: String,
         palette: RimeThemePalette? = nil) {
        explicitPalette = palette
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = RimeUI.isLiquidGlass ? 10 : 8
        layer?.cornerCurve = .continuous
        imageView.image = PluginVisualIdentity.image(
            symbolName: symbolName,
            accessibilityDescription: accessibilityDescription,
            pointSize: 16,
            weight: .medium
        )
        imageView.imageScaling = .scaleProportionallyDown
        imageView.setAccessibilityElement(false)
        imageView.translatesAutoresizingMaskIntoConstraints = false
        addSubview(imageView)
        translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            widthAnchor.constraint(equalToConstant: 30),
            heightAnchor.constraint(equalToConstant: 30),
            imageView.centerXAnchor.constraint(equalTo: centerXAnchor),
            imageView.centerYAnchor.constraint(equalTo: centerYAnchor),
            imageView.widthAnchor.constraint(equalToConstant: 19),
            imageView.heightAnchor.constraint(equalToConstant: 19),
        ])
        updateThemeColors()
    }

    required init?(coder: NSCoder) { nil }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateThemeColors()
    }

    private func updateThemeColors() {
        let palette = explicitPalette ?? RimeUI.palette
        layer?.backgroundColor = (explicitPalette == nil
            ? RimeUI.surface3 : RimeUI.color(palette.surfaceTertiary)).cgColor
        layer?.borderColor = (explicitPalette == nil
            ? RimeUI.border : RimeUI.color(palette.border)).cgColor
        layer?.borderWidth = SettingsVisualStyle.hairline(backingScale: window?.backingScaleFactor)
        imageView.contentTintColor = explicitPalette == nil
            ? RimeUI.textSecondary : RimeUI.color(palette.textSecondary)
    }
}

/// A full-card hit target around the existing fixed-accent radio control. The
/// control remains the accessible element and action owner; the wrapper only
/// supplies React's choice-card geometry and forwards clicks in its padding.
private final class SettingsChoiceCardView: NSView {
    enum VisualState: Equatable {
        case idle
        case hovered
        case selected
    }

    private let choice: RimeFixedAccentChoiceButton
    private var trackingAreaRef: NSTrackingArea?
    private var pointerInside = false

    init(choice: RimeFixedAccentChoiceButton,
         title: String,
         detail: String,
         symbolName: String) {
        self.choice = choice
        super.init(frame: .zero)
        choice.showsTitle = false
        choice.managesPointingHandCursor = false
        choice.removeFromSuperview()

        let icon = NSImageView()
        icon.image = PluginVisualIdentity.image(
            symbolName: symbolName,
            accessibilityDescription: title,
            pointSize: 18,
            weight: .medium
        )
        icon.imageScaling = .scaleProportionallyDown
        icon.contentTintColor = RimeUI.textSecondary
        icon.setAccessibilityElement(false)
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.widthAnchor.constraint(equalToConstant: 24).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 24).isActive = true

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 11, weight: .semibold)
        titleLabel.textColor = RimeUI.textPrimary
        titleLabel.lineBreakMode = .byTruncatingTail
        titleLabel.toolTip = detail
        titleLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let row = NSStackView(views: [icon, titleLabel, NSView(), choice])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 9
        row.edgeInsets = NSEdgeInsets(top: 10, left: 10, bottom: 10, right: 10)
        row.translatesAutoresizingMaskIntoConstraints = false
        addSubview(row)
        translatesAutoresizingMaskIntoConstraints = false
        heightAnchor.constraint(equalToConstant: 68).isActive = true
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: leadingAnchor),
            row.trailingAnchor.constraint(equalTo: trailingAnchor),
            row.topAnchor.constraint(equalTo: topAnchor),
            row.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        choice.onVisualStateChange = { [weak self] in
            self?.choiceVisualStateDidChange()
        }
        setAccessibilityElement(false)
    }

    required init?(coder: NSCoder) { nil }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingAreaRef { removeTrackingArea(trackingAreaRef) }
        let next = NSTrackingArea(
            rect: bounds,
            options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(next)
        trackingAreaRef = next
    }

    override func mouseEntered(with event: NSEvent) {
        pointerInside = true
        RimePointingHandCursorRules.mouseEntered(enabled: choice.isEnabled)
        needsDisplay = true
    }

    override func mouseExited(with event: NSEvent) {
        pointerInside = false
        RimePointingHandCursorRules.mouseExited()
        needsDisplay = true
    }

    override func resetCursorRects() {
        super.resetCursorRects()
        RimePointingHandCursorRules.resetCursorRect(
            for: self,
            enabled: choice.isEnabled
        )
    }

    override func mouseDown(with event: NSEvent) {
        choice.performClick(self)
        needsDisplay = true
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        // AppKit passes `point` in our superview's coordinate space. Comparing
        // it directly with local `bounds` makes horizontally arranged cards
        // alias the first card's area (the last sibling can steal the click),
        // while most other card areas appear dead. Convert once before every
        // local geometry test, but keep the original point for `super` because
        // NSView's implementation expects that same superview coordinate.
        guard !isHidden, alphaValue > 0, frame.contains(point) else { return nil }
        let localPoint = convert(point, from: superview)
        let choiceRect = convert(choice.bounds, from: choice)
        return choiceRect.contains(localPoint) ? super.hitTest(point) : self
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }

    var visualState: VisualState {
        if choice.state == .on { return .selected }
        return pointerInside ? .hovered : .idle
    }

    func setPointerInsideForSmoke(_ inside: Bool) {
        pointerInside = inside
        needsDisplay = true
    }

    var pointingHandCursorKindForSmoke: RimePointingHandCursorKind {
        RimePointingHandCursorRules.kind(enabled: choice.isEnabled)
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let path = NSBezierPath(
            roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5),
            xRadius: SettingsVisualStyle.cardRadius,
            yRadius: SettingsVisualStyle.cardRadius
        )
        switch visualState {
        case .selected:
            SettingsVisualStyle.selectedChoice.setFill()
        case .hovered, .idle:
            RimeUI.surface2.setFill()
        }
        path.fill()
        switch visualState {
        case .selected:
            RimeUI.accentTextColor.withAlphaComponent(0.60).setStroke()
            path.lineWidth = 1.2
        case .hovered:
            RimeUI.borderStrong.setStroke()
            path.lineWidth = 1
        case .idle:
            RimeUI.border.setStroke()
            path.lineWidth = 1
        }
        path.stroke()
    }

    private func choiceVisualStateDidChange() {
        needsDisplay = true
        window?.invalidateCursorRects(for: self)
        if pointerInside {
            RimePointingHandCursorRules.mouseEntered(enabled: choice.isEnabled)
        }
    }
}

private final class SettingsThemeCardButton: SettingsPointingButton {
    let mode: RimeAppearanceMode

    init(mode: RimeAppearanceMode, selected: Bool, target: AnyObject, action: Selector) {
        self.mode = mode
        super.init(frame: .zero)
        self.target = target
        self.action = action
        title = ""
        isBordered = false
        wantsLayer = true
        layer?.cornerRadius = mode == .liquidGlass ? 16 : 8
        layer?.cornerCurve = .continuous
        translatesAutoresizingMaskIntoConstraints = false

        let palette = mode.palette
        let isGlass = mode == .liquidGlass
        if isGlass { appearance = NSApplication.shared.effectiveAppearance }
        let preview = NSView()
        preview.wantsLayer = true
        preview.layer?.cornerRadius = isGlass ? 12 : 6
        preview.layer?.cornerCurve = .continuous
        preview.layer?.borderWidth = SettingsVisualStyle.hairline(backingScale: nil)
        preview.layer?.borderColor = (isGlass ? NSColor.separatorColor : RimeUI.color(palette.border)).cgColor
        preview.layer?.backgroundColor = (isGlass ? NSColor.windowBackgroundColor : RimeUI.color(palette.surface)).cgColor
        preview.translatesAutoresizingMaskIntoConstraints = false
        addSubview(preview)

        let colors: [NSColor] = mode == .liquidGlass
            ? [.controlBackgroundColor, .selectedContentBackgroundColor, .controlAccentColor]
            : (mode == .rasta
                ? [palette.brandRed, palette.brandYellow, palette.brandGreen]
                : [palette.surfaceSecondary, palette.selectedCandidateBackground,
                   palette.accentGreen]).map { RimeUI.color($0) }
        let swatches = colors.map { color -> NSView in
            let swatch = NSView()
            swatch.wantsLayer = true
            swatch.layer?.cornerRadius = isGlass ? 8 : 4
            swatch.layer?.backgroundColor = color.cgColor
            swatch.translatesAutoresizingMaskIntoConstraints = false
            return swatch
        }
        let swatchRow = NSStackView(views: swatches)
        swatchRow.orientation = .horizontal
        swatchRow.distribution = .fillEqually
        swatchRow.spacing = 6
        swatchRow.translatesAutoresizingMaskIntoConstraints = false
        preview.addSubview(swatchRow)

        let name = NSTextField(labelWithString: mode.title)
        name.font = .systemFont(ofSize: 11, weight: .semibold)
        name.textColor = isGlass ? .labelColor : RimeUI.color(palette.textPrimary)
        name.toolTip = mode.detailText
        name.translatesAutoresizingMaskIntoConstraints = false
        addSubview(name)

        let family = NSTextField(labelWithString: mode.family.title)
        family.font = .systemFont(ofSize: 9)
        family.textColor = isGlass ? .secondaryLabelColor : RimeUI.color(palette.textMuted)
        family.translatesAutoresizingMaskIntoConstraints = false
        addSubview(family)

        let selectedMark = NSImageView()
        selectedMark.image = NSImage(systemSymbolName: "checkmark.circle.fill",
                                     accessibilityDescription: nil)
        selectedMark.contentTintColor = isGlass ? .controlAccentColor : RimeUI.color(palette.accentText)
        selectedMark.isHidden = !selected
        selectedMark.translatesAutoresizingMaskIntoConstraints = false
        addSubview(selectedMark)
        NSLayoutConstraint.activate([
            preview.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            preview.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            preview.topAnchor.constraint(equalTo: topAnchor, constant: 10),
            preview.heightAnchor.constraint(equalToConstant: 68),
            swatchRow.leadingAnchor.constraint(equalTo: preview.leadingAnchor, constant: 12),
            swatchRow.trailingAnchor.constraint(equalTo: preview.trailingAnchor, constant: -12),
            swatchRow.centerYAnchor.constraint(equalTo: preview.centerYAnchor),
            swatchRow.heightAnchor.constraint(equalToConstant: 30),
            name.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            name.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),
            family.leadingAnchor.constraint(equalTo: name.trailingAnchor, constant: 8),
            family.firstBaselineAnchor.constraint(equalTo: name.firstBaselineAnchor),
            selectedMark.trailingAnchor.constraint(equalTo: preview.trailingAnchor, constant: -7),
            selectedMark.topAnchor.constraint(equalTo: preview.topAnchor, constant: 7),
            selectedMark.widthAnchor.constraint(equalToConstant: 16),
            selectedMark.heightAnchor.constraint(equalToConstant: 16),
        ])
        layer?.backgroundColor = (isGlass ? NSColor.controlBackgroundColor : RimeUI.color(palette.surfaceSecondary)).cgColor
        layer?.borderColor = (isGlass
            ? (selected ? NSColor.controlAccentColor : .separatorColor)
            : RimeUI.color(selected ? palette.selectedCandidateBackground : palette.border)).cgColor
        layer?.borderWidth = selected ? 2 : SettingsVisualStyle.hairline(backingScale: nil)
        setAccessibilityLabel("\(mode.title)主题，\(selected ? "正在使用" : "可用")")
    }

    required init?(coder: NSCoder) { nil }

    override func hitTest(_ point: NSPoint) -> NSView? {
        // `point` belongs to the vertical stack's coordinate system, not this
        // button's local bounds. Matching it against `frame` keeps the bottom
        // theme card from claiming clicks intended for either sibling.
        guard !isHidden, alphaValue > 0, frame.contains(point) else { return nil }
        return self
    }
}

private final class SettingsCardActionSmokeProbe: NSObject {
    private(set) var choiceTags: [Int] = []
    private(set) var appearanceModes: [RimeAppearanceMode] = []

    @objc func choose(_ sender: RimeFixedAccentChoiceButton) {
        choiceTags.append(sender.tag)
    }

    @objc func chooseAppearance(_ sender: SettingsThemeCardButton) {
        appearanceModes.append(sender.mode)
    }
}

enum SettingsWindowPresentationRules {
    private static let standaloneShowCommands: Set<String> = [
        "settings-preview",
        "theme-appkit-smoke",
    ]

    static func isStandaloneShowCommand(arguments: [String]) -> Bool {
        !standaloneShowCommands.isDisjoint(with: arguments)
    }

    static func allowsShow(
        currentInputSourceIsOwn: Bool,
        isStandaloneShowCommand: Bool
    ) -> Bool {
        currentInputSourceIsOwn || isStandaloneShowCommand
    }
}

/// The gateway remains one local MCP server; these choices only change the
/// client-side configuration format shown in Settings.
private enum GatewayClientConfiguration: Int, CaseIterable {
    case cursor
    case codex
    case claudeCode
    case genericMCP

    static let redactedToken = "••••••••"

    var title: String {
        switch self {
        case .cursor: return "Cursor"
        case .codex: return "Codex"
        case .claudeCode: return "Claude Code"
        case .genericMCP: return "通用 MCP"
        }
    }

    var detail: String {
        switch self {
        case .cursor:
            return "粘贴到项目的 .cursor/mcp.json 或用户级 ~/.cursor/mcp.json。"
        case .codex:
            return "粘贴到 ~/.codex/config.toml；令牌通过环境变量提供，不写进配置文件。"
        case .claudeCode:
            return "粘贴到项目的 .mcp.json，或复制下方可选的命令在终端注册。"
        case .genericMCP:
            return "适用于接受 Streamable HTTP MCP JSON 配置的客户端。"
        }
    }

    var supplementaryTitle: String? {
        switch self {
        case .codex: return "环境变量（当前预览已脱敏）"
        case .claudeCode: return "终端命令（可选，当前预览已脱敏）"
        case .cursor, .genericMCP: return nil
        }
    }

    var supplementaryCopyTitle: String? {
        switch self {
        case .codex: return "复制环境变量"
        case .claudeCode: return "复制终端命令"
        case .cursor, .genericMCP: return nil
        }
    }

    var copyStatus: String {
        "已复制 \(title) 配置"
    }
}

/// Central settings surface for input schemas, candidate UI, buffer mode,
/// AI/local connectors, and diagnostics.
final class SettingsWindowController: NSObject, NSTextFieldDelegate, NSWindowDelegate {
    static let shared = SettingsWindowController()
    private static let previewContentSize = NSSize(width: 980, height: 680)

    private var window: NSWindow?
    private let sidebarScrollView = NSScrollView()
    private let sidebarDocumentView = SettingsPageDocumentView()
    private let sidebar = NSStackView()
    private let contentHost = NSView()
    private var routeCatalog = try! SettingsRouteCatalog()
    private lazy var navigation = SettingsNavigationState(catalog: routeCatalog)
    private var navButtons: [SettingsRouteID: NSButton] = [:]
    private var activePluginSettingsController: NSViewController?
    private var statsObserver: NSObjectProtocol?
    private var pluginObserver: NSObjectProtocol?
    private var registryObserver: NSObjectProtocol?
    private var activeBufferPluginObserver: NSObjectProtocol?
    private var inputConfigurationObserver: NSObjectProtocol?
    private var dictionaryHealthObserver: NSObjectProtocol?
    private weak var dictionaryWarningLabel: NSTextField?
    private var aiConnectorObserver: NSObjectProtocol?
    private var aiConnectorAvailabilityObserver: NSObjectProtocol?
    private var appearanceObserver: NSObjectProtocol?
    private var permissionRefreshObserver: NSObjectProtocol?
    private var capsuleSyncSettingsObserver: NSObjectProtocol?
    private var mailboxSettingsObservation: MailboxStoreObservation?
    private weak var mailboxSettingsStatusBadge: NSTextField?
    private weak var mailboxSettingsSummaryLabel: NSTextField?
    private weak var mailboxSettingsStorageButton: NSButton?
    private weak var capsuleSyncSettingsStatusBadge: NSTextField?
    private weak var capsuleSyncSettingsDetailLabel: NSTextField?
    private weak var capsuleSyncSettingsSyncButton: NSButton?
    private weak var capsuleSyncSettingsChooseButton: NSButton?
    private weak var capsuleSyncSettingsDisableButton: NSButton?

    private var encodingRadios: [InputEncoding: RimeFixedAccentChoiceButton] = [:]
    private var externalSchemaRadios: [String: RimeFixedAccentChoiceButton] = [:]
    private var externalSchemaChoiceIDs: [String] = []
    private var chordSchemaStatusRow: NSView?
    private let appearancePopUp = RimeFixedAccentPopUpButton()
    private let alignToInputBoxCheck = RimeFixedAccentSwitch(frame: .zero)
    private let alignToInputBoxStatusLabel = NSTextField(wrappingLabelWithString: "")
    private let clipboardHistoryCheck = RimeFixedAccentSwitch(frame: .zero)
    private let clipboardAutoPasteCheck = RimeFixedAccentSwitch(frame: .zero)
    private let clipboardAutoPasteStatusLabel =
        NSTextField(wrappingLabelWithString: "")
    private let closeAfterLastDeliveryCheck = RimeFixedAccentSwitch(frame: .zero)
    private let moveBufferWindowButton = SettingsPointingButton(
        title: "移到当前屏幕",
        target: nil,
        action: nil
    )
    /// The alignment and auto-paste features default to on but stay inert
    /// without the grant, and the probe deliberately never prompts on its own.
    /// This is the explicit user action that asks for it.
    private let accessibilityGrantButton = SettingsPointingButton(
        title: "请求辅助功能权限",
        target: nil,
        action: nil
    )
    private let resetOnAppSwitchCheck = RimeFixedAccentSwitch(frame: .zero)
    private let gatewayEnableCheck = RimeFixedAccentSwitch(frame: .zero)
    private let gatewayClientTabs = SettingsPointingSegmentedControl(
        labels: GatewayClientConfiguration.allCases.map(\.title),
        trackingMode: .selectOne,
        target: nil,
        action: nil
    )
    private var selectedGatewayClient = GatewayClientConfiguration.cursor
    private let gatewayConfigField = NSTextField(string: "")
    private let gatewayCopyConfigButton = SettingsPointingButton(
        title: "复制配置",
        target: nil,
        action: nil
    )
    private let gatewaySupplementaryField = NSTextField(string: "")
    private let gatewayCopySupplementaryButton = SettingsPointingButton(
        title: "复制",
        target: nil,
        action: nil
    )
    private let aiProviderPicker = RimeFixedAccentPopUpButton()
    private var selectedAIProviderID: UUID?
    private var selectedAIProviderDocumentationURL: URL?
    private let aiProviderNameField = NSTextField(string: "")
    private let aiBaseURLField = NSTextField(string: "")
    private let aiDocumentationURLField = NSTextField(string: "")
    private let aiModelField = NSTextField(string: "")
    private let aiAPIKeyField = NSSecureTextField(string: "")
    private let aiConfigurationStatus = NSTextField(labelWithString: "")
    private var aiConnectorRadios: [AITextProviderKind: RimeFixedAccentChoiceButton] = [:]
    private let codexLoginButton = SettingsPointingButton(
        title: "登录 Codex",
        target: nil,
        action: nil
    )
    private let codexCopyLoginLinkButton = SettingsPointingButton(
        title: "复制登录链接",
        target: nil,
        action: nil
    )
    private let codexLoginSpinner = NSProgressIndicator()
    private let codexLoginStatusLabel = NSTextField(wrappingLabelWithString: "")
    private var codexLoginOperation: AITextCodexLoginOperation?
    private var codexLoginSessionID: UUID?
    private var codexLoginCancelling = false
    private var codexAuthorizationURL: URL?
    private var codexLoginFeedback: String?
    private var codexLoginFeedbackIsError = false
    private let claudeLoginButton = SettingsPointingButton(
        title: "登录 Claude",
        target: nil,
        action: nil
    )
    private let claudeLoginSpinner = NSProgressIndicator()
    private let claudeLoginStatusLabel = NSTextField(wrappingLabelWithString: "")
    private var claudeLoginOperation: AITextClaudeLoginOperation?
    private var claudeLoginSessionID: UUID?
    private var claudeLoginCancelling = false
    private var claudeLoginFeedback: String?
    private var claudeLoginFeedbackIsError = false
    private let candidateArrangementPopUp = NSPopUpButton(frame: .zero, pullsDown: false)
    private var candidateMetricFields: [CandidateWindowMetric: NSTextField] = [:]
    private var candidateMetricSliders: [CandidateWindowMetric: NSSlider] = [:]
    private var candidateMetricHints: [CandidateWindowMetric: NSTextField] = [:]
    private var candidatePreview: CandidatePreviewView?
    private let bufferWidthSlider = NSSlider(
        value: 760,
        minValue: Double(BufferWindowGeometry.standardMinimumWidth),
        maxValue: Double(BufferWindowGeometry.standardMaximumWidth),
        target: nil,
        action: nil
    )
    private let bufferWidthField = NSTextField(string: "760")
    private let shortcutFeedbackLabel = NSTextField(wrappingLabelWithString: "")
    private let statsDatePicker = NSDatePicker()
    private let statsSummary = NSTextField(labelWithString: "")
    private let statsTopKey = NSTextField(labelWithString: "")
    private let installStatus = NSTextField(labelWithString: "")
    private let heatmapView = KeyboardHeatmapView()
    private let pluginRowsStack = NSStackView()
    private let pluginStatusLabel = NSTextField(labelWithString: "")
    private let settingsStatusLabel = NSTextField(labelWithString: "")
    private let settingsRouteLabel = NSTextField(labelWithString: "")
    private var pluginDownloadInProgress = false
    private var pluginRefreshScheduled = false
    private var chordExtensionDeploymentInProgress = false
    private var pluginConfigurationSheet: NSPanel?
    private var cardDetailsPopover: NSPopover?

    private var userDir: URL {
        if let override = ProcessInfo.processInfo.environment["RIMEBUFFER_USER_DIR"],
           !override.isEmpty {
            return URL(fileURLWithPath: override, isDirectory: true)
        }
        return URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/\(RimesPaths.directoryName)", isDirectory: true)
    }

    private var installLogURL: URL {
        URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("rimebuffer-install.log")
    }

    /// Theme accents may be bright enough for controls but not for small text.
    /// The palette resolves an AA-safe status tone for its own surface.
    private var themeStatusColor: NSColor {
        RimeUI.accentTextColor
    }

    @discardableResult
    func show() -> Bool {
        let isStandaloneShowCommand = SettingsWindowPresentationRules
            .isStandaloneShowCommand(arguments: CommandLine.arguments)
        guard SettingsWindowPresentationRules.allowsShow(
            currentInputSourceIsOwn: RimeInputSourceAuthority.currentSourceIsOwn(),
            isStandaloneShowCommand: isStandaloneShowCommand
        ) else {
            IMELog.write("Settings open ignored; RIMES is not selected")
            return false
        }
        // `settings-preview` reaches this method before `app.run()`; the live
        // IMK process is already running, so this branch is a preview-only
        // launch prerequisite and a no-op in production.
        if !NSApp.isRunning {
            NSApp.finishLaunching()
        }
        if window == nil { build() }
        if window?.isVisible == true {
            if let window {
                StandaloneWindowFocusCoordinator.shared.windowWillPresent(window)
            }
            NSApp.activate(ignoringOtherApps: true)
            window?.makeKeyAndOrderFront(nil)
            return true
        }
        rebuildRouteCatalog()
        reload()
        showCurrentRoute()
        if let window {
            StandaloneWindowFocusCoordinator.shared.windowWillPresent(window)
        }
        NSApp.activate(ignoringOtherApps: true)
        window?.center()
        window?.makeKeyAndOrderFront(nil)
        window?.contentView?.layoutSubtreeIfNeeded()
        window?.contentView?.display()
        return true
    }

    func showBufferSettings() {
        guard show() else { return }
        _ = navigation.selectRoute(SettingsCoreRoute.buffer.id,
                                   catalog: routeCatalog)
        _ = navigation.selectSubpage(
            SettingsSubpageID(rawValue: "buffer"),
            catalog: routeCatalog
        )
        showCurrentRoute()
        window?.makeKeyAndOrderFront(nil)
    }

    func showPluginConfiguration(pluginKey: PluginKey) {
        guard show() else { return }
        _ = navigation.selectRoute(SettingsCoreRoute.plugins.id,
                                   catalog: routeCatalog)
        _ = navigation.selectSubpage(PluginManagementSubpage.bufferPlugins.id,
                                     catalog: routeCatalog)
        showCurrentRoute()
        presentPluginConfiguration(pluginKey: pluginKey)
    }

    func showCapsulePlugins() {
        guard show() else { return }
        _ = navigation.selectRoute(SettingsCoreRoute.plugins.id,
                                   catalog: routeCatalog)
        _ = navigation.selectSubpage(PluginManagementSubpage.capsulePlugins.id,
                                     catalog: routeCatalog)
        showCurrentRoute()
        window?.makeKeyAndOrderFront(nil)
    }

    func showMailboxPlugins() {
        guard show() else { return }
        _ = navigation.selectRoute(SettingsCoreRoute.plugins.id, catalog: routeCatalog)
        _ = navigation.selectSubpage(PluginManagementSubpage.mailboxPlugins.id,
                                     catalog: routeCatalog)
        showCurrentRoute()
        window?.makeKeyAndOrderFront(nil)
    }

    /// Dev-only: render one settings page to a PNG by drawing the window's own
    /// view hierarchy (no screen-recording permission needed). Used to preview
    /// the UI without a live input session.
    func renderForPreview(pageIndex: Int, to path: String) {
        if window == nil { build() }
        rebuildRouteCatalog()
        reload()
        let targets = previewTargets()
        let target = targets.indices.contains(pageIndex)
            ? targets[pageIndex]
            : (SettingsCoreRoute.buffer.id, SettingsSubpageID(rawValue: "buffer"), "buffer")
        selectPreviewTarget(routeID: target.0, subpageID: target.1)
        renderCurrentView(to: path)
    }

    /// Renders every route/subpage from the live catalog and writes a manifest
    /// so visual checks never depend on enum ordinals or a hard-coded page
    /// count. Preview user-data isolation is established by main.swift.
    @discardableResult
    func renderAllForPreview(to directory: String) -> Bool {
        if window == nil { build() }
        rebuildRouteCatalog()
        reload()
        let root = URL(fileURLWithPath: directory, isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: root,
                                                    withIntermediateDirectories: true)
        } catch {
            print("settings render directory failed: \(error.localizedDescription)")
            return false
        }

        var manifest: [[String: String]] = []
        var allRendered = true
        for target in previewTargets() {
            selectPreviewTarget(routeID: target.0, subpageID: target.1)
            let fileName = target.2 + ".png"
            let path = root.appendingPathComponent(fileName).path
            allRendered = renderCurrentView(to: path) && allRendered
            manifest.append([
                "routeID": target.0.rawValue,
                "subpageID": target.1.rawValue,
                "file": fileName,
            ])
        }
        do {
            let data = try JSONSerialization.data(withJSONObject: manifest,
                                                  options: [.prettyPrinted, .sortedKeys])
            try data.write(to: root.appendingPathComponent("manifest.json"),
                           options: .atomic)
        } catch {
            print("settings render manifest failed: \(error.localizedDescription)")
            allRendered = false
        }
        return allRendered
    }

    /// Public-only paired Linux fixtures. Volatile defaults never touch the user's
    /// preferences, tokens, gateway listener, or live Rime data directory.
    func renderGatewayForPreview(to directory: String) -> Bool {
        let defaults = UserDefaults.standard
        let old = defaults.volatileDomain(forName: UserDefaults.argumentDomain)
        defer { defaults.setVolatileDomain(old, forName: UserDefaults.argumentDomain) }
        let root = URL(fileURLWithPath: directory, isDirectory: true)
        do { try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true) }
        catch { return false }
        var manifest: [[String: Any]] = []
        var clients: [[String: Any]] = []
        var success = true
        let identifiers = ["cursor", "codex", "claudeCode", "genericMCP"]
        for enabled in [true, false] {
            var preview = old
            preview["gatewayEnabled"] = enabled
            preview["gatewayPort"] = 47700
            defaults.setVolatileDomain(preview, forName: UserDefaults.argumentDomain)
            if window == nil { build() }
            rebuildRouteCatalog()
            reload()
            for client in GatewayClientConfiguration.allCases {
                selectedGatewayClient = client
                selectPreviewTarget(routeID: SettingsCoreRoute.connectors.id,
                                    subpageID: SettingsSubpageID(rawValue: "local-gateway"))
                let id = identifiers[client.rawValue]
                let name = "gateway-\(id)-\(enabled ? "enabled" : "disabled").png"
                success = renderCurrentView(to: root.appendingPathComponent(name).path) && success
                manifest.append(["file": name, "client": id, "enabled": enabled,
                                 "theme": RimeUI.appearance.rawValue, "width": 980, "height": 680])
                if enabled {
                    clients.append(["id": id, "title": client.title, "detail": client.detail,
                        "configuration": gatewayConfiguration(for: client, token: GatewayClientConfiguration.redactedToken),
                        "supplementary": gatewaySupplementaryConfiguration(for: client, token: GatewayClientConfiguration.redactedToken) ?? ""])
                }
            }
        }
        do {
            try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
                .write(to: root.appendingPathComponent("manifest.json"), options: .atomic)
            try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "purpose": "public-gateway-settings", "clients": clients,
                "sources": ["Sources/RimeBuffer/SettingsWindow.swift": SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: #filePath))).map { String(format: "%02x", $0) }.joined()]], options: [.prettyPrinted, .sortedKeys])
                .write(to: root.appendingPathComponent("settings-contract.json"), options: .atomic)
        } catch { return false }
        return success
    }

    private func previewTargets() -> [(SettingsRouteID, SettingsSubpageID, String)] {
        routeCatalog.orderedRoutes.flatMap { route in
            route.subpages.map { subpage in
                let routeSlug = route.id.rawValue
                    .replacingOccurrences(of: ".", with: "-")
                let subpageSlug = subpage.id.rawValue
                    .replacingOccurrences(of: ".", with: "-")
                return (route.id, subpage.id, "\(routeSlug)--\(subpageSlug)")
            }
        }
    }

    private func selectPreviewTarget(routeID: SettingsRouteID,
                                     subpageID: SettingsSubpageID) {
        _ = navigation.selectRoute(routeID, catalog: routeCatalog)
        _ = navigation.selectSubpage(subpageID, catalog: routeCatalog)
        showCurrentRoute()
    }

    func validateSidebarLayoutForSmoke() -> Bool {
        sidebar.layoutSubtreeIfNeeded()
        return !navButtons.isEmpty && navButtons.values.allSatisfy { button in
            guard let row = button as? SettingsRouteButton else { return false }
            let iconPoint = row.convert(NSPoint(x: 20, y: row.bounds.midY), to: row.superview)
            let labelPoint = row.convert(NSPoint(x: 60, y: row.bounds.midY), to: row.superview)
            let aligned = row.hasAlignedContent
            let iconHit = row.hitTest(iconPoint) === row
            let labelHit = row.hitTest(labelPoint) === row
            if !aligned || !iconHit || !labelHit {
                print("sidebar row \(row.routeID): aligned=\(aligned) iconHit=\(iconHit) labelHit=\(labelHit) frame=\(row.frame)")
            }
            return aligned && iconHit && labelHit
        }
    }

    func validateTitlebarCoverageForSmoke() -> Bool {
        guard let window, let background = window.contentView as? SettingsBackgroundView,
              let guide = window.contentLayoutGuide as? NSLayoutGuide else { return false }
        background.layoutSubtreeIfNeeded()
        let material = background.subviews.compactMap { $0 as? RimeGlassBackgroundView }.first
        let fullFrame = background.convert(background.bounds, to: nil)
        let titlebarHeight = background.bounds.height - guide.frame.height
        let titlebar = NSRect(x: fullFrame.minX, y: fullFrame.maxY - titlebarHeight,
                              width: fullFrame.width, height: titlebarHeight)
        guard let material else { return false }
        let materialFrame = material.convert(material.bounds, to: nil)
        let controlsFrame = contentHost.convert(contentHost.bounds, to: nil)
        return window.styleMask.contains(.fullSizeContentView)
            && titlebarHeight > 0
            && abs(fullFrame.maxY - window.frame.height) < 0.5
            && materialFrame.insetBy(dx: -0.5, dy: -0.5).contains(titlebar)
            && controlsFrame.maxY <= titlebar.minY + 0.5
            && material.hasVisibleMaterial == RimeUI.usesLiquidGlassTransparency
    }

    /// AppKit delivers `hitTest(_:)` points in each receiver's superview
    /// coordinate system. Keep a smoke over the actual Settings page hierarchy
    /// so a full-card override cannot accidentally compare those points with
    /// local `bounds` and let the last sibling claim another card's click.
    func validateChoiceCardHitTestingForSmoke() -> Bool {
        if window == nil { build() }
        guard let window else {
            print("settings card hit-test smoke: missing window")
            return false
        }

        let previousRouteID = navigation.currentRouteID
        let previousSubpageID = navigation.selectedSubpage()
        defer {
            _ = navigation.selectRoute(previousRouteID, catalog: routeCatalog)
            if let previousSubpageID {
                _ = navigation.selectSubpage(previousSubpageID, catalog: routeCatalog)
            }
            showCurrentRoute()
        }

        window.setContentSize(Self.previewContentSize)

        var pointerPolicyOK = RimePointingHandCursorRules.kind(enabled: true)
                == .pointingHand
            && RimePointingHandCursorRules.kind(enabled: false) == .arrow
            && navButtons.values.allSatisfy { $0 is SettingsPointingButton }

        func descendants<T: NSView>(of type: T.Type, in root: NSView) -> [T] {
            root.subviews.flatMap { child -> [T] in
                let own = (child as? T).map { [$0] } ?? []
                return own + descendants(of: type, in: child)
            }
        }

        func exactCenterHits<T: NSView>(_ views: [T],
                                        expectedCount: Int,
                                        label: String) -> Bool {
            guard views.count == expectedCount else {
                print("settings card hit-test smoke: expected \(expectedCount) \(label), got \(views.count)")
                return false
            }
            for view in views {
                guard let parent = view.superview else {
                    print("settings card hit-test smoke: \(label) has no parent")
                    return false
                }
                let centerInParent = NSPoint(x: view.frame.midX, y: view.frame.midY)
                let pointForParentHitTest = parent.convert(
                    centerInParent,
                    to: parent.superview
                )
                guard parent.hitTest(pointForParentHitTest) === view else {
                    print("settings card hit-test smoke: wrong \(label) center target frame=\(view.frame)")
                    return false
                }
            }
            return true
        }

        selectPreviewTarget(
            routeID: SettingsCoreRoute.appearance.id,
            subpageID: SettingsSubpageID(rawValue: "theme")
        )
        window.contentView?.layoutSubtreeIfNeeded()
        let themeCards = descendants(of: SettingsThemeCardButton.self, in: contentHost)
        pointerPolicyOK = pointerPolicyOK
            && descendants(of: NSButton.self, in: contentHost).allSatisfy {
                $0 is SettingsPointingButton
            }
            && descendants(of: NSSegmentedControl.self, in: contentHost)
                .allSatisfy { $0 is SettingsPointingSegmentedControl }
        var themeOK = exactCenterHits(
            themeCards,
            expectedCount: RimeAppearanceMode.allCases.count,
            label: "theme card"
        )

        let actionProbe = SettingsCardActionSmokeProbe()
        for mode in RimeAppearanceMode.allCases {
            guard let button = themeCards.first(where: { $0.mode == mode }) else {
                print("settings card hit-test smoke: missing theme action \(mode.rawValue)")
                themeOK = false
                continue
            }
            let previousTarget = button.target
            let previousAction = button.action
            button.target = actionProbe
            button.action = #selector(SettingsCardActionSmokeProbe.chooseAppearance(_:))
            button.performClick(nil)
            button.target = previousTarget
            button.action = previousAction
            guard actionProbe.appearanceModes.last == mode else {
                print("settings card hit-test smoke: wrong theme action for \(mode.rawValue)")
                themeOK = false
                continue
            }
        }
        themeOK = themeOK
            && actionProbe.appearanceModes == RimeAppearanceMode.allCases

        selectPreviewTarget(
            routeID: SettingsCoreRoute.inputMethod.id,
            subpageID: SettingsSubpageID(rawValue: "encoding")
        )
        window.contentView?.layoutSubtreeIfNeeded()
        let encodingCards = descendants(of: SettingsChoiceCardView.self, in: contentHost)
        pointerPolicyOK = pointerPolicyOK
            && !encodingCards.isEmpty
            && descendants(of: NSButton.self, in: contentHost).allSatisfy {
                $0 is SettingsPointingButton
            }
            && descendants(of: NSSegmentedControl.self, in: contentHost)
                .allSatisfy { $0 is SettingsPointingSegmentedControl }
        var encodingOK = exactCenterHits(
            encodingCards,
            expectedCount: InputEncoding.allCases.count + externalSchemaChoiceIDs.count,
            label: "input scheme card"
        )
        var taggedCards: [(tag: Int,
                           card: SettingsChoiceCardView,
                           choice: RimeFixedAccentChoiceButton)] = []
        for card in encodingCards {
            let choices = descendants(of: RimeFixedAccentChoiceButton.self, in: card)
            guard choices.count == 1, let choice = choices.first else {
                print("settings card hit-test smoke: input scheme card has \(choices.count) choices")
                encodingOK = false
                continue
            }
            taggedCards.append((choice.tag, card, choice))
            let previousTarget = choice.target
            let previousAction = choice.action
            choice.target = actionProbe
            choice.action = #selector(SettingsCardActionSmokeProbe.choose(_:))
            choice.state = .off
            guard let event = NSEvent.mouseEvent(
                with: .leftMouseDown,
                location: card.convert(
                    NSPoint(x: card.bounds.midX, y: card.bounds.midY),
                    to: nil
                ),
                modifierFlags: [],
                timestamp: 0,
                windowNumber: window.windowNumber,
                context: nil,
                eventNumber: 0,
                clickCount: 1,
                pressure: 1
            ) else {
                print("settings card hit-test smoke: could not create encoding click")
                choice.target = previousTarget
                choice.action = previousAction
                encodingOK = false
                continue
            }
            card.mouseDown(with: event)
            choice.target = previousTarget
            choice.action = previousAction
            guard choice.state == .on,
                  actionProbe.choiceTags.last == choice.tag,
                  (0..<(InputEncoding.allCases.count + externalSchemaChoiceIDs.count)).contains(choice.tag) else {
                print("settings card hit-test smoke: wrong input scheme action tag=\(choice.tag)")
                encodingOK = false
                continue
            }
        }
        encodingOK = encodingOK
            && actionProbe.choiceTags.count == InputEncoding.allCases.count + externalSchemaChoiceIDs.count
            && Set(actionProbe.choiceTags) == Set(0..<(InputEncoding.allCases.count + externalSchemaChoiceIDs.count))

        taggedCards.sort { $0.tag < $1.tag }
        var cursorOwnershipOK = taggedCards.allSatisfy {
            !$0.choice.managesPointingHandCursor
                && $0.card.pointingHandCursorKindForSmoke == .pointingHand
        }
        if let probe = taggedCards.first {
            probe.card.setPointerInsideForSmoke(true)
            probe.card.needsDisplay = false
            probe.choice.isEnabled = false
            cursorOwnershipOK = cursorOwnershipOK
                && probe.card.pointingHandCursorKindForSmoke == .arrow
                && probe.card.needsDisplay
            probe.card.needsDisplay = false
            probe.choice.isEnabled = true
            cursorOwnershipOK = cursorOwnershipOK
                && probe.card.pointingHandCursorKindForSmoke == .pointingHand
                && probe.card.needsDisplay
            probe.card.setPointerInsideForSmoke(false)
        }
        if !cursorOwnershipOK {
            print("settings card cursor smoke: child/parent ownership or disabled state drifted")
        }

        var visualStateOK = taggedCards.count >= 3
        if visualStateOK {
            taggedCards.forEach { $0.choice.state = .off }
            taggedCards.forEach { $0.card.needsDisplay = false }

            taggedCards[0].choice.state = .on
            visualStateOK = taggedCards[0].card.needsDisplay
                && taggedCards.enumerated().allSatisfy {
                    $0.element.card.visualState == ($0.offset == 0 ? .selected : .idle)
                }

            taggedCards.forEach { $0.card.needsDisplay = false }
            taggedCards[0].choice.state = .off
            taggedCards[1].choice.state = .on
            visualStateOK = visualStateOK
                && taggedCards[0].card.needsDisplay
                && taggedCards[1].card.needsDisplay
                && taggedCards.enumerated().allSatisfy {
                    $0.element.card.visualState == ($0.offset == 1 ? .selected : .idle)
                }

            taggedCards[2].card.setPointerInsideForSmoke(true)
            visualStateOK = visualStateOK
                && taggedCards.enumerated().allSatisfy {
                    let expected: SettingsChoiceCardView.VisualState = $0.offset == 1
                        ? .selected
                        : ($0.offset == 2 ? .hovered : .idle)
                    return $0.element.card.visualState == expected
                }
            taggedCards[2].card.setPointerInsideForSmoke(false)
            visualStateOK = visualStateOK
                && taggedCards.enumerated().allSatisfy {
                    $0.element.card.visualState == ($0.offset == 1 ? .selected : .idle)
                }
        }
        if !visualStateOK {
            print("settings card visual-state smoke: stale or overlapping state")
        }

        selectPreviewTarget(
            routeID: SettingsCoreRoute.inputMethod.id,
            subpageID: SettingsSubpageID(rawValue: "dictionaries")
        )
        window.contentView?.layoutSubtreeIfNeeded()
        let lexiconButtons = descendants(of: SettingsLexiconButton.self,
                                         in: contentHost)
        pointerPolicyOK = pointerPolicyOK
            && descendants(of: NSButton.self, in: contentHost).allSatisfy {
                $0 is SettingsPointingButton
            }
            && descendants(of: NSSegmentedControl.self, in: contentHost)
                .allSatisfy { $0 is SettingsPointingSegmentedControl }
        let lexiconLabels = Set(
            descendants(of: NSTextField.self, in: contentHost).map(\.stringValue)
        )
        let expectedLexiconTitles: Set<String> = [
            "雾凇拼音", "五笔86", "Easy English",
        ]
        var lexiconOK = expectedLexiconTitles.isSubset(of: lexiconLabels)
            && lexiconButtons.count == UserLexiconKind.allCases.count * 2
        for kind in UserLexiconKind.allCases {
            let actions = lexiconButtons.filter { $0.lexiconKind == kind }
            lexiconOK = lexiconOK
                && actions.count == 2
                && Set(actions.map(\.title)) == ["导入学习…", "导出学习…"]
        }
        if !lexiconOK {
            print("settings lexicon card smoke: missing or misrouted dictionary actions")
        }
        if !pointerPolicyOK {
            print("settings pointing-hand smoke: missing enabled-aware cursor owner")
        }

        if themeOK && encodingOK && cursorOwnershipOK && visualStateOK && lexiconOK
            && pointerPolicyOK {
            print("settings card hit-test smoke: OK")
        }
        return themeOK && encodingOK && cursorOwnershipOK && visualStateOK && lexiconOK
            && pointerPolicyOK
    }

    @discardableResult
    private func renderCurrentView(to path: String) -> Bool {
        guard let window, let content = window.contentView else { return false }
        window.setContentSize(Self.previewContentSize)
        content.layoutSubtreeIfNeeded()
        let actualSize = content.bounds.size
        guard abs(actualSize.width - Self.previewContentSize.width) < 0.5,
              abs(actualSize.height - Self.previewContentSize.height) < 0.5 else {
            print("settings render size drifted to \(actualSize.width)x\(actualSize.height)")
            return false
        }
        guard validatePreviewStructure(in: content) else { return false }
        content.display()
        guard let rep = content.bitmapImageRepForCachingDisplay(in: content.bounds) else { return false }
        content.cacheDisplay(in: content.bounds, to: rep)
        let pixelsAreValid = validatePreviewPixels(in: content, bitmap: rep)
        guard let data = rep.representation(using: .png, properties: [:]) else { return false }
        do {
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
            return pixelsAreValid
        } catch {
            print("settings render failed \(path): \(error.localizedDescription)")
            return false
        }
    }

    /// Keep the off-screen renderer useful as an assembly smoke, not merely a
    /// screenshot command. Every route retains navigation, heading and body,
    /// with status embedded in the top navigation rather than a footer.
    private func validatePreviewStructure(in content: NSView) -> Bool {
        let required: [(String, CGFloat?)] = [
            ("settings.sidebar", nil),
            ("settings.subpage-bar", 46),
            ("settings.page-heading", 84),
            ("settings.page-scroll", nil),
            ("settings.status-bar", 30),
        ]
        for (identifier, expectedHeight) in required {
            guard let view = descendant(
                identifiedBy: NSUserInterfaceItemIdentifier(identifier),
                in: content
            ) else {
                print("settings render missing required view: \(identifier)")
                return false
            }
            if let expectedHeight,
               abs(view.frame.height - expectedHeight) >= 0.5 {
                print("settings render \(identifier) height drifted to \(view.frame.height)")
                return false
            }
        }
        guard let tabs = descendant(identifiedBy: .init("settings.subpage-bar"), in: content),
              let status = descendant(identifiedBy: .init("settings.status-bar"), in: content),
              status.superview === tabs,
              tabs.bounds.insetBy(dx: -0.5, dy: -0.5).contains(status.frame),
              !tabs.hasAmbiguousLayout, !status.hasAmbiguousLayout else {
            print("settings render status must stay inside the top navigation")
            return false
        }
        guard let sidebarView = descendant(
            identifiedBy: NSUserInterfaceItemIdentifier("settings.sidebar"),
            in: content
        ), abs(sidebarView.frame.width - 160) < 0.5 else {
            print("settings render sidebar content width drifted")
            return false
        }
        if selectedCoreRoute == .mailbox || selectedCoreRoute == .capsule {
            let routeName = selectedCoreRoute == .mailbox ? "mailbox" : "capsule"
            let configurationID = NSUserInterfaceItemIdentifier(
                "settings.\(routeName)-configuration"
            )
            let legacyPaneID = NSUserInterfaceItemIdentifier(
                "settings.\(routeName)-pane"
            )
            guard let configuration = descendant(
                identifiedBy: configurationID,
                in: content
            ), descendant(identifiedBy: legacyPaneID, in: content) == nil else {
                print("settings render \(routeName) embedded a management pane")
                return false
            }
            guard let scroll = descendant(
                identifiedBy: NSUserInterfaceItemIdentifier("settings.page-scroll"),
                in: content
            ) as? NSScrollView else {
                print("settings render \(routeName) configuration is not scrollable")
                return false
            }
            let configurationRect = configuration.convert(
                configuration.bounds,
                to: content
            )
            let viewportRect = scroll.contentView.convert(
                scroll.contentView.bounds,
                to: content
            )
            guard configurationRect.minX >= viewportRect.minX - 0.5,
                  configurationRect.maxX <= viewportRect.maxX + 0.5,
                  scroll.hasHorizontalScroller == false else {
                print("settings render \(routeName) overflows 980pt preview width")
                return false
            }
            let routeSpecificIDs: [String] = selectedCoreRoute == .mailbox
                ? [
                    "settings.mailbox-actions",
                    "settings.utility-shortcut.openMailbox",
                ]
                : [
                    "settings.capsule-passcode-configuration",
                    "settings.capsule-cloud-actions",
                    "settings.utility-shortcut.toggleClipboardHistory",
                ]
            for identifier in routeSpecificIDs where descendant(
                identifiedBy: NSUserInterfaceItemIdentifier(identifier),
                in: configuration
            ) == nil {
                print("settings render \(routeName) missing configuration: \(identifier)")
                return false
            }
        }
        return true
    }

    /// Catch compositing failures that leave a structurally complete AppKit
    /// hierarchy covered by one flat fill. AX/frame validation alone cannot
    /// distinguish that state from a correctly rendered settings page.
    private func validatePreviewPixels(in content: NSView,
                                       bitmap: NSBitmapImageRep) -> Bool {
        guard bitmap.pixelsWide > 0,
              bitmap.pixelsHigh > 0,
              content.bounds.width > 0,
              content.bounds.height > 0 else {
            print("settings render produced an empty bitmap")
            return false
        }

        let regionIDs = [
            "settings.sidebar",
            "settings.subpage-bar",
            "settings.page-heading",
            "settings.page-scroll",
        ]
        let scaleX = CGFloat(bitmap.pixelsWide) / content.bounds.width
        let scaleY = CGFloat(bitmap.pixelsHigh) / content.bounds.height

        func rgb(_ color: NSColor?) -> (CGFloat, CGFloat, CGFloat)? {
            guard let value = color?.usingColorSpace(.sRGB) else { return nil }
            return (value.redComponent, value.greenComponent, value.blueComponent)
        }

        func pixel(at point: NSPoint, bitmapYFlipped: Bool) -> NSColor? {
            let rawX = Int((point.x - content.bounds.minX) * scaleX)
            let rawY = Int((point.y - content.bounds.minY) * scaleY)
            let x = min(max(rawX, 0), bitmap.pixelsWide - 1)
            let lowerY = min(max(rawY, 0), bitmap.pixelsHigh - 1)
            let y = bitmapYFlipped ? bitmap.pixelsHigh - 1 - lowerY : lowerY
            return bitmap.colorAt(x: x, y: y)
        }

        // cacheDisplay stores rows from the bitmap's top edge. The root view
        // uses AppKit coordinates; do not infer orientation from a footer fill,
        // because status now shares the header's background.
        let bitmapYFlipped = !content.isFlipped

        var allRegionsVisible = true
        for identifier in regionIDs {
            guard let view = descendant(
                identifiedBy: NSUserInterfaceItemIdentifier(identifier),
                in: content
            ) else {
                allRegionsVisible = false
                continue
            }
            let rect = view.convert(view.bounds, to: content)
                .intersection(content.bounds)
                .insetBy(dx: 3, dy: 3)
            guard !rect.isEmpty else {
                print("settings render pixel region is empty: \(identifier)")
                allRegionsVisible = false
                continue
            }

            var minimumLuminance: CGFloat = 1
            var maximumLuminance: CGFloat = 0
            var colorBuckets = Set<Int>()
            var y = rect.minY
            while y < rect.maxY {
                var x = rect.minX
                while x < rect.maxX {
                    if let (red, green, blue) = rgb(pixel(
                        at: NSPoint(x: x, y: y),
                        bitmapYFlipped: bitmapYFlipped
                    )) {
                        let luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue
                        minimumLuminance = min(minimumLuminance, luminance)
                        maximumLuminance = max(maximumLuminance, luminance)
                        let bucket = (Int(red * 15) << 8)
                            | (Int(green * 15) << 4)
                            | Int(blue * 15)
                        colorBuckets.insert(bucket)
                    }
                    x += 2
                }
                y += 2
            }

            let luminanceSpan = maximumLuminance - minimumLuminance
            if colorBuckets.count < 4 || luminanceSpan < 0.08 {
                print(
                    "settings render region is visually blank: \(identifier) "
                        + "(colors=\(colorBuckets.count), luminanceSpan=\(luminanceSpan))"
                )
                allRegionsVisible = false
            }
        }
        return allRegionsVisible
    }

    private func descendant(identifiedBy identifier: NSUserInterfaceItemIdentifier,
                            in root: NSView) -> NSView? {
        if root.identifier == identifier { return root }
        for child in root.subviews {
            if let match = descendant(identifiedBy: identifier, in: child) {
                return match
            }
        }
        return nil
    }

    // MARK: UI construction

    private func build() {
        let win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 980, height: 680),
                           styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                           backing: .buffered, defer: false)
        win.title = "\(ProductIdentity.displayName) 设置"
        win.isReleasedWhenClosed = false
        win.delegate = self
        win.minSize = NSSize(width: 860, height: 600)
        win.appearance = RimeUI.appKitAppearance
        win.backgroundColor = SettingsVisualStyle.background
        win.isOpaque = !RimeUI.usesLiquidGlassTransparency
        win.titlebarAppearsTransparent = true

        configureControls()

        sidebar.orientation = .vertical
        sidebar.alignment = .leading
        sidebar.spacing = 4
        sidebar.edgeInsets = NSEdgeInsets(top: 16, left: 12, bottom: 16, right: 12)
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        sidebar.identifier = NSUserInterfaceItemIdentifier("settings.sidebar-content")
        rebuildSidebar()

        sidebarScrollView.drawsBackground = !RimeUI.usesLiquidGlassTransparency
        sidebarScrollView.backgroundColor = SettingsVisualStyle.background
        sidebarScrollView.borderType = .noBorder
        sidebarScrollView.hasVerticalScroller = true
        sidebarScrollView.hasHorizontalScroller = false
        sidebarScrollView.autohidesScrollers = true
        sidebarScrollView.horizontalScrollElasticity = .none
        sidebarScrollView.translatesAutoresizingMaskIntoConstraints = false
        sidebarScrollView.identifier = NSUserInterfaceItemIdentifier("settings.sidebar")
        sidebarDocumentView.translatesAutoresizingMaskIntoConstraints = false
        sidebarDocumentView.addSubview(sidebar)
        sidebarScrollView.documentView = sidebarDocumentView

        let divider = SettingsSeparatorView()
        divider.translatesAutoresizingMaskIntoConstraints = false

        contentHost.translatesAutoresizingMaskIntoConstraints = false
        contentHost.identifier = NSUserInterfaceItemIdentifier("settings.content")

        let background = SettingsBackgroundView()
        win.contentView = background
        // Material covers the whole window, including the native titlebar.
        // Controls stay inside AppKit's content layout guide so they never
        // overlap the traffic lights or the titlebar's drag region.
        let contentGuide = win.contentLayoutGuide as! NSLayoutGuide
        background.addSubview(sidebarScrollView)
        background.addSubview(divider)
        background.addSubview(contentHost)
        contentHost.setContentHuggingPriority(.defaultLow, for: .horizontal)
        contentHost.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        NSLayoutConstraint.activate([
            sidebarScrollView.leadingAnchor.constraint(equalTo: background.leadingAnchor),
            sidebarScrollView.topAnchor.constraint(equalTo: contentGuide.topAnchor),
            sidebarScrollView.bottomAnchor.constraint(equalTo: background.bottomAnchor),
            sidebarScrollView.widthAnchor.constraint(equalToConstant: 160),
            sidebarDocumentView.leadingAnchor.constraint(equalTo: sidebarScrollView.contentView.leadingAnchor),
            sidebarDocumentView.trailingAnchor.constraint(equalTo: sidebarScrollView.contentView.trailingAnchor),
            sidebarDocumentView.topAnchor.constraint(equalTo: sidebarScrollView.contentView.topAnchor),
            sidebarDocumentView.widthAnchor.constraint(equalTo: sidebarScrollView.contentView.widthAnchor),
            sidebarDocumentView.heightAnchor.constraint(greaterThanOrEqualTo: sidebarScrollView.contentView.heightAnchor),
            sidebar.leadingAnchor.constraint(equalTo: sidebarDocumentView.leadingAnchor),
            sidebar.trailingAnchor.constraint(equalTo: sidebarDocumentView.trailingAnchor),
            sidebar.topAnchor.constraint(equalTo: sidebarDocumentView.topAnchor),
            sidebar.bottomAnchor.constraint(equalTo: sidebarDocumentView.bottomAnchor),
            divider.leadingAnchor.constraint(equalTo: sidebarScrollView.trailingAnchor),
            divider.topAnchor.constraint(equalTo: contentGuide.topAnchor),
            divider.bottomAnchor.constraint(equalTo: background.bottomAnchor),
            divider.widthAnchor.constraint(equalToConstant: 1),
            contentHost.leadingAnchor.constraint(equalTo: divider.trailingAnchor),
            contentHost.trailingAnchor.constraint(equalTo: background.trailingAnchor),
            contentHost.topAnchor.constraint(equalTo: contentGuide.topAnchor),
            contentHost.bottomAnchor.constraint(equalTo: background.bottomAnchor),
        ])

        permissionRefreshObserver = NotificationCenter.default.addObserver(
            forName: NSWindow.didBecomeKeyNotification, object: window, queue: .main
        ) { [weak self] _ in
            guard let self, self.navigation.selectedSubpage()?.rawValue == "permissions" else { return }
            self.reload()
        }
        statsObserver = NotificationCenter.default.addObserver(
            forName: .keyFrequencyDidChange,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard self?.window?.isVisible == true,
                  self?.selectedBuiltInPluginID == BuiltInPluginID.statistics else { return }
            self?.refreshStats()
        }

        pluginObserver = NotificationCenter.default.addObserver(
            forName: ActionPluginManager.didChangeNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            guard let self,
                  notification.userInfo?[ActionPluginManager.rootPathUserInfoKey] as? String
                    == ActionPluginManager.shared.rootURL.path,
                  self.window?.isVisible == true,
                  self.selectedCoreRoute == .plugins else { return }
            self.schedulePluginListRefresh()
        }

        registryObserver = NotificationCenter.default.addObserver(
            forName: .pluginRegistryDidChange,
            object: PluginRegistry.shared,
            queue: .main
        ) { [weak self] _ in
            DispatchQueue.main.async { [weak self] in
                guard let self, self.window?.isVisible == true else { return }
                self.rebuildRouteCatalog()
                self.showCurrentRoute()
            }
        }

        activeBufferPluginObserver = NotificationCenter.default.addObserver(
            forName: .activeBufferPluginDidChange,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self,
                  self.window?.isVisible == true,
                  self.selectedCoreRoute == .plugins else { return }
            self.schedulePluginListRefresh()
        }

        inputConfigurationObserver = NotificationCenter.default.addObserver(
            forName: .inputConfigurationDidChange,
            object: InputConfigurationStore.shared,
            queue: .main
        ) { [weak self] _ in
            self?.refreshInputConfigurationSelection()
        }
        dictionaryHealthObserver = NotificationCenter.default.addObserver(
            forName: .rimeUserDictionaryHealthChanged, object: nil, queue: .main
        ) { [weak self] _ in
            self?.dictionaryWarningLabel?.stringValue = UserDictionaryHealth.shared.warning ?? ""
            self?.dictionaryWarningLabel?.isHidden = UserDictionaryHealth.shared.warning == nil
        }

        aiConnectorObserver = NotificationCenter.default.addObserver(
            forName: .aiTextConnectorDidChange,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self else { return }
            self.refreshAIConnectorSelection()
            guard self.window?.isVisible == true,
                  self.selectedCoreRoute == .connectors,
                  self.navigation.selectedSubpage()?.rawValue == "ai-model" else { return }
            // Connector selection notifications are synchronous. Rebuild after
            // the card action has returned so the selected control is not
            // removed while AppKit is still dispatching its click.
            DispatchQueue.main.async { [weak self] in
                self?.showCurrentRoute()
            }
        }
        aiConnectorAvailabilityObserver = NotificationCenter.default.addObserver(
            forName: .aiTextConnectorAvailabilityDidChange,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            guard let self,
                  let rawKind = notification.userInfo?["kind"] as? String,
                  let kind = AITextProviderKind(rawValue: rawKind) else { return }
            if kind == .claudeCodeCLI, self.claudeLoginOperation == nil {
                self.claudeLoginFeedback = nil
                self.claudeLoginFeedbackIsError = false
            }
            if kind == .codexCLI, self.codexLoginOperation == nil {
                self.codexLoginFeedback = nil
                self.codexLoginFeedbackIsError = false
            }
            guard self.window?.isVisible == true,
                  self.selectedCoreRoute == .connectors,
                  self.navigation.selectedSubpage()?.rawValue == "ai-model" else { return }
            DispatchQueue.main.async { [weak self] in
                self?.showCurrentRoute()
            }
        }

        appearanceObserver = NotificationCenter.default.addObserver(
            forName: .rimeAppearanceDidChange,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            // The popup posts synchronously. Rebuild on the next run-loop turn
            // so its action is never removing the control that is dispatching.
            DispatchQueue.main.async { [weak self] in
                self?.applySelectedAppearance(rebuildVisibleRoute: true)
            }
        }

        capsuleSyncSettingsObserver = NotificationCenter.default.addObserver(
            forName: .capsuleCloudSyncStatusDidChange,
            object: CapsuleCloudSyncController.shared,
            queue: .main
        ) { [weak self] _ in
            guard let self,
                  self.window?.isVisible == true,
                  self.selectedCoreRoute == .capsule else { return }
            self.refreshCapsuleSyncSettingsControls()
        }

        mailboxSettingsObservation = MailboxStore.shared.observe(
            deliverInitial: false
        ) { [weak self] _ in
            guard let self,
                  self.window?.isVisible == true,
                  self.selectedCoreRoute == .mailbox else { return }
            self.refreshMailboxSettingsStatus()
        }

        window = win
    }

    private func applySelectedAppearance(rebuildVisibleRoute: Bool) {
        guard let window else { return }
        window.appearance = RimeUI.appKitAppearance
        window.backgroundColor = SettingsVisualStyle.background
        window.isOpaque = !RimeUI.usesLiquidGlassTransparency
        window.titlebarAppearsTransparent = true
        sidebarScrollView.drawsBackground = !RimeUI.usesLiquidGlassTransparency
        sidebarScrollView.backgroundColor = SettingsVisualStyle.background
        pluginConfigurationSheet?.appearance = RimeUI.appKitAppearance
        settingsStatusLabel.textColor = RimeUI.textMuted
        settingsRouteLabel.textColor = RimeUI.textMuted
        installStatus.textColor = RimeUI.textMuted
        statsTopKey.textColor = RimeUI.textSecondary
        candidateMetricSliders.values.forEach { $0.trackFillColor = RimeUI.accentGreen }
        bufferWidthSlider.trackFillColor = RimeUI.accentGreen
        // A theme change can preserve the effective Aqua appearance. In that
        // case AppKit does not invalidate the custom-drawn chrome for us.
        func invalidateChrome(_ view: NSView) {
            view.needsDisplay = true
            view.subviews.forEach(invalidateChrome)
        }
        if let content = window.contentView { invalidateChrome(content) }
        refreshSidebarSelection()
        guard rebuildVisibleRoute, window.isVisible else { return }
        reload()
        showCurrentRoute()
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        confirmLeavingChordEditor()
    }

    private func confirmLeavingChordEditor() -> Bool {
        guard !ChordKeymapActivationCoordinator.shared.isApplying,
              !chordExtensionDeploymentInProgress else { return false }
        return (activePluginSettingsController as? FlyChordLearningSettingsViewController)?
            .confirmCanLeave() ?? true
    }

    func windowWillClose(_ notification: Notification) {
        guard let closingWindow = notification.object as? NSWindow,
              closingWindow === window else { return }
        if let sheet = pluginConfigurationSheet {
            window?.endSheet(sheet)
            sheet.orderOut(nil)
            pluginConfigurationSheet = nil
        }
        if let operation = codexLoginOperation {
            codexLoginCancelling = true
            codexAuthorizationURL = nil
            codexLoginFeedback = "正在取消 Codex 登录…"
            codexLoginFeedbackIsError = false
            operation.cancel()
        }
        if let operation = claudeLoginOperation {
            claudeLoginCancelling = true
            claudeLoginFeedback = "正在取消 Claude 登录…"
            claudeLoginFeedbackIsError = false
            operation.cancel()
        }
        // The controller is a process-lifetime singleton, but dynamic plugin
        // pages must not be: they observe high-frequency metric stores. Drop
        // the hosted view/controller so a closed Settings window does no
        // hidden AppKit work on the IME main thread.
        activePluginSettingsController?.viewWillDisappear()
        contentHost.subviews.forEach { $0.removeFromSuperview() }
        activePluginSettingsController = nil
        candidatePreview = nil
        StandaloneWindowFocusCoordinator.shared.windowWillClose(closingWindow)
    }

    private func configureControls() {
        for (index, encoding) in InputEncoding.allCases.enumerated() {
            let button = RimeFixedAccentChoiceButton.radio(
                title: encoding.title,
                target: self,
                action: #selector(inputEncodingSelected(_:))
            )
            button.tag = index
            button.font = .systemFont(ofSize: 13, weight: .medium)
            button.translatesAutoresizingMaskIntoConstraints = false
            encodingRadios[encoding] = button
        }
        for (index, kind) in AITextProviderKind.allCases.enumerated() {
            let button = RimeFixedAccentChoiceButton.radio(
                title: kind.displayName,
                target: self,
                action: #selector(aiConnectorSelected(_:))
            )
            button.tag = index
            button.font = .systemFont(ofSize: 13, weight: .medium)
            button.translatesAutoresizingMaskIntoConstraints = false
            aiConnectorRadios[kind] = button
        }
        codexLoginButton.target = self
        codexLoginButton.action = #selector(codexLoginButtonPressed)
        codexCopyLoginLinkButton.target = self
        codexCopyLoginLinkButton.action = #selector(copyCodexLoginLink)
        codexCopyLoginLinkButton.isHidden = true
        codexLoginSpinner.style = .spinning
        codexLoginSpinner.controlSize = .small
        codexLoginSpinner.isDisplayedWhenStopped = false
        codexLoginSpinner.translatesAutoresizingMaskIntoConstraints = false
        codexLoginSpinner.widthAnchor.constraint(equalToConstant: 16).isActive = true
        codexLoginSpinner.heightAnchor.constraint(equalToConstant: 16).isActive = true
        codexLoginStatusLabel.font = .systemFont(ofSize: 11)
        codexLoginStatusLabel.textColor = RimeUI.textMuted
        codexLoginStatusLabel.translatesAutoresizingMaskIntoConstraints = false
        codexLoginStatusLabel.widthAnchor.constraint(equalToConstant: 626).isActive = true
        claudeLoginButton.target = self
        claudeLoginButton.action = #selector(claudeLoginButtonPressed)
        claudeLoginSpinner.style = .spinning
        claudeLoginSpinner.controlSize = .small
        claudeLoginSpinner.isDisplayedWhenStopped = false
        claudeLoginSpinner.translatesAutoresizingMaskIntoConstraints = false
        claudeLoginSpinner.widthAnchor.constraint(equalToConstant: 16).isActive = true
        claudeLoginSpinner.heightAnchor.constraint(equalToConstant: 16).isActive = true
        claudeLoginStatusLabel.font = .systemFont(ofSize: 11)
        claudeLoginStatusLabel.textColor = RimeUI.textMuted
        claudeLoginStatusLabel.translatesAutoresizingMaskIntoConstraints = false
        claudeLoginStatusLabel.widthAnchor.constraint(equalToConstant: 626).isActive = true
        alignToInputBoxCheck.target = self
        alignToInputBoxCheck.action = #selector(alignToInputBoxToggled)
        alignToInputBoxCheck.setAccessibilityLabel("工作台对齐目标输入框")
        alignToInputBoxStatusLabel.font = .systemFont(ofSize: 11)
        alignToInputBoxStatusLabel.textColor = RimeUI.textMuted
        clipboardHistoryCheck.target = self
        clipboardHistoryCheck.action = #selector(clipboardHistoryToggled)
        clipboardHistoryCheck.setAccessibilityLabel("允许独立 Clipboard History 收录剪贴板内容")
        accessibilityGrantButton.target = self
        accessibilityGrantButton.action = #selector(requestAccessibilityGrant)
        clipboardAutoPasteCheck.target = self
        clipboardAutoPasteCheck.action = #selector(clipboardAutoPasteToggled)
        clipboardAutoPasteCheck.setAccessibilityLabel("回车后自动粘贴")
        clipboardAutoPasteStatusLabel.font = .systemFont(ofSize: 11)
        clipboardAutoPasteStatusLabel.textColor = RimeUI.textMuted
        closeAfterLastDeliveryCheck.target = self
        closeAfterLastDeliveryCheck.action = #selector(closeAfterLastDeliveryToggled)
        closeAfterLastDeliveryCheck.setAccessibilityLabel("最后一块上屏后关闭工作台")
        moveBufferWindowButton.target = self
        moveBufferWindowButton.action = #selector(moveBufferWindow)
        resetOnAppSwitchCheck.target = self
        resetOnAppSwitchCheck.action = #selector(resetOnAppSwitchToggled)
        resetOnAppSwitchCheck.setAccessibilityLabel("切换应用时清空本地缓冲")
        gatewayEnableCheck.target = self
        gatewayEnableCheck.action = #selector(gatewayToggled)
        gatewayEnableCheck.setAccessibilityLabel("启用本地网关")
        gatewayClientTabs.target = self
        gatewayClientTabs.action = #selector(gatewayClientChosen(_:))
        gatewayClientTabs.segmentStyle = .rounded
        gatewayClientTabs.segmentDistribution = .fillEqually
        gatewayClientTabs.selectedSegmentBezelColor = RimeUI.accentGreen
        gatewayClientTabs.controlSize = .small
        gatewayClientTabs.translatesAutoresizingMaskIntoConstraints = false
        gatewayClientTabs.widthAnchor.constraint(equalToConstant: 626).isActive = true
        for client in GatewayClientConfiguration.allCases {
            gatewayClientTabs.setToolTip(client.title, forSegment: client.rawValue)
        }
        gatewayConfigField.isEditable = false
        gatewayConfigField.isSelectable = true
        gatewayConfigField.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        gatewayConfigField.lineBreakMode = .byCharWrapping
        gatewayConfigField.maximumNumberOfLines = 12
        gatewayConfigField.translatesAutoresizingMaskIntoConstraints = false
        gatewayConfigField.widthAnchor.constraint(equalToConstant: 626).isActive = true
        gatewayCopyConfigButton.target = self
        gatewayCopyConfigButton.action = #selector(copyGatewayConfig)
        gatewaySupplementaryField.isEditable = false
        gatewaySupplementaryField.isSelectable = true
        gatewaySupplementaryField.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        gatewaySupplementaryField.lineBreakMode = .byCharWrapping
        gatewaySupplementaryField.maximumNumberOfLines = 4
        gatewaySupplementaryField.translatesAutoresizingMaskIntoConstraints = false
        gatewaySupplementaryField.widthAnchor.constraint(equalToConstant: 626).isActive = true
        gatewayCopySupplementaryButton.target = self
        gatewayCopySupplementaryButton.action = #selector(copyGatewaySupplementaryConfiguration)

        aiProviderPicker.target = self
        aiProviderPicker.action = #selector(aiProviderSelected(_:))
        aiProviderPicker.translatesAutoresizingMaskIntoConstraints = false
        aiProviderPicker.widthAnchor.constraint(equalToConstant: 236).isActive = true
        aiProviderNameField.placeholderString = "例如：CometAPI / OpenRouter"
        aiProviderNameField.font = .systemFont(ofSize: 11)
        aiBaseURLField.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        aiBaseURLField.placeholderString = "https://api.openai.com/v1"
        aiDocumentationURLField.placeholderString = "https://provider.example/docs（可选）"
        aiDocumentationURLField.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        aiModelField.placeholderString = "普通文本模型（留空即停用）"
        aiModelField.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        aiAPIKeyField.placeholderString = "API Key（留空保持不变）"
        aiAPIKeyField.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        for field in [
            aiProviderNameField,
            aiBaseURLField,
            aiDocumentationURLField,
            aiModelField,
            aiAPIKeyField,
        ] {
            field.translatesAutoresizingMaskIntoConstraints = false
            field.widthAnchor.constraint(equalToConstant: 420).isActive = true
        }
        aiConfigurationStatus.font = .systemFont(ofSize: 11)
        aiConfigurationStatus.textColor = RimeUI.textMuted
        aiConfigurationStatus.lineBreakMode = .byTruncatingTail
        aiConfigurationStatus.translatesAutoresizingMaskIntoConstraints = false
        aiConfigurationStatus.widthAnchor.constraint(equalToConstant: 626).isActive = true

        appearancePopUp.removeAllItems()
        for mode in RimeAppearanceMode.allCases {
            appearancePopUp.addItem(withTitle: mode.selectionTitle)
            appearancePopUp.lastItem?.representedObject = mode.rawValue
        }
        appearancePopUp.target = self
        appearancePopUp.action = #selector(appearanceChosen)
        configureCandidateMetricControls()

        statsDatePicker.datePickerElements = [.yearMonthDay]
        statsDatePicker.datePickerStyle = .textFieldAndStepper
        statsDatePicker.dateValue = Date()
        statsDatePicker.target = self
        statsDatePicker.action = #selector(statsDateChanged)

        statsSummary.font = .systemFont(ofSize: 13, weight: .semibold)
        statsTopKey.font = .systemFont(ofSize: 12)
        statsTopKey.textColor = RimeUI.textSecondary
        installStatus.font = .systemFont(ofSize: 11)
        installStatus.textColor = RimeUI.textMuted
        heatmapView.translatesAutoresizingMaskIntoConstraints = false
        heatmapView.heightAnchor.constraint(greaterThanOrEqualToConstant: 260).isActive = true

        pluginStatusLabel.font = .systemFont(ofSize: 11)
        pluginStatusLabel.textColor = RimeUI.textSecondary
        pluginStatusLabel.lineBreakMode = .byTruncatingTail

        settingsStatusLabel.font = .systemFont(ofSize: 9)
        settingsStatusLabel.textColor = RimeUI.textMuted
        settingsStatusLabel.lineBreakMode = .byTruncatingTail
        settingsStatusLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        settingsRouteLabel.font = .systemFont(ofSize: 9)
        settingsRouteLabel.textColor = RimeUI.textMuted
        settingsRouteLabel.setContentHuggingPriority(.required, for: .horizontal)
        settingsRouteLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        settingsRouteLabel.lineBreakMode = .byTruncatingMiddle

        pluginRowsStack.orientation = .vertical
        pluginRowsStack.alignment = .width
        pluginRowsStack.distribution = .fill
        pluginRowsStack.spacing = 6
        pluginRowsStack.translatesAutoresizingMaskIntoConstraints = false
        pluginRowsStack.setContentHuggingPriority(.required, for: .vertical)
        pluginRowsStack.setContentCompressionResistancePriority(.required, for: .vertical)
        pluginRowsStack.widthAnchor.constraint(equalToConstant: 650).isActive = true
    }

    private func configureCandidateMetricControls() {
        for metric in CandidateWindowMetric.allCases {
            let formatter = NumberFormatter()
            formatter.minimumFractionDigits = 0
            formatter.maximumFractionDigits = 0
            formatter.allowsFloats = false
            formatter.minimum = NSNumber(value: metric.range.lowerBound)
            formatter.maximum = NSNumber(value: metric.range.upperBound)

            let field = NSTextField(string: "")
            field.formatter = formatter
            field.alignment = .right
            field.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular)
            field.target = self
            field.action = #selector(candidateMetricFieldChanged(_:))
            field.delegate = self
            field.tag = metric.tag
            field.translatesAutoresizingMaskIntoConstraints = false
            field.widthAnchor.constraint(equalToConstant: 52).isActive = true

            let slider = NSSlider(value: metric.defaultValue,
                                  minValue: metric.range.lowerBound,
                                  maxValue: metric.range.upperBound,
                                  target: self,
                                  action: #selector(candidateMetricSliderChanged(_:)))
            slider.isContinuous = true
            slider.tag = metric.tag
            slider.trackFillColor = RimeUI.accentGreen
            slider.translatesAutoresizingMaskIntoConstraints = false
            slider.widthAnchor.constraint(equalToConstant: 190).isActive = true

            let hint = NSTextField(labelWithString: "")
            hint.font = .systemFont(ofSize: 10)
            hint.textColor = RimeUI.textMuted
            hint.isHidden = true

            candidateMetricFields[metric] = field
            candidateMetricSliders[metric] = slider
            candidateMetricHints[metric] = hint
        }

        let widthFormatter = NumberFormatter()
        widthFormatter.minimumFractionDigits = 0
        widthFormatter.maximumFractionDigits = 0
        widthFormatter.allowsFloats = false
        widthFormatter.minimum = NSNumber(
            value: Double(BufferWindowGeometry.standardMinimumWidth)
        )
        widthFormatter.maximum = NSNumber(
            value: Double(BufferWindowGeometry.standardMaximumWidth)
        )
        bufferWidthField.formatter = widthFormatter
        bufferWidthField.alignment = .right
        bufferWidthField.font = .monospacedDigitSystemFont(
            ofSize: 12,
            weight: .regular
        )
        bufferWidthField.target = self
        bufferWidthField.action = #selector(bufferWidthFieldChanged)
        bufferWidthField.delegate = self
        bufferWidthField.translatesAutoresizingMaskIntoConstraints = false
        bufferWidthField.widthAnchor.constraint(equalToConstant: 58).isActive = true

        bufferWidthSlider.target = self
        bufferWidthSlider.action = #selector(bufferWidthSliderChanged)
        bufferWidthSlider.isContinuous = true
        bufferWidthSlider.trackFillColor = RimeUI.accentGreen
        bufferWidthSlider.translatesAutoresizingMaskIntoConstraints = false
        bufferWidthSlider.widthAnchor.constraint(equalToConstant: 260).isActive = true

        shortcutFeedbackLabel.font = .systemFont(ofSize: 11)
        shortcutFeedbackLabel.textColor = RimeUI.textMuted
        shortcutFeedbackLabel.isHidden = true
    }

    private var selectedRoute: SettingsRouteDescriptor? {
        routeCatalog.route(for: navigation.currentRouteID)
    }

    private var selectedCoreRoute: SettingsCoreRoute? {
        guard case let .core(route)? = selectedRoute?.source else { return nil }
        return route
    }

    private var selectedBuiltInPluginID: String? {
        guard case let .builtInPlugin(key)? = selectedRoute?.source else { return nil }
        return key.rawID
    }

    private func rebuildRouteCatalog() {
        do {
            let next = try SettingsRouteCatalog(
                pluginContributions: PluginRegistry.shared.enabledSettingsContributions()
            )
            routeCatalog = next
            navigation.reconcile(with: next)
            if window != nil { rebuildSidebar() }
        } catch {
            IMELog.write("settings route catalog rejected: \(error)")
        }
    }

    private func rebuildSidebar() {
        sidebar.arrangedSubviews.forEach {
            sidebar.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }
        navButtons.removeAll()
        for (sectionIndex, section) in routeCatalog.sections.enumerated() {
            sidebar.addArrangedSubview(
                sidebarGroupHeader(section.title, first: sectionIndex == 0)
            )
            for route in section.routes {
                let button = SettingsRouteButton(
                    title: route.title,
                    target: self,
                    action: #selector(routeChosen(_:))
                )
                button.routeID = route.id
                button.configure(title: route.title, symbolName: route.symbolName)
                button.translatesAutoresizingMaskIntoConstraints = false
                sidebar.addArrangedSubview(button)
                button.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
                button.widthAnchor.constraint(
                    equalTo: sidebar.widthAnchor,
                    constant: -(sidebar.edgeInsets.left + sidebar.edgeInsets.right)
                ).isActive = true
                button.heightAnchor.constraint(equalToConstant: 32).isActive = true
                navButtons[route.id] = button
            }
        }
        sidebar.addArrangedSubview(flexSpacer())
        refreshSidebarSelection()
    }

    private func refreshSidebarSelection() {
        var selectedButton: NSButton?
        for (routeID, button) in navButtons {
            let selected = routeID == navigation.currentRouteID
            if selected { selectedButton = button }
            button.state = selected ? .on : .off
            if let routeButton = button as? SettingsRouteButton {
                routeButton.isRouteSelected = selected
                routeButton.updateVisualState()
            }
        }
        if let selectedButton {
            sidebar.layoutSubtreeIfNeeded()
            sidebarDocumentView.scrollToVisible(selectedButton.frame.insetBy(dx: 0, dy: -8))
        }
    }

    private func showCurrentRoute() {
        guard let route = selectedRoute else { return }
        cardDetailsPopover?.close()
        cardDetailsPopover = nil
        refreshSidebarSelection()
        // Plugin views are embedded directly, not as child controllers. Notify
        // the page before releasing its owner so active practice can freeze and
        // save an interrupted result before its editor disappears.
        activePluginSettingsController?.viewWillDisappear()
        activePluginSettingsController = nil
        contentHost.subviews.forEach { $0.removeFromSuperview() }

        let subpageID = navigation.selectedSubpage()?.rawValue
        let body: NSView
        switch route.source {
        case let .core(core):
            body = makeCorePage(core, subpageID: subpageID)
        case let .builtInPlugin(pluginKey):
            if let subpageID,
               let controller = PluginRegistry.shared.makeSettingsViewController(
                    pluginKey: pluginKey,
                    subpageID: subpageID
               ) {
                activePluginSettingsController = controller
                body = controller.view
            } else {
                body = contentColumn([])
            }
        }

        let pageView = pageShell(route: route, body: body)
        pageView.translatesAutoresizingMaskIntoConstraints = false
        contentHost.addSubview(pageView)
        NSLayoutConstraint.activate([
            pageView.leadingAnchor.constraint(equalTo: contentHost.leadingAnchor),
            pageView.trailingAnchor.constraint(equalTo: contentHost.trailingAnchor),
            pageView.topAnchor.constraint(equalTo: contentHost.topAnchor),
            pageView.bottomAnchor.constraint(equalTo: contentHost.bottomAnchor),
        ])

        switch route.source {
        case .core(.appearance): refreshCandidateMetricControls()
        case .core(.plugins): refreshPluginList()
        case .builtInPlugin(let key) where key.rawID == BuiltInPluginID.statistics:
            refreshStats()
        default: break
        }
    }

    private func makeCorePage(_ route: SettingsCoreRoute,
                              subpageID: String?) -> NSView {
        switch route {
        case .rimes: return rimesPage(subpageID: subpageID ?? "about")
        case .inputMethod: return inputPage(subpageID: subpageID ?? "encoding")
        case .appearance: return appearancePage(subpageID: subpageID ?? "theme")
        case .buffer: return bufferPage(subpageID: subpageID ?? "buffer")
        case .mailbox: return mailboxPage()
        case .capsule: return capsulePage()
        case .connectors: return connectionsPage(subpageID: subpageID ?? "ai-model")
        case .plugins: return pluginsPage(subpageID: subpageID ?? "all")
        case .maintenance: return maintenancePage(subpageID: subpageID ?? "update-restart")
        }
    }

    private func pageShell(route: SettingsRouteDescriptor, body: NSView) -> NSView {
        let tabs = SettingsPointingSegmentedControl(
            labels: route.subpages.map(\.title),
            trackingMode: .selectOne,
            target: self,
            action: #selector(subpageChosen(_:))
        )
        tabs.segmentStyle = .rounded
        tabs.segmentDistribution = .fillProportionally
        tabs.selectedSegmentBezelColor = RimeUI.accentGreen
        tabs.controlSize = .small
        tabs.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        for (index, subpage) in route.subpages.enumerated() {
            tabs.setToolTip(subpage.title, forSegment: index)
        }
        if let selected = navigation.selectedSubpage(),
           let index = route.subpages.firstIndex(where: { $0.id == selected }) {
            tabs.selectedSegment = index
        }

        let tabsBar = SettingsChromeView(fill: .settings, border: .bottom)
        tabsBar.identifier = NSUserInterfaceItemIdentifier("settings.subpage-bar")
        tabsBar.translatesAutoresizingMaskIntoConstraints = false
        tabs.translatesAutoresizingMaskIntoConstraints = false
        tabsBar.addSubview(tabs)
        NSLayoutConstraint.activate([
            tabsBar.heightAnchor.constraint(equalToConstant: 46),
            tabs.leadingAnchor.constraint(equalTo: tabsBar.leadingAnchor, constant: 24),
            tabs.centerYAnchor.constraint(equalTo: tabsBar.centerYAnchor),
            tabs.trailingAnchor.constraint(lessThanOrEqualTo: tabsBar.trailingAnchor,
                                           constant: -24),
        ])

        let selectedSubpage = route.subpages.first { $0.id == navigation.selectedSubpage() }
        let heading = route.id == SettingsCoreRoute.rimes.id
            ? (selectedSubpage?.id.rawValue == "about" ? "关于 \(ProductBrand.name)" : selectedSubpage?.title ?? "\(ProductBrand.name)")
            : route.title
        let headingTitle = NSTextField(labelWithString: heading)
        headingTitle.font = .systemFont(ofSize: 20, weight: .bold)
        headingTitle.textColor = RimeUI.textPrimary
        headingTitle.lineBreakMode = .byTruncatingTail
        headingTitle.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        var headingViews: [NSView] = [headingTitle, flexSpacer()]
        if let configure = headerConfigurationButton(for: route) {
            headingViews.append(configure)
        }
        let headingRow = NSStackView(views: headingViews)
        headingRow.orientation = .horizontal
        headingRow.alignment = .centerY
        headingRow.spacing = 16
        headingRow.translatesAutoresizingMaskIntoConstraints = false
        let headingBar = SettingsChromeView(fill: .settings, border: .none)
        headingBar.identifier = NSUserInterfaceItemIdentifier("settings.page-heading")
        headingBar.translatesAutoresizingMaskIntoConstraints = false
        headingBar.addSubview(headingRow)
        NSLayoutConstraint.activate([
            headingBar.heightAnchor.constraint(equalToConstant: 84),
            headingRow.leadingAnchor.constraint(equalTo: headingBar.leadingAnchor, constant: 24),
            headingRow.trailingAnchor.constraint(equalTo: headingBar.trailingAnchor, constant: -24),
            headingRow.centerYAnchor.constraint(equalTo: headingBar.centerYAnchor),
            headingTitle.widthAnchor.constraint(lessThanOrEqualToConstant: 650),
        ])

        let bodyHost: NSView
        if body is NSScrollView {
            // Page-owned plugin controllers may preserve their own scroll positions;
            // do not nest them in another scroll view with zero intrinsic height.
            bodyHost = body
        } else {
            let scroll = NSScrollView()
            scroll.drawsBackground = false
            scroll.hasVerticalScroller = true
            scroll.autohidesScrollers = true
            let document = SettingsPageDocumentView()
            scroll.documentView = document
            document.translatesAutoresizingMaskIntoConstraints = false
            body.translatesAutoresizingMaskIntoConstraints = false
            document.addSubview(body)
            NSLayoutConstraint.activate([
                document.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
                document.trailingAnchor.constraint(equalTo: scroll.contentView.trailingAnchor),
                document.topAnchor.constraint(equalTo: scroll.contentView.topAnchor),
                document.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor),
                body.leadingAnchor.constraint(equalTo: document.leadingAnchor),
                body.trailingAnchor.constraint(equalTo: document.trailingAnchor),
                body.topAnchor.constraint(equalTo: document.topAnchor),
                body.bottomAnchor.constraint(equalTo: document.bottomAnchor),
            ])
            bodyHost = scroll
        }

        bodyHost.identifier = NSUserInterfaceItemIdentifier("settings.page-scroll")

        settingsStatusLabel.removeFromSuperview()
        settingsRouteLabel.removeFromSuperview()
        let subpageTitle = route.subpages.first { $0.id == navigation.selectedSubpage() }?.title
        settingsRouteLabel.stringValue = subpageTitle.map {
            $0 == route.title ? route.title : "\(route.title) · \($0)"
        } ?? route.title
        settingsRouteLabel.toolTip = "\(route.id.rawValue) · \(navigation.selectedSubpage()?.rawValue ?? "")"
        settingsRouteLabel.isHidden = route.id == SettingsCoreRoute.rimes.id
        let statusRow = NSStackView(
            views: [settingsStatusLabel, flexSpacer(), settingsRouteLabel]
        )
        statusRow.orientation = .horizontal
        statusRow.alignment = .centerY
        statusRow.spacing = 7
        statusRow.translatesAutoresizingMaskIntoConstraints = false
        let statusBar = SettingsChromeView(fill: .settings, border: .none)
        statusBar.identifier = NSUserInterfaceItemIdentifier("settings.status-bar")
        statusBar.translatesAutoresizingMaskIntoConstraints = false
        statusBar.addSubview(statusRow)
        tabsBar.addSubview(statusBar)
        NSLayoutConstraint.activate([
            statusBar.leadingAnchor.constraint(equalTo: tabs.trailingAnchor, constant: 16),
            statusBar.trailingAnchor.constraint(equalTo: tabsBar.trailingAnchor, constant: -12),
            statusBar.centerYAnchor.constraint(equalTo: tabsBar.centerYAnchor),
            statusBar.heightAnchor.constraint(equalToConstant: 30),
            statusRow.leadingAnchor.constraint(equalTo: statusBar.leadingAnchor, constant: 12),
            statusRow.trailingAnchor.constraint(equalTo: statusBar.trailingAnchor, constant: -12),
            statusRow.centerYAnchor.constraint(equalTo: statusBar.centerYAnchor),
        ])

        let root = NSStackView(views: [tabsBar, headingBar, bodyHost])
        root.orientation = .vertical
        root.alignment = .leading
        root.spacing = 0
        root.identifier = NSUserInterfaceItemIdentifier("settings.page-shell")
        tabsBar.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true
        headingBar.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true
        bodyHost.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true
        bodyHost.setContentHuggingPriority(.defaultLow, for: .vertical)
        bodyHost.setContentCompressionResistancePriority(.defaultLow, for: .vertical)
        return root
    }

    private func headerConfigurationButton(
        for route: SettingsRouteDescriptor
    ) -> NSButton? {
        guard case let .builtInPlugin(pluginKey) = route.source,
              PluginRegistry.shared.hasConfiguration(for: pluginKey) else { return nil }
        let button = SettingsPluginConfigurationButton(
            title: "",
            target: self,
            action: #selector(configureBufferPlugin(_:))
        )
        button.pluginKey = pluginKey
        button.image = NSImage(systemSymbolName: "gearshape",
                               accessibilityDescription: "配置 \(route.title)")?
            .withSymbolConfiguration(.init(pointSize: 14, weight: .medium))
        button.imagePosition = .imageOnly
        button.bezelStyle = .rounded
        button.controlSize = .small
        button.toolTip = "配置 \(route.title)"
        button.setAccessibilityLabel("配置 \(route.title)")
        button.translatesAutoresizingMaskIntoConstraints = false
        button.widthAnchor.constraint(equalToConstant: 28).isActive = true
        button.heightAnchor.constraint(equalToConstant: 28).isActive = true
        return button
    }

    private func sidebarGroupHeader(_ title: String, first: Bool) -> NSView {
        let label = NSTextField(labelWithString: title.uppercased())
        label.font = .systemFont(ofSize: 10, weight: .semibold)
        label.textColor = RimeUI.textMuted
        let wrap = NSStackView(views: [label])
        wrap.orientation = .horizontal
        wrap.edgeInsets = NSEdgeInsets(top: first ? 0 : 18, left: 8, bottom: 3, right: 8)
        return wrap
    }

    private func rimesPage(subpageID: String) -> NSView {
        if subpageID != "about" { return rimesLinkPage(subpageID: subpageID) }
        let logo = NSImageView()
        let logoURL = Bundle.main.url(forResource: "AppIcon", withExtension: "icns")
            ?? URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
                .appendingPathComponent("Logo/AppIcon.icns")
        logo.image = NSImage(contentsOf: logoURL) ?? NSApp.applicationIconImage
        logo.imageScaling = .scaleProportionallyUpOrDown
        logo.setAccessibilityElement(false)
        logo.identifier = .init("settings.rimes.logo")
        logo.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            logo.widthAnchor.constraint(equalToConstant: 88),
            logo.heightAnchor.constraint(equalToConstant: 88),
        ])

        let brand = NSTextField(labelWithString: ProductIdentity.displayName)
        let font = NSFont.systemFont(ofSize: 32, weight: .bold)
        brand.font = font.fontDescriptor.withDesign(.rounded)
            .flatMap { NSFont(descriptor: $0, size: 32) } ?? font
        brand.textColor = RimeUI.textPrimary
        let chinese = NSTextField(labelWithString: "世界对智者太过挑剔\n好奇的人需要朋友")
        chinese.font = .systemFont(ofSize: 13)
        chinese.textColor = RimeUI.textSecondary
        let english = NSTextField(labelWithString: "The world is too hard on the wise.\nCurious minds need friends.")
        english.font = .systemFont(ofSize: 11)
        english.textColor = RimeUI.textSecondary
        let identity = NSStackView(views: [brand, chinese, english])
        identity.orientation = .vertical
        identity.alignment = .leading
        identity.spacing = 6
        let hero = NSStackView(views: [logo, identity, flexSpacer()])
        hero.orientation = .horizontal
        hero.alignment = .centerY
        hero.spacing = 18
        hero.edgeInsets = NSEdgeInsets(top: 16, left: 16, bottom: 16, right: 16)
        hero.wantsLayer = true
        hero.layer?.backgroundColor = RimeUI.accentGreen.withAlphaComponent(0.08).cgColor
        hero.layer?.cornerRadius = RimeUI.isLiquidGlass ? 24 : 16
        hero.layer?.cornerCurve = .continuous
        hero.identifier = .init("settings.rimes.hero")
        hero.translatesAutoresizingMaskIntoConstraints = false
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        let credit = caption("\(version.map { "\(ProductBrand.name) \($0) · " } ?? "")学术海 · librime")
        let column = contentColumn([hero, spacer(4), credit])
        let preferredWidth = hero.widthAnchor.constraint(equalToConstant: 650)
        preferredWidth.priority = .defaultHigh
        NSLayoutConstraint.activate([
            preferredWidth,
            hero.widthAnchor.constraint(lessThanOrEqualTo: column.widthAnchor, constant: -48),
        ])
        return column
    }

    private func rimesLinkPage(subpageID: String) -> NSView {
        let title: String
        let summary: String
        let action: String
        let destination: String
        switch subpageID {
        case "website":
            (title, summary, action, destination) = (
                "xsime.com", "访问 \(ProductBrand.name) 官网，了解产品与项目动态。", "打开官网",
                "https://xsime.com")
        case "contact":
            (title, summary, action, destination) = (
                "pm@scholay.com", "反馈使用体验、问题或合作想法。", "写邮件",
                "mailto:pm@scholay.com")
        case "source":
            (title, summary, action, destination) = (
                "scholay / xsime", "查看源码、提交问题或参与改进。", "查看源码",
                "https://github.com/scholay/xsime")
        case "readme":
            (title, summary, action, destination) = (
                "从这里了解 \(ProductBrand.name)", "输入法、Buffer、Capsule 与 Mailbox 的功能和安装说明。", "阅读 README",
                "https://github.com/scholay/xsime/blob/main/README.md")
        default:
            (title, summary, action, destination) = (
                "Apache License 2.0", "查看 \(ProductBrand.name) 的开源许可原文。", "查看完整许可",
                "https://github.com/scholay/xsime/blob/main/LICENSE")
        }

        let heading = NSTextField(labelWithString: title)
        heading.font = .systemFont(ofSize: 22, weight: .semibold)
        heading.textColor = RimeUI.textPrimary
        heading.isSelectable = true
        let detail = NSTextField(labelWithString: summary)
        detail.font = .systemFont(ofSize: 13)
        detail.textColor = RimeUI.textSecondary
        detail.identifier = .init("settings.rimes.\(subpageID).summary")
        let button = SettingsBrandLinkButton(title: action, target: self,
                                              action: #selector(openBrandLink(_:)))
        button.destination = URL(string: destination)
        button.bezelStyle = .rounded
        button.controlSize = .regular
        button.toolTip = destination
        button.identifier = .init("settings.rimes.\(subpageID).open")
        return contentColumn([heading, detail, spacer(12), button])
    }

    @objc private func openBrandLink(_ sender: SettingsBrandLinkButton) {
        guard let destination = sender.destination else { return }
        if !NSWorkspace.shared.open(destination) {
            settingsStatusLabel.stringValue = "未能打开链接，请检查浏览器或邮件应用设置"
            settingsStatusLabel.textColor = .systemRed
        }
    }

    private func inputPage(subpageID: String) -> NSView {
        let dictionaryWarning = caption(UserDictionaryHealth.shared.warning ?? "")
        dictionaryWarningLabel = dictionaryWarning
        dictionaryWarning.textColor = .systemRed
        dictionaryWarning.isHidden = UserDictionaryHealth.shared.warning == nil
        dictionaryWarning.identifier = .init("settings.input.dictionary-warning")
        let importButton = SettingsPointingButton(title: "导入与管理 Rime 方案 / 文本词库…", target: self, action: #selector(openSchemeManager))
        let openDirBtn = SettingsPointingButton(
            title: "打开配置目录",
            target: self,
            action: #selector(openDir)
        )
        openDirBtn.toolTip = "配置目录是 ~/Library/\(RimesPaths.directoryName)。未显示的方案文件仅作为词典或反查依赖保留，不会出现在 F4。"

        switch subpageID {
        case "dictionaries":
            return contentColumn([
                dictionaryWarning,
                importButton,
                sectionLabel("已安装词库"),
                lexiconCard(kind: .chinese,
                            title: "雾凇拼音",
                            detail: "中文主词库 · 全拼、自然码双拼、小鹤双拼与飞耀方案共享"),
                lexiconCard(kind: .wubi86,
                            title: "五笔86",
                            detail: "五笔86 码表与独立用户词频"),
                lexiconCard(kind: .english,
                            title: "Easy English",
                            detail: "英文候选、补全、生词兜底与独立学习"),
                spacer(16),
                sectionLabel("用户学习"),
                openDirBtn,
            ])
        default:
            if let imported = MacRimeSchemeLibrary.shared.runtimePackage {
                return contentColumn([dictionaryWarning, sectionLabel("当前使用：\(imported.name)"),
                    caption("从方案管理切换包或恢复内置方案；各方案包的学习词库独立保存。"), importButton])
            }
            let chordStatus = chordSchemaStatusView()
            let recovery = caption(ChordKeymapActivationCoordinator.shared.recoveryMessage ?? "")
            recovery.textColor = .systemRed
            recovery.isHidden = ChordKeymapActivationCoordinator.shared.recoveryMessage == nil
            return contentColumn([
                dictionaryWarning,
                importButton,
                chordStatus,
                recovery,
                inputEncodingSelectionView(),
            ])
        }
    }

    @objc private func openSchemeManager() { MacRimeSchemeManager.shared.show() }

    private func inputModeCard(title: String,
                               detail: String,
                               active: Bool,
                               inactiveLabel: String = "规划中") -> NSView {
        let name = NSTextField(labelWithString: title)
        name.font = .systemFont(ofSize: 11, weight: .semibold)
        name.textColor = RimeUI.textPrimary
        let status = NSTextField(labelWithString: active ? "可用" : inactiveLabel)
        status.font = .systemFont(ofSize: 9, weight: .semibold)
        status.textColor = active ? themeStatusColor : RimeUI.textMuted
        let detailLabel = NSTextField(wrappingLabelWithString: detail)
        detailLabel.font = .systemFont(ofSize: 9)
        detailLabel.textColor = RimeUI.textMuted
        let header = NSStackView(views: [name, flexSpacer(), status])
        header.orientation = .horizontal
        let card = NSStackView(views: [header, detailLabel])
        card.orientation = .vertical
        card.alignment = .leading
        card.spacing = 5
        card.edgeInsets = NSEdgeInsets(top: 10, left: 12, bottom: 10, right: 12)
        card.wantsLayer = true
        card.layer?.backgroundColor = RimeUI.surface2.cgColor
        card.layer?.borderColor = RimeUI.border.cgColor
        card.layer?.borderWidth = SettingsVisualStyle.hairline(
            backingScale: window?.backingScaleFactor
        )
        card.layer?.cornerRadius = SettingsVisualStyle.cardRadius
        card.layer?.cornerCurve = .continuous
        card.translatesAutoresizingMaskIntoConstraints = false
        card.widthAnchor.constraint(equalToConstant: 650).isActive = true
        header.widthAnchor.constraint(equalTo: card.widthAnchor, constant: -24).isActive = true
        detailLabel.widthAnchor.constraint(equalTo: card.widthAnchor, constant: -24).isActive = true
        card.alphaValue = active ? 1 : 0.68
        return card
    }

    private func connectorDetailPanel(_ views: [NSView],
                                      enabled: Bool = true) -> NSView {
        let panel = NSStackView(views: views)
        panel.orientation = .vertical
        panel.alignment = .leading
        panel.spacing = 10
        panel.edgeInsets = NSEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)
        panel.wantsLayer = true
        panel.layer?.backgroundColor = RimeUI.surface2.cgColor
        panel.layer?.borderColor = RimeUI.border.cgColor
        panel.layer?.borderWidth = SettingsVisualStyle.hairline(
            backingScale: window?.backingScaleFactor
        )
        panel.layer?.cornerRadius = SettingsVisualStyle.panelRadius
        panel.layer?.cornerCurve = .continuous
        panel.translatesAutoresizingMaskIntoConstraints = false
        panel.widthAnchor.constraint(equalToConstant: 650).isActive = true
        panel.alphaValue = enabled ? 1 : 0.55
        return panel
    }

    private func connectorStatusBadge(_ text: String, active: Bool) -> NSTextField {
        let badge = NSTextField(labelWithString: text)
        badge.font = .systemFont(ofSize: 9, weight: .semibold)
        badge.textColor = active ? themeStatusColor : RimeUI.textMuted
        badge.setContentHuggingPriority(.required, for: .horizontal)
        return badge
    }

    /// The row's name is its identifier, not a description, so it stays on
    /// screen; `detail` is explanatory prose and stays off it, kept reachable
    /// only as a tooltip so the information isn't lost, just not always-on.
    private func settingsRow(title: String,
                             detail: String,
                             symbolName: String,
                             control: NSView,
                             width: CGFloat = 650,
                             nameSize: CGFloat = 11) -> NSView {
        control.removeFromSuperview()
        let icon = SettingsIconTileView(
            symbolName: symbolName,
            accessibilityDescription: title
        )
        let name = NSTextField(labelWithString: title)
        name.font = .systemFont(ofSize: nameSize, weight: .semibold)
        name.textColor = RimeUI.textPrimary
        name.alignment = .left
        name.lineBreakMode = .byTruncatingTail
        name.toolTip = detail

        let row = NSStackView(views: [icon, name, flexSpacer(), control])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 10
        row.edgeInsets = NSEdgeInsets(top: 8, left: 11, bottom: 8, right: 10)
        row.wantsLayer = true
        row.layer?.backgroundColor = RimeUI.surface2.cgColor
        row.layer?.borderColor = RimeUI.border.cgColor
        row.layer?.borderWidth = SettingsVisualStyle.hairline(
            backingScale: window?.backingScaleFactor
        )
        row.layer?.cornerRadius = SettingsVisualStyle.cardRadius
        row.layer?.cornerCurve = .continuous
        row.translatesAutoresizingMaskIntoConstraints = false
        row.widthAnchor.constraint(equalToConstant: width).isActive = true
        row.heightAnchor.constraint(greaterThanOrEqualToConstant: 46).isActive = true
        control.setContentHuggingPriority(.required, for: .horizontal)
        return row
    }

    private func utilityStatusBadge(_ text: String,
                                    active: Bool) -> NSTextField {
        let badge = NSTextField(labelWithString: text)
        badge.font = .systemFont(ofSize: 10, weight: .semibold)
        badge.alignment = .center
        badge.wantsLayer = true
        badge.layer?.borderWidth = SettingsVisualStyle.hairline(
            backingScale: window?.backingScaleFactor
        )
        badge.layer?.cornerRadius = RimeUI.isLiquidGlass ? 11 : 7
        badge.translatesAutoresizingMaskIntoConstraints = false
        badge.widthAnchor.constraint(greaterThanOrEqualToConstant: 62).isActive = true
        badge.heightAnchor.constraint(equalToConstant: 24).isActive = true
        applyUtilityStatusBadge(badge, text: text, active: active)
        return badge
    }

    private func applyUtilityStatusBadge(_ badge: NSTextField,
                                         text: String,
                                         active: Bool) {
        badge.stringValue = text
        badge.textColor = active ? themeStatusColor : RimeUI.textMuted
        badge.layer?.backgroundColor = (active
            ? RimeUI.accentGreen.withAlphaComponent(0.12)
            : RimeUI.surface3).cgColor
        badge.layer?.borderColor = (active
            ? RimeUI.accentGreen.withAlphaComponent(0.34)
            : RimeUI.border).cgColor
    }

    private func utilityShortcutRecorder(
        for action: RimeShortcutAction
    ) -> RimeShortcutRecorderButton {
        let recorder = RimeShortcutRecorderButton(action: action)
        recorder.identifier = NSUserInterfaceItemIdentifier(
            "settings.utility-shortcut.\(action.rawValue)"
        )
        recorder.onFeedback = { [weak self] message in
            guard let self else { return }
            self.settingsStatusLabel.stringValue = message
                ?? ""
            self.settingsStatusLabel.textColor = message?.contains("冲突") == true
                ? .systemRed
                : RimeUI.textMuted
        }
        return recorder
    }

    private func utilityActionRow(_ buttons: [NSButton]) -> NSView {
        let row = NSStackView(views: buttons)
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        row.translatesAutoresizingMaskIntoConstraints = false
        row.widthAnchor.constraint(lessThanOrEqualToConstant: 650).isActive = true
        return row
    }

    private func refreshMailboxSettingsStatus(
        snapshot: MailboxStoreSnapshot = MailboxStore.shared.snapshot
    ) {
        switch snapshot.persistence {
        case .available:
            if let badge = mailboxSettingsStatusBadge {
                applyUtilityStatusBadge(badge, text: "可用", active: true)
            }
            mailboxSettingsSummaryLabel?.stringValue =
                "当前有 \(snapshot.threads.count) 个会话，\(snapshot.unreadCount) 条未读。"
            mailboxSettingsStorageButton?.isEnabled = true
        case let .unavailable(reason):
            if let badge = mailboxSettingsStatusBadge {
                applyUtilityStatusBadge(badge, text: "不可用", active: false)
            }
            mailboxSettingsSummaryLabel?.stringValue = reason
            mailboxSettingsStorageButton?.isEnabled = false
        }
    }

    private func capsuleCloudStatusTitle(
        _ status: CapsuleCloudSyncStatus
    ) -> String {
        switch status.phase {
        case .unconfigured: return "未设置"
        case .unavailable: return "不可用"
        case .idle: return "等待同步"
        case .syncing: return "同步中"
        case .synced: return "已同步"
        case .failed: return "同步失败"
        }
    }

    private func capsuleCloudStatusDetail(
        _ status: CapsuleCloudSyncStatus
    ) -> String {
        var pieces: [String] = []
        if let folderName = status.folderName, !folderName.isEmpty {
            pieces.append("文件夹：\(folderName)")
        }
        pieces.append(status.message)
        if status.conflictCount > 0 {
            pieces.append("\(status.conflictCount) 个冲突")
        }
        if status.deferredCount > 0 {
            pieces.append("\(status.deferredCount) 个媒体待下载")
        }
        return pieces.joined(separator: " · ")
    }

    private func capsuleCloudActionRow(
        status: CapsuleCloudSyncStatus
    ) -> NSView {
        let choose = SettingsPointingButton(
            title: status.isConfigured ? "更换文件夹…" : "选择 iCloud 文件夹…",
            target: self,
            action: #selector(chooseCapsuleCloudFolder)
        )
        choose.bezelStyle = .rounded

        let sync = SettingsPointingButton(
            title: status.isBusy ? "正在同步…" : "立即同步",
            target: self,
            action: #selector(syncCapsuleCloudNow)
        )
        sync.bezelStyle = .rounded
        sync.bezelColor = RimeUI.accentGreen

        let disable = SettingsPointingButton(
            title: "停用自动同步",
            target: self,
            action: #selector(disableCapsuleCloudSync)
        )
        disable.bezelStyle = .rounded

        capsuleSyncSettingsSyncButton = sync
        capsuleSyncSettingsChooseButton = choose
        capsuleSyncSettingsDisableButton = disable
        let row = utilityActionRow([sync, choose, disable])
        row.identifier = NSUserInterfaceItemIdentifier(
            "settings.capsule-cloud-actions"
        )
        refreshCapsuleSyncSettingsControls(status: status)
        return row
    }

    private func refreshCapsuleSyncSettingsControls(
        status: CapsuleCloudSyncStatus = CapsuleCloudSyncController.shared.status
    ) {
        if let badge = capsuleSyncSettingsStatusBadge {
            applyUtilityStatusBadge(
                badge,
                text: capsuleCloudStatusTitle(status),
                active: status.phase == .idle || status.phase == .synced
                    || status.phase == .syncing
            )
        }
        capsuleSyncSettingsDetailLabel?.stringValue = capsuleCloudStatusDetail(status)
        capsuleSyncSettingsChooseButton?.title = status.isConfigured
            ? "更换文件夹…"
            : "选择 iCloud 文件夹…"
        capsuleSyncSettingsSyncButton?.isHidden = !status.isConfigured
        capsuleSyncSettingsDisableButton?.isHidden = !status.isConfigured
        capsuleSyncSettingsSyncButton?.title = status.isBusy
            ? "正在同步…"
            : "立即同步"
        capsuleSyncSettingsSyncButton?.isEnabled = status.isConfigured && !status.isBusy
        capsuleSyncSettingsChooseButton?.isEnabled = !status.isBusy
        capsuleSyncSettingsDisableButton?.isEnabled = status.isConfigured && !status.isBusy
    }

    private func themePreviewCard(_ mode: RimeAppearanceMode) -> NSView {
        let container = NSView()
        container.translatesAutoresizingMaskIntoConstraints = false
        container.heightAnchor.constraint(equalToConstant: 116).isActive = true
        let card = SettingsThemeCardButton(
            mode: mode,
            selected: RimeUI.appearance == mode,
            target: self,
            action: #selector(appearanceCardChosen(_:))
        )
        let details = SettingsThemeDetailsButton(
            title: "", target: self, action: #selector(showThemeCardDetails(_:))
        )
        details.mode = mode
        configureCardIconButton(details, symbol: "info.circle",
                                label: "查看\(mode.title)主题详情")
        // The button sits on the preview's palette, not the current window's.
        details.contentTintColor = RimeUI.color(mode.palette.textSecondary)
        container.addSubview(card)
        container.addSubview(details)
        NSLayoutConstraint.activate([
            card.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            card.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            card.topAnchor.constraint(equalTo: container.topAnchor),
            card.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            details.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -9),
            details.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -8),
        ])
        return container
    }

    private func configureCardIconButton(_ button: SettingsPointingButton,
                                         symbol: String, label: String) {
        button.title = ""
        button.image = NSImage(systemSymbolName: symbol,
                               accessibilityDescription: nil)?
            .withSymbolConfiguration(.init(pointSize: 13, weight: .medium))
        button.imagePosition = .imageOnly
        button.isBordered = false
        button.contentTintColor = RimeUI.textSecondary
        button.toolTip = label
        button.setAccessibilityLabel(label)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.widthAnchor.constraint(equalToConstant: 24).isActive = true
        button.heightAnchor.constraint(equalToConstant: 24).isActive = true
    }

    private func cardGrid(_ cards: [NSView], columns: Int,
                          height: CGFloat) -> NSView {
        let grid = NSStackView()
        grid.orientation = .vertical
        grid.alignment = .width
        grid.spacing = 8
        grid.translatesAutoresizingMaskIntoConstraints = false
        grid.widthAnchor.constraint(equalToConstant: 650).isActive = true
        for start in stride(from: 0, to: cards.count, by: columns) {
            var rowCards = Array(cards[start..<min(start + columns, cards.count)])
            while rowCards.count < columns {
                let placeholder = NSView()
                placeholder.translatesAutoresizingMaskIntoConstraints = false
                placeholder.heightAnchor.constraint(equalToConstant: height).isActive = true
                rowCards.append(placeholder)
            }
            let row = NSStackView(views: rowCards)
            row.orientation = .horizontal
            row.alignment = .centerY
            row.distribution = .fillEqually
            row.spacing = 8
            row.translatesAutoresizingMaskIntoConstraints = false
            row.widthAnchor.constraint(equalToConstant: 650).isActive = true
            grid.addArrangedSubview(row)
        }
        return grid
    }

    private func dictionaryCard(title: String, detail: String) -> NSView {
        inputModeCard(title: title, detail: detail, active: true)
    }

    private func lexiconCard(kind: UserLexiconKind,
                             title: String,
                             detail: String) -> NSView {
        let status = UserLexiconService.shared.status(for: kind)
        let name = NSTextField(labelWithString: title)
        name.font = .systemFont(ofSize: 11, weight: .semibold)
        name.textColor = RimeUI.textPrimary

        let statusLabel = NSTextField(labelWithString:
            status.hasLearningDatabase ? "学习库已建立" : "尚未建立学习库")
        statusLabel.font = .systemFont(ofSize: 9, weight: .semibold)
        statusLabel.textColor = status.hasLearningDatabase ? themeStatusColor : RimeUI.textMuted
        name.toolTip = detail

        let importButton = SettingsLexiconButton(title: "导入学习…",
                                                  target: self,
                                                  action: #selector(importUserLexicon(_:)))
        importButton.lexiconKind = kind
        importButton.controlSize = .small

        let exportButton = SettingsLexiconButton(title: "导出学习…",
                                                  target: self,
                                                  action: #selector(exportUserLexicon(_:)))
        exportButton.lexiconKind = kind
        exportButton.controlSize = .small
        exportButton.isEnabled = status.hasLearningDatabase

        let header = NSStackView(views: [name, statusLabel])
        header.orientation = .horizontal
        header.alignment = .firstBaseline
        header.spacing = 6
        header.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let actions = NSStackView(views: [importButton, exportButton])
        actions.orientation = .horizontal
        actions.alignment = .centerY
        actions.spacing = 6

        let icon = SettingsIconTileView(
            symbolName: "book.closed",
            accessibilityDescription: title
        )
        let card = NSStackView(views: [icon, header, flexSpacer(), actions])
        card.orientation = .horizontal
        card.alignment = .centerY
        card.spacing = 10
        card.edgeInsets = NSEdgeInsets(top: 8, left: 11, bottom: 8, right: 10)
        card.wantsLayer = true
        card.layer?.backgroundColor = RimeUI.surface2.cgColor
        card.layer?.borderColor = RimeUI.border.cgColor
        card.layer?.borderWidth = SettingsVisualStyle.hairline(
            backingScale: window?.backingScaleFactor
        )
        card.layer?.cornerRadius = SettingsVisualStyle.cardRadius
        card.layer?.cornerCurve = .continuous
        card.translatesAutoresizingMaskIntoConstraints = false
        card.widthAnchor.constraint(equalToConstant: 650).isActive = true
        card.heightAnchor.constraint(greaterThanOrEqualToConstant: 46).isActive = true
        return card
    }

    private func inputEncodingSelectionView() -> NSView {
        // Refresh metadata after an external deploy before rebuilding cards.
        _ = rimeEngine.schemaList()
        var cards = InputEncoding.allCases.compactMap { encoding -> NSView? in
            guard let button = encodingRadios[encoding] else { return nil }
            let detail: String
            let symbol: String
            switch encoding {
            case .fullPinyin:
                detail = "雾凇词库与完整拼音输入"
                symbol = "textformat.abc"
            case .naturalDoublePinyin:
                detail = "自然码双拼方案"
                symbol = "keyboard"
            case .xiaoheDoublePinyin:
                detail = "小鹤双拼方案"
                symbol = "bird"
            case .wubi86:
                detail = "86 版五笔 · z 引导全拼查字"
                symbol = "square.grid.3x3"
            case .lingxiWubi:
                detail = "五笔与全拼混输 · 拼音候选提示五笔码"
                symbol = "square.grid.3x3"
            case .english:
                detail = "英文候选与补全"
                symbol = "character.cursor.ibeam"
            }
            return SettingsChoiceCardView(
                choice: button,
                title: encoding.title,
                detail: detail,
                symbolName: symbol
            )
        }
        externalSchemaRadios.removeAll()
        let external = InputSchemaCatalog.options.filter {
            InputSchemaCatalog.isExternalOrdinaryID($0.id)
        }
        externalSchemaChoiceIDs = external.map(\.id)
        for (index, option) in external.enumerated() {
            let button = RimeFixedAccentChoiceButton.radio(
                title: option.name, target: self,
                action: #selector(externalInputSchemaSelected(_:))
            )
            button.tag = InputEncoding.allCases.count + index
            button.font = .systemFont(ofSize: 13, weight: .medium)
            button.translatesAutoresizingMaskIntoConstraints = false
            button.state = InputConfigurationStore.shared.selectedSchemaID == option.id ? .on : .off
            externalSchemaRadios[option.id] = button
            cards.append(SettingsChoiceCardView(
                choice: button, title: option.name, detail: option.detail,
                symbolName: "keyboard"
            ))
        }
        return choiceGrid(cards)
    }

    private func chordSchemaStatusView() -> NSView {
        let implementation = ChordExtensionStore.shared.implementationName
        let badge = NSTextField(labelWithString: "并击")
        badge.font = .systemFont(ofSize: 10, weight: .semibold)
        badge.textColor = themeStatusColor
        badge.setContentHuggingPriority(.required, for: .horizontal)
        let row = settingsRow(
            title: "当前使用并击扩展",
            detail: "正在使用\(implementation)；选择下方任一方案即可切回普通输入。",
            symbolName: "hands.sparkles",
            control: badge
        )
        row.identifier = NSUserInterfaceItemIdentifier(
            "settings.input-scheme.chord-status"
        )
        row.isHidden = InputConfigurationStore.shared.selectedSchemaID
            != ChordExtensionStore.schemaID
        chordSchemaStatusRow = row
        return row
    }

    private func choiceGrid(_ cards: [NSView]) -> NSView {
        guard !cards.isEmpty else { return NSView() }
        let columnCount: Int
        switch cards.count {
        case 1...3: columnCount = cards.count
        case 4: columnCount = 2
        default: columnCount = 3
        }

        var rows: [NSView] = []
        var index = 0
        while index < cards.count {
            var rowViews = Array(cards[index..<min(index + columnCount, cards.count)])
            while rowViews.count < columnCount {
                let placeholder = NSView()
                placeholder.translatesAutoresizingMaskIntoConstraints = false
                placeholder.heightAnchor.constraint(equalToConstant: 68).isActive = true
                rowViews.append(placeholder)
            }
            let row = NSStackView(views: rowViews)
            row.orientation = .horizontal
            row.alignment = .centerY
            row.distribution = .fillEqually
            row.spacing = 8
            row.translatesAutoresizingMaskIntoConstraints = false
            row.widthAnchor.constraint(equalToConstant: 650).isActive = true
            rows.append(row)
            index += columnCount
        }

        let grid = NSStackView(views: rows)
        grid.orientation = .vertical
        grid.alignment = .leading
        grid.spacing = 8
        grid.translatesAutoresizingMaskIntoConstraints = false
        grid.widthAnchor.constraint(equalToConstant: 650).isActive = true
        return grid
    }

    private func appearancePage(subpageID: String) -> NSView {
        if subpageID == "theme" {
            appearancePopUp.removeFromSuperview()
            return contentColumn([
                sectionLabel("选择主题"),
                cardGrid(RimeAppearanceMode.allCases.map(themePreviewCard),
                         columns: 2, height: 116),
            ])
        }
        let preview = CandidatePreviewView(maxWidth: 620)
        candidatePreview = preview
        refreshBufferWidthControls()
        return contentColumn([
            sectionLabel("候选窗预览"),
            preview,
            spacer(12),
            candidateArrangementView(),
            candidateMetricsView(),
            spacer(20),
            sectionLabel("缓冲工作台"),
            bufferWidthView(),
        ])
    }

    private func bufferPage(subpageID: String) -> NSView {
        _ = subpageID
        return bufferSettingsPage()
    }

    private func mailboxPage() -> NSView {
        let snapshot = MailboxStore.shared.snapshot
        let persistenceDetail: String
        let persistenceBadge: NSTextField
        switch snapshot.persistence {
        case .available:
            persistenceDetail = "0600 JSON 原子落盘；内容只保存在本机。"
            persistenceBadge = utilityStatusBadge("可用", active: true)
        case let .unavailable(reason):
            persistenceDetail = reason
            persistenceBadge = utilityStatusBadge("不可用", active: false)
        }

        let openButton = SettingsPointingButton(
            title: "打开 Mailbox",
            target: self,
            action: #selector(openMailboxWindowFromSettings)
        )
        openButton.bezelStyle = .rounded
        openButton.bezelColor = RimeUI.accentGreen

        let storageButton = SettingsPointingButton(
            title: "打开存储目录",
            target: self,
            action: #selector(openMailboxStorageDirectory)
        )
        storageButton.bezelStyle = .rounded
        storageButton.isEnabled = snapshot.persistence == .available
        mailboxSettingsStorageButton = storageButton
        mailboxSettingsStatusBadge = persistenceBadge

        let connectorButton = SettingsPointingButton(
            title: "AI 模型设置",
            target: self,
            action: #selector(openAIModelSettingsFromMailbox)
        )
        connectorButton.bezelStyle = .rounded

        let shortcutRecorder = utilityShortcutRecorder(for: .openMailbox)
        let actions = utilityActionRow([openButton, storageButton, connectorButton])
        actions.identifier = NSUserInterfaceItemIdentifier(
            "settings.mailbox-actions"
        )
        let path = MailboxStore.shared.storageDirectoryURL.path
        let pathLabel = secondaryLabel("本地路径：\(path)")
        pathLabel.toolTip = path
        pathLabel.translatesAutoresizingMaskIntoConstraints = false
        pathLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 650).isActive = true
        let summaryLabel = secondaryLabel("")
        summaryLabel.translatesAutoresizingMaskIntoConstraints = false
        summaryLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 650).isActive = true
        mailboxSettingsSummaryLabel = summaryLabel
        refreshMailboxSettingsStatus(snapshot: snapshot)

        let page = contentColumn([
            sectionLabel("运行与存储"),
            settingsRow(
                title: "本地会话存储",
                detail: persistenceDetail,
                symbolName: "externaldrive",
                control: persistenceBadge
            ),
            summaryLabel,
            actions,
            pathLabel,
            spacer(12),
            sectionLabel("全局快捷键"),
            settingsRow(
                title: "显示或隐藏 Mailbox",
                detail: "当前为 \(RimeShortcutPreferences.shortcut(for: .openMailbox).displayTitle)；在其他输入法下同样可用。",
                symbolName: "keyboard",
                control: shortcutRecorder
            ),
        ])
        page.identifier = NSUserInterfaceItemIdentifier(
            "settings.mailbox-configuration"
        )
        return page
    }

    private func capsulePage() -> NSView {
        let localDetail: String
        let localIsAvailable: Bool
        do {
            let contentCount = try CapsuleContentStore.shared.listRecords().count
            let passwordCount = try CapsulePasswordStore().listSummaries().count
            localDetail = "\(contentCount) 个普通条目，\(passwordCount) 个加密密码条目。"
            localIsAvailable = true
        } catch {
            localDetail = "无法读取本地资料库：\(error.localizedDescription)"
            localIsAvailable = false
        }
        let localRoot = CapsuleContentStore.shared.rootURL
        let localBadge = utilityStatusBadge(
            localIsAvailable ? "可用" : "检查失败",
            active: localIsAvailable
        )

        let storageButton = SettingsPointingButton(
            title: "打开资料目录",
            target: self,
            action: #selector(openCapsuleStorageDirectory)
        )
        storageButton.bezelStyle = .rounded

        let shortcutRecorder = utilityShortcutRecorder(for: .toggleClipboardHistory)
        let localActions = utilityActionRow([storageButton])
        let pathLabel = secondaryLabel("本地路径：\(localRoot.path)")
        pathLabel.toolTip = localRoot.path
        pathLabel.translatesAutoresizingMaskIntoConstraints = false
        pathLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 650).isActive = true

        let syncStatus = CapsuleCloudSyncController.shared.status
        let syncBadge = utilityStatusBadge(
            capsuleCloudStatusTitle(syncStatus),
            active: syncStatus.phase == .idle || syncStatus.phase == .synced
                || syncStatus.phase == .syncing
        )
        capsuleSyncSettingsStatusBadge = syncBadge
        let syncDetailLabel = secondaryLabel(capsuleCloudStatusDetail(syncStatus))
        syncDetailLabel.translatesAutoresizingMaskIntoConstraints = false
        syncDetailLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 650).isActive = true
        capsuleSyncSettingsDetailLabel = syncDetailLabel
        let syncActions = capsuleCloudActionRow(status: syncStatus)

        let revealPasscodeSettings = CapsuleRevealPasscodeSettingsView()
        revealPasscodeSettings.identifier = NSUserInterfaceItemIdentifier(
            "settings.capsule-passcode-configuration"
        )
        revealPasscodeSettings.translatesAutoresizingMaskIntoConstraints = false
        revealPasscodeSettings.widthAnchor.constraint(equalToConstant: 650).isActive = true

        // Capsule's Recent tab is the clipboard history, so its capture and
        // paste settings live on this page too.
        let page = contentColumn([
            settingsRow(
                title: "允许收录剪贴板内容",
                detail: "\(ProductBrand.name) 运行时后台收录；安全输入、锁屏和休眠期间不会读取。",
                symbolName: "clipboard",
                control: clipboardHistoryCheck
            ),
            settingsRow(
                title: "回车后自动粘贴",
                detail: "无法经输入法直接上屏的内容，改为发送一次 ⌘V，省去手动粘贴。",
                symbolName: "doc.on.clipboard",
                control: clipboardAutoPasteCheck
            ),
            clipboardAutoPasteStatusLabel,
            spacer(12),
            sectionLabel("本地资料库"),
            settingsRow(
                title: "本地 Capsule",
                detail: localDetail,
                symbolName: "archivebox",
                control: localBadge
            ),
            localActions,
            pathLabel,
            spacer(12),
            sectionLabel("全局快捷键"),
            settingsRow(
                title: "显示或隐藏 Capsule",
                detail: "当前为 \(RimeShortcutPreferences.shortcut(for: .toggleClipboardHistory).displayTitle)；在其他输入法下同样可用。",
                symbolName: "keyboard",
                control: shortcutRecorder
            ),
            spacer(12),
            revealPasscodeSettings,
            spacer(12),
            sectionLabel("iCloud 自动同步"),
            settingsRow(
                title: "同步状态",
                detail: "配置 iCloud Drive 文件夹，并自动同步普通条目与媒体。",
                symbolName: "icloud",
                control: syncBadge
            ),
            syncDetailLabel,
            syncActions,
        ])
        page.identifier = NSUserInterfaceItemIdentifier(
            "settings.capsule-configuration"
        )
        return page
    }

    private func bufferSettingsPage() -> NSView {
        return contentColumn([
            settingsRow(
                title: "最后一块上屏后关闭工作台",
                detail: "适用于 Default 与所有缓冲插件；部分失败或内容变化时保持打开。",
                symbolName: "checkmark.rectangle",
                control: closeAfterLastDeliveryCheck
            ),
            settingsRow(
                title: "对齐目标输入框",
                detail: "在目标输入框正下方打开：左边缘对齐、宽度随其变化；下方空间不足时改在上方。",
                symbolName: "text.alignleft",
                control: alignToInputBoxCheck
            ),
            alignToInputBoxStatusLabel,
            accessibilityGrantButton,
            settingsRow(
                title: "切换应用时清空本地缓冲",
                detail: "只在没有外部来源块时执行；默认关闭。",
                symbolName: "trash",
                control: resetOnAppSwitchCheck
            ),
            moveBufferWindowButton,
            spacer(20),
            sectionLabel("快捷键"),
            shortcutSettingsView(),
        ])
    }

    private func connectionsPage(subpageID: String) -> NSView {
        if subpageID == "ai-model" {
            return aiModelConnectionsPage()
        }
        guard subpageID == "local-gateway" else {
            return contentColumn([])
        }

        let gatewayEnabled = LocalGateway.shared.enabled
        gatewayClientTabs.removeFromSuperview()
        gatewayCopyConfigButton.removeFromSuperview()
        gatewayConfigField.removeFromSuperview()
        gatewaySupplementaryField.removeFromSuperview()
        gatewayCopySupplementaryButton.removeFromSuperview()
        refreshGatewayClientConfigurationPreview()
        gatewayClientTabs.isEnabled = gatewayEnabled
        gatewayCopyConfigButton.isEnabled = gatewayEnabled
        gatewayCopySupplementaryButton.isEnabled = gatewayEnabled

        let clientDetail = NSTextField(wrappingLabelWithString: selectedGatewayClient.detail)
        clientDetail.font = .systemFont(ofSize: 11)
        clientDetail.textColor = RimeUI.textMuted
        clientDetail.translatesAutoresizingMaskIntoConstraints = false
        clientDetail.widthAnchor.constraint(equalToConstant: 626).isActive = true

        let configurationTitle = NSTextField(labelWithString: "配置预览（令牌已脱敏）")
        configurationTitle.font = .systemFont(ofSize: 11, weight: .semibold)
        configurationTitle.textColor = RimeUI.textPrimary
        configurationTitle.toolTip = "预览永不读取或显示真实令牌；只有点按复制时才生成可直接使用的配置。"
        configurationTitle.setContentCompressionResistancePriority(
            .defaultLow,
            for: .horizontal
        )
        let configurationHeader = NSStackView(views: [
            configurationTitle,
            flexSpacer(),
            gatewayCopyConfigButton,
        ])
        configurationHeader.orientation = .horizontal
        configurationHeader.alignment = .top
        configurationHeader.spacing = 12
        configurationHeader.translatesAutoresizingMaskIntoConstraints = false
        configurationHeader.widthAnchor.constraint(equalToConstant: 626).isActive = true

        var configurationViews: [NSView] = [
            gatewayClientTabs,
            clientDetail,
            configurationHeader,
            gatewayConfigField,
        ]
        if let supplementaryTitle = selectedGatewayClient.supplementaryTitle,
           let supplementaryCopyTitle = selectedGatewayClient.supplementaryCopyTitle {
            let supplementaryLabel = NSTextField(labelWithString: supplementaryTitle)
            supplementaryLabel.font = .systemFont(ofSize: 11, weight: .semibold)
            supplementaryLabel.textColor = RimeUI.textPrimary
            supplementaryLabel.setContentCompressionResistancePriority(
                .defaultLow,
                for: .horizontal
            )
            gatewayCopySupplementaryButton.title = supplementaryCopyTitle
            let supplementaryHeader = NSStackView(views: [
                supplementaryLabel,
                flexSpacer(),
                gatewayCopySupplementaryButton,
            ])
            supplementaryHeader.orientation = .horizontal
            supplementaryHeader.alignment = .top
            supplementaryHeader.spacing = 12
            supplementaryHeader.translatesAutoresizingMaskIntoConstraints = false
            supplementaryHeader.widthAnchor.constraint(equalToConstant: 626).isActive = true
            configurationViews.append(contentsOf: [
                supplementaryHeader,
                gatewaySupplementaryField,
            ])
        }

        return contentColumn([
            settingsRow(
                title: "启用本地网关",
                detail: "允许本机智能体通过标准 MCP / HTTP 推送待确认内容；配置仅指向 127.0.0.1。",
                symbolName: "network",
                control: gatewayEnableCheck
            ),
            connectorDetailPanel(configurationViews, enabled: gatewayEnabled),
        ])
    }

    private func aiModelConnectionsPage() -> NSView {
        let connectors = AITextConnectorRegistry.shared
        let selected = AITextConnectorSelectionStore.shared.selectedKind
        let detailPanel: NSView

        switch selected {
        case .codexCLI:
            let availability = connectors.availability(for: .codexCLI)
            let ready = availability == .ready
            let detail: String
            switch availability {
            case .ready:
                detail = "使用 \(ProductIdentity.displayName) 专用的 ChatGPT 登录；不会读取 ~/.codex 中的 MCP、工具、Hook 或技能。"
            case let .unavailable(message):
                detail = message
            }
            refreshCodexLoginControls(
                hasCredential: connectors.codexHasStoredChatGPTCredential
            )
            codexLoginButton.removeFromSuperview()
            codexCopyLoginLinkButton.removeFromSuperview()
            codexLoginSpinner.removeFromSuperview()
            codexLoginStatusLabel.removeFromSuperview()
            let actions = NSStackView(views: [
                codexLoginButton,
                codexCopyLoginLinkButton,
                codexLoginSpinner,
                flexSpacer(),
            ])
            actions.orientation = .horizontal
            actions.alignment = .centerY
            actions.spacing = 8
            actions.translatesAutoresizingMaskIntoConstraints = false
            actions.widthAnchor.constraint(equalToConstant: 626).isActive = true
            detailPanel = connectorDetailPanel([
                settingsRow(
                    title: "Codex CLI",
                    detail: detail,
                    symbolName: PluginVisualIdentity.chatGPTSymbolName,
                    control: connectorStatusBadge(ready ? "可用" : "不可用", active: ready),
                    width: 626
                ),
                actions,
                codexLoginStatusLabel,
            ])

        case .claudeCodeCLI:
            let availability = connectors.availability(for: .claudeCodeCLI)
            let ready = availability == .ready
            let detail: String
            switch availability {
            case .ready:
                detail = "使用本机已登录的 claude 命令行；工具调用与会话持久化被关闭。"
            case let .unavailable(message):
                detail = message
            }
            refreshClaudeLoginControls(
                authenticationStatus: connectors.claudeAuthenticationStatus
            )
            claudeLoginButton.removeFromSuperview()
            claudeLoginSpinner.removeFromSuperview()
            claudeLoginStatusLabel.removeFromSuperview()
            let actions = NSStackView(views: [
                claudeLoginButton,
                claudeLoginSpinner,
                flexSpacer(),
            ])
            actions.orientation = .horizontal
            actions.alignment = .centerY
            actions.spacing = 8
            actions.translatesAutoresizingMaskIntoConstraints = false
            actions.widthAnchor.constraint(equalToConstant: 626).isActive = true
            detailPanel = connectorDetailPanel([
                settingsRow(
                    title: "Claude Code CLI",
                    detail: detail,
                    symbolName: PluginVisualIdentity.claudeSymbolName,
                    control: connectorStatusBadge(ready ? "可用" : "不可用", active: ready),
                    width: 626
                ),
                actions,
                claudeLoginStatusLabel,
            ])

        case .openAICompatible:
            detailPanel = aiProviderProfilesDetailPanel()
        }

        return contentColumn([
            aiConnectorSelectionView(),
            detailPanel,
        ])
    }

    private func aiProviderProfilesDetailPanel() -> NSView {
        aiProviderPicker.removeFromSuperview()
        aiProviderNameField.removeFromSuperview()
        aiBaseURLField.removeFromSuperview()
        aiDocumentationURLField.removeFromSuperview()
        aiModelField.removeFromSuperview()
        aiAPIKeyField.removeFromSuperview()
        aiConfigurationStatus.removeFromSuperview()

        let statuses = refreshAIProviderProfileConfiguration()
        let selectedStatus = statuses.first(where: { $0.id == selectedAIProviderID })
        let hasSelectedProfile = selectedStatus != nil

        let newProvider = SettingsPointingButton(
            title: "新建",
            target: self,
            action: #selector(createAIProviderProfile)
        )
        let addOpenRouter = SettingsPointingButton(
            title: "添加 OpenRouter",
            target: self,
            action: #selector(createOpenRouterAIProvider)
        )
        let deleteProvider = SettingsPointingButton(
            title: "删除",
            target: self,
            action: #selector(confirmDeleteAIProviderProfile)
        )
        let save = SettingsPointingButton(
            title: "保存 Provider",
            target: self,
            action: #selector(saveAIProviderProfile)
        )
        let clearKey = SettingsPointingButton(
            title: "清除密钥",
            target: self,
            action: #selector(clearAIProviderAPIKey)
        )
        let setCurrentTextModel = SettingsPointingButton(
            title: "设为当前文本模型",
            target: self,
            action: #selector(selectCurrentAIProviderTextRoute)
        )
        let queryModels = SettingsPointingButton(
            title: "查询模型 / 测试连接",
            target: self,
            action: #selector(querySelectedAIProviderModels)
        )
        let selectedTextRoute = selectedStatus?.routes.first(where: {
            $0.adapter == .openAIChatCompletions
                && $0.isEnabled
                && $0.modelID != nil
        })
        let canSelectTextRoute = selectedStatus?.isEnabled == true
            && selectedTextRoute != nil
            && (selectedStatus?.requiresAPIKey != true
                || selectedStatus?.hasStoredAPIKey == true)
        for button in [
            newProvider,
            addOpenRouter,
            deleteProvider,
            save,
            clearKey,
            setCurrentTextModel,
            queryModels,
        ] {
            button.controlSize = .small
        }
        deleteProvider.isEnabled = hasSelectedProfile
        save.isEnabled = hasSelectedProfile
        clearKey.isEnabled = hasSelectedProfile && selectedStatus?.hasStoredAPIKey == true
        setCurrentTextModel.isEnabled = canSelectTextRoute
        queryModels.isEnabled = hasSelectedProfile
            && (selectedStatus?.requiresAPIKey != true
                || selectedStatus?.hasStoredAPIKey == true)

        let pickerControls = NSStackView(views: [
            aiProviderPicker,
            newProvider,
            addOpenRouter,
            deleteProvider,
        ])
        pickerControls.orientation = .horizontal
        pickerControls.alignment = .centerY
        pickerControls.spacing = 8

        var actionViews: [NSView] = [
            save,
            clearKey,
            queryModels,
            setCurrentTextModel,
        ]
        if selectedAIProviderDocumentationURL != nil {
            let openDocumentation = SettingsPointingButton(
                title: "打开文档",
                target: self,
                action: #selector(openSelectedAIProviderDocumentation)
            )
            openDocumentation.controlSize = .small
            actionViews.append(openDocumentation)
        }
        actionViews.append(flexSpacer())
        let actions = NSStackView(views: actionViews)
        actions.orientation = .horizontal
        actions.alignment = .centerY
        actions.spacing = 8
        actions.translatesAutoresizingMaskIntoConstraints = false
        actions.widthAnchor.constraint(equalToConstant: 626).isActive = true

        var views: [NSView] = [
            labeledSettingsRow("Provider", control: pickerControls),
            labeledSettingsRow("名称", control: aiProviderNameField),
            labeledSettingsRow("Base URL", control: aiBaseURLField),
            labeledSettingsRow("文档", control: aiDocumentationURLField),
            labeledSettingsRow("文本模型", control: aiModelField),
            labeledSettingsRow("API Key", control: aiAPIKeyField),
            actions,
            aiConfigurationStatus,
        ]
        if let selectedStatus {
            views.append(sectionLabel("模型路由"))
            views.append(contentsOf: selectedStatus.routes.map {
                aiProviderRouteStatusView($0)
            })
        } else {
            views.append(secondaryLabel(
                "新建通用 OpenAI Chat Provider，或添加含 Native Decisions/Jev 专用路线的 OpenRouter Provider。"
            ))
        }
        return connectorDetailPanel(views)
    }

    private func aiProviderRouteStatusView(
        _ route: AIProviderRouteRedactedStatus
    ) -> NSView {
        let state = aiProviderRouteState(route)
        let title = NSTextField(labelWithString: route.displayName)
        title.font = .systemFont(ofSize: 11, weight: .semibold)
        title.textColor = RimeUI.textPrimary
        let model = NSTextField(labelWithString: route.modelID ?? "未选择模型")
        model.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        model.textColor = RimeUI.textSecondary
        model.lineBreakMode = .byTruncatingMiddle
        model.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let header = NSStackView(views: [
            title,
            model,
            flexSpacer(),
            connectorStatusBadge(state.label, active: state.active),
        ])
        header.orientation = .horizontal
        header.alignment = .centerY
        header.spacing = 8
        header.translatesAutoresizingMaskIntoConstraints = false

        let detail = NSTextField(wrappingLabelWithString:
            "Adapter：\(route.adapter.displayName)\n"
                + "能力：\(aiProviderCapabilitiesDescription(route.capabilities))\n"
                + "状态：\(state.detail)"
        )
        detail.font = .systemFont(ofSize: 10)
        detail.textColor = RimeUI.textMuted
        detail.translatesAutoresizingMaskIntoConstraints = false

        let card = NSStackView(views: [header, detail])
        card.orientation = .vertical
        card.alignment = .leading
        card.spacing = 5
        card.edgeInsets = NSEdgeInsets(top: 9, left: 10, bottom: 9, right: 10)
        card.wantsLayer = true
        card.layer?.backgroundColor = RimeUI.surface3.cgColor
        card.layer?.borderColor = RimeUI.border.cgColor
        card.layer?.borderWidth = SettingsVisualStyle.hairline(
            backingScale: window?.backingScaleFactor
        )
        card.layer?.cornerRadius = SettingsVisualStyle.cardRadius
        card.layer?.cornerCurve = .continuous
        card.translatesAutoresizingMaskIntoConstraints = false
        card.widthAnchor.constraint(equalToConstant: 626).isActive = true
        header.widthAnchor.constraint(equalTo: card.widthAnchor, constant: -20).isActive = true
        detail.widthAnchor.constraint(equalTo: card.widthAnchor, constant: -20).isActive = true
        return card
    }

    private func aiProviderRouteState(
        _ route: AIProviderRouteRedactedStatus
    ) -> (label: String, detail: String, active: Bool) {
        if route.adapter == .openRouterDecisions {
            let configured = route.isEnabled && route.modelID != nil
            return (
                configured ? "专用结构化路线" : "专用路线未配置",
                "仅展示给显式使用结构化结果的功能；不参与普通文本模型选择。",
                false
            )
        }
        guard route.isEnabled else {
            return ("已停用", "此路线已停用。", false)
        }
        guard route.modelID != nil else {
            return ("未选择模型", "填写文本模型后才会成为可用路线。", false)
        }
        if route.adapter.supportsAITextBlocks {
            return ("文本路线候选", "可由支持该 Adapter 的文本功能显式选用。", true)
        }
        return ("已配置", "等待匹配的请求 Adapter 接入。", false)
    }

    private func aiProviderCapabilitiesDescription(
        _ capabilities: Set<AIModelCapability>
    ) -> String {
        let labels = AIModelCapability.allCases.compactMap { capability -> String? in
            guard capabilities.contains(capability) else { return nil }
            switch capability {
            case .textGeneration: return "文本生成"
            case .streamingText: return "流式文本"
            case .jsonSchema: return "JSON Schema"
            case .toolCalling: return "工具调用"
            case .modelDiscovery: return "模型发现"
            case .nativeStructuredOutput: return "原生结构化输出"
            case .decision: return "决策"
            }
        }
        return labels.isEmpty ? "未声明" : labels.joined(separator: " · ")
    }

    private func refreshAIProviderProfileConfiguration() -> [AIProviderProfileRedactedStatus] {
        do {
            let store = AIProviderProfileCatalogStore.shared
            let catalog = try store.loadMigratingLegacyOpenAICompatibleIfNeeded()
            let statuses = try store.redactedStatuses(
                migratingLegacyOpenAICompatible: false
            )
            if let selectedAIProviderID,
               !statuses.contains(where: { $0.id == selectedAIProviderID }) {
                self.selectedAIProviderID = nil
            }
            if selectedAIProviderID == nil {
                selectedAIProviderID = statuses.first?.id
            }

            aiProviderPicker.removeAllItems()
            for status in statuses {
                aiProviderPicker.addItem(
                    withTitle: "\(status.displayName) · \(status.routes.count) 条路线"
                )
                aiProviderPicker.lastItem?.representedObject = status.id.uuidString
            }
            if let selectedAIProviderID,
               let index = statuses.firstIndex(where: { $0.id == selectedAIProviderID }) {
                aiProviderPicker.selectItem(at: index)
            }

            let selectedProfile = selectedAIProviderID.flatMap {
                catalog.profile(id: $0)
            }
            let selectedStatus = selectedAIProviderID.flatMap { id in
                statuses.first(where: { $0.id == id })
            }
            let textRoute = selectedProfile?.routes.first(where: {
                $0.adapter == .openAIChatCompletions
            })
            selectedAIProviderDocumentationURL = aiProviderDocumentationURL(
                selectedProfile?.documentationURL
            )
            aiProviderNameField.stringValue = selectedProfile?.displayName ?? ""
            aiBaseURLField.stringValue = selectedProfile?.baseURL ?? ""
            aiDocumentationURLField.stringValue = selectedProfile?.documentationURL ?? ""
            aiModelField.stringValue = textRoute?.modelID ?? ""
            aiAPIKeyField.stringValue = ""
            aiProviderPicker.isEnabled = selectedProfile != nil
            for field in [
                aiProviderNameField,
                aiBaseURLField,
                aiDocumentationURLField,
                aiModelField,
                aiAPIKeyField,
            ] {
                field.isEnabled = selectedProfile != nil
            }
            if let selectedStatus {
                aiAPIKeyField.placeholderString = selectedStatus.hasStoredAPIKey
                    ? "已保存（留空保持不变）"
                    : (selectedStatus.requiresAPIKey
                        ? "尚未保存（此 Provider 需要 API Key）"
                        : "API Key（可选；留空保持不变）")
                aiConfigurationStatus.stringValue =
                    "版本 \(selectedStatus.revision) · \(selectedStatus.isEnabled ? "已启用" : "已停用")"
                        + " · 密钥：\(selectedStatus.hasStoredAPIKey ? "已保存（脱敏）" : "未保存")"
                aiConfigurationStatus.textColor = RimeUI.textMuted
            } else {
                aiAPIKeyField.placeholderString = "先新建 Provider"
                aiConfigurationStatus.stringValue =
                    "尚未保存 Provider；现有单一 OpenAI 配置会在首次打开时自动迁移。"
                aiConfigurationStatus.textColor = RimeUI.textMuted
            }
            return statuses
        } catch {
            selectedAIProviderID = nil
            selectedAIProviderDocumentationURL = nil
            aiProviderPicker.removeAllItems()
            aiProviderPicker.isEnabled = false
            for field in [
                aiProviderNameField,
                aiBaseURLField,
                aiDocumentationURLField,
                aiModelField,
                aiAPIKeyField,
            ] {
                field.stringValue = ""
                field.isEnabled = false
            }
            aiAPIKeyField.placeholderString = "无法读取已保存密钥"
            aiConfigurationStatus.stringValue = "读取 Provider 配置失败，请检查本地文件权限"
            aiConfigurationStatus.textColor = .systemRed
            return []
        }
    }

    private func aiProviderDocumentationURL(_ rawURL: String?) -> URL? {
        guard let rawURL,
              let components = URLComponents(string: rawURL),
              components.scheme?.lowercased() == "https",
              components.host?.isEmpty == false,
              components.user == nil,
              components.password == nil else {
            return nil
        }
        return components.url
    }

    private func aiConnectorSelectionView() -> NSView {
        refreshAIConnectorSelection()
        let cards = AITextProviderKind.allCases.compactMap { kind -> NSView? in
            guard let button = aiConnectorRadios[kind] else { return nil }
            let cardTitle: String
            let detail: String
            let symbol: String
            switch kind {
            case .codexCLI:
                cardTitle = "ChatGPT"
                detail = "浏览器授权 · 隔离运行"
                symbol = PluginVisualIdentity.chatGPTSymbolName
            case .claudeCodeCLI:
                cardTitle = "Claude"
                detail = "官方 CLI 授权"
                symbol = PluginVisualIdentity.claudeSymbolName
            case .openAICompatible:
                cardTitle = "AI Provider"
                detail = "多个 OpenAI 兼容 Provider"
                symbol = "network"
            }
            return SettingsChoiceCardView(
                choice: button,
                title: cardTitle,
                detail: detail,
                symbolName: symbol
            )
        }
        return choiceGrid(cards)
    }

    private func labeledSettingsRow(_ labelText: String, control: NSView) -> NSView {
        control.removeFromSuperview()
        let label = NSTextField(labelWithString: labelText)
        label.font = .systemFont(ofSize: 12)
        label.textColor = RimeUI.textSecondary
        label.alignment = .right
        label.translatesAutoresizingMaskIntoConstraints = false
        label.widthAnchor.constraint(equalToConstant: 76).isActive = true
        let row = NSStackView(views: [label, control])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        return row
    }

    /// Renders a client-side config only. Callers deliberately pass the token:
    /// previews use the redacted placeholder, while copy actions fetch it at
    /// the last possible moment.
    private func gatewayConfiguration(
        for client: GatewayClientConfiguration,
        token: String
    ) -> String {
        let url = "http://127.0.0.1:\(LocalGateway.shared.port)/mcp"
        switch client {
        case .codex:
            return """
            [mcp_servers.rimes]
            url = "\(url)"
            bearer_token_env_var = "RIMES_MCP_TOKEN"
            default_tools_approval_mode = "prompt"
            """
        case .cursor, .claudeCode, .genericMCP:
            return """
            {
              "mcpServers": {
                "rimes": {
                  "type": "http",
                  "url": "\(url)",
                  "headers": {
                    "Authorization": "Bearer \(token)"
                  }
                }
              }
            }
            """
        }
    }

    private func gatewaySupplementaryConfiguration(
        for client: GatewayClientConfiguration,
        token: String
    ) -> String? {
        let url = "http://127.0.0.1:\(LocalGateway.shared.port)/mcp"
        switch client {
        case .codex:
            return "export RIMES_MCP_TOKEN='\(token)'"
        case .claudeCode:
            return "claude mcp add --transport http rimes \(url) "
                + "--header \"Authorization: Bearer \(token)\""
        case .cursor, .genericMCP:
            return nil
        }
    }

    private func refreshGatewayClientConfigurationPreview() {
        gatewayClientTabs.selectedSegment = selectedGatewayClient.rawValue
        gatewayConfigField.stringValue = gatewayConfiguration(
            for: selectedGatewayClient,
            token: GatewayClientConfiguration.redactedToken
        )
        gatewaySupplementaryField.stringValue = gatewaySupplementaryConfiguration(
            for: selectedGatewayClient,
            token: GatewayClientConfiguration.redactedToken
        ) ?? ""
    }

    @objc private func gatewayToggled() {
        let enabled = gatewayEnableCheck.state == .on
        LocalGateway.shared.enabled = enabled
        settingsStatusLabel.stringValue = enabled ? "已启用本地网关" : "已关闭本地网关"
        settingsStatusLabel.textColor = RimeUI.textMuted
        DispatchQueue.main.async { [weak self] in
            self?.showCurrentRoute()
        }
    }

    @objc private func gatewayClientChosen(_ sender: NSSegmentedControl) {
        guard let client = GatewayClientConfiguration(rawValue: sender.selectedSegment) else {
            return
        }
        selectedGatewayClient = client
        DispatchQueue.main.async { [weak self] in
            self?.showCurrentRoute()
        }
    }

    @objc private func copyGatewayConfig() {
        let token = selectedGatewayClient == .codex
            ? GatewayClientConfiguration.redactedToken
            : GatewayToken.current()
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(
            gatewayConfiguration(for: selectedGatewayClient, token: token),
            forType: .string
        )
        settingsStatusLabel.stringValue = selectedGatewayClient.copyStatus
        settingsStatusLabel.textColor = RimeUI.textMuted
    }

    @objc private func copyGatewaySupplementaryConfiguration() {
        guard let configuration = gatewaySupplementaryConfiguration(
            for: selectedGatewayClient,
            token: GatewayToken.current()
        ) else {
            return
        }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(configuration, forType: .string)
        settingsStatusLabel.stringValue = "已复制 \(selectedGatewayClient.title) 补充配置"
        settingsStatusLabel.textColor = RimeUI.textMuted
    }

    @objc private func aiProviderSelected(_ sender: NSPopUpButton) {
        guard let rawID = sender.selectedItem?.representedObject as? String,
              let providerID = UUID(uuidString: rawID) else { return }
        selectedAIProviderID = providerID
        DispatchQueue.main.async { [weak self] in
            self?.showCurrentRoute()
        }
    }

    @objc private func createAIProviderProfile() {
        let profile = AIProviderProfile(
            displayName: "新 OpenAI Provider",
            baseURL: "https://api.openai.com/v1",
            routes: [
                AIModelRoute(
                    displayName: "OpenAI Chat",
                    adapter: .openAIChatCompletions,
                    capabilities: [.textGeneration, .streamingText],
                    isEnabled: false
                ),
            ],
            requiresAPIKey: true
        )
        saveNewAIProviderProfile(profile, successMessage: "已新建通用 OpenAI Provider")
    }

    @objc private func createOpenRouterAIProvider() {
        saveNewAIProviderProfile(
            AIProviderProfile.openRouter(),
            successMessage: "已添加 OpenRouter Provider；Native Decisions/Jev 保持为专用路线"
        )
    }

    private func saveNewAIProviderProfile(
        _ profile: AIProviderProfile,
        successMessage: String
    ) {
        do {
            let store = AIProviderProfileCatalogStore.shared
            _ = try store.loadMigratingLegacyOpenAICompatibleIfNeeded()
            let saved = try store.upsert(profile)
            selectedAIProviderID = saved.id
            refreshAIProviderPage(after: successMessage)
        } catch {
            presentAIProviderConfigurationError(error)
        }
    }

    @objc private func saveAIProviderProfile() {
        window?.makeFirstResponder(nil)
        do {
            let store = AIProviderProfileCatalogStore.shared
            let catalog = try store.loadMigratingLegacyOpenAICompatibleIfNeeded()
            guard let providerID = selectedAIProviderID,
                  var profile = catalog.profile(id: providerID) else {
                throw AIProviderProfileStoreError.missingProfile
            }

            let modelID = aiModelField.stringValue.trimmingCharacters(
                in: .whitespacesAndNewlines
            )
            let textRouteIndex = profile.routes.firstIndex(where: {
                $0.adapter == .openAIChatCompletions
            })
            var textRoute = textRouteIndex.map { profile.routes[$0] }
                ?? AIModelRoute(
                    displayName: "OpenAI Chat",
                    adapter: .openAIChatCompletions,
                    capabilities: [.textGeneration, .streamingText],
                    isEnabled: false
                )
            textRoute.modelID = modelID.isEmpty ? nil : modelID
            textRoute.adapter = .openAIChatCompletions
            textRoute.capabilities = [.textGeneration, .streamingText]
            textRoute.isEnabled = !modelID.isEmpty
            if let textRouteIndex {
                profile.routes[textRouteIndex] = textRoute
            } else {
                profile.routes.append(textRoute)
            }

            profile.displayName = aiProviderNameField.stringValue
            profile.baseURL = aiBaseURLField.stringValue
            let documentationURL = aiDocumentationURLField.stringValue
                .trimmingCharacters(in: .whitespacesAndNewlines)
            profile.documentationURL = documentationURL.isEmpty ? nil : documentationURL
            let enteredKey = aiAPIKeyField.stringValue
            let secretUpdate: AIProviderSecretUpdate = enteredKey.isEmpty
                ? .preserve
                : .set(enteredKey)
            let saved = try store.upsert(profile, secretUpdate: secretUpdate)
            selectedAIProviderID = saved.id
            refreshAIProviderPage(after: "已保存 Provider：\(saved.displayName)")
        } catch {
            presentAIProviderConfigurationError(error)
        }
    }

    @objc private func clearAIProviderAPIKey() {
        window?.makeFirstResponder(nil)
        do {
            let store = AIProviderProfileCatalogStore.shared
            let catalog = try store.loadMigratingLegacyOpenAICompatibleIfNeeded()
            guard let providerID = selectedAIProviderID,
                  let profile = catalog.profile(id: providerID) else {
                throw AIProviderProfileStoreError.missingProfile
            }
            let saved = try store.upsert(profile, secretUpdate: .clear)
            selectedAIProviderID = saved.id
            refreshAIProviderPage(after: "已清除 \(saved.displayName) 的本地 API Key")
        } catch {
            presentAIProviderConfigurationError(error)
        }
    }

    @objc private func selectCurrentAIProviderTextRoute() {
        do {
            let store = AIProviderProfileCatalogStore.shared
            let catalog = try store.loadMigratingLegacyOpenAICompatibleIfNeeded()
            guard let providerID = selectedAIProviderID,
                  let profile = catalog.profile(id: providerID),
                  profile.isEnabled else {
                throw AIProviderProfileStoreError.missingProfile
            }
            guard let route = profile.routes.first(where: {
                $0.adapter == .openAIChatCompletions
                    && $0.isEnabled
                    && $0.hasSelectedModel
            }) else {
                throw AIProviderProfileStoreError.invalidConfiguration(
                    "请先保存一个已启用的 OpenAI Chat 文本模型"
                )
            }
            if profile.requiresAPIKey,
               !(try store.secretStore.hasStoredAPIKey(for: profile.id)) {
                throw AIProviderProfileStoreError.missingAPIKey
            }

            let changed = AITextConnectorSelectionStore.shared.selectProviderRoute(
                profileID: profile.id,
                routeID: route.id
            )
            settingsStatusLabel.stringValue = changed
                ? "已将 \(route.modelID ?? route.displayName) 设为当前文本模型"
                : "\(route.modelID ?? route.displayName) 已是当前文本模型"
            settingsStatusLabel.textColor = RimeUI.textMuted
            if changed {
                BufferWindowController.shared.refresh()
                RIMESController.refreshActiveUI()
            }
        } catch {
            presentAIProviderConfigurationError(error)
        }
    }

    /// A user-initiated, no-billing model-directory request. It deliberately
    /// does not select a returned model or invoke a generation route; model
    /// choice remains an explicit edit and Jev remains outside text routing.
    @objc private func querySelectedAIProviderModels() {
        guard let profileID = selectedAIProviderID else { return }
        aiConfigurationStatus.stringValue = "正在查询 Provider 官方模型目录…"
        aiConfigurationStatus.textColor = RimeUI.textMuted
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let result: Result<[String], Error>
            do {
                result = .success(
                    try AIProviderModelCatalogProbe.fetch(profileID: profileID)
                )
            } catch {
                result = .failure(error)
            }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.selectedAIProviderID == profileID else { return }
                switch result {
                case let .success(modelIDs):
                    let examples = modelIDs.prefix(3).joined(separator: "、")
                    self.aiConfigurationStatus.stringValue =
                        "已查询 \(modelIDs.count) 个模型；例如：\(examples)"
                    self.aiConfigurationStatus.textColor = RimeUI.textMuted
                case let .failure(error):
                    self.aiConfigurationStatus.stringValue =
                        "模型目录查询失败：\(error.localizedDescription)"
                    self.aiConfigurationStatus.textColor = .systemRed
                }
            }
        }
    }

    @objc private func openSelectedAIProviderDocumentation() {
        guard let url = selectedAIProviderDocumentationURL else {
            aiConfigurationStatus.stringValue = "当前 Provider 没有可打开的 HTTPS 文档地址"
            aiConfigurationStatus.textColor = .systemRed
            return
        }
        guard NSWorkspace.shared.open(url) else {
            aiConfigurationStatus.stringValue = "无法打开 Provider 文档"
            aiConfigurationStatus.textColor = .systemRed
            return
        }
        settingsStatusLabel.stringValue = "已在浏览器打开 Provider 文档"
        settingsStatusLabel.textColor = RimeUI.textMuted
    }

    @objc private func confirmDeleteAIProviderProfile() {
        guard let providerID = selectedAIProviderID else { return }
        let profileName = aiProviderNameField.stringValue
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "删除 Provider？"
        alert.informativeText =
            "将删除本机保存的 \(profileName.isEmpty ? "此 Provider" : profileName) 配置和 API Key。此操作无法撤销。"
        alert.addButton(withTitle: "删除")
        alert.addButton(withTitle: "取消")
        let completion: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .alertFirstButtonReturn else { return }
            self?.deleteAIProviderProfile(id: providerID)
        }
        if let window {
            alert.beginSheetModal(for: window, completionHandler: completion)
        } else {
            completion(alert.runModal())
        }
    }

    private func deleteAIProviderProfile(id providerID: UUID) {
        do {
            try AIProviderProfileCatalogStore.shared.removeProfile(id: providerID)
            selectedAIProviderID = nil
            refreshAIProviderPage(after: "已删除 Provider")
        } catch {
            presentAIProviderConfigurationError(error)
        }
    }

    private func refreshAIProviderPage(after statusMessage: String) {
        settingsStatusLabel.stringValue = statusMessage
        settingsStatusLabel.textColor = RimeUI.textMuted
        DispatchQueue.main.async { [weak self] in
            self?.showCurrentRoute()
        }
    }

    private func presentAIProviderConfigurationError(_ error: Error) {
        aiConfigurationStatus.stringValue =
            "Provider 配置失败：\(error.localizedDescription)"
        aiConfigurationStatus.textColor = .systemRed
        settingsStatusLabel.stringValue = "Provider 配置失败"
        settingsStatusLabel.textColor = .systemRed
    }

    private func pluginsPage(subpageID: String) -> NSView {
        let installButton = SettingsPointingButton(
            title: "安装…",
            target: self,
            action: #selector(showPluginInstallDialog)
        )
        let uninstallButton = SettingsPointingButton(
            title: "卸载…",
            target: self,
            action: #selector(showPluginUninstallDialog)
        )
        let manageButton = SettingsPointingButton(
            title: "管理…",
            target: self,
            action: #selector(showPluginManagementDialog)
        )
        for button in [installButton, uninstallButton, manageButton] {
            button.controlSize = .small
        }
        let actions = NSStackView(views: [installButton, uninstallButton, manageButton])
        actions.orientation = .horizontal
        actions.alignment = .centerY
        actions.spacing = 6

        let heading = NSStackView(views: [flexSpacer(), actions])
        heading.orientation = .horizontal
        heading.alignment = .centerY
        heading.spacing = 12
        heading.translatesAutoresizingMaskIntoConstraints = false
        heading.widthAnchor.constraint(equalToConstant: 650).isActive = true

        pluginRowsStack.removeFromSuperview()

        let showExternal = subpageID == PluginManagementSubpage.all.rawValue
            || subpageID == PluginManagementSubpage.bufferPlugins.rawValue
        let showCapsule = subpageID == PluginManagementSubpage.all.rawValue
            || subpageID == PluginManagementSubpage.capsulePlugins.rawValue
        let showBuiltIns = subpageID == PluginManagementSubpage.all.rawValue
            || subpageID == PluginManagementSubpage.builtInExtensions.rawValue
        var views: [NSView] = showExternal ? [heading, spacer(8)] : []
        if subpageID == PluginManagementSubpage.all.rawValue
            || subpageID == PluginManagementSubpage.mailboxPlugins.rawValue {
            let modules = PluginRegistry.shared.plugins(source: .builtIn).filter {
                $0.descriptor.kind == .mailbox
            }
            views.append(sectionLabel("Mailbox 插件"))
            views.append(cardGrid(modules.map {
                pluginCard($0, mode: .enablement)
            }, columns: 3, height: 116))
            views.append(spacer(16))
        }
        if showCapsule {
            let modules = PluginRegistry.shared.plugins(source: .builtIn).filter {
                $0.descriptor.kind == .capsule
            }
            views.append(sectionLabel("Capsule 插件"))
            views.append(cardGrid(modules.map {
                pluginCard($0, mode: .enablement)
            }, columns: 3, height: 116))
            if showExternal || showBuiltIns { views.append(spacer(16)) }
        }
        if showBuiltIns {
            let builtIns = PluginRegistry.shared.plugins(source: .builtIn).filter {
                $0.descriptor.kind == .extensionModule
            }
            views.append(sectionLabel("内置扩展"))
            views.append(cardGrid(builtIns.map {
                pluginCard($0, mode: .enablement)
            }, columns: 3, height: 116))
            if showExternal { views.append(spacer(16)) }
        }
        if showExternal {
            views.append(sectionLabel("缓冲插件"))
            views.append(pluginRowsStack)
            views.append(pluginStatusLabel)
        }
        return pluginContentColumn(views)
    }

    private func pluginCard(_ plugin: RegisteredPlugin,
                            mode: SettingsPluginSwitchMode) -> NSView {
        let icon = SettingsIconTileView(
            symbolName: plugin.descriptor.symbolName,
            accessibilityDescription: plugin.descriptor.name
        )
        icon.toolTip = plugin.descriptor.name

        let details = SettingsPluginDetailsButton(
            title: "", target: self, action: #selector(showPluginCardDetails(_:))
        )
        details.pluginKey = plugin.descriptor.key
        configureCardIconButton(details, symbol: "info.circle",
                                label: "查看\(plugin.descriptor.name)详情")

        var topViews: [NSView] = [icon, flexSpacer(), details]
        if plugin.isInstalled,
           PluginRegistry.shared.hasConfiguration(for: plugin.descriptor.key) {
            let configure = SettingsPluginConfigurationButton(
                title: "", target: self,
                action: #selector(configureBufferPlugin(_:))
            )
            configure.pluginKey = plugin.descriptor.key
            configureCardIconButton(configure, symbol: "gearshape",
                                    label: "配置\(plugin.descriptor.name)")
            topViews.append(configure)
        }
        let top = NSStackView(views: topViews)
        top.orientation = .horizontal
        top.alignment = .centerY
        top.spacing = 4

        let name = NSTextField(labelWithString: plugin.descriptor.name)
        name.font = .systemFont(ofSize: 11, weight: .semibold)
        name.textColor = RimeUI.textPrimary
        name.lineBreakMode = .byTruncatingTail
        name.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        name.toolTip = plugin.descriptor.summary

        let producer = NSTextField(labelWithString:
            "出品方 · \(plugin.descriptor.producerName)")
        producer.font = .systemFont(ofSize: 9)
        producer.textColor = RimeUI.textMuted
        producer.lineBreakMode = .byTruncatingTail

        let isActive = mode == .bufferEnablement
            && BufferPluginSelectionStore.shared.activeKey == plugin.descriptor.key
        let status = NSTextField(labelWithString: !plugin.isInstalled
            ? "未安装" : isActive ? "正在使用" : plugin.isEnabled ? "已启用" : "已停用")
        status.font = .systemFont(ofSize: 9, weight: .medium)
        status.textColor = isActive ? themeStatusColor : RimeUI.textMuted
        status.setContentHuggingPriority(.required, for: .horizontal)
        var bottomViews: [NSView] = [status, flexSpacer()]
        if plugin.isInstalled {
            let toggle = SettingsPluginSwitch(frame: .zero)
            toggle.pluginKey = plugin.descriptor.key
            toggle.mode = mode
            toggle.state = plugin.isEnabled ? .on : .off
            toggle.controlSize = .small
            toggle.target = self
            toggle.action = #selector(pluginSwitchToggled(_:))
            if plugin.descriptor.key.rawID == ChordExtensionStore.pluginID {
                toggle.isEnabled = !chordExtensionDeploymentInProgress
            }
            toggle.toolTip = mode == .bufferEnablement
                ? (toggle.state == .on
                    ? "停用插件并从工作台移除"
                    : "启用插件并加入工作台")
                : (toggle.state == .on ? "停用" : "启用")
                    + plugin.descriptor.kind.title
            toggle.setAccessibilityLabel(
                mode == .bufferEnablement
                    ? "在缓冲工作台启用\(plugin.descriptor.name)"
                    : "启用\(plugin.descriptor.kind.title)\(plugin.descriptor.name)"
            )
            toggle.setContentHuggingPriority(.required, for: .horizontal)
            bottomViews.append(toggle)
        } else {
            let download = SettingsPluginDownloadButton(
                title: "",
                target: self,
                action: #selector(downloadPresetBufferPlugin(_:))
            )
            download.pluginKey = plugin.descriptor.key
            download.isEnabled = !pluginDownloadInProgress
            configureCardIconButton(download, symbol: "arrow.down.circle",
                                    label: "安装\(plugin.descriptor.name)")
            bottomViews.append(download)
        }
        let bottom = NSStackView(views: bottomViews)
        bottom.orientation = .horizontal
        bottom.alignment = .centerY
        bottom.spacing = 6
        let content = NSStackView(views: [top, name, producer, bottom])
        content.orientation = .vertical
        content.alignment = .leading
        content.spacing = 5
        content.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            top.widthAnchor.constraint(equalTo: content.widthAnchor),
            name.widthAnchor.constraint(equalTo: content.widthAnchor),
            producer.widthAnchor.constraint(equalTo: content.widthAnchor),
            bottom.widthAnchor.constraint(equalTo: content.widthAnchor),
        ])

        let card = NSView()
        card.wantsLayer = true
        card.layer?.backgroundColor = RimeUI.surface2.cgColor
        card.layer?.borderColor = (isActive ? themeStatusColor : RimeUI.border).cgColor
        card.layer?.borderWidth = isActive ? 1.5 : SettingsVisualStyle.hairline(
            backingScale: window?.backingScaleFactor
        )
        card.layer?.cornerRadius = SettingsVisualStyle.cardRadius
        card.layer?.cornerCurve = .continuous
        card.translatesAutoresizingMaskIntoConstraints = false
        card.heightAnchor.constraint(equalToConstant: 116).isActive = true
        card.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 11),
            content.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -11),
            content.topAnchor.constraint(equalTo: card.topAnchor, constant: 10),
            content.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -10),
        ])
        return card
    }

    private func pluginContentColumn(_ views: [NSView]) -> NSView {
        let column = NSStackView(views: views)
        column.orientation = .vertical
        column.alignment = .leading
        column.spacing = 8
        column.edgeInsets = NSEdgeInsets(top: 0, left: 24, bottom: 22, right: 24)
        return column
    }

    /// A disabled preview row for a not-yet-built connection/processor, with a
    /// milestone tag so the settings window shows where the workbench is going
    /// without pretending the control works yet.
    private func comingSoonRow(_ name: String, _ detail: String, _ milestone: String) -> NSView {
        let dot = NSView()
        dot.wantsLayer = true
        dot.layer?.backgroundColor = RimeUI.textMuted.cgColor
        dot.layer?.cornerRadius = 3
        dot.translatesAutoresizingMaskIntoConstraints = false
        dot.widthAnchor.constraint(equalToConstant: 6).isActive = true
        dot.heightAnchor.constraint(equalToConstant: 6).isActive = true

        let nameLabel = NSTextField(labelWithString: name)
        nameLabel.font = .systemFont(ofSize: 13, weight: .medium)
        let detailLabel = NSTextField(labelWithString: detail)
        detailLabel.font = .systemFont(ofSize: 11)
        detailLabel.textColor = RimeUI.textSecondary
        let textCol = NSStackView(views: [nameLabel, detailLabel])
        textCol.orientation = .vertical
        textCol.alignment = .leading
        textCol.spacing = 1

        let tag = NSTextField(labelWithString: milestone)
        tag.font = .monospacedDigitSystemFont(ofSize: 10, weight: .semibold)
        tag.textColor = RimeUI.textMuted

        let row = NSStackView(views: [dot, textCol, flexSpacer(), tag])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        row.translatesAutoresizingMaskIntoConstraints = false
        row.widthAnchor.constraint(equalToConstant: 650).isActive = true
        row.alphaValue = 0.7
        return row
    }

    private func maintenancePage(subpageID: String) -> NSView {
        let checkUpdateBtn = SettingsPointingButton(
            title: "检查更新…",
            target: self,
            action: #selector(checkUpdate)
        )
        let openLogBtn = SettingsPointingButton(
            title: "打开运行日志",
            target: self,
            action: #selector(openRuntimeLog)
        )
        let restartBtn = SettingsPointingButton(
            title: "重启输入法进程",
            target: self,
            action: #selector(restartInputMethod)
        )
        let runtimeButtons = NSStackView(views: [checkUpdateBtn, openLogBtn, restartBtn])
        runtimeButtons.orientation = .horizontal
        runtimeButtons.spacing = 8

        let reinstallBtn = SettingsPointingButton(
            title: "重新安装输入法",
            target: self,
            action: #selector(reinstallInputMethod)
        )
        let openInstallLogBtn = SettingsPointingButton(
            title: "打开安装日志",
            target: self,
            action: #selector(openInstallLog)
        )
        let installButtons = NSStackView(views: [reinstallBtn, openInstallLogBtn])
        installButtons.orientation = .horizontal
        installButtons.spacing = 8

        if subpageID == "permissions" {
            return permissionsPage()
        }
        if subpageID == "logs-data" {
            let openConfigBtn = SettingsPointingButton(
                title: "打开 \(ProductIdentity.displayName) 数据目录",
                target: self,
                action: #selector(openDir)
            )
            openConfigBtn.toolTip =
                "配置、词库学习、插件、统计和练习进度保存在 ~/Library/\(RimesPaths.directoryName)；"
                + "Clipboard History 单独保存在 ~/Library/Application Support/RIMES/clipboard。"
                + "缓冲区正文与发送历史不会持久化。"
            let logButtons = NSStackView(views: [openLogBtn, openInstallLogBtn])
            logButtons.orientation = .horizontal
            logButtons.spacing = 8
            return contentColumn([
                sectionLabel("日志"),
                logButtons,
                spacer(16),
                sectionLabel("本地数据"),
                openConfigBtn,
            ])
        }
        return contentColumn([
            sectionLabel("运行状态"),
            runtimeButtons,
            spacer(12),
            sectionLabel("安装"),
            installButtons,
            installStatus,
        ])
    }

    private func permissionsPage() -> NSView {
        contentColumn([SystemPermissionsView { permission, intent in
            MainActor.assumeIsolated {
                CapturePermissionGuide.shared.show(permission, intent: intent)
            }
        }])
    }

    private func contentColumn(_ views: [NSView]) -> NSView {
        let column = NSStackView(views: views)
        column.orientation = .vertical
        column.alignment = .leading
        column.spacing = 8
        column.edgeInsets = NSEdgeInsets(top: 0, left: 24, bottom: 22, right: 24)
        return column
    }

    private func caption(_ s: String) -> NSTextField {
        let l = NSTextField(wrappingLabelWithString: s)
        l.font = .systemFont(ofSize: 10)
        l.textColor = RimeUI.textMuted
        l.alignment = .left
        return l
    }

    private func sectionLabel(_ s: String) -> NSTextField {
        let l = NSTextField(labelWithString: s)
        l.font = .systemFont(ofSize: 12, weight: .semibold)
        l.textColor = RimeUI.textSecondary
        l.alignment = .left
        return l
    }

    private func secondaryLabel(_ s: String) -> NSTextField {
        let l = NSTextField(wrappingLabelWithString: s)
        l.font = .systemFont(ofSize: 10)
        l.textColor = RimeUI.textMuted
        l.alignment = .left
        return l
    }

    private func spacer(_ h: CGFloat) -> NSView {
        let v = NSView()
        v.translatesAutoresizingMaskIntoConstraints = false
        v.heightAnchor.constraint(equalToConstant: h).isActive = true
        return v
    }

    private func flexSpacer() -> NSView {
        let v = NSView()
        v.setContentHuggingPriority(.defaultLow, for: .horizontal)
        v.setContentHuggingPriority(.defaultLow, for: .vertical)
        return v
    }

    private func candidateArrangementView() -> NSView {
        candidateArrangementPopUp.removeFromSuperview()
        candidateArrangementPopUp.removeAllItems()
        candidateArrangementPopUp.addItems(withTitles: ["横向", "纵向"])
        candidateArrangementPopUp.selectItem(at: CandidateArrangement.current == .vertical ? 1 : 0)
        candidateArrangementPopUp.target = self
        candidateArrangementPopUp.action = #selector(candidateArrangementChanged)
        candidateArrangementPopUp.setAccessibilityLabel("候选排列")
        let row = NSStackView(views: [NSTextField(labelWithString: "候选排列"), candidateArrangementPopUp])
        row.spacing = 12
        return row
    }

    @objc private func candidateArrangementChanged() {
        CandidateArrangement.current = candidateArrangementPopUp.indexOfSelectedItem == 1 ? .vertical : .horizontal
        candidatePreview?.metrics = candidateMetrics(from: liveMetricValues())
    }

    private func candidateMetricsView() -> NSView {
        let rows = CandidateWindowMetric.allCases.map(candidateMetricRow)
        let applyBtn = SettingsPointingButton(
            title: "应用修改",
            target: self,
            action: #selector(applyCandidateMetrics)
        )
        applyBtn.bezelStyle = .rounded
        applyBtn.bezelColor = RimeUI.accentGreen

        let resetBtn = SettingsPointingButton(
            title: "恢复默认",
            target: self,
            action: #selector(resetCandidateMetrics)
        )
        resetBtn.bezelStyle = .rounded
        let actions = NSStackView(views: [applyBtn, resetBtn])
        actions.orientation = .horizontal
        actions.alignment = .centerY
        actions.spacing = 8

        let stack = NSStackView(views: rows + [secondaryLabel("放不下的候选自动进入下一页。"), actions])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 7
        return stack
    }

    private func bufferWidthView() -> NSView {
        bufferWidthSlider.removeFromSuperview()
        bufferWidthField.removeFromSuperview()

        let label = NSTextField(labelWithString: "工作台宽度")
        label.alignment = .right
        label.font = .systemFont(ofSize: 12)
        label.textColor = RimeUI.textSecondary
        label.translatesAutoresizingMaskIntoConstraints = false
        label.widthAnchor.constraint(equalToConstant: 84).isActive = true

        let unit = NSTextField(labelWithString: "px")
        unit.font = .systemFont(ofSize: 11)
        unit.textColor = RimeUI.textMuted

        let reset = SettingsPointingButton(
            title: "恢复默认",
            target: self,
            action: #selector(resetBufferWidth)
        )
        reset.bezelStyle = .rounded

        let row = NSStackView(
            views: [label, bufferWidthSlider, bufferWidthField, unit, reset]
        )
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8

        let stack = NSStackView(views: [row])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 7
        return stack
    }

    private func shortcutSettingsView() -> NSView {
        shortcutFeedbackLabel.removeFromSuperview()
        shortcutFeedbackLabel.stringValue = ""
        shortcutFeedbackLabel.isHidden = true

        let rows = RimeShortcutAction.allCases.map { action -> NSView in
            let titleLabel = NSTextField(labelWithString: action.title)
            titleLabel.font = .systemFont(ofSize: 11, weight: .semibold)
            titleLabel.textColor = RimeUI.textPrimary
            titleLabel.toolTip = action.detail
            titleLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 390).isActive = true

            let recorder = RimeShortcutRecorderButton(action: action)
            recorder.onFeedback = { [weak self] message in
                guard let self else { return }
                self.shortcutFeedbackLabel.stringValue = message ?? ""
                self.shortcutFeedbackLabel.isHidden = message == nil
            }

            let row = NSStackView(views: [titleLabel, flexSpacer(), recorder])
            row.orientation = .horizontal
            row.alignment = .centerY
            row.spacing = 16
            row.edgeInsets = NSEdgeInsets(top: 9, left: 12, bottom: 9, right: 12)
            row.wantsLayer = true
            row.layer?.backgroundColor = RimeUI.surface2.cgColor
            row.layer?.borderColor = RimeUI.border.cgColor
            row.layer?.borderWidth = SettingsVisualStyle.hairline(
                backingScale: window?.backingScaleFactor
            )
            row.layer?.cornerRadius = SettingsVisualStyle.cardRadius
            row.layer?.cornerCurve = .continuous
            row.translatesAutoresizingMaskIntoConstraints = false
            row.widthAnchor.constraint(equalToConstant: 650).isActive = true
            return row
        }

        let reset = SettingsPointingButton(
            title: "恢复全部默认快捷键",
            target: self,
            action: #selector(resetAllShortcuts)
        )
        reset.bezelStyle = .rounded

        let stack = NSStackView(
            views: rows + [shortcutFeedbackLabel, reset]
        )
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        return stack
    }

    private func candidateMetricRow(_ metric: CandidateWindowMetric) -> NSView {
        let label = NSTextField(labelWithString: metric.title)
        label.alignment = .right
        label.font = .systemFont(ofSize: 12)
        label.textColor = RimeUI.textSecondary
        label.translatesAutoresizingMaskIntoConstraints = false
        label.widthAnchor.constraint(equalToConstant: 84).isActive = true

        let unit = NSTextField(labelWithString: metric.unit)
        unit.font = .systemFont(ofSize: 11)
        unit.textColor = RimeUI.textMuted
        unit.translatesAutoresizingMaskIntoConstraints = false
        unit.widthAnchor.constraint(equalToConstant: 20).isActive = true

        let slider = candidateMetricSliders[metric] ?? NSSlider()
        let field = candidateMetricFields[metric] ?? NSTextField(string: "")
        let hint = candidateMetricHints[metric] ?? NSTextField(labelWithString: "")
        slider.removeFromSuperview()
        field.removeFromSuperview()
        hint.removeFromSuperview()

        let row = NSStackView(views: [label, slider, field, unit, hint])
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 8
        return row
    }

    // MARK: State

    private func reload() {
        refreshInputConfigurationSelection()
        alignToInputBoxCheck.state = FocusedInputBoxProbe.alignmentEnabled ? .on : .off
        accessibilityGrantButton.isHidden = FocusedInputBoxProbe.isPermitted
        alignToInputBoxStatusLabel.stringValue = FocusedInputBoxProbe.isPermitted
            ? "已授权辅助功能：工作台会贴合当前输入框；密码框与整页文本区仍跟随光标。"
            : "未授权辅助功能：打开开关会请求权限。未授权时工作台保持跟随光标。"
        clipboardHistoryCheck.state = ClipboardHistoryWindowController.shared.captureEnabled
            ? .on
            : .off
        clipboardAutoPasteCheck.state = ClipboardAutoPaste.enabled ? .on : .off
        clipboardAutoPasteStatusLabel.stringValue = ClipboardAutoPaste.isPermitted
            ? "已开启，图片和文件会自动粘贴。"
            : "开启辅助功能后可自动粘贴，也可手动按 ⌘V。"
        closeAfterLastDeliveryCheck.state = BufferWindowController.shared
            .closeAfterLastDeliveryEnabled ? .on : .off
        resetOnAppSwitchCheck.state = BufferModel.shared.resetOnAppSwitch ? .on : .off
        gatewayEnableCheck.state = LocalGateway.shared.enabled ? .on : .off
        refreshGatewayClientConfigurationPreview()
        refreshAIConnectorSelection()
        _ = refreshAIProviderProfileConfiguration()
        if let idx = (0..<appearancePopUp.numberOfItems).first(where: {
            appearancePopUp.item(at: $0)?.representedObject as? String == RimeUI.appearance.rawValue
        }) {
            appearancePopUp.selectItem(at: idx)
        }
        refreshCandidateMetricControls()
        refreshBufferWidthControls()
        refreshStats()
    }

    private func refreshInputConfigurationSelection() {
        let selectedSchemaID = InputConfigurationStore.shared.selectedSchemaID
        chordSchemaStatusRow?.isHidden = selectedSchemaID
            != ChordExtensionStore.schemaID
        for encoding in InputEncoding.allCases {
            let ordinarySchemaID = InputConfigurationResolver.profiles.first {
                $0.configuration.encoding == encoding
                    && $0.configuration.keyingMode == .sequential
            }?.schemaID
            encodingRadios[encoding]?.state = ordinarySchemaID == selectedSchemaID
                ? .on
                : .off
        }
        for (schemaID, button) in externalSchemaRadios {
            button.state = schemaID == selectedSchemaID ? .on : .off
        }
    }

    private func refreshAIConnectorSelection() {
        let selected = AITextConnectorSelectionStore.shared.selectedKind
        for kind in AITextProviderKind.allCases {
            aiConnectorRadios[kind]?.state = kind == selected ? .on : .off
        }
    }

    private func refreshCodexLoginControls(hasCredential: Bool? = nil) {
        let authenticated = hasCredential
            ?? AITextConnectorRegistry.shared.codexHasStoredChatGPTCredential
        let isRunning = codexLoginOperation != nil
        codexLoginButton.title = AITextCodexLoginPresentation.buttonTitle(
            isRunning: isRunning,
            hasCredential: authenticated
        )
        codexLoginButton.isEnabled = !codexLoginCancelling
        codexCopyLoginLinkButton.isHidden = codexAuthorizationURL == nil
        if isRunning {
            codexLoginSpinner.startAnimation(nil)
        } else {
            codexLoginSpinner.stopAnimation(nil)
        }
        if let codexLoginFeedback {
            codexLoginStatusLabel.stringValue = codexLoginFeedback
            codexLoginStatusLabel.textColor = codexLoginFeedbackIsError
                ? .systemRed
                : RimeUI.textSecondary
            codexLoginStatusLabel.isHidden = false
        } else {
            codexLoginStatusLabel.stringValue = ""
            codexLoginStatusLabel.isHidden = true
        }
    }

    private func codexLoginErrorMessage(_ error: AITextProviderError) -> String {
        switch error {
        case let .unavailable(message), let .invalidConfiguration(message):
            return message
        case .invalidResult:
            return "Codex 登录响应无效，请重试。"
        case .resultTooLarge:
            return "Codex 登录响应异常过大，已安全中止。"
        case .timedOut:
            return "等待 Codex 登录超时，请重新发起授权。"
        case .cancelled:
            return "已取消 Codex 登录。"
        case .failed:
            return "Codex 登录暂时不可用，请重试。"
        }
    }

    private func refreshClaudeLoginControls(authenticationStatus: Bool? = nil) {
        let status = authenticationStatus
            ?? AITextConnectorRegistry.shared.claudeAuthenticationStatus
        let isRunning = claudeLoginOperation != nil
        claudeLoginButton.title = AITextClaudeLoginPresentation.buttonTitle(
            isRunning: isRunning,
            authenticationStatus: status
        )
        claudeLoginButton.isEnabled = !claudeLoginCancelling
        if isRunning {
            claudeLoginSpinner.startAnimation(nil)
        } else {
            claudeLoginSpinner.stopAnimation(nil)
        }
        if let claudeLoginFeedback {
            claudeLoginStatusLabel.stringValue = claudeLoginFeedback
            claudeLoginStatusLabel.textColor = claudeLoginFeedbackIsError
                ? .systemRed
                : RimeUI.textSecondary
            claudeLoginStatusLabel.isHidden = false
        } else {
            claudeLoginStatusLabel.stringValue = ""
            claudeLoginStatusLabel.isHidden = true
        }
    }

    private func claudeLoginErrorMessage(_ error: AITextProviderError) -> String {
        switch error {
        case let .unavailable(message), let .invalidConfiguration(message):
            return message
        case .invalidResult:
            return "Claude 登录响应无效，请重试。"
        case .resultTooLarge:
            return "Claude 登录响应异常过大，已安全中止。"
        case .timedOut:
            return "等待 Claude 登录超时，请重新发起授权。"
        case .cancelled:
            return "已取消 Claude 登录。"
        case .failed:
            return "Claude 登录暂时不可用，请重试。"
        }
    }

    private func refreshPluginList(statusMessage: String? = nil) {
        let plugins = PluginRegistry.shared.plugins(capability: .bufferAction)
        pluginRowsStack.arrangedSubviews.forEach {
            pluginRowsStack.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }

        if plugins.isEmpty {
            let empty = NSTextField(wrappingLabelWithString:
                "当前没有可用的缓冲插件。")
            empty.alignment = .center
            empty.font = .systemFont(ofSize: 12)
            empty.textColor = RimeUI.textSecondary
            empty.translatesAutoresizingMaskIntoConstraints = false
            empty.heightAnchor.constraint(equalToConstant: 58).isActive = true
            pluginRowsStack.addArrangedSubview(empty)
        } else {
            pluginRowsStack.addArrangedSubview(cardGrid(plugins.map {
                pluginCard($0, mode: .bufferEnablement)
            }, columns: 3, height: 116))
        }

        if let statusMessage {
            setPluginStatus(statusMessage)
        } else if !pluginDownloadInProgress {
            let installedCount = plugins.filter(\.isInstalled).count
            let enabledCount = plugins.filter(\.isEnabled).count
            let activeName = BufferPluginSelectionStore.shared.activeKey.flatMap { key in
                plugins.first(where: { $0.descriptor.key == key })?.descriptor.name
            }
            let current = activeName ?? BufferPluginMenuCatalog.defaultTitle
            setPluginStatus(
                "已安装 \(installedCount) 个，已开启 \(enabledCount) 个；工作台当前：\(current)"
            )
        }
    }

    /// Local manager notifications are posted synchronously. Defer and
    /// coalesce the rebuild so an AppKit control is not removed from its row
    /// while that control's action selector is still executing.
    private func schedulePluginListRefresh() {
        guard !pluginRefreshScheduled else { return }
        pluginRefreshScheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.pluginRefreshScheduled = false
            guard self.selectedCoreRoute == .plugins else { return }
            self.refreshPluginList()
        }
    }

    private func setPluginDownloadInProgress(_ inProgress: Bool) {
        pluginDownloadInProgress = inProgress
    }

    private func setPluginStatus(_ message: String, isError: Bool = false) {
        pluginStatusLabel.stringValue = message
        pluginStatusLabel.textColor = isError ? .systemRed : RimeUI.textSecondary
        pluginStatusLabel.toolTip = message
        settingsStatusLabel.stringValue = message
        settingsStatusLabel.textColor = isError ? .systemRed : RimeUI.textMuted
    }

    private func refreshCandidateMetricControls() {
        candidateArrangementPopUp.selectItem(at: CandidateArrangement.current == .vertical ? 1 : 0)
        var stored: [CandidateWindowMetric: Double] = [:]
        for metric in CandidateWindowMetric.allCases {
            stored[metric] = Double(CandidateWindowMetrics.value(for: metric))
        }
        updateCandidateControls(resolveMetricValues(stored))
    }

    private func refreshBufferWidthControls() {
        let width = Double(BufferWindowController.shared.configuredWidth)
        bufferWidthSlider.doubleValue = width
        bufferWidthField.stringValue = String(Int(width.rounded()))
    }

    /// Current (possibly unsaved) values straight off the live controls.
    private func liveMetricValues() -> [CandidateWindowMetric: Double] {
        var values: [CandidateWindowMetric: Double] = [:]
        for metric in CandidateWindowMetric.allCases {
            values[metric] = candidateMetricSliders[metric]?.doubleValue
                ?? Double(CandidateWindowMetrics.value(for: metric))
        }
        return values
    }

    /// Resolve raw control values through the full dependency chain so an
    /// unsupported interval can never be previewed or committed.
    private func resolveMetricValues(_ raw: [CandidateWindowMetric: Double]) -> [CandidateWindowMetric: Double] {
        CandidateWindowMetrics.resolvedValues(raw)
    }

    /// Push a resolved value set into every control (value + supported bounds +
    /// constraint hint) and refresh the live preview. Does NOT persist.
    private func updateCandidateControls(_ values: [CandidateWindowMetric: Double]) {
        for metric in CandidateWindowMetric.allCases {
            let supported = metric.supportedRange(given: values)
            let value = values[metric] ?? metric.defaultValue

            if let slider = candidateMetricSliders[metric] {
                slider.minValue = supported.lowerBound
                slider.maxValue = supported.upperBound
                slider.doubleValue = value
            }
            candidateMetricFields[metric]?.stringValue = formatMetricValue(CGFloat(value))
            (candidateMetricFields[metric]?.formatter as? NumberFormatter)?
                .maximum = NSNumber(value: supported.upperBound)

            if let hint = candidateMetricHints[metric] {
                let capped = supported.upperBound < metric.range.upperBound - 0.5
                if capped, let dep = metric.containerMetric {
                    hint.stringValue = "≤ \(Int(supported.upperBound))（受\(dep.metric.title)限制）"
                    hint.isHidden = false
                } else {
                    hint.stringValue = ""
                    hint.isHidden = true
                }
            }
        }
        candidatePreview?.metrics = candidateMetrics(from: values)
    }

    private func candidateMetrics(from values: [CandidateWindowMetric: Double]) -> CandidateWindowMetrics {
        func get(_ metric: CandidateWindowMetric) -> CGFloat {
            CGFloat(values[metric] ?? metric.defaultValue)
        }
        return CandidateWindowMetrics(
            baseWidth: get(.baseWidth),
            compactStripHeight: get(.compactStripHeight),
            compactCandidateHeight: get(.compactCandidateHeight),
            preeditHeight: get(.preeditHeight),
            candidateFontSize: get(.candidateFontSize),
            labelFontSize: get(.labelFontSize),
            candidatesPerPage: Int(get(.candidatesPerPage)),
            verticalCandidates: CandidateArrangement.current == .vertical
        )
    }

    private func formatMetricValue(_ value: CGFloat) -> String {
        "\(Int(value.rounded()))"
    }

    private func refreshStats() {
        let snapshot = KeyFrequencyStore.shared.snapshot(for: statsDatePicker.dateValue)
        heatmapView.snapshot = snapshot
        statsSummary.stringValue = "\(snapshot.dayKey) · 总按键 \(snapshot.total) 次 · 覆盖 \(snapshot.counts.count) 个键"
        if let top = snapshot.topKeyId {
            let count = snapshot.counts[top] ?? 0
            let ratio = snapshot.total > 0 ? Double(count) / Double(snapshot.total) * 100 : 0
            statsTopKey.stringValue = "最高频：\(KeyboardLayout.displayName(for: top)) · \(count) 次 · \(String(format: "%.1f", ratio))%"
        } else {
            statsTopKey.stringValue = "最高频：暂无"
        }
    }

    // MARK: Actions

    @objc private func openMailboxWindowFromSettings() {
        let threadID = MailboxStore.shared.selectLatestUnreadOrMostRecent()
        MailboxWindowController.shared.show(selecting: threadID)
        settingsStatusLabel.stringValue = "已打开独立 Mailbox 窗口"
        settingsStatusLabel.textColor = RimeUI.textMuted
    }

    @objc private func openMailboxStorageDirectory() {
        openUtilityDirectory(
            MailboxStore.shared.storageDirectoryURL,
            title: "Mailbox 存储目录"
        )
    }

    @objc private func openAIModelSettingsFromMailbox() {
        guard navigation.selectRoute(
            SettingsCoreRoute.connectors.id,
            catalog: routeCatalog
        ), navigation.selectSubpage(
            SettingsSubpageID(rawValue: "ai-model"),
            catalog: routeCatalog
        ) else { return }
        settingsStatusLabel.stringValue = "已打开 AI 模型设置"
        settingsStatusLabel.textColor = RimeUI.textMuted
        reload()
        showCurrentRoute()
    }

    @objc private func openCapsuleStorageDirectory() {
        openUtilityDirectory(
            CapsuleContentStore.shared.rootURL,
            title: "Capsule 资料目录"
        )
    }

    private func openUtilityDirectory(_ url: URL, title: String) {
        do {
            try FileManager.default.createDirectory(
                at: url,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            guard NSWorkspace.shared.open(url) else {
                throw CocoaError(.fileNoSuchFile)
            }
            settingsStatusLabel.stringValue = "已打开\(title)"
            settingsStatusLabel.textColor = RimeUI.textMuted
        } catch {
            settingsStatusLabel.stringValue = "无法打开\(title)：\(error.localizedDescription)"
            settingsStatusLabel.textColor = .systemRed
        }
    }

    @objc private func chooseCapsuleCloudFolder() {
        let panel = NSOpenPanel()
        panel.title = "选择 Capsule iCloud Drive 文件夹"
        panel.message = "请在 iCloud Drive 中新建或选择一个空文件夹。普通条目和图片、PDF 会同步；密码、Skill 路径、查看口令与主密钥不会上传。"
        panel.prompt = "使用此文件夹"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        panel.resolvesAliases = false
        let home = FileManager.default.homeDirectoryForCurrentUser
        let candidates = [
            home.appendingPathComponent(
                "Library/Mobile Documents/com~apple~CloudDocs",
                isDirectory: true
            ),
            home.appendingPathComponent(
                "Library/CloudStorage",
                isDirectory: true
            ),
        ]
        panel.directoryURL = candidates.first {
            FileManager.default.fileExists(atPath: $0.path)
        }
        let handle: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            self?.settingsStatusLabel.stringValue = "正在配置 Capsule iCloud 同步…"
            self?.settingsStatusLabel.textColor = RimeUI.textMuted
            CapsuleCloudSyncController.shared.configure(folderURL: url) {
                [weak self] result in
                switch result {
                case .success:
                    self?.settingsStatusLabel.stringValue = "已启用 Capsule iCloud 自动同步"
                    self?.settingsStatusLabel.textColor = RimeUI.textMuted
                case let .failure(error):
                    self?.presentCapsuleCloudSyncError(error)
                }
            }
        }
        if let window {
            panel.beginSheetModal(for: window, completionHandler: handle)
        } else {
            handle(panel.runModal())
        }
    }

    @objc private func syncCapsuleCloudNow() {
        guard CapsuleCloudSyncController.shared.status.isConfigured else { return }
        settingsStatusLabel.stringValue = "正在同步 Capsule…"
        settingsStatusLabel.textColor = RimeUI.textMuted
        CapsuleCloudSyncController.shared.requestSync(after: 0)
    }

    @objc private func disableCapsuleCloudSync() {
        guard CapsuleCloudSyncController.shared.status.isConfigured else { return }
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = "停用 Capsule iCloud 自动同步？"
        alert.informativeText = "这只会移除本机同步配置，不会删除本机或 iCloud Drive 中的内容。"
        alert.addButton(withTitle: "停用同步")
        alert.addButton(withTitle: "取消")
        let handle: (NSApplication.ModalResponse) -> Void = { [weak self] response in
            guard response == .alertFirstButtonReturn else { return }
            CapsuleCloudSyncController.shared.disable { [weak self] result in
                switch result {
                case .success:
                    self?.settingsStatusLabel.stringValue = "已停用 Capsule iCloud 自动同步"
                    self?.settingsStatusLabel.textColor = RimeUI.textMuted
                case let .failure(error):
                    self?.presentCapsuleCloudSyncError(error)
                }
            }
        }
        if let window {
            alert.beginSheetModal(for: window, completionHandler: handle)
        } else {
            handle(alert.runModal())
        }
    }

    private func presentCapsuleCloudSyncError(_ error: Error) {
        settingsStatusLabel.stringValue = error.localizedDescription
        settingsStatusLabel.textColor = .systemRed
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Capsule iCloud 同步配置失败"
        alert.informativeText = error.localizedDescription
        alert.addButton(withTitle: "好")
        if let window {
            alert.beginSheetModal(for: window)
        } else {
            alert.runModal()
        }
    }

    @objc private func routeChosen(_ sender: SettingsRouteButton) {
        window?.makeFirstResponder(sender)
        if sender.routeID == navigation.currentRouteID {
            refreshSidebarSelection()
            return
        }
        guard confirmLeavingChordEditor() else { return }
        guard navigation.selectRoute(sender.routeID, catalog: routeCatalog) else { return }
        if let route = routeCatalog.route(for: sender.routeID) {
            settingsStatusLabel.stringValue = route.id == SettingsCoreRoute.rimes.id
                ? "" : "已打开\(route.title)"
            settingsStatusLabel.textColor = RimeUI.textMuted
        }
        reload()
        showCurrentRoute()
    }

    @objc private func subpageChosen(_ sender: NSSegmentedControl) {
        guard let route = selectedRoute,
              route.subpages.indices.contains(sender.selectedSegment) else { return }
        let subpage = route.subpages[sender.selectedSegment].id
        if subpage == navigation.selectedSubpage() { return }
        guard confirmLeavingChordEditor() else {
            if let old = route.subpages.firstIndex(where: { $0.id == navigation.selectedSubpage() }) {
                sender.selectedSegment = old
            }
            return
        }
        guard navigation.selectSubpage(subpage, catalog: routeCatalog) else { return }
        settingsStatusLabel.stringValue = route.id == SettingsCoreRoute.rimes.id
            ? "" : "已打开\(route.subpages[sender.selectedSegment].title)"
        settingsStatusLabel.textColor = RimeUI.textMuted
        showCurrentRoute()
    }

    @objc private func openPluginDirectory() {
        let directory = ActionPluginManager.shared.rootURL
        do {
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            NSWorkspace.shared.open(directory)
        } catch {
            setPluginStatus("无法打开插件目录：\(error.localizedDescription)", isError: true)
        }
    }

    @objc private func showPluginInstallDialog() {
        guard let window, !pluginDownloadInProgress else { return }
        let alert = NSAlert()
        alert.messageText = "安装缓冲插件"
        alert.informativeText = "从本地插件目录、manifest.json 文件，或 HTTPS 清单地址安装。"
        alert.alertStyle = .informational
        alert.addButton(withTitle: "本地文件…")
        alert.addButton(withTitle: "HTTPS 地址…")
        alert.addButton(withTitle: "取消")
        alert.beginSheetModal(for: window) { [weak self] response in
            guard let self else { return }
            DispatchQueue.main.async {
                switch response {
                case .alertFirstButtonReturn:
                    self.installLocalPlugin()
                case .alertSecondButtonReturn:
                    self.showRemotePluginInstallDialog()
                default:
                    break
                }
            }
        }
    }

    private func showRemotePluginInstallDialog() {
        guard let window, !pluginDownloadInProgress else { return }
        let field = NSTextField(string: "")
        field.placeholderString = "https://example.com/plugin/manifest.json"
        field.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        field.translatesAutoresizingMaskIntoConstraints = false
        field.widthAnchor.constraint(equalToConstant: 430).isActive = true

        let alert = NSAlert()
        alert.messageText = "从 HTTPS 安装"
        alert.informativeText = "只下载并验证 manifest.json，不会执行安装脚本。"
        alert.accessoryView = field
        alert.addButton(withTitle: "安装")
        alert.addButton(withTitle: "取消")
        alert.beginSheetModal(for: window) { [weak self] response in
            guard let self, response == .alertFirstButtonReturn else { return }
            self.installRemotePlugin(from: field.stringValue)
        }
    }

    @objc private func showPluginUninstallDialog() {
        guard let window else { return }
        let plugins = PluginRegistry.shared.plugins(capability: .bufferAction)
            .filter { $0.isInstalled && $0.descriptor.canUninstall }
        guard !plugins.isEmpty else {
            info("当前没有可以卸载的插件。内置插件随应用提供，不能单独卸载。")
            return
        }

        let popup = RimeFixedAccentPopUpButton()
        popup.translatesAutoresizingMaskIntoConstraints = false
        popup.widthAnchor.constraint(equalToConstant: 360).isActive = true
        for plugin in plugins {
            popup.addItem(withTitle: "\(plugin.descriptor.name)  ·  v\(plugin.descriptor.version)")
            popup.lastItem?.representedObject = plugin.descriptor.key
        }

        let alert = NSAlert()
        alert.messageText = "卸载插件"
        alert.informativeText = "卸载会停止插件并移除安装内容，保留已有配置和用户数据。"
        alert.alertStyle = .warning
        alert.accessoryView = popup
        alert.addButton(withTitle: "卸载")
        alert.addButton(withTitle: "取消")
        alert.beginSheetModal(for: window) { [weak self] response in
            guard let self,
                  response == .alertFirstButtonReturn,
                  let key = popup.selectedItem?.representedObject as? PluginKey else { return }
            self.uninstallPlugin(key: key)
        }
    }

    @objc private func showPluginManagementDialog() {
        guard let window else { return }
        let bufferPlugins = PluginRegistry.shared.plugins(capability: .bufferAction)
        let externalCount = bufferPlugins.filter { $0.descriptor.source == .external }.count
        let details = NSTextField(wrappingLabelWithString:
            "缓冲插件：\(bufferPlugins.count)\n外部插件：\(externalCount)\n目录：\(ActionPluginManager.shared.rootURL.path)")
        details.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        details.textColor = RimeUI.textSecondary
        details.translatesAutoresizingMaskIntoConstraints = false
        details.widthAnchor.constraint(equalToConstant: 430).isActive = true

        let alert = NSAlert()
        alert.messageText = "管理插件"
        alert.informativeText = "刷新插件清单，或在 Finder 中查看外部插件文件。"
        alert.accessoryView = details
        alert.addButton(withTitle: "刷新")
        alert.addButton(withTitle: "打开插件目录")
        alert.addButton(withTitle: "完成")
        alert.beginSheetModal(for: window) { [weak self] response in
            guard let self else { return }
            switch response {
            case .alertFirstButtonReturn:
                self.refreshPluginList(statusMessage: "插件列表已刷新")
                ActionPluginHost.shared.refreshStatuses(force: true)
            case .alertSecondButtonReturn:
                self.openPluginDirectory()
            default:
                break
            }
        }
    }

    @objc private func installLocalPlugin() {
        guard let window else { return }
        let panel = NSOpenPanel()
        panel.title = "安装工作台插件"
        panel.message = "选择包含 manifest.json 的目录，或直接选择 manifest.json 文件。"
        panel.prompt = "安装"
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.resolvesAliases = true
        panel.beginSheetModal(for: window) { [weak self] response in
            guard let self, response == .OK, let source = panel.url else { return }
            do {
                let plugin = try ActionPluginManager.shared.installLocal(url: source)
                self.refreshPluginList(statusMessage: "已安装或更新插件：\(plugin.name)")
            } catch {
                self.setPluginStatus("本地安装失败：\(error.localizedDescription)", isError: true)
                self.refreshPluginList()
            }
        }
    }

    private func installRemotePlugin(from rawValue: String) {
        guard !pluginDownloadInProgress else { return }
        let raw = rawValue
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: raw),
              url.scheme?.lowercased() == "https",
              !raw.isEmpty else {
            setPluginStatus("请输入有效的 HTTPS manifest.json 地址", isError: true)
            return
        }
        setPluginDownloadInProgress(true)
        setPluginStatus("正在下载并验证插件清单…")
        ActionPluginManager.shared.installRemote(url: url) { [weak self] result in
            guard let self else { return }
            self.setPluginDownloadInProgress(false)
            switch result {
            case let .success(plugin):
                self.refreshPluginList(statusMessage: "已安装或更新插件：\(plugin.name)")
            case let .failure(error):
                self.setPluginStatus("下载安装失败：\(error.localizedDescription)", isError: true)
                self.refreshPluginList()
            }
        }
    }

    @objc private func downloadPresetBufferPlugin(
        _ sender: SettingsPluginDownloadButton
    ) {
        guard !pluginDownloadInProgress,
              sender.pluginKey.domain == .builtIn else { return }
        let pluginName = PluginRegistry.shared.allPlugins()
            .first(where: { $0.descriptor.key == sender.pluginKey })?
            .descriptor.name ?? sender.pluginKey.rawID
        setPluginDownloadInProgress(true)
        refreshPluginList(statusMessage: "正在安装并校验 \(pluginName)…")
        PresetBufferPluginInstallationStore.shared.install(
            id: sender.pluginKey.rawID
        ) { [weak self] result in
            guard let self else { return }
            self.setPluginDownloadInProgress(false)
            switch result {
            case let .success(entry):
                self.refreshPluginList(
                    statusMessage: "已安装 \(entry.nameZH)，默认保持关闭；可使用右侧开关启用"
                )
            case let .failure(error):
                self.refreshPluginList(
                    statusMessage: "下载安装失败：\(error.localizedDescription)"
                )
                self.setPluginStatus(
                    "下载安装失败：\(error.localizedDescription)",
                    isError: true
                )
            }
        }
    }

    @objc private func pluginSwitchToggled(_ sender: SettingsPluginSwitch) {
        let on = sender.state == .on
        let descriptor = PluginRegistry.shared.allPlugins()
            .first(where: { $0.descriptor.key == sender.pluginKey })?
            .descriptor
        let pluginName = descriptor?.name ?? sender.pluginKey.rawID
        if sender.pluginKey.domain == .builtIn,
           sender.pluginKey.rawID == ChordExtensionStore.pluginID {
            applyChordExtensionEnablement(
                on,
                sender: sender,
                pluginName: pluginName
            )
            return
        }
        do {
            switch sender.mode {
            case .bufferEnablement:
                try PluginRegistry.shared.setEnabled(on, for: sender.pluginKey)
                setPluginStatus(on
                    ? "已启用并加入工作台：\(pluginName)"
                    : "已停用并从工作台移除：\(pluginName)")
            case .enablement:
                try PluginRegistry.shared.setEnabled(on, for: sender.pluginKey)
                let kind = descriptor?.kind.title ?? "插件"
                setPluginStatus(on
                    ? "已启用\(kind)：\(pluginName)"
                    : "已停用\(kind)：\(pluginName)")
            }
            DispatchQueue.main.async { [weak self] in self?.refreshPluginList() }
        } catch {
            setPluginStatus("更新插件状态失败：\(error.localizedDescription)", isError: true)
            refreshPluginList()
        }
    }

    /// Changing the optional chord extension also changes the deployed Rime
    /// schema set. Runtime gating happens immediately; the schema file and
    /// compiled deployment then move together, followed by the normal IME
    /// process relaunch. A failed deploy restores the previous feature state,
    /// selected schema and schema list before attempting a recovery deploy.
    private func applyChordExtensionEnablement(
        _ enabled: Bool,
        sender: SettingsPluginSwitch,
        pluginName: String
    ) {
        let store = ChordExtensionStore.shared
        guard !chordExtensionDeploymentInProgress,
              !ChordKeymapActivationCoordinator.shared.isApplying else {
            sender.state = store.isEnabled ? .on : .off
            return
        }
        let previousEnabled = store.isEnabled
        guard previousEnabled != enabled else {
            sender.state = previousEnabled ? .on : .off
            return
        }

        let schemaListURL = userDir.appendingPathComponent("default.custom.yaml")
        let storedPreviousIDs = SchemaListStore.enabledIDs(at: schemaListURL)
        let previousIDs = storedPreviousIDs.isEmpty
            ? InputSchemaCatalog.enabledIDs(
                chordExtensionEnabled: previousEnabled
              )
            : storedPreviousIDs
        let previousSchemaID = InputConfigurationStore.shared.selectedSchemaID

        RIMESController.active?.forceCommit()
        do {
            if enabled {
                try ChordKeymapRuntimeFiles(root: userDir).writeSchema(
                    for: ChordKeymapStore.shared.activeProfile
                )
            }
            try PluginRegistry.shared.setEnabled(enabled, for: sender.pluginKey)
            let nextIDs = InputSchemaCatalog.enabledIDs(
                chordExtensionEnabled: store.isEnabled
            )
            try persistSchemaSelection(nextIDs)
        } catch {
            _ = store.setEnabled(previousEnabled, source: .rollback)
            _ = InputConfigurationStore.shared.select(schemaID: previousSchemaID)
            RIMESController.applyStoredInputConfiguration()
            try? SchemaListStore.writeEnabledIDs(previousIDs, to: schemaListURL)
            sender.state = previousEnabled ? .on : .off
            setPluginStatus(
                "无法更新\(pluginName)：\(error.localizedDescription)",
                isError: true
            )
            refreshPluginList()
            return
        }

        chordExtensionDeploymentInProgress = true
        ChordKeymapActivationCoordinator.shared.extensionDeploymentInProgress = true
        sender.isEnabled = false
        setPluginStatus(
            enabled
                ? "正在启用\(pluginName)并部署输入方案…"
                : "正在停用\(pluginName)并部署普通输入方案…"
        )
        refreshPluginList()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            _ = rimeEngine.start()
            let deployed = BBRimeDeploy()
            if deployed {
                rimeEngine.invalidateSchemaListCacheAfterDeployment()
            }
            IMELog.write(
                "settings: chord extension enabled=\(enabled) deploy=\(deployed)"
            )
            DispatchQueue.main.async { [weak self, weak sender] in
                guard let self else { return }
                self.chordExtensionDeploymentInProgress = false
                ChordKeymapActivationCoordinator.shared.extensionDeploymentInProgress = false
                if deployed {
                    self.setPluginStatus(
                        enabled
                            ? "已启用\(pluginName)，输入法正在重启"
                            : "已停用\(pluginName)，输入法正在重启"
                    )
                    InputMetricsPersistence.saveNow()
                    exit(0)
                }

                _ = store.setEnabled(previousEnabled, source: .rollback)
                _ = InputConfigurationStore.shared.select(
                    schemaID: previousSchemaID
                )
                RIMESController.applyStoredInputConfiguration()
                let restoredList: Bool
                do {
                    try SchemaListStore.writeEnabledIDs(
                        previousIDs,
                        to: schemaListURL
                    )
                    restoredList = true
                } catch {
                    restoredList = false
                    IMELog.write(
                        "settings: chord extension rollback schema list failed "
                            + error.localizedDescription
                    )
                }
                sender?.state = previousEnabled ? .on : .off
                self.setPluginStatus(
                    restoredList
                        ? "部署失败，已恢复之前的\(pluginName)状态；输入法没有重启"
                        : "部署失败，已恢复运行状态，但方案文件恢复失败；请查看运行日志",
                    isError: true
                )
                self.refreshPluginList()

                guard restoredList else { return }
                DispatchQueue.global(qos: .utility).async {
                    let recovered = BBRimeDeploy()
                    if recovered {
                        rimeEngine.invalidateSchemaListCacheAfterDeployment()
                    }
                    IMELog.write(
                        "settings: chord extension rollback deploy=\(recovered)"
                    )
                }
            }
        }
    }

    @objc private func configureBufferPlugin(
        _ sender: SettingsPluginConfigurationButton
    ) {
        presentPluginConfiguration(pluginKey: sender.pluginKey)
    }

    @objc private func showPluginCardDetails(_ sender: SettingsPluginDetailsButton) {
        guard let plugin = PluginRegistry.shared.allPlugins().first(where: {
            $0.descriptor.key == sender.pluginKey
        }) else { return }
        let installation: String
        if plugin.descriptor.source == .external { installation = "外部插件" }
        else if !plugin.isInstalled { installation = "未下载" }
        else if PresetBufferPluginCatalog.entry(id: plugin.descriptor.key.rawID)?
                    .defaultInstalled == true { installation = "已预装" }
        else { installation = "已安装" }
        showCardDetails(title: plugin.descriptor.name,
                        metadata: "\(plugin.descriptor.kind.title) · v\(plugin.descriptor.version) · \(installation) · 出品方：\(plugin.descriptor.producerName)",
                        detail: plugin.descriptor.summary,
                        from: sender)
    }

    @objc private func showThemeCardDetails(_ sender: SettingsThemeDetailsButton) {
        let mode = sender.mode
        showCardDetails(title: mode.title,
                        metadata: mode.family == .classic ? "经典配色" : "独立主题",
                        detail: mode.detailText,
                        from: sender)
    }

    private func showCardDetails(title: String, metadata: String,
                                 detail: String, from button: NSButton) {
        cardDetailsPopover?.close()
        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        let metadataLabel = NSTextField(labelWithString: metadata)
        metadataLabel.font = .systemFont(ofSize: 10)
        metadataLabel.textColor = RimeUI.textMuted
        let detailLabel = NSTextField(wrappingLabelWithString: detail)
        detailLabel.font = .systemFont(ofSize: 11)
        detailLabel.textColor = RimeUI.textSecondary
        detailLabel.maximumNumberOfLines = 4
        let stack = NSStackView(views: [titleLabel, metadataLabel, detailLabel])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 7
        stack.translatesAutoresizingMaskIntoConstraints = false
        let body = NSView(frame: NSRect(x: 0, y: 0, width: 260, height: 150))
        body.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: body.leadingAnchor, constant: 14),
            stack.trailingAnchor.constraint(equalTo: body.trailingAnchor, constant: -14),
            stack.topAnchor.constraint(equalTo: body.topAnchor, constant: 12),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: body.bottomAnchor, constant: -12),
        ])
        let controller = NSViewController()
        controller.view = body
        let popover = NSPopover()
        popover.behavior = .transient
        popover.contentViewController = controller
        popover.contentSize = body.frame.size
        cardDetailsPopover = popover
        popover.show(relativeTo: button.bounds, of: button, preferredEdge: .maxY)
    }

    private func presentPluginConfiguration(pluginKey: PluginKey) {
        guard let parentWindow = window,
              pluginConfigurationSheet == nil else { return }
        let plugin = PluginRegistry.shared.allPlugins().first {
            $0.descriptor.key == pluginKey
        }
        do {
            guard let controller = try PluginRegistry.shared
                .makePluginConfigurationViewController(
                    pluginKey: pluginKey
                ) else {
                setPluginStatus("这个插件当前没有可配置项", isError: true)
                return
            }
            let sheet = PluginConfigurationSheetFactory.make(
                contentViewController: controller,
                title: "\(plugin?.descriptor.name ?? "插件") 设置"
            )
            pluginConfigurationSheet = sheet
            if let form = controller as? PluginConfigurationViewController {
                form.onDismiss = { [weak self, weak parentWindow, weak sheet] in
                    guard let self, let sheet else { return }
                    parentWindow?.endSheet(sheet)
                    sheet.orderOut(nil)
                    self.pluginConfigurationSheet = nil
                    self.refreshPluginList()
                }
            }
            parentWindow.beginSheet(sheet)
        } catch {
            setPluginStatus(
                "无法打开插件设置：\(error.localizedDescription)",
                isError: true
            )
        }
    }

    private func uninstallPlugin(key: PluginKey) {
        if key.domain == .builtIn {
            do {
                try PluginRegistry.shared.uninstallInternalPlugin(key)
                refreshPluginList(statusMessage: "插件已卸载，配置和用户数据已保留")
            } catch {
                setPluginStatus("卸载失败：\(error.localizedDescription)", isError: true)
                refreshPluginList()
            }
            return
        }
        guard let plugin = ActionPluginManager.shared.listInstalledPlugins()
            .first(where: { $0.id == key.rawID }) else {
            setPluginStatus("插件列表已经变化，请刷新后重试", isError: true)
            refreshPluginList()
            return
        }
        do {
            try PluginRegistry.shared.setBufferPluginActive(
                false,
                for: PluginKey(domain: .externalActionV1, rawID: plugin.id)
            )
            try ActionPluginManager.shared.uninstall(id: plugin.id)
            refreshPluginList(statusMessage: "已卸载插件：\(plugin.name)")
        } catch {
            setPluginStatus("卸载失败：\(error.localizedDescription)", isError: true)
            refreshPluginList()
        }
    }

    @objc private func inputEncodingSelected(_ sender: RimeFixedAccentChoiceButton) {
        guard InputEncoding.allCases.indices.contains(sender.tag) else { return }
        _ = InputConfigurationStore.shared.select(
            encoding: InputEncoding.allCases[sender.tag]
        )
        RIMESController.applyStoredInputConfiguration()
        reload()
    }

    @objc private func externalInputSchemaSelected(_ sender: RimeFixedAccentChoiceButton) {
        let index = sender.tag - InputEncoding.allCases.count
        guard externalSchemaChoiceIDs.indices.contains(index),
              InputConfigurationStore.shared.select(schemaID: externalSchemaChoiceIDs[index]) else { return }
        RIMESController.applyStoredInputConfiguration()
        reload()
    }

    @objc private func aiConnectorSelected(_ sender: RimeFixedAccentChoiceButton) {
        guard AITextProviderKind.allCases.indices.contains(sender.tag) else { return }
        let kind = AITextProviderKind.allCases[sender.tag]
        let changed = AITextConnectorRegistry.shared.select(kind)
        refreshAIConnectorSelection()
        if changed {
            settingsStatusLabel.stringValue = "已切换到 \(kind.displayName)"
            settingsStatusLabel.textColor = RimeUI.textMuted
        }
        BufferWindowController.shared.refresh()
        RIMESController.refreshActiveUI()
    }

    @objc private func codexLoginButtonPressed() {
        if let operation = codexLoginOperation {
            codexLoginCancelling = true
            codexAuthorizationURL = nil
            codexLoginFeedback = "正在取消 Codex 登录…"
            codexLoginFeedbackIsError = false
            refreshCodexLoginControls()
            operation.cancel()
            return
        }

        let sessionID = UUID()
        codexLoginSessionID = sessionID
        codexLoginCancelling = false
        codexAuthorizationURL = nil
        codexLoginFeedback = AITextCodexLoginStatus.launching.displayText
        codexLoginFeedbackIsError = false

        let operation = AITextCodexLoginOperation(
            onAuthorizationURL: { [weak self] url in
                guard let self,
                      self.codexLoginSessionID == sessionID,
                      !self.codexLoginCancelling else { return }
                self.codexAuthorizationURL = url
                if NSWorkspace.shared.open(url) {
                    self.codexLoginFeedback = AITextCodexLoginStatus.waitingForBrowser.displayText
                    self.codexLoginFeedbackIsError = false
                } else {
                    self.codexLoginFeedback = "浏览器未能自动打开；可复制登录链接后继续授权。"
                    self.codexLoginFeedbackIsError = true
                }
                self.refreshCodexLoginControls()
            },
            onStatus: { [weak self] status in
                guard let self,
                      self.codexLoginSessionID == sessionID,
                      !self.codexLoginCancelling else { return }
                self.codexLoginFeedback = status.displayText
                self.codexLoginFeedbackIsError = false
                self.refreshCodexLoginControls()
            },
            completion: { [weak self] result in
                guard let self, self.codexLoginSessionID == sessionID else { return }
                self.codexLoginOperation = nil
                self.codexLoginSessionID = nil
                self.codexLoginCancelling = false
                self.codexAuthorizationURL = nil
                var authorizationChanged = false
                switch result {
                case .success:
                    self.codexLoginFeedback = "ChatGPT 订阅授权成功，Codex 连接器已就绪。"
                    self.codexLoginFeedbackIsError = false
                    authorizationChanged = true
                case .failure(.cancelled):
                    self.codexLoginFeedback = "已取消 Codex 登录。"
                    self.codexLoginFeedbackIsError = false
                case let .failure(error):
                    self.codexLoginFeedback = self.codexLoginErrorMessage(error)
                    self.codexLoginFeedbackIsError = true
                }
                self.refreshCodexLoginControls()
                if authorizationChanged {
                    AITextPluginRuntimeRegistry.shared.workspace(for: .codexCLI)?
                        .configurationDidChange()
                    BufferWindowController.shared.refresh()
                    RIMESController.refreshActiveUI()
                }
                DispatchQueue.main.async { [weak self] in
                    guard let self,
                          self.window?.isVisible == true,
                          self.selectedCoreRoute == .connectors,
                          self.navigation.selectedSubpage()?.rawValue == "ai-model" else { return }
                    self.reload()
                    self.showCurrentRoute()
                }
            }
        )
        codexLoginOperation = operation
        refreshCodexLoginControls()
        operation.start()
    }

    @objc private func copyCodexLoginLink() {
        guard let url = codexAuthorizationURL else { return }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(url.absoluteString, forType: .string)
        codexLoginFeedback = "登录链接已复制；请在浏览器中打开并完成授权。"
        codexLoginFeedbackIsError = false
        refreshCodexLoginControls()
    }

    @objc private func claudeLoginButtonPressed() {
        if let operation = claudeLoginOperation {
            claudeLoginCancelling = true
            claudeLoginFeedback = "正在取消 Claude 登录…"
            claudeLoginFeedbackIsError = false
            refreshClaudeLoginControls()
            operation.cancel()
            return
        }

        let sessionID = UUID()
        claudeLoginSessionID = sessionID
        claudeLoginCancelling = false
        claudeLoginFeedback = AITextClaudeLoginStatus.launching.displayText
        claudeLoginFeedbackIsError = false

        let operation = AITextClaudeLoginOperation(
            onStatus: { [weak self] status in
                guard let self,
                      self.claudeLoginSessionID == sessionID,
                      !self.claudeLoginCancelling else { return }
                self.claudeLoginFeedback = status.displayText
                self.claudeLoginFeedbackIsError = false
                self.refreshClaudeLoginControls()
            },
            completion: { [weak self] result in
                guard let self, self.claudeLoginSessionID == sessionID else { return }
                self.claudeLoginOperation = nil
                self.claudeLoginSessionID = nil
                self.claudeLoginCancelling = false
                switch result {
                case .success:
                    self.claudeLoginFeedback = "Claude Code CLI 授权成功，连接器已就绪。"
                    self.claudeLoginFeedbackIsError = false
                    AITextConnectorRegistry.shared.claudeAuthenticationDidChange(true)
                case .failure(.cancelled):
                    self.claudeLoginFeedback = "已取消 Claude 登录。"
                    self.claudeLoginFeedbackIsError = false
                    AITextConnectorRegistry.shared.claudeAuthenticationDidChange(nil)
                case let .failure(error):
                    self.claudeLoginFeedback = self.claudeLoginErrorMessage(error)
                    self.claudeLoginFeedbackIsError = true
                    AITextConnectorRegistry.shared.claudeAuthenticationDidChange(nil)
                }
                self.refreshClaudeLoginControls()
                BufferWindowController.shared.refresh()
                RIMESController.refreshActiveUI()
                DispatchQueue.main.async { [weak self] in
                    guard let self,
                          self.window?.isVisible == true,
                          self.selectedCoreRoute == .connectors,
                          self.navigation.selectedSubpage()?.rawValue == "ai-model" else { return }
                    self.reload()
                    self.showCurrentRoute()
                }
            }
        )
        claudeLoginOperation = operation
        refreshClaudeLoginControls()
        operation.start()
    }

    private func persistSchemaSelection(_ ids: [String]? = nil) throws {
        let enabled = ids ?? InputSchemaCatalog.enabledIDs(
            chordExtensionEnabled: ChordExtensionStore.shared.isEnabled
        )
        try SchemaListStore.writeEnabledIDs(enabled,
                                            to: userDir.appendingPathComponent("default.custom.yaml"))
        let preferred = InputConfigurationStore.shared.runtimeProfile.schemaID
        UserDefaults.standard.set(preferred, forKey: "preferredSchema")
        IMELog.write("settings: F4 schemas -> \(enabled.joined(separator: ","))")
    }

    @objc private func deployAndRestart() {
        guard RimeInputSourceAuthority.currentSourceIsOwn() else {
            IMELog.write("settings deploy ignored; RIMES is not selected")
            return
        }
        do {
            try persistSchemaSelection()
        } catch {
            info("无法应用方案：\(error.localizedDescription)")
            return
        }
        RIMESController.active?.forceCommit()
        guard info("开始部署…完成后输入法会自动重启。") else {
            return
        }
        DispatchQueue.global(qos: .userInitiated).async {
            _ = rimeEngine.start()
            let ok = BBRimeDeploy()
            if ok {
                rimeEngine.invalidateSchemaListCacheAfterDeployment()
            }
            IMELog.write("settings: deploy=\(ok)")
            DispatchQueue.main.async {
                guard ok else {
                    self.info("部署失败，输入法没有重启。请查看运行日志。")
                    return
                }
                InputMetricsPersistence.saveNow()
                exit(0)   // text-input system relaunches us
            }
        }
    }

    @objc private func reinstallInputMethod() {
        guard let script = installScriptURL() else {
            info("找不到 build_install.sh。默认查找：~/Documents/05-dev/apps/rimes，兼容旧版 rime-buffer 目录。")
            return
        }

        let alert = NSAlert()
        alert.messageText = "重新安装 \(ProductIdentity.displayName)？"
        alert.informativeText = "将从 \(script.deletingLastPathComponent().path) 运行 build_install.sh。构建完成后当前输入法进程会被重启。"
        alert.addButton(withTitle: "重新安装")
        alert.addButton(withTitle: "取消")
        alert.window.appearance = RimeUI.appKitAppearance
        guard StandaloneWindowFocusCoordinator.shared
            .runModalAlertIfRIMESActive(alert) == .alertFirstButtonReturn else {
            return
        }

        RIMESController.active?.forceCommit()
        InputMetricsPersistence.saveNow()

        let command = [
            "cd \(shellQuote(script.deletingLastPathComponent().path))",
            "nohup env RB_KEEP_USERDB=1 /bin/bash ./build_install.sh > \(shellQuote(installLogURL.path)) 2>&1 &",
        ].joined(separator: " && ")
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/zsh")
        process.arguments = ["-lc", command]
        do {
            try process.run()
            installStatus.stringValue = "安装已启动，日志：~/rimebuffer-install.log"
            IMELog.write("settings: launched install script \(script.path)")
        } catch {
            installStatus.stringValue = "安装启动失败"
            info("安装启动失败：\(error.localizedDescription)")
        }
    }

    private func installScriptURL() -> URL? {
        let home = URL(fileURLWithPath: NSHomeDirectory())
        let candidates = [
            home.appendingPathComponent("Documents/05-dev/apps/rimes/build_install.sh"),
            home.appendingPathComponent("Documents/DEV/rime-buffer-1/build_install.sh"),
            home.appendingPathComponent("Documents/05-dev/apps/rime-buffer-1/build_install.sh"),
            home.appendingPathComponent("Documents/DEV/rime-buffer/build_install.sh"),
            home.appendingPathComponent("Documents/05-dev/apps/rime-buffer/build_install.sh"),
        ]
        return candidates.first { FileManager.default.fileExists(atPath: $0.path) }
    }

    private func shellQuote(_ value: String) -> String {
        "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
    }

    @objc private func resetOnAppSwitchToggled() {
        BufferModel.shared.resetOnAppSwitch = resetOnAppSwitchCheck.state == .on
        IMELog.write("setting resetOnAppSwitch=\(resetOnAppSwitchCheck.state == .on)")
    }

    /// The switch records intent; the grant is what actually enables the
    /// probe. Enabling without it would silently do nothing, so the first
    /// enable raises the system prompt. macOS answers only after the user acts
    /// in System Settings, so the status line — not the switch — reports
    /// whether alignment is live.
    @objc private func alignToInputBoxToggled() {
        let enabled = alignToInputBoxCheck.state == .on
        FocusedInputBoxProbe.alignmentEnabled = enabled
        if enabled, !FocusedInputBoxProbe.isPermitted {
            SystemPermissionAudit.requestOrReveal(.accessibility)
        }
        reload()
        IMELog.write(
            "setting alignToInputBox=\(enabled) "
            + "permitted=\(FocusedInputBoxProbe.isPermitted)"
        )
    }

    @objc private func requestAccessibilityGrant() {
        // Falls back to opening the pane when no dialog can appear, instead
        // of asking for a grant and showing the user nothing.
        SystemPermissionAudit.requestOrReveal(.accessibility)
        reload()
        IMELog.write(
            "setting accessibility grant requested permitted="
            + "\(FocusedInputBoxProbe.isPermitted)"
        )
    }

    @objc private func clipboardAutoPasteToggled() {
        let enabled = clipboardAutoPasteCheck.state == .on
        ClipboardAutoPaste.enabled = enabled
        if enabled, !ClipboardAutoPaste.isPermitted {
            SystemPermissionAudit.requestOrReveal(.accessibility)
        }
        reload()
        IMELog.write(
            "setting clipboardAutoPaste=\(enabled) "
            + "permitted=\(ClipboardAutoPaste.isPermitted)"
        )
    }

    @objc private func clipboardHistoryToggled() {
        ClipboardHistoryWindowController.shared.captureEnabled =
            clipboardHistoryCheck.state == .on
        reload()
    }

    @objc private func closeAfterLastDeliveryToggled() {
        BufferWindowController.shared.closeAfterLastDeliveryEnabled =
            closeAfterLastDeliveryCheck.state == .on
        reload()
    }

    @objc private func moveBufferWindow() {
        BufferWindowController.shared.show()
        BufferWindowController.shared.moveToCurrentScreen()
        reload()
    }

    @objc private func appearanceChosen() {
        guard let raw = appearancePopUp.selectedItem?.representedObject as? String,
              let mode = RimeAppearanceMode(rawValue: raw) else { return }
        RimeUI.appearance = mode
        IMELog.write("appearance -> \(mode.rawValue)")
    }

    @objc private func appearanceCardChosen(_ sender: SettingsThemeCardButton) {
        RimeUI.appearance = sender.mode
        settingsStatusLabel.stringValue = "已切换到\(sender.mode.title)主题"
        settingsStatusLabel.textColor = RimeUI.textMuted
        IMELog.write("appearance -> \(sender.mode.rawValue)")
    }

    @objc private func candidateMetricSliderChanged(_ sender: NSSlider) {
        handleCandidateMetricEdit(tag: sender.tag, value: sender.doubleValue)
    }

    @objc private func candidateMetricFieldChanged(_ sender: NSTextField) {
        handleCandidateMetricEdit(tag: sender.tag, value: sender.doubleValue)
    }

    func controlTextDidEndEditing(_ obj: Notification) {
        guard let field = obj.object as? NSTextField else { return }
        if field === bufferWidthField {
            applyBufferWidth(field.doubleValue)
            return
        }
        guard candidateMetricFields.values.contains(where: { $0 === field }) else { return }
        handleCandidateMetricEdit(tag: field.tag, value: field.doubleValue)
    }

    /// Live edit of one metric: fold it into the current control values, re-resolve
    /// the supported set (so dependents follow), and push everything back — the
    /// preview updates immediately, nothing is persisted until "应用修改".
    private func handleCandidateMetricEdit(tag: Int, value: Double) {
        guard let metric = CandidateWindowMetric.fromTag(tag) else { return }
        var raw = liveMetricValues()
        raw[metric] = value
        updateCandidateControls(resolveMetricValues(raw))
    }

    @objc private func applyCandidateMetrics() {
        window?.makeFirstResponder(nil)
        let resolved = resolveMetricValues(liveMetricValues())
        CandidateWindowMetrics.apply(resolved)
        updateCandidateControls(resolved)
    }

    @objc private func resetCandidateMetrics() {
        CandidateWindowMetrics.resetToDefaults()
        refreshCandidateMetricControls()
    }

    @objc private func bufferWidthSliderChanged() {
        applyBufferWidth(bufferWidthSlider.doubleValue)
    }

    @objc private func bufferWidthFieldChanged() {
        applyBufferWidth(bufferWidthField.doubleValue)
    }

    private func applyBufferWidth(_ value: Double) {
        window?.makeFirstResponder(nil)
        BufferWindowController.shared.setConfiguredWidth(CGFloat(value))
        refreshBufferWidthControls()
    }

    @objc private func resetBufferWidth() {
        BufferWindowController.shared.resetConfiguredWidth()
        refreshBufferWidthControls()
    }

    @objc private func resetAllShortcuts() {
        RimeShortcutPreferences.resetAll()
        DispatchQueue.main.async { [weak self] in
            self?.showCurrentRoute()
        }
    }

    @objc private func statsDateChanged() {
        refreshStats()
    }

    @objc private func refreshStatsTapped() {
        refreshStats()
    }

    @objc private func clearStatsDay() {
        KeyFrequencyStore.shared.clear(day: statsDatePicker.dateValue)
        refreshStats()
    }

    @objc private func clearStatsAll() {
        KeyFrequencyStore.shared.clear(day: nil)
        refreshStats()
    }

    @objc private func importUserLexicon(_ sender: SettingsLexiconButton) {
        let kind = sender.lexiconKind
        let panel = NSOpenPanel()
        panel.title = "导入\(kind.displayName)"
        panel.message = "选择由 \(ProductIdentity.displayName) 或 Rime 用户词典管理器导出的 TSV；记录会合并，不会替换现有学习数据。"
        panel.prompt = "选择并导入"
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.tabSeparatedText, .plainText]
        panel.appearance = RimeUI.appKitAppearance
        guard panel.runModal() == .OK,
              RimeInputSourceAuthority.currentSourceIsOwn(),
              let sourceURL = panel.url else { return }

        let confirmation = NSAlert()
        confirmation.alertStyle = .informational
        confirmation.messageText = "合并到\(kind.displayName)？"
        confirmation.informativeText = "Rime 会短暂收束当前组字并重新建立输入会话；已有词频不会被清空。"
        confirmation.addButton(withTitle: "导入并合并")
        confirmation.addButton(withTitle: "取消")
        confirmation.window.appearance = RimeUI.appKitAppearance
        guard StandaloneWindowFocusCoordinator.shared
            .runModalAlertIfRIMESActive(confirmation)
                == .alertFirstButtonReturn else { return }

        do {
            let result = try UserLexiconService.shared.importLearningData(kind,
                                                                          from: sourceURL)
            info("已向\(kind.displayName)合并 \(result.entryCount) 条学习记录。")
            showCurrentRoute()
        } catch {
            showLexiconError(error, operation: "导入")
        }
    }

    @objc private func exportUserLexicon(_ sender: SettingsLexiconButton) {
        let kind = sender.lexiconKind
        let panel = NSSavePanel()
        panel.title = "导出\(kind.displayName)"
        panel.message = "导出为可再次导入的 UTF-8 TSV，不包含基础词库、输入正文或其他统计。"
        panel.prompt = "导出"
        panel.nameFieldStringValue = kind.suggestedFileName
        panel.canCreateDirectories = true
        panel.allowedContentTypes = [.tabSeparatedText]
        panel.appearance = RimeUI.appKitAppearance
        guard panel.runModal() == .OK,
              RimeInputSourceAuthority.currentSourceIsOwn(),
              let destinationURL = panel.url else { return }

        do {
            let result = try UserLexiconService.shared.exportLearningData(kind,
                                                                          to: destinationURL)
            info("已导出 \(result.entryCount) 条\(kind.displayName)记录。")
            NSWorkspace.shared.activateFileViewerSelecting([destinationURL])
        } catch {
            showLexiconError(error, operation: "导出")
        }
    }

    private func showLexiconError(_ error: Error, operation: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "学习词库\(operation)失败"
        alert.informativeText = (error as? LocalizedError)?.errorDescription
            ?? error.localizedDescription
        alert.window.appearance = RimeUI.appKitAppearance
        _ = StandaloneWindowFocusCoordinator.shared
            .runModalAlertIfRIMESActive(alert)
    }

    @objc private func openDir() {
        NSWorkspace.shared.open(userDir)
    }

    @objc private func checkUpdate() {
        UpdateManager.shared.checkNowManually()
    }

    @objc private func openRuntimeLog() {
        let url = URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("rimebuffer.log")
        NSWorkspace.shared.open(url)
    }

    @objc private func restartInputMethod() {
        RIMESController.active?.forceCommit()
        InputMetricsPersistence.saveNow()
        IMELog.write("settings: restart requested")
        exit(0)
    }

    @objc private func openInstallLog() {
        NSWorkspace.shared.open(installLogURL)
    }

    @discardableResult
    private func info(_ message: String) -> Bool {
        let alert = NSAlert()
        alert.messageText = message
        alert.window.appearance = RimeUI.appKitAppearance
        return StandaloneWindowFocusCoordinator.shared
            .runModalAlertIfRIMESActive(alert) != nil
    }
}
