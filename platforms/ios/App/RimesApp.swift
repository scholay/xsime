import SwiftUI
import StoreKit
import UniformTypeIdentifiers
import RimesCore

@main
struct RimesApp: App {
    @StateObject private var model = SettingsModel()
    var body: some Scene { WindowGroup { HomeView().environmentObject(model).tint(.teal).task {
        #if DEBUG
        await DevelopmentSmoke.runIfRequested()
        #endif
    } } }
}
@MainActor final class SettingsModel: ObservableObject {
    @Published var value = ConfigurationStore().load()
    @Published var error: String?
    init() {
        do { try MobileOfficialPlugins.makeStore().bootstrap() }
        catch { self.error = error.localizedDescription }
    }
    func save() { do { try ConfigurationStore().save(value) } catch { self.error = error.localizedDescription } }
}
struct HomeView: View {
    @EnvironmentObject private var model: SettingsModel
    /// isaac2026 is listed only while the slide-chord plugin is on.
    private var chordPluginOn: Bool { (try? MobileOfficialPlugins.makeStore().isEnabled(legacyID: "chord")) == true }
    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack(spacing: 16) {
                        Image("BrandLogo").resizable().scaledToFit()
                            .frame(width: 72, height: 72)
                            .clipShape(RoundedRectangle(cornerRadius: 16))
                            .accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 8) {
                            Text("\(ProductBrand.name)").font(.system(size: 32, weight: .bold, design: .rounded))
                            VStack(alignment: .leading, spacing: 4) {
                                Text(L("世界对智者太过挑剔", "The world is too hard on the wise."))
                                Text(L("好奇的人需要朋友", "Curious minds need friends."))
                            }.font(.system(size: 13)).foregroundStyle(.secondary)
                                .lineLimit(1).minimumScaleFactor(0.8)
                        }.frame(maxWidth: .infinity, alignment: .leading)
                    }.padding(.vertical, 12)
                        .accessibilityElement(children: .combine)
                        .accessibilityIdentifier("settings.home.hero")
                }.listRowBackground(Color.teal.opacity(0.08))
                Section(L("开始使用", "Get started")) {
                    NavigationLink { SetupView() } label: { Label(L("启用 \(ProductBrand.name) 键盘", "Enable \(ProductBrand.name) keyboard"), systemImage: "keyboard") }
                    NavigationLink { PlaygroundView() } label: { Label(L("输入体验", "Try typing"), systemImage: "square.and.pencil") }
                    NavigationLink { TypingStatsCardView() } label: { Label(L("打字统计卡片", "Typing stats card"), systemImage: "square.grid.3x3") }
                }
                Section(L("你的输入方式", "Your typing")) {
                    NavigationLink { InputHabitsView() } label: { Label(L("输入习惯", "Input preferences"), systemImage: "slider.horizontal.3") }
                    Picker(L("默认方案", "Default scheme"), selection: $model.value.scheme) { ForEach(InputScheme.allCases.filter { $0 != .chord || chordPluginOn || model.value.scheme == .chord }) { Text($0.title).tag($0) } }.onChange(of: model.value.scheme) { _,_ in do { try RimeSchemeStore().activate(nil); model.value.schemeSelectionRevision = UUID(); model.save() } catch { model.error = error.localizedDescription } }
                    NavigationLink { ChordProfilesView() } label: { Label(L("滑动并击与键位", "Slide chords & mappings"), systemImage: "hand.draw") }
                    NavigationLink { KeyboardAppearanceView() } label: { Label(L("键盘布局与换肤", "Keyboard layout & skins"), systemImage: "keyboard") }
                    NavigationLink { RimeSchemesView() } label: { Label(L("Rime 方案包与导入", "Rime schemes & import"), systemImage: "shippingbox") }
                    NavigationLink { TranslationSetupView() } label: { Label(L("Apple 本地翻译语言包", "Apple translation languages"), systemImage: "translate") }
                    NavigationLink { OfficialPluginsView() } label: { Label(L("官方插件", "Official plugins"), systemImage: "puzzlepiece.extension") }
                    NavigationLink { ProvidersView() } label: { Label(L("AI 服务", "AI services"), systemImage: "sparkles") }
                    NavigationLink { AssistantView() } label: { Label(L("AI 助手与快捷入口", "AI assistant & quick entrance"), systemImage: "wand.and.stars") }
                    NavigationLink { PoemLibraryView() } label: { Label(L("AI 作诗：句式与词卡", "AI Poem: patterns & word cards"), systemImage: "text.book.closed") }
                    NavigationLink { StatusSkinsView() } label: { Label(L("宠物轮换", "Pet rotation"), systemImage: "pawprint") }
                }
                Section {
                    NavigationLink { DataManagementView() } label: { Label(L("数据管理", "Data management"), systemImage: "externaldrive") }
                    NavigationLink { StoreUpdateView() } label: { Label(L("检查更新", "Software update"), systemImage: "arrow.down.circle") }
                    NavigationLink(L("隐私与第三方许可", "Privacy & licenses")) { PrivacyView() }
                    Text("\(ProductBrand.name) " + (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.1.0")).font(.caption).foregroundStyle(.secondary)
                } footer: {
                    VStack(spacing: 0) {
                        Link("xsime.com", destination: URL(string: "https://xsime.com")!)
                            .accessibilityLabel(L("官网，xsime.com", "Website, xsime.com"))
                            .accessibilityIdentifier("settings.home.website")
                            .frame(minHeight: 44)
                        Link("pm@scholay.com", destination: URL(string: "mailto:pm@scholay.com")!)
                            .accessibilityLabel(L("联系邮箱，pm@scholay.com", "Email, pm@scholay.com"))
                            .accessibilityIdentifier("settings.home.email")
                            .frame(minHeight: 44)
                    }.font(.footnote).frame(maxWidth: .infinity)
                }
            }.navigationTitle(L("欢迎", "Welcome"))
            .alert(L("无法保存", "Could not save"), isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) { Button("OK") { model.error = nil } } message: { Text(model.error ?? "") }
        }
    }
}
struct SetupView: View {
    var body: some View {
        List {
            Section(L("添加键盘", "Add the keyboard")) {
                Text(L("1. 打开系统设置 → 通用 → 键盘 → 键盘。\n2. 点击“添加新键盘”，选择 \(ProductBrand.name)。\n3. 回到输入框，长按地球按钮切换至 \(ProductBrand.name)。", "1. Open Settings → General → Keyboard → Keyboards.\n2. Tap Add New Keyboard and choose \(ProductBrand.name).\n3. In a text field, hold the globe key and choose \(ProductBrand.name)."))
                Button(L("打开设置", "Open Settings")) { if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) } }
            }
            Section(L("完全访问是可选的", "Full Access is optional")) {
                Text(L("普通输入、Buffer 和统计图片预览可离线使用。键盘内图片导出、剪贴板和联网 AI 需要完全访问；保存到相册时只申请添加照片的权限，AI 发送还会单独征求同意。", "Typing, Buffer and stats previews work offline. Image export, clipboard access and online AI in the keyboard need Full Access. Saving to Photos requests add-only access; AI sends require separate consent."))
                Text(L("密码等安全输入框、禁止第三方键盘的 App 将使用系统键盘。", "Secure fields and apps that disallow third-party keyboards use the system keyboard."))
            }
        }.navigationTitle(L("启用键盘", "Enable keyboard"))
    }
}
struct PlaygroundView: View {
    @State private var text = ""
    @State private var second = ""
    var body: some View {
        Form {
            Section(L("使用系统地球键切换至 \(ProductBrand.name)", "Switch to \(ProductBrand.name) using the globe key")) { TextEditor(text: $text).frame(minHeight: 170).accessibilityIdentifier("playground.primary") }
            Section(L("另一个输入框", "Another field")) { TextField(L("测试切换输入目标", "Test switching fields"), text: $second).submitLabel(.send).accessibilityIdentifier("playground.secondary") }
            #if DEBUG
            NavigationLink(L("本地引擎检查", "Local engine check")) { EngineCheckView() }
            #endif
        }.navigationTitle(L("输入体验", "Try typing"))
    }
}
struct EngineCheckView: View {
    @State private var text = "nihao"
    @State private var scheme: InputScheme = .pinyin
    @State private var candidates = [String]()
    private let engine = MobileEngine()
    var body: some View {
        Form {
            Picker("Scheme", selection: $scheme) { ForEach([InputScheme.pinyin,.ziranma,.wubi86,.lingxiWubi]) { Text($0.title).tag($0) } }
            TextField("Code",text:$text).textInputAutocapitalization(.never).autocorrectionDisabled()
            Button(L("检查候选", "Check candidates")) {
                guard engine.select(schema: scheme.schemaID) else { candidates = ["Engine unavailable"]; return }
                var snapshot = EngineSnapshot(); for c in text.unicodeScalars { snapshot = engine.process(key: Int32(c.value)) }; candidates = snapshot.candidates
            }
            ForEach(Array(candidates.enumerated()),id:\.offset) { Text($0.element) }
        }.navigationTitle(L("引擎检查", "Engine check"))
    }
}
struct ProvidersView: View {
    @EnvironmentObject private var model: SettingsModel
    @State private var editing: ProviderConfiguration?
    var body: some View {
        List {
            Section {
                ForEach(model.value.providers) { p in
                    Button { editing = p } label: { HStack { VStack(alignment:.leading) { Text(p.name); Text(p.model).font(.caption).foregroundStyle(.secondary) }; Spacer(); if model.value.selectedProvider == p.id { Image(systemName:"checkmark.circle.fill") } } }
                    .swipeActions { Button(role:.destructive) { do { try KeychainStore().delete(p.id); model.value.providers.removeAll { $0.id == p.id }; if model.value.selectedProvider == p.id { model.value.selectedProvider = nil }; model.save() } catch { model.error = error.localizedDescription } } label: { Label("Delete",systemImage:"trash") } }
                }
                Button { editing = ProviderConfiguration() } label: { Label(L("添加服务", "Add service"),systemImage:"plus") }
            } footer: {
                VStack(alignment: .leading, spacing: 8) {
                    Text(L("已选 AI 服务用于快问快答、AI 润色、AI 作诗和 AI 字符画。Apple 本地翻译使用本机语言包，单独在“Apple 本地翻译语言包”中准备。", "The selected AI service powers Quick Q&A, AI Polish, AI Poem and AI Text Art. Apple on-device translation uses language packs prepared separately under Apple translation languages."))
                    Text(L("使用自己的 API Key。支持采用通用接口格式的 HTTPS AI 服务。Key 仅保存在本设备 Keychain。", "Bring your own API key. Supports HTTPS AI services using a general-purpose interface format. Keys stay in this device's Keychain."))
                }
            }
        }.navigationTitle(L("AI 服务", "AI services"))
        .sheet(item:$editing) { ProviderEditor(provider:$0).environmentObject(model) }
    }
}
struct ProviderEditor: View {
    @EnvironmentObject private var model: SettingsModel
    @Environment(\.dismiss) private var dismiss
    @State var provider: ProviderConfiguration
    @State private var key = ""
    @State private var consent = false
    @State private var models = [String]()
    @State private var message = ""
    @State private var probing = false
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField(L("名称", "Name"),text:$provider.name)
                    TextField("https://api.example.com/v1",text:$provider.baseURL).keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled().onChange(of:provider.baseURL) { _,_ in consent = false }
                    SecureField("API Key",text:$key).textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("Model ID",text:$provider.model).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button(probing ? L("查询中…", "Loading…") : L("查询模型列表", "Fetch models")) {
                        probing = true; message = ""
                        Task { do { models = try await AIClient().models(provider:provider,key:key); message = models.isEmpty ? L("列表为空，可手动填写模型 ID", "No models returned; enter a model ID manually") : "" } catch { message = L("查询失败，可手动填写模型 ID", "Lookup failed; enter a model ID manually") }; probing = false }
                    }.disabled(probing || key.isEmpty || (try? provider.endpoint("models")) == nil)
                    Text(L("查询模型列表会将 API Key 发送到上方地址，不发送 Buffer 文本。查询失败不影响手动填写和保存。", "Fetching models sends the API key to the address above, without Buffer text. You can enter and save a model ID even if lookup fails.")).font(.caption).foregroundStyle(.secondary)
                    if !models.isEmpty { Picker("Models",selection:$provider.model) { Text(provider.model).tag(provider.model); ForEach(models.filter { $0 != provider.model },id:\.self) { Text($0).tag($0) } } }
                } header: { Text(L("连接信息", "Connection")) } footer: {
                    Text(L("保存并选用后，在键盘选择快问快答、AI 润色、AI 作诗或 AI 字符画，输入内容并点 ▶ 执行。请先为 \(ProductBrand.name) 开启完全访问。Apple 本地翻译始终使用本机语言包。", "After Save & select, choose Quick Q&A, AI Polish, AI Poem or AI Text Art in the keyboard, enter text and tap ▶ to run. Enable Full Access for \(ProductBrand.name) first. Apple on-device translation uses the device's language packs."))
                }
                Section(L("发送许可", "Sending permission")) {
                    Text(provider.consentIdentity.isEmpty ? L("填写有效地址后显示接收方", "Enter a valid endpoint to see the recipient") : provider.consentIdentity).font(.caption).textSelection(.enabled)
                    Toggle(L("允许将主动提交的 Buffer 文本发送到此服务", "Allow explicitly submitted Buffer text to be sent to this service"),isOn:$consent)
                    Text(L("不会自动发送打字记录、宿主全文或剪贴板。服务方的数据处理政策适用；不同意也可以保存配置并使用离线输入。", "No typing history, host documents or clipboard are sent automatically. The provider's data policy applies. You may save without consent and keep typing offline.")).font(.caption)
                }
                if !message.isEmpty { Text(message).foregroundStyle(.red) }
            }.navigationTitle(L("AI 配置", "AI configuration"))
            .toolbar { ToolbarItem(placement:.cancellationAction) { Button(L("取消", "Cancel")) { dismiss() } }; ToolbarItem(placement:.confirmationAction) { Button(L("保存并选用", "Save & select")) { save() } } }
            .onAppear { do { key = try KeychainStore().read(provider.id); consent = model.value.consents.contains(provider.consentIdentity) } catch { message = L("无法读取密钥", "Could not read key") } }
        }
    }
    private func save() {
        do {
            _ = try provider.endpoint("chat/completions")
            guard !provider.name.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty, !provider.model.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty else { message = L("请填写名称和模型 ID", "Enter a name and model ID"); return }
            try KeychainStore().save(key,id:provider.id)
            var next = model.value
            let old = next.providers.first { $0.id == provider.id }
            next.providers.removeAll { $0.id == provider.id }; next.providers.append(provider); next.selectedProvider = provider.id
            if let old { next.consents.removeAll { $0 == old.consentIdentity } }
            if consent { next.consents.append(provider.consentIdentity) }
            try ConfigurationStore().save(next); model.value = next; dismiss()
        } catch { message = error.localizedDescription }
    }
}
struct StatusSkinsView: View {
    @EnvironmentObject private var model: SettingsModel
    private var chosen: [StatusSkin] { StatusSkin.rotation(model.value.statusSkins ?? []) }
    var body: some View {
        List {
            Section { Text(L("轻点键盘 Buffer 左上角的宠物，会在选中的主题之间轮换，同时切换整个键盘的配色，包括并击键盘。", "Tap the pet at the top left of the Buffer to rotate through these themes. The whole keyboard, including chord mode, changes colors with it.")).font(.callout) }
            Section(L("轮换的样式", "Looks in the rotation")) {
                ForEach(StatusSkin.themes) { skin in
                    Toggle(skin.title + (skin.isNoto ? L("（动画）", " (animated)") : ""), isOn: Binding(get: { chosen.contains(skin) }, set: { on in
                        var next = chosen
                        if on { next.append(skin) } else if next.count > 1 { next.removeAll { $0 == skin } }
                        model.value.statusSkins = StatusSkin.themes.filter(next.contains).map(\.rawValue)
                        model.value.statusSkinsRevision = UUID(); model.save()
                    }))
                }
            }
            Section { Text(L("动画宠物来自 Google Noto Animated Emoji，采用 CC BY 4.0 许可；为键盘缩小为 96 像素并精简了帧。", "Animated pets: Google Noto Animated Emoji, licensed CC BY 4.0; resized to 96 px with fewer frames for the keyboard.")).font(.caption).foregroundStyle(.secondary) }
        }.navigationTitle(L("宠物轮换", "Pet rotation"))
    }
}
struct PoemLibraryView: View {
    @EnvironmentObject private var model: SettingsModel
    @State private var pattern: PoemPattern?
    @State private var card: PoemWordCard?
    var body: some View {
        List {
            Section { Text(L("在键盘里打开“AI 作诗”，点输入行左侧的选项键选择即兴、藏头或藏尾，以及每句字数、句式和词卡。写好后点 ▶ 才会生成。", "Open AI Poem in the keyboard and use the options key left of the input line to choose improvise, hidden start or hidden end, line length, pattern and word cards. Nothing is generated until you tap ▶.")).font(.callout) }
            Section(L("内置句式", "Built-in patterns")) {
                ForEach(PoemPattern.builtIn) { item in VStack(alignment: .leading, spacing: 2) { Text(item.name); Text(item.instruction).font(.caption).foregroundStyle(.secondary) } }
            }
            Section {
                ForEach(model.value.poemLibrary.patterns) { item in
                    Button { pattern = item } label: { VStack(alignment: .leading, spacing: 2) { Text(item.name).foregroundStyle(.primary); Text(item.instruction).font(.caption).foregroundStyle(.secondary).lineLimit(2) } }
                }.onDelete { model.value.poemLibrary.patterns.remove(atOffsets: $0); model.save() }
                Button { pattern = PoemPattern() } label: { Label(L("添加句式", "Add pattern"), systemImage: "plus") }
            } header: { Text(L("自定义句式", "Custom patterns")) } footer: { Text(L("用一句话描述格式，例如“每句以‘你’结尾，语气温柔”。", "Describe the form in a sentence, e.g. “every line ends with ‘you’, gentle tone”.")) }
            Section {
                ForEach(model.value.poemLibrary.cards) { item in
                    Button { card = item } label: { VStack(alignment: .leading, spacing: 2) { Text(item.name).foregroundStyle(.primary); Text(item.words.joined(separator: " ")).font(.caption).foregroundStyle(.secondary).lineLimit(2) } }
                }.onDelete { model.value.poemLibrary.cards.remove(atOffsets: $0); model.save() }
                Button { card = PoemWordCard() } label: { Label(L("添加词卡", "Add word card"), systemImage: "plus") }
            } header: { Text(L("词卡", "Word cards")) } footer: { Text(L("选中的词卡会让 AI 尽量把其中的词语写进诗里。", "The AI tries to weave words from the selected cards into the poem.")) }
        }.navigationTitle(L("AI 作诗", "AI Poem"))
        .sheet(item: $pattern) { PoemPatternEditor(pattern: $0).environmentObject(model) }
        .sheet(item: $card) { PoemCardEditor(card: $0).environmentObject(model) }
    }
}
struct PoemPatternEditor: View {
    @EnvironmentObject private var model: SettingsModel
    @Environment(\.dismiss) private var dismiss
    @State var pattern: PoemPattern
    @State private var message = ""
    var body: some View {
        NavigationStack {
            Form {
                TextField(L("名称，如“七绝”", "Name, e.g. “Quatrain”"), text: $pattern.name)
                TextField(L("格式要求", "Form description"), text: $pattern.instruction, axis: .vertical).lineLimit(3...6)
                Picker(L("即兴时的句数", "Lines when improvising"), selection: $pattern.lines) {
                    Text(L("不固定", "Any")).tag(Int?.none)
                    ForEach([2, 4, 6, 8, 12], id: \.self) { Text("\($0)").tag(Int?.some($0)) }
                }
                if !message.isEmpty { Text(message).foregroundStyle(.red) }
            }.navigationTitle(L("句式", "Pattern"))
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button(L("取消", "Cancel")) { dismiss() } }; ToolbarItem(placement: .confirmationAction) { Button(L("保存", "Save")) { save() } } }
        }
    }
    private func save() {
        pattern.name = pattern.name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !pattern.name.isEmpty else { message = L("请填写名称", "Enter a name"); return }
        var next = model.value
        if let index = next.poemLibrary.patterns.firstIndex(where: { $0.id == pattern.id }) { next.poemLibrary.patterns[index] = pattern } else { next.poemLibrary.patterns.append(pattern) }
        do { try ConfigurationStore().save(next); model.value = next; dismiss() } catch { message = error.localizedDescription }
    }
}
struct PoemCardEditor: View {
    @EnvironmentObject private var model: SettingsModel
    @Environment(\.dismiss) private var dismiss
    @State var card: PoemWordCard
    @State private var words = ""
    @State private var message = ""
    var body: some View {
        NavigationStack {
            Form {
                TextField(L("名称，如“春日”", "Name, e.g. “Spring”"), text: $card.name)
                Section { TextField(L("词语，用空格或逗号分隔", "Words, separated by spaces or commas"), text: $words, axis: .vertical).lineLimit(3...8) } footer: { Text(L("例如：杏花 细雨 燕归 东风", "For example: blossom drizzle swallow breeze")) }
                if !message.isEmpty { Text(message).foregroundStyle(.red) }
            }.navigationTitle(L("词卡", "Word card"))
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button(L("取消", "Cancel")) { dismiss() } }; ToolbarItem(placement: .confirmationAction) { Button(L("保存", "Save")) { save() } } }
            .onAppear { words = card.words.joined(separator: " ") }
        }
    }
    private func save() {
        card.name = card.name.trimmingCharacters(in: .whitespacesAndNewlines)
        card.words = words.components(separatedBy: CharacterSet(charactersIn: " ,，、;；\n\t")).map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        guard !card.name.isEmpty, !card.words.isEmpty else { message = L("请填写名称和至少一个词语", "Enter a name and at least one word"); return }
        var next = model.value
        if let index = next.poemLibrary.cards.firstIndex(where: { $0.id == card.id }) { next.poemLibrary.cards[index] = card } else { next.poemLibrary.cards.append(card) }
        do { try ConfigurationStore().save(next); model.value = next; dismiss() } catch { message = error.localizedDescription }
    }
}
struct ChordProfilesView: View {
    @EnvironmentObject private var model: SettingsModel
    @State private var importing = false
    @State private var editing: ChordProfile?
    var body: some View {
        List {
            Section { Text(L("每手：起点＋终点。途中经过的键不计入；双手全部松开时提交。划回起点恢复单键。", "Each hand selects start + end. Passed keys do not count. Release both hands to submit; return to the start for a single key.")) }
            Section(L("当前方案", "Active profile")) {
                ForEach([ChordProfile.builtIn] + model.value.profiles) { p in
                    HStack { Button(p.id == ChordProfile.builtIn.id ? L("默认并击", "Default chord") : p.name) { model.value.chord = p; model.save() }; Spacer(); if model.value.chord.id == p.id { Image(systemName:"checkmark").foregroundStyle(.teal) }; Button { editing = p.id == "builtin.flyyao" ? p.copy() : p } label: { Image(systemName:"pencil") } }
                    .contextMenu { Button(L("复制方案", "Duplicate profile")) { editing = p.copy() } }
                }
            }
            Button(L("复制默认并击", "Copy default chord")) { editing = ChordProfile.builtIn.copy() }
            Button(L("导入 JSON", "Import JSON")) { importing = true }
        }.navigationTitle(L("滑动并击", "Slide chords"))
        .fileImporter(isPresented:$importing,allowedContentTypes:[.json]) { result in
            do { let url = try result.get(); let access = url.startAccessingSecurityScopedResource(); defer { if access { url.stopAccessingSecurityScopedResource() } }; editing = try ChordProfile.imported(Data(contentsOf:url,options:.mappedIfSafe)) } catch { model.error = error.localizedDescription }
        }.sheet(item:$editing) { ChordEditor(profile:$0).environmentObject(model) }
    }
}
struct JSONDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.json] }
    var data: Data
    init(_ data: Data) { self.data = data }
    init(configuration: ReadConfiguration) throws { data = configuration.file.regularFileContents ?? Data() }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper { FileWrapper(regularFileWithContents:data) }
}
struct ChordEditor: View {
    @EnvironmentObject private var model: SettingsModel
    @Environment(\.dismiss) private var dismiss
    @State var profile: ChordProfile
    @State private var search = ""
    @State private var message = ""
    @State private var exporting = false
    @State private var document = JSONDocument(Data())
    var body: some View {
        NavigationStack {
            Form {
                TextField(L("名称", "Name"),text:$profile.name)
                TextField(L("左区字母", "Left keys"),text:$profile.leftKeys).textInputAutocapitalization(.never).autocorrectionDisabled()
                TextField(L("右区字母", "Right keys"),text:$profile.rightKeys).textInputAutocapitalization(.never).autocorrectionDisabled()
                Picker(L("输出编码", "Output encoding"),selection:$profile.outputEncoding) { Text("全拼").tag(ChordOutputEncoding.fullPinyin); Text("自然码").tag(ChordOutputEncoding.ziranma) }
                Picker(L("音节边界", "Syllable boundaries"),selection:$profile.boundaryPolicy) { Text(L("每批次", "Each batch")).tag(ChordBoundaryPolicy.legacyBatches); Text(L("按映射类型", "By mapping kind")).tag(ChordBoundaryPolicy.explicitSyllables) }
                Section(L("映射", "Mappings")) {
                    TextField(L("查找键或拼音", "Find keys or pinyin"),text:$search).textInputAutocapitalization(.never)
                    ForEach(profile.mappings.indices.filter { search.isEmpty || profile.mappings[$0].keys.contains(search) || profile.mappings[$0].output.contains(search) },id:\.self) { i in
                        HStack {
                            TextField("keys",text:$profile.mappings[i].keys).frame(width:65)
                            TextField("pinyin",text:$profile.mappings[i].output)
                            Picker("Kind",selection:$profile.mappings[i].kind) { Text(L("音节", "Syllable")).tag(ChordMappingKind.syllable); Text(L("片段", "Fragment")).tag(ChordMappingKind.fragment) }.labelsHidden()
                        }.textInputAutocapitalization(.never).autocorrectionDisabled()
                    }.onDelete { offsets in let indices = profile.mappings.indices.filter { search.isEmpty || profile.mappings[$0].keys.contains(search) || profile.mappings[$0].output.contains(search) }; for i in offsets.map({ indices[$0] }).sorted(by:>) { profile.mappings.remove(at:i) } }
                    Button(L("添加映射", "Add mapping")) { profile.mappings.append(.init(keys:"",output:"",kind:.syllable)); search = "" }
                }
                Button(L("导出 JSON", "Export JSON")) { do { _ = try profile.validated(); document = JSONDocument(try JSONEncoder().encode(profile)); exporting = true } catch { message = error.localizedDescription } }
                Button(L("另存为副本", "Save a copy")) { profile = profile.copy(); message = L("已创建副本，点击“保存并应用”完成。", "Copy created. Choose Save & apply to keep it.") }
                if !message.isEmpty { Text(message).foregroundStyle(.red) }
            }.navigationTitle(L("键位方案", "Chord profile"))
            .toolbar { ToolbarItem(placement:.cancellationAction) { Button(L("取消", "Cancel")) { dismiss() } }; ToolbarItem(placement:.confirmationAction) { Button(L("保存并应用", "Save & apply")) { do { profile = try profile.validated(); var next = model.value; next.profiles.removeAll { $0.id == profile.id }; next.profiles.append(profile); next.chord = profile; try ConfigurationStore().save(next); model.value = next; dismiss() } catch { message = error.localizedDescription } } } }
            .fileExporter(isPresented:$exporting,document:document,contentType:.json,defaultFilename:"rimes-chord") { _ in }
        }
    }
}
struct PrivacyView: View {
    var body: some View {
        List {
            Section(L("本机输入", "On-device typing")) { Text(L("词频只保存在设备。无账户、遥测或输入正文日志。Buffer 草稿不会落盘；键盘会话结束时清除。", "Learning stays on your device. No account, telemetry or text logs. Buffer drafts are not saved to disk and are cleared when the keyboard session ends.")) }
            Section(L("本地剪贴板", "Local clipboard")) { Text(L("打开键盘设置或 Buffer 设置里的“本地剪贴板”。只有主动点收录才读取当前文字，需要完全访问；不会后台监听。最多 40 条、总共 128 KiB、单条 16 KiB，保存在键盘私有目录并排除备份，可逐条删除或清空。点条目只加入 Buffer，不自动上屏或联网；已保存的本地历史不需要完全访问。", "Open Local clipboard from the keyboard or Buffer settings. Only an explicit Collect tap reads current text and needs Full Access; there is no background monitoring. Up to 40 entries, 128 KiB total and 16 KiB each stay in the keyboard’s private storage, excluded from backup. Delete entries or clear the list. Tapping an entry only adds it to Buffer, without automatic insertion or networking. Saved local history works without Full Access.")) }
            Section(L("统计图片", "Stats images")) { Text(L("图片在本机生成，只含汇总统计，不含输入正文。保存时只申请相册添加权限，不读取你的照片；App 同时保留最新一张主动保存的卡片，不参与备份，可在卡片页面删除。相册中的副本请在照片 App 中删除。", "Images are generated on-device with aggregate statistics, without typed text. Saving requests add-only access and never reads your photos. The app keeps the latest explicitly saved card, excluded from backup and deletable on its page. Delete Photos copies in the Photos app.")) }
            Section("AI") { Text(L("只发送你主动提交的 Buffer 文本到你配置并同意的服务；API Key 保存在仅限本设备的 Keychain 中。请求不会跟随重定向。", "Only explicitly submitted Buffer text goes to your configured, consented service. API keys stay in this device's Keychain. Requests never follow redirects.")) }
            Section(L("Apple 本地翻译", "Apple on-device translation")) { Text(L("翻译在设备上使用已下载的 Apple 语言模型。下载语言包可能需要联网；翻译不可用时不会自动改用 AI 服务。", "Translation uses downloaded Apple language models on your device. Preparing languages may need a network connection. Unavailable translations never fall back to an AI service automatically.")) }
            Section {
                Link(L("隐私政策", "Privacy policy"), destination: URL(string: "https://scholay.github.io/rimes/ios/privacy/")!)
                Link(L("使用帮助与联系", "Help & contact"), destination: URL(string: "https://scholay.github.io/rimes/ios/support/")!)
            }
            Section(L("第三方声明", "Third-party notices")) { Text(Bundle.main.url(forResource:"THIRD_PARTY",withExtension:"txt").flatMap { try? String(contentsOf:$0,encoding:.utf8) } ?? L("许可文件缺失", "License file missing")).font(.caption).textSelection(.enabled) }
        }.navigationTitle(L("隐私与许可", "Privacy & licenses"))
    }
}

/// App Store is the only installer for the production iOS application.
private struct StoreUpdateView: View {
    @Environment(\.openURL) private var openURL
    @State private var checking = false
    @State private var available: String?
    @State private var message = ""
    private var current: String { Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.0.0" }
    private var testFlight: Bool { Bundle.main.appStoreReceiptURL?.lastPathComponent == "sandboxReceipt" }
    var body: some View {
        Form {
            Section {
                LabeledContent(L("当前版本", "Current version"), value: current)
                if !message.isEmpty { Text(message).foregroundStyle(.secondary).accessibilityIdentifier("update.status") }
                if checking { ProgressView() }
                Button(L("检查更新", "Check for updates")) { Task { await check() } }.disabled(checking)
                    .accessibilityIdentifier("update.check")
                if let available {
                    Button(L("前往 App Store 更新至 \(available)", "Update to \(available) in App Store")) {
                        UserDefaults.standard.set(available, forKey: "storeUpdateTarget")
                        openURL(URL(string: "https://apps.apple.com/app/id6814620242")!) { accepted in
                            message = accepted ? L("请在 App Store 完成下载与安装，然后重新打开应用。", "Download and install in App Store, then reopen the app.") : L("无法打开 App Store，请重试。", "Could not open App Store. Try again.")
                        }
                    }.accessibilityIdentifier("update.install")
                } else {
                    Link(L("在 App Store 查看", "View in App Store"), destination: URL(string: "https://apps.apple.com/app/id6814620242")!)
                }
                if testFlight {
                    Button(L("打开 TestFlight", "Open TestFlight")) { openURL(URL(string: "https://testflight.apple.com/join/Kdj9RB4q")!) }
                    Text(L("这是测试渠道，测试版本更新由 TestFlight 管理。", "TestFlight manages updates for this testing channel."))
                }
            } footer: {
                Text(L("iOS 更新由 Apple 商店下载和安装，保留现有设置、方案及学习记录。", "Apple downloads and installs iOS updates, preserving settings, schemes and learned words."))
            }
        }.navigationTitle(L("应用更新", "Software update"))
            .onAppear {
                if let target = UserDefaults.standard.string(forKey: "storeUpdateTarget"),
                   current.compare(target, options: .numeric) != .orderedAscending {
                    message = L("更新完成，当前版本 \(current)", "Update complete. Version \(current)")
                    UserDefaults.standard.removeObject(forKey: "storeUpdateTarget")
                }
            }
    }
    @MainActor private func check() async {
        checking = true; available = nil
        defer { checking = false }
        do {
            // The Apple-account storefront can differ from the device's region.
            // Fetch it for each check; never cache it or silently query another country.
            guard let storefront = await Storefront.current,
                  let country = StorefrontRegion.lookupCountry(for: storefront.countryCode) else {
                message = L("无法确定当前 App Store 地区，请直接在 App Store 查看更新。", "The App Store region is unavailable. Check for updates in App Store.")
                return
            }
            var url = URLComponents(string: "https://itunes.apple.com/lookup")!
            url.queryItems = [URLQueryItem(name: "id", value: "6814620242"),
                             URLQueryItem(name: "country", value: country)]
            var request = URLRequest(url: url.url!, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 25)
            request.setValue("application/json", forHTTPHeaderField: "Accept")
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200, data.count <= 1024 * 1024 else { throw URLError(.badServerResponse) }
            struct Lookup: Decodable {
                struct App: Decodable { let trackId: Int; let bundleId: String; let version: String; let minimumOsVersion: String }
                let results: [App]
            }
            let result = try JSONDecoder().decode(Lookup.self, from: data)
            guard let app = result.results.first(where: { $0.trackId == 6814620242 && $0.bundleId == "org.scholay.rimes.ios" }) else {
                message = L("当前地区暂未查到商店版本，请在 App Store 查看。", "No listing was found for this region. Check App Store.")
                return
            }
            guard app.version.range(of: #"^\d+\.\d+\.\d+$"#, options: .regularExpression) != nil else { throw URLError(.cannotParseResponse) }
            guard UIDevice.current.systemVersion.compare(app.minimumOsVersion, options: .numeric) != .orderedAscending else {
                message = L("新版本需要 iOS \(app.minimumOsVersion) 或更新系统。", "The update requires iOS \(app.minimumOsVersion) or later."); return
            }
            if app.version.compare(current, options: .numeric) == .orderedDescending {
                available = app.version
                message = L("发现商店新版本 \(app.version)", "App Store version \(app.version) is available")
            } else {
                message = L("当前已是最新商店版本（商店 \(app.version)）。", "Up to date with App Store (\(app.version)).")
            }
        } catch {
            message = L("检查失败，可重试：", "Check failed. Try again: ") + error.localizedDescription
        }
    }
}
