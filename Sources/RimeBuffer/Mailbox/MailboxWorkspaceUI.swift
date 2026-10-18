import Cocoa

final class MailboxSurface: NSView {
    var color: NSColor = .windowBackgroundColor
    override func draw(_ dirtyRect: NSRect) { color.setFill(); bounds.fill() }
    init(_ color: NSColor = .windowBackgroundColor) {
        self.color = color
        super.init(frame: .zero)
        wantsLayer = true
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
}

final class MailboxActionButton: RimePointingHandButton {
    var handler: () -> Void
    override var isFlipped: Bool { false }
    override var alignmentRectInsets: NSEdgeInsets { NSEdgeInsetsZero }
    var emphasized = false { didSet { needsDisplay = true } }
    override var title: String { didSet { invalidateIntrinsicContentSize(); needsDisplay = true } }
    override var intrinsicContentSize: NSSize {
        let textWidth = (title as NSString).size(withAttributes: [.font: font ?? .systemFont(ofSize: 12)]).width
        return NSSize(width: title.isEmpty ? 28 : ceil(textWidth) + (image == nil ? 20 : 39), height: MailboxUI.actionHeight)
    }
    init(_ title: String, symbol: String? = nil, action: @escaping () -> Void) {
        handler = action
        super.init(frame: .zero)
        self.title = title
        bezelStyle = .rounded
        font = .systemFont(ofSize: 12)
        alignment = .center
        setContentHuggingPriority(.defaultHigh, for: .horizontal)
        setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        translatesAutoresizingMaskIntoConstraints = false
        heightAnchor.constraint(equalToConstant: MailboxUI.actionHeight).isActive = true
        widthAnchor.constraint(greaterThanOrEqualToConstant: 28).isActive = true
        target = self
        self.action = #selector(invoke)
        if let symbol {
            image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
            imagePosition = title.isEmpty ? .imageOnly : .imageLeading
        }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
    @objc private func invoke() { handler() }
    override func draw(_ dirtyRect: NSRect) {
        let pressed = cell?.isHighlighted == true
        let shape = NSBezierPath(roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5), xRadius: 6, yRadius: 6)
        if isBordered || pressed {
            (emphasized && isEnabled ? MailboxUI.accent : pressed ? MailboxUI.selection : .controlBackgroundColor).setFill()
            shape.fill()
            if !emphasized { NSColor.separatorColor.withAlphaComponent(0.3).setStroke(); shape.stroke() }
        }
        let color: NSColor = !isEnabled ? .disabledControlTextColor : emphasized ? .textBackgroundColor : contentTintColor ?? .labelColor
        let font = font ?? .systemFont(ofSize: 12)
        let textSize = (title as NSString).size(withAttributes: [.font: font])
        let iconWidth: CGFloat = image == nil ? 0 : MailboxUI.actionIconSize
        let gap: CGFloat = image == nil || title.isEmpty ? 0 : MailboxUI.actionIconGap
        let available = max(0, bounds.width - 16 - iconWidth - gap)
        let textWidth = min(textSize.width, available)
        let groupWidth = iconWidth + gap + textWidth
        let x = alignment == .left ? 8 : floor((bounds.width - groupWidth) / 2)
        if let image {
            MailboxUI.drawSymbol(image, in: NSRect(x: x, y: (bounds.height - MailboxUI.actionIconSize) / 2, width: MailboxUI.actionIconSize, height: MailboxUI.actionIconSize), color: color)
        }
        let paragraph = NSMutableParagraphStyle(); paragraph.lineBreakMode = .byTruncatingTail
        (title as NSString).draw(in: NSRect(x: x + iconWidth + gap, y: floor((bounds.height - textSize.height) / 2),
            width: textWidth, height: ceil(textSize.height)), withAttributes: [.font: font, .foregroundColor: color, .paragraphStyle: paragraph])
    }
    override var focusRingMaskBounds: NSRect { bounds }
    override func drawFocusRingMask() { NSBezierPath(roundedRect: bounds, xRadius: 6, yRadius: 6).fill() }
}

/// Draw icon and label in separate, fixed slots; unread counts never change
/// the navigation item's width or the label's baseline.
final class MailboxNavigationButton: RimePointingHandButton {
    override var isFlipped: Bool { false }
    override var alignmentRectInsets: NSEdgeInsets { NSEdgeInsetsZero }
    var selected = false { didSet { needsDisplay = true } }
    var unreadCount = 0 { didSet { needsDisplay = true } }
    private let handler: () -> Void
    init(_ module: MailboxModuleID, action: @escaping () -> Void) {
        handler = action
        super.init(frame: .zero)
        title = module.title; image = NSImage(systemSymbolName: module.symbol, accessibilityDescription: nil)
        isBordered = false; target = self; self.action = #selector(invoke)
        translatesAutoresizingMaskIntoConstraints = false
        widthAnchor.constraint(equalToConstant: MailboxUI.navigationWidth).isActive = true
        heightAnchor.constraint(equalToConstant: MailboxUI.navigationHeight).isActive = true
        setAccessibilityLabel(module.title)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
    @objc private func invoke() { handler() }
    override func draw(_ dirtyRect: NSRect) {
        if selected || cell?.isHighlighted == true {
            MailboxUI.selection.setFill()
            NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
        }
        let color: NSColor = selected ? MailboxUI.accent : .secondaryLabelColor
        if let image { MailboxUI.drawSymbol(image, in: NSRect(x: 15, y: 29, width: 18, height: 18), color: color) }
        let paragraph = NSMutableParagraphStyle(); paragraph.alignment = .center
        (title as NSString).draw(in: NSRect(x: 0, y: 7, width: bounds.width, height: 16),
            withAttributes: [.font: NSFont.systemFont(ofSize: 11, weight: selected ? .semibold : .regular),
                             .foregroundColor: color, .paragraphStyle: paragraph])
        if unreadCount > 0 {
            MailboxUI.accent.setFill()
            let badge = NSRect(x: 30, y: 37, width: 16, height: 14)
            NSBezierPath(roundedRect: badge, xRadius: 7, yRadius: 7).fill()
            ((unreadCount > 9 ? "9+" : String(unreadCount)) as NSString).draw(in: badge.offsetBy(dx: 0, dy: -1),
                withAttributes: [.font: NSFont.systemFont(ofSize: 9, weight: .semibold),
                                 .foregroundColor: NSColor.textBackgroundColor, .paragraphStyle: paragraph])
        }
    }
}

final class MailboxFeedbackLabel: NSTextField {
    override var stringValue: String {
        didSet { isHidden = stringValue.isEmpty; toolTip = stringValue }
    }
    init() {
        super.init(frame: .zero)
        isEditable = false; isBordered = false; drawsBackground = false; isSelectable = true
        font = .systemFont(ofSize: 11); textColor = .secondaryLabelColor
        lineBreakMode = .byTruncatingTail; isHidden = true
        setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
}

class MailboxColumn: NSStackView {
    override func addArrangedSubview(_ view: NSView) {
        super.addArrangedSubview(view)
        view.translatesAutoresizingMaskIntoConstraints = false
        view.widthAnchor.constraint(equalTo: widthAnchor).isActive = true
    }
}
final class MailboxFlowView: MailboxColumn { override var isFlipped: Bool { true } }

final class MailboxListButton: RimePointingHandButton {
    override var isFlipped: Bool { false }
    private let primary: String
    private let secondary: String
    private let metadata: String
    private let selected: Bool
    private let unread: Bool
    private let handler: () -> Void
    init(title: String, subtitle: String, metadata: String, selected: Bool,
         unread: Bool = false, action: @escaping () -> Void) {
        primary = title; secondary = subtitle; self.metadata = metadata
        self.selected = selected; self.unread = unread; handler = action
        super.init(frame: .zero)
        isBordered = false
        self.title = title
        target = self; self.action = #selector(invoke)
        setAccessibilityLabel([title, subtitle, metadata].joined(separator: ", "))
        translatesAutoresizingMaskIntoConstraints = false
        heightAnchor.constraint(equalToConstant: MailboxUI.rowHeight).isActive = true
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) unavailable") }
    @objc private func invoke() { handler() }
    override func draw(_ dirtyRect: NSRect) {
        if selected {
            MailboxUI.selection.setFill()
            NSBezierPath(roundedRect: bounds.insetBy(dx: 7, dy: 3), xRadius: 7, yRadius: 7).fill()
        }
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byTruncatingTail
        func line(_ string: String, y: CGFloat, size: CGFloat, color: NSColor, weight: NSFont.Weight = .regular) {
            (string as NSString).draw(in: NSRect(x: 18, y: y, width: bounds.width - 40, height: 19),
                withAttributes: [.font: NSFont.systemFont(ofSize: size, weight: weight),
                                 .foregroundColor: color, .paragraphStyle: paragraph])
        }
        line(primary, y: 54, size: 13, color: .labelColor, weight: unread ? .semibold : .medium)
        line(secondary.replacingOccurrences(of: "\n", with: " "), y: 32, size: 11, color: .secondaryLabelColor)
        line(metadata, y: 11, size: 10, color: .tertiaryLabelColor)
        if unread {
            MailboxUI.accent.setFill()
            NSBezierPath(ovalIn: NSRect(x: bounds.width - 18, y: 61, width: 5, height: 5)).fill()
        }
    }
}

final class MailboxComposerTextView: NSTextView {
    var submit: (() -> Void)?
    var changed: ((String) -> Void)?
    var placeholder = "输入消息…" { didSet { needsDisplay = true } }
    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        if string.isEmpty && !hasMarkedText() {
            (placeholder as NSString).draw(at: NSPoint(x: textContainerInset.width + (textContainer?.lineFragmentPadding ?? 5), y: textContainerInset.height),
                withAttributes: [.font: font ?? NSFont.systemFont(ofSize: 14), .foregroundColor: NSColor.placeholderTextColor])
        }
    }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 36, !event.modifierFlags.contains(.shift),
           !event.modifierFlags.contains(.option), !hasMarkedText() {
            submit?()
        } else { super.keyDown(with: event) }
    }
    override func didChangeText() { super.didChangeText(); needsDisplay = true; changed?(string) }
}

enum MailboxUI {
    static let defaultSize = NSSize(width: 1000, height: 700)
    static let minimumSize = NSSize(width: 680, height: 500)
    static let contentInset: CGFloat = 16
    static let railWidth: CGFloat = 64
    static let chatListWidth: CGFloat = 216
    static let inboxListWidth: CGFloat = 232
    static let sidebarBreakpoint: CGFloat = 760
    static let navigationWidth: CGFloat = 48
    static let navigationHeight: CGFloat = 56
    static let rowHeight: CGFloat = 84
    static let actionHeight: CGFloat = 28
    static let actionIconSize: CGFloat = 14
    static let actionIconGap: CGFloat = 5
    static let messageSpacing: CGFloat = 12
    static let actionSpacing: CGFloat = 8
    static let bodyFontSize: CGFloat = 14
    static let bodyLineSpacing: CGFloat = 5
    static let bodyParagraphSpacing: CGFloat = 9
    static let inboxImageHeight: CGFloat = 300
    static let chatImageHeight: CGFloat = 240
    static func drawSymbol(_ image: NSImage, in rect: NSRect, color: NSColor) {
        let symbol = image.withSymbolConfiguration(.init(paletteColors: [color])) ?? image
        let scale = min(rect.width / max(1, symbol.size.width), rect.height / max(1, symbol.size.height))
        let size = NSSize(width: symbol.size.width * scale, height: symbol.size.height * scale)
        symbol.draw(in: NSRect(x: rect.midX - size.width / 2, y: rect.midY - size.height / 2,
                              width: size.width, height: size.height))
    }
    static let accent = NSColor(name: "MailboxAccent") { appearance in
        appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            ? NSColor(srgbRed: 0.58, green: 0.74, blue: 0.65, alpha: 1)
            : NSColor(srgbRed: 0.29, green: 0.46, blue: 0.36, alpha: 1)
    }
    static let sidebar = NSColor(name: "MailboxSidebar") { appearance in
        appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            ? NSColor(srgbRed: 0.14, green: 0.16, blue: 0.15, alpha: 1)
            : NSColor(srgbRed: 0.955, green: 0.961, blue: 0.949, alpha: 1)
    }
    static let selection = NSColor(name: "MailboxSelection") { appearance in
        appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            ? NSColor(srgbRed: 0.23, green: 0.31, blue: 0.26, alpha: 1)
            : NSColor(srgbRed: 0.88, green: 0.92, blue: 0.87, alpha: 1)
    }
    static func label(_ text: String, size: CGFloat = 13,
                      weight: NSFont.Weight = .regular, secondary: Bool = false) -> NSTextField {
        let view = NSTextField(labelWithString: text)
        view.font = .systemFont(ofSize: size, weight: weight)
        view.textColor = secondary ? .secondaryLabelColor : .labelColor
        view.alignment = .left
        view.setContentCompressionResistancePriority(NSLayoutConstraint.Priority(749), for: .horizontal)
        view.lineBreakMode = .byTruncatingTail
        view.maximumNumberOfLines = 1
        return view
    }
    static func paragraph(_ text: String, size: CGFloat = 13) -> NSTextField {
        let view = NSTextField(wrappingLabelWithString: text)
        view.font = .systemFont(ofSize: size)
        view.textColor = .secondaryLabelColor
        view.isSelectable = true
        view.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return view
    }
    static func spacer() -> NSView {
        let view = NSView()
        for axis in [NSLayoutConstraint.Orientation.horizontal, .vertical] {
            view.setContentHuggingPriority(NSLayoutConstraint.Priority(1), for: axis)
            view.setContentCompressionResistancePriority(NSLayoutConstraint.Priority(1), for: axis)
        }
        return view
    }
    static func line() -> NSView {
        let view = MailboxSurface(.separatorColor)
        view.translatesAutoresizingMaskIntoConstraints = false
        view.heightAnchor.constraint(equalToConstant: 1).isActive = true
        return view
    }
    static func stack(_ views: [NSView], vertical: Bool = true,
                      spacing: CGFloat = 8) -> NSStackView {
        let view: NSStackView = vertical ? MailboxColumn() : NSStackView()
        view.orientation = vertical ? .vertical : .horizontal
        view.alignment = vertical ? .leading : .centerY
        view.distribution = .fill
        view.spacing = spacing
        view.translatesAutoresizingMaskIntoConstraints = false
        for child in views { view.addArrangedSubview(child) }
        return view
    }
    static func pin(_ child: NSView, to parent: NSView, inset: CGFloat = 0) {
        child.translatesAutoresizingMaskIntoConstraints = false
        parent.addSubview(child)
        NSLayoutConstraint.activate([
            child.leadingAnchor.constraint(equalTo: parent.leadingAnchor, constant: inset),
            child.trailingAnchor.constraint(equalTo: parent.trailingAnchor, constant: -inset),
            child.topAnchor.constraint(equalTo: parent.topAnchor, constant: inset),
            child.bottomAnchor.constraint(equalTo: parent.bottomAnchor, constant: -inset),
        ])
    }
    static func padded(_ child: NSView, inset: CGFloat = 20, color: NSColor = .clear) -> NSView {
        let view = MailboxSurface(color)
        pin(child, to: view, inset: inset)
        return view
    }
    static func scroll(_ stack: NSStackView) -> NSScrollView {
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.distribution = .fill
        stack.spacing = 0
        stack.translatesAutoresizingMaskIntoConstraints = false
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = stack
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
            stack.topAnchor.constraint(equalTo: scroll.contentView.topAnchor),
            stack.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor),
        ])
        return scroll
    }
    static func clear(_ stack: NSStackView) {
        stack.arrangedSubviews.forEach { stack.removeArrangedSubview($0); $0.removeFromSuperview() }
    }
    static func body(_ message: MailboxMessage) -> MailboxMessageTextView {
        let presentation = MailboxMessageFormatting.presentation(for: message, style: MailboxBodyStyle(
            textColor: .labelColor, secondaryTextColor: .secondaryLabelColor,
            codeTextColor: .labelColor, codeBackgroundColor: .controlBackgroundColor,
            linkColor: .linkColor
        ))
        let attributed = NSMutableAttributedString(attributedString: presentation.attributedText)
        if presentation.kind == .prose {
            let paragraph = NSMutableParagraphStyle()
            paragraph.lineSpacing = bodyLineSpacing
            paragraph.paragraphSpacing = bodyParagraphSpacing
            attributed.addAttributes([.font: NSFont.systemFont(ofSize: bodyFontSize), .paragraphStyle: paragraph],
                                     range: NSRange(location: 0, length: attributed.length))
        }
        if presentation.kind == .markdown {
            let original = presentation.attributedText
            original.enumerateAttributes(in: NSRange(location: 0, length: original.length)) { attributes, range, _ in
                guard attributes[.backgroundColor] == nil else { return }
                let oldFont = attributes[.font] as? NSFont ?? .systemFont(ofSize: 12)
                let traits = NSFontManager.shared.traits(of: oldFont)
                var font = NSFont.systemFont(ofSize: max(14, oldFont.pointSize), weight: traits.contains(.boldFontMask) ? .semibold : .regular)
                if traits.contains(.italicFontMask) { font = NSFontManager.shared.convert(font, toHaveTrait: .italicFontMask) }
                attributed.addAttribute(.font, value: font, range: range)
                if let old = attributes[.paragraphStyle] as? NSParagraphStyle,
                   let paragraph = old.mutableCopy() as? NSMutableParagraphStyle {
                    paragraph.minimumLineHeight = 0; paragraph.maximumLineHeight = 0
                    paragraph.lineSpacing = 4
                    attributed.addAttribute(.paragraphStyle, value: paragraph, range: range)
                }
            }
        }
        return MailboxMessageTextView(presentation: MailboxBodyPresentation(
            kind: presentation.kind, attributedText: attributed,
            formatBadgeTitle: presentation.formatBadgeTitle, linkTextColor: presentation.linkTextColor
        ))
    }
    static func time(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = Calendar.current.isDateInToday(date) ? "HH:mm" : "MM-dd HH:mm"
        return formatter.string(from: date)
    }
}
