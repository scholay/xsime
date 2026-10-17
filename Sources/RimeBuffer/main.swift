import CryptoKit
import Cocoa
import InputMethodKit
import Carbon
import Darwin
import CRimeBridge

if let status = MacRimeSchemeLibrary.deploymentCommand(CommandLine.arguments) {
    exit(status)
}

// OpenSSH invokes this same executable as a narrowly scoped SSH_ASKPASS
// helper. Handle that request before any smoke, installer, AppKit, or IMK
// startup so the helper can return the saved password and terminate without
// creating a second input-method process.
// The very first statement, deliberately. The data directory and the
// preference domain both changed with the rename, and anything that resolves
// either one first — the Rime engine bootstrap, a store, even a log file —
// creates the new directory before the migration can fill it, after which the
// migration sees an occupied destination and correctly refuses to merge two
// histories. Ordering is the whole guarantee here; nothing else can supply it.
RimesDataMigration.migrateIfNeeded(
    from: RimesPaths.legacyUserDirectory(),
    to: RimesPaths.userDirectory()
)
RimesPreferenceMigration.migrateIfNeeded(
    from: RimesIdentity.unregistrableBundleIdentifier,
    marker: "rimes.preferencesMigratedFromScholayIsaac.v1"
)
RimesPreferenceMigration.migrateIfNeeded(
    from: RimesIdentity.legacyBundleIdentifier
)

if let status = RemarkableSSHAskPassHandler.handleIfRequested() {
    exit(status)
}

// Capsule management is an explicit standalone CLI surface. It runs before
// AppKit/IMK startup; the password namespace never exposes plaintext output.
if let status = CapsulePasswordCLI.handleIfRequested(
    arguments: CommandLine.arguments
) {
    exit(status)
}

// Clipboard migration/audit commands are aggregate-only, run before AppKit
// and IMK startup, and never share clipboard text through stdout or logs.
if let status = PasteClipboardHistoryCLI.handleIfRequested(
    arguments: CommandLine.arguments
) {
    exit(status)
}

// Provider setup is deliberately a narrow standalone command: it accepts a
// credential only from protected interactive stdin and exits before AppKit/IMK
// startup. The normal UI uses the same private profile store.
if let status = AIProviderProfileCLI.handleIfRequested(
    arguments: CommandLine.arguments
) {
    exit(Int32(status))
}

// Installer reconciliation deliberately runs each TIS mutation/verification
// in a fresh process. Handle those private phases before any smoke, AppKit,
// IMK, or librime bootstrap.
if let status = inputSourceInstallPhaseExitStatus(
    arguments: CommandLine.arguments
) {
    exit(status)
}

/// Standalone commands share the shipped executable with the live IMK server,
/// but must never share its LevelDB-backed Rime user directory. Keep this rule
/// above every smoke/subcommand dispatch so even a newly added smoke that starts
/// `rimeEngine` inherits isolation before the lazy global is initialized.
private enum StandaloneRimeCommandRules {
    private static let engineHostingCommands: Set<String> = [
        "settings-preview",
        "settings-render",
        "gateway-settings-render",
    ]

    static func requiresIsolatedUserDir(arguments: [String]) -> Bool {
        arguments.dropFirst().contains { argument in
            argument == "smoke"
                || argument.hasSuffix("-smoke")
                || engineHostingCommands.contains(argument)
        }
    }

    static func isLiveUserDir(_ path: String,
                              homeDirectory: String = NSHomeDirectory()) -> Bool {
        let candidateURL = URL(fileURLWithPath: path)
            .standardizedFileURL
            .resolvingSymlinksInPath()
        // Both names are live during and after the rename: the migration
        // copies rather than moves, so the old directory still holds the
        // user's dictionaries and Capsule records. A standalone process must
        // be kept out of either one.
        let liveURLs = [RimesPaths.directoryName, RimesPaths.legacyDirectoryName]
            .map { name in
                URL(fileURLWithPath: homeDirectory, isDirectory: true)
                    .appendingPathComponent("Library/\(name)", isDirectory: true)
                    .standardizedFileURL
                    .resolvingSymlinksInPath()
            }
        return liveURLs.contains { matchesLiveDirectory(candidateURL, live: $0) }
    }

    private static func matchesLiveDirectory(_ candidateURL: URL,
                                             live liveURL: URL) -> Bool {

        // macOS user volumes are commonly case-insensitive, and firmlinks or
        // symlinks can spell the same directory with a different path. Compare
        // every existing candidate ancestor by filesystem identity so a child
        // of the live profile is rejected even when its final leaf does not yet
        // exist. The string fallback is deliberately case-insensitive/fail-
        // closed for test homes or a profile that has not been created yet.
        func identity(_ url: URL) -> (device: UInt64, inode: UInt64)? {
            var metadata = stat()
            guard url.path.withCString({ stat($0, &metadata) }) == 0 else {
                return nil
            }
            return (UInt64(metadata.st_dev), UInt64(metadata.st_ino))
        }
        if let liveIdentity = identity(liveURL) {
            var ancestor = candidateURL
            // Bound traversal by the syntactic depth as well as checking `/`.
            // Foundation may represent deleting the parent of root as `/..` on
            // some SDKs, so parent equality alone is not a termination proof.
            for _ in 0...candidateURL.pathComponents.count {
                if let candidateIdentity = identity(ancestor),
                   candidateIdentity.device == liveIdentity.device,
                   candidateIdentity.inode == liveIdentity.inode {
                    return true
                }
                if ancestor.path == "/" { break }
                let parent = ancestor.deletingLastPathComponent()
                if parent.path == ancestor.path { break }
                ancestor = parent
            }
        }

        let candidate = candidateURL.path
        let live = liveURL.path
        return candidate.compare(live, options: [.caseInsensitive]) == .orderedSame
            || candidate.range(
                of: live + "/",
                options: [.anchored, .caseInsensitive]
            ) != nil
    }
}

private var automaticallyCreatedStandaloneUserDir: String?

private func removeAutomaticallyCreatedStandaloneUserDir() {
    guard let path = automaticallyCreatedStandaloneUserDir else { return }
    automaticallyCreatedStandaloneUserDir = nil
    let root = URL(fileURLWithPath: path, isDirectory: true)
        .standardizedFileURL
        .resolvingSymlinksInPath()
    let temporaryRoot = FileManager.default.temporaryDirectory
        .standardizedFileURL
        .resolvingSymlinksInPath()
    guard root.deletingLastPathComponent().path == temporaryRoot.path,
          root.lastPathComponent.hasPrefix("rimebuffer-standalone-") else {
        return
    }
    try? FileManager.default.removeItem(at: root)
}

/// Finds the source trees used by `build_install.sh` when the smoke binary is
/// run directly from a SwiftPM checkout. A packaged ETInput.app already has a
/// complete SharedSupport directory and never takes this path.
private func engineSmokeRepositoryRoot() -> URL? {
    let fileManager = FileManager.default
    var startingPoints = [
        URL(fileURLWithPath: fileManager.currentDirectoryPath, isDirectory: true),
    ]
    if let executable = CommandLine.arguments.first, !executable.isEmpty {
        let executableURL = URL(fileURLWithPath: executable)
        startingPoints.append(
            executableURL.isFileURL
                ? executableURL.deletingLastPathComponent()
                : executableURL
        )
    }

    for startingPoint in startingPoints {
        var candidate = startingPoint.standardizedFileURL
        for _ in 0..<8 {
            let productData = candidate.appendingPathComponent(
                "rime-data/default.custom.yaml"
            )
            let vendorData = candidate.appendingPathComponent(
                "Vendor/rime/SharedSupport/default.yaml"
            )
            let vendorRuntime = candidate.appendingPathComponent(
                "Vendor/rime/Frameworks/librime.1.dylib"
            )
            if fileManager.fileExists(atPath: productData.path),
               fileManager.fileExists(atPath: vendorData.path),
               fileManager.fileExists(atPath: vendorRuntime.path) {
                return candidate
            }
            let parent = candidate.deletingLastPathComponent()
            if parent.path == candidate.path { break }
            candidate = parent
        }
    }
    return nil
}

/// Merge a reviewed fixture tree without following symlinks. The destination
/// is always a fresh child of the isolated smoke directory, so replacements
/// can only affect disposable test data.
private func mergeEngineSmokeFixtureTree(from source: URL,
                                         into destination: URL) throws {
    let fileManager = FileManager.default
    try fileManager.createDirectory(
        at: destination,
        withIntermediateDirectories: true,
        attributes: [.posixPermissions: 0o700]
    )
    guard let enumerator = fileManager.enumerator(
        at: source,
        includingPropertiesForKeys: [
            .isDirectoryKey,
            .isRegularFileKey,
            .isSymbolicLinkKey,
        ],
        options: [.skipsHiddenFiles]
    ) else {
        throw CocoaError(.fileReadUnknown)
    }

    let sourcePrefix = source.standardizedFileURL.path + "/"
    for case let item as URL in enumerator {
        let values = try item.resourceValues(forKeys: [
            .isDirectoryKey,
            .isRegularFileKey,
            .isSymbolicLinkKey,
        ])
        guard values.isSymbolicLink != true,
              item.standardizedFileURL.path.hasPrefix(sourcePrefix) else {
            throw CocoaError(.fileReadInvalidFileName)
        }
        let relativePath = String(
            item.standardizedFileURL.path.dropFirst(sourcePrefix.count)
        )
        guard !relativePath.isEmpty else { continue }
        let target = destination.appendingPathComponent(
            relativePath,
            isDirectory: values.isDirectory == true
        )
        if values.isDirectory == true {
            try fileManager.createDirectory(
                at: target,
                withIntermediateDirectories: true
            )
        } else if values.isRegularFile == true {
            if fileManager.fileExists(atPath: target.path) {
                try fileManager.removeItem(at: target)
            }
            try fileManager.copyItem(at: item, to: target)
        } else {
            throw CocoaError(.fileReadUnsupportedScheme)
        }
    }
}

/// A checkout build has no app-bundle SharedSupport. Recreate the same
/// Vendor+rime-data overlay as `build_install.sh`, but only inside the isolated
/// smoke directory. This avoids both Squirrel's schemas and the live RIMES
/// userdb while exercising the current checkout's product data.
private func configureEngineSmokeRuntime(isolatedUserDir: String) -> Bool {
    let environment = ProcessInfo.processInfo.environment
    // The release catalog keeps my_combo optional, so a clean profile deploys
    // only ordinary schemas. This harness also verifies the chord engine; opt
    // it in explicitly inside the disposable user directory rather than
    // weakening the fresh-install default.
    let smokeCustomURL = URL(
        fileURLWithPath: isolatedUserDir,
        isDirectory: true
    ).appendingPathComponent("default.custom.yaml")
    do {
        try SchemaListStore.writeEnabledIDs(
            InputSchemaCatalog.enabledIDs(chordExtensionEnabled: true),
            to: smokeCustomURL
        )
    } catch {
        FileHandle.standardError.write(Data(
            "cannot enable optional chord schema for engine smoke\n".utf8
        ))
        return false
    }
    if let explicit = environment["RIMEBUFFER_SHARED_DIR"], !explicit.isEmpty {
        return true
    }
    if let bundled = Bundle.main.sharedSupportURL,
       FileManager.default.fileExists(
           atPath: bundled.appendingPathComponent("default.custom.yaml").path
       ),
       FileManager.default.fileExists(
           atPath: bundled.appendingPathComponent("my_combo.schema.yaml").path
       ) {
        return true
    }
    guard let repositoryRoot = engineSmokeRepositoryRoot() else {
        FileHandle.standardError.write(Data(
            "cannot locate product Rime data for engine smoke\n".utf8
        ))
        return false
    }

    let isolatedRoot = URL(fileURLWithPath: isolatedUserDir, isDirectory: true)
    let sharedSupport = isolatedRoot.appendingPathComponent(
        ".engine-smoke-shared-\(UUID().uuidString)",
        isDirectory: true
    )
    do {
        try mergeEngineSmokeFixtureTree(
            from: repositoryRoot.appendingPathComponent(
                "Vendor/rime/SharedSupport",
                isDirectory: true
            ),
            into: sharedSupport
        )
        try mergeEngineSmokeFixtureTree(
            from: repositoryRoot.appendingPathComponent(
                "rime-data",
                isDirectory: true
            ),
            into: sharedSupport
        )
    } catch {
        FileHandle.standardError.write(Data(
            "cannot stage product Rime data for engine smoke\n".utf8
        ))
        return false
    }

    guard setenv("RIMEBUFFER_SHARED_DIR", sharedSupport.path, 1) == 0 else {
        FileHandle.standardError.write(Data(
            "cannot activate product Rime data for engine smoke\n".utf8
        ))
        return false
    }
    if environment["RIMEBUFFER_FRAMEWORKS_DIR"]
        .flatMap({ $0.isEmpty ? nil : $0 }) == nil {
        let frameworks = repositoryRoot.appendingPathComponent(
            "Vendor/rime/Frameworks",
            isDirectory: true
        )
        guard setenv("RIMEBUFFER_FRAMEWORKS_DIR", frameworks.path, 1) == 0 else {
            FileHandle.standardError.write(Data(
                "cannot activate isolated Rime runtime for engine smoke\n".utf8
            ))
            return false
        }
    }
    return true
}

private func standaloneIsolationAliasProbe(isolatedUserDir: String) -> Bool {
    let fileManager = FileManager.default
    let fixture = URL(fileURLWithPath: isolatedUserDir, isDirectory: true)
        .appendingPathComponent(
            ".isolation-alias-probe-\(UUID().uuidString)",
            isDirectory: true
        )
    defer { try? fileManager.removeItem(at: fixture) }
    let fakeHome = fixture.appendingPathComponent("home", isDirectory: true)
    let live = fakeHome.appendingPathComponent(
        "Library/\(RimesPaths.directoryName)",
        isDirectory: true
    )
    let alias = fixture.appendingPathComponent("profile-alias", isDirectory: true)
    do {
        try fileManager.createDirectory(
            at: live,
            withIntermediateDirectories: true
        )
        try fileManager.createSymbolicLink(at: alias, withDestinationURL: live)
    } catch {
        return false
    }
    return StandaloneRimeCommandRules.isLiveUserDir(
            alias.appendingPathComponent("future-child").path,
            homeDirectory: fakeHome.path
        )
        && !StandaloneRimeCommandRules.isLiveUserDir(
            fixture.appendingPathComponent("unrelated/profile").path,
            homeDirectory: fakeHome.path
        )
}

/// Returns the isolated directory in force, respecting an explicit non-live
/// override. An override that resolves to the installed IME's user directory is
/// replaced rather than trusted: smoke tooling is never authorized to open the
/// live userdb, even when invoked accidentally with that path in its environment.
@discardableResult
private func isolateStandaloneRimeUserDir() -> String? {
    let environment = ProcessInfo.processInfo.environment
    let configured = environment["RIMEBUFFER_USER_DIR"]
        .flatMap { $0.isEmpty ? nil : $0 }
    let directory: String
    let createdAutomatically: Bool
    if let configured,
       !StandaloneRimeCommandRules.isLiveUserDir(configured) {
        directory = configured
        createdAutomatically = false
    } else {
        directory = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "rimebuffer-standalone-\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString)",
                isDirectory: true
            ).path
        createdAutomatically = true
        if configured != nil {
            FileHandle.standardError.write(Data(
                "refusing live RIMEBUFFER_USER_DIR for standalone command; using an isolated directory\n".utf8
            ))
        }
    }

    do {
        try FileManager.default.createDirectory(
            atPath: directory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )
    } catch {
        FileHandle.standardError.write(Data(
            "cannot create isolated Rime user directory\n".utf8
        ))
        return nil
    }
    guard setenv("RIMEBUFFER_USER_DIR", directory, 1) == 0 else {
        FileHandle.standardError.write(Data(
            "cannot activate isolated Rime user directory\n".utf8
        ))
        return nil
    }
    if createdAutomatically,
       automaticallyCreatedStandaloneUserDir == nil {
        automaticallyCreatedStandaloneUserDir = directory
        atexit(removeAutomaticallyCreatedStandaloneUserDir)
    }
    return directory
}

if StandaloneRimeCommandRules.requiresIsolatedUserDir(
    arguments: CommandLine.arguments
) {
    guard let isolatedUserDir = isolateStandaloneRimeUserDir() else {
        exit(1)
    }
    if (CommandLine.arguments.contains("smoke")
        || CommandLine.arguments.contains("shift-punctuation-smoke")
        || CommandLine.arguments.contains("candidate-pagination-smoke")
        || CommandLine.arguments.contains("user-lexicon-bridge-smoke")
        || CommandLine.arguments.contains("external-schema-engine-smoke")
        || CommandLine.arguments.contains("scheme-import-smoke")
        || CommandLine.arguments.contains("chord-keymap-engine-smoke")
        || CommandLine.arguments.contains("chord-ziranma-engine-smoke")
        || CommandLine.arguments.contains("yoyo-engine-smoke")),
       !configureEngineSmokeRuntime(isolatedUserDir: isolatedUserDir) {
        exit(1)
    }
}

// Recovery seam for the unpacked companion extension. Credentials remain
// internal: reset rotates them and reports only a non-secret completion line.
if CommandLine.arguments.contains("marine-chrome-reset-pairing") {
    do {
        // Advance the credential generation under the same cross-process lock
        // used by interactive issuance. A pre-reset approval can therefore no
        // longer race this helper and publish a usable credential afterwards.
        _ = try MarineChromeCredentialTransaction.reset()
        DistributedNotificationCenter.default().postNotificationName(
            .marineChromeCredentialsDidReset,
            object: nil,
            userInfo: nil,
            deliverImmediately: true
        )
        print("marine-chrome pairing reset")
        exit(0)
    } catch {
        FileHandle.standardError.write(Data(
            "cannot reset marine-chrome pairing\n".utf8
        ))
        exit(1)
    }
}

private var marineChromeCredentialsResetObserver: NSObjectProtocol?

private func installMarineChromePairingApprovalHandler() {
    MarineChromePairingBroker.shared.setApprovalHandler { request, respond in
        guard MarineChromeGatewayAvailability.shared.isAvailable else {
            respond(false)
            return
        }
        MarineChromePairingPromptController.shared.present(
            request,
            respond: respond
        )
    }
    MarineChromePairingBroker.shared.setCancellationHandler { requestID in
        DispatchQueue.main.async {
            MarineChromePairingPromptController.shared.cancel(
                requestID: requestID
            )
        }
    }
    guard marineChromeCredentialsResetObserver == nil else { return }
    marineChromeCredentialsResetObserver = DistributedNotificationCenter
        .default().addObserver(
            forName: .marineChromeCredentialsDidReset,
            object: nil,
            queue: .main
        ) { _ in
            MarineChromePairingBroker.shared.cancelAll()
            MarineChromePairingPromptController.shared.cancel()
            MarineChromeContextStore.shared.clear()
            NotificationCenter.default.post(
                name: .marineChromePairingDidChange,
                object: nil
            )
        }
}

// Safe maintenance seam used by installers and support diagnostics. Loading
// applies narrowly scoped, atomic configuration migrations without printing
// Base URLs, model names, or API keys.
if CommandLine.arguments.contains("openai-config-migrate") {
    do {
        guard try OpenAICompatibleConfigurationStore.shared.load() != nil else {
            print("OpenAI-compatible configuration is not set")
            exit(2)
        }
        print("OpenAI-compatible configuration is ready")
        exit(0)
    } catch {
        print("OpenAI-compatible configuration is invalid")
        exit(1)
    }
}

/// Main-thread integration boundary between Registry lifecycle and the gateway
/// queue's availability snapshot. Revoking the optional plug-in also revokes
/// every in-memory browser authority; persisted pairing credentials remain
/// available for a later explicit re-enable.
private func reconcileMarineChromeGatewayAvailability(
    registry: PluginRegistry
) {
    dispatchPrecondition(condition: .onQueue(.main))
    let key = PluginKey(
        domain: .builtIn,
        rawID: BuiltInPluginID.marineChrome
    )
    let available = registry.isEnabled(key)
    MarineChromeGatewayAvailabilityLifecycle.reconcile(
        available,
        cancelPairing: { MarineChromePairingBroker.shared.cancelAll() },
        cancelPrompt: { MarineChromePairingPromptController.shared.cancel() },
        clearContext: { MarineChromeContextStore.shared.clear() }
    )
}

private var marineChromePluginAvailabilityObserver: NSObjectProtocol?

private func installMarineChromeGatewayAvailabilityObserver(
    registry: PluginRegistry
) {
    reconcileMarineChromeGatewayAvailability(registry: registry)
    guard marineChromePluginAvailabilityObserver == nil else { return }
    marineChromePluginAvailabilityObserver = NotificationCenter.default.addObserver(
        forName: .pluginRegistryDidChange,
        object: registry,
        queue: .main
    ) { _ in
        reconcileMarineChromeGatewayAvailability(registry: registry)
    }
}

// `swift run RimeBuffer smoke` validates the engine end-to-end without IMK.
if CommandLine.arguments.contains("buffer-music-audio-smoke") {
    exit(runBufferMusicAudioSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("buffer-music-smoke") {
    let sessionPassed = runBufferMusicSessionSmokeTest()
    let keyboardPassed = runBufferMusicKeyboardSmokeTest()
    exit(sessionPassed && keyboardPassed ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "buffer-music-view-smoke") {
    let outputURL = CommandLine.arguments.count > index + 1
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(runBufferMusicViewSmokeTest(outputURL: outputURL) ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "buffer-music-panel-smoke") {
    _ = NSApplication.shared
    let outputURL = CommandLine.arguments.count > index + 1
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(withInstalledMusicPluginSmoke {
        BufferWindowController.shared.exerciseMusicPresentationForSmoke(outputURL: outputURL)
    } ? 0 : 1)
}
if CommandLine.arguments.contains("stats-smoke") {
    exit(runStatsSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("buffer-smoke") {
    exit(runBufferSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("buffer-window-smoke") {
    exit(runBufferWindowSmokeTest() ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(where: { $0 == "capsule-password-linux-fixtures" || $0 == "capsule-password-linux-verify" }),
   CommandLine.arguments.indices.contains(index + 1) {
    do {
        try MainActor.assumeIsolated {
            let target = URL(fileURLWithPath: CommandLine.arguments[index + 1])
            if CommandLine.arguments[index] == "capsule-password-linux-fixtures" { try CapsulePasswordLinuxFixtures.export(to: target) }
            else { try CapsulePasswordLinuxFixtures.verifyLinux(at: target) }
        }
        exit(0)
    } catch { fputs("Public password fixture operation failed\n", stderr); exit(1) }
}

if let index = CommandLine.arguments.firstIndex(of: "gateway-linux-fixtures"),
   CommandLine.arguments.indices.contains(index + 1) {
    do {
        let output = URL(fileURLWithPath: CommandLine.arguments[index + 1])
        let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        var contract = LocalGateway.linuxContract()
        var hashes: [String: String] = [:]
        for path in ["Sources/RimeBuffer/Inbound/LocalGateway.swift",
                     "Sources/RimeBuffer/Inbound/InboundBus.swift",
                     "Sources/RimeBuffer/Inbound/GatewayToken.swift"] {
            let data = try Data(contentsOf: root.appendingPathComponent(path))
            hashes[path] = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        }
        contract["sources"] = hashes
        let data = try JSONSerialization.data(withJSONObject: contract, options: [.prettyPrinted, .sortedKeys])
        try FileManager.default.createDirectory(at: output.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: output, options: .atomic)
        exit(0)
    } catch { fputs("gateway contract export failed: \(error)\n", stderr); exit(1) }
}

if let index = CommandLine.arguments.firstIndex(where: { $0 == "mailbox-linux-fixtures" || $0 == "mailbox-linux-verify" }),
   CommandLine.arguments.indices.contains(index + 1) {
    _ = NSApplication.shared
    exit(MainActor.assumeIsolated {
        do {
            let path = URL(fileURLWithPath: CommandLine.arguments[index + 1])
            if CommandLine.arguments[index] == "mailbox-linux-fixtures" { try MailboxLinuxFixtures.export(to: path) }
            else { try MailboxLinuxFixtures.verifyLinux(at: path) }
            return 0
        } catch { fputs("Public Mailbox fixture operation failed\n", stderr); return 1 }
    })
}
if let index = CommandLine.arguments.firstIndex(of: "capsule-password-visual-fixtures"),
   CommandLine.arguments.indices.contains(index + 1) {
    _ = NSApplication.shared
    exit(MainActor.assumeIsolated {
        do { try CapsulePasswordVisualFixtures.export(to: URL(fileURLWithPath: CommandLine.arguments[index + 1])); return 0 }
        catch { fputs("Password visual fixture export failed\n", stderr); return 1 }
    })
}
if let index = CommandLine.arguments.firstIndex(of: "capture-linux-fixtures"),
   CommandLine.arguments.indices.contains(index + 1) {
    _ = NSApplication.shared
    do {
        try MainActor.assumeIsolated {
            try CaptureLinuxFixtures.export(to: URL(fileURLWithPath: CommandLine.arguments[index + 1]))
        }
        exit(0)
    } catch { fputs("Capture fixture export failed: \(error)\n", stderr); exit(1) }
}

if let index = CommandLine.arguments.firstIndex(of: "capsule-resources-preview"),
   CommandLine.arguments.indices.contains(index + 1) {
    _ = NSApplication.shared
    exit(MainActor.assumeIsolated {
        CapsuleReferenceLinuxFixtures.renderManager(to: CommandLine.arguments[index + 1])
    } ? 0 : 1)
}

if let index = CommandLine.arguments.firstIndex(of: "capsule-reference-linux-fixtures"),
   CommandLine.arguments.indices.contains(index + 1) {
    do {
        try CapsuleReferenceLinuxFixtures.export(to: CommandLine.arguments[index + 1])
        exit(0)
    } catch { fputs("Capsule reference fixture export failed\n", stderr); exit(1) }
}

if let index = CommandLine.arguments.firstIndex(of: "clipboard-linux-fixtures"),
   CommandLine.arguments.indices.contains(index + 1) {
    _ = NSApplication.shared
    exit(MainActor.assumeIsolated {
        ClipboardHistorySmoke.exportLinuxParityFixtures(to: CommandLine.arguments[index + 1])
    } ? 0 : 1)
}
if CommandLine.arguments.contains("clipboard-history-smoke") {
    _ = NSApplication.shared
    exit(MainActor.assumeIsolated { ClipboardHistorySmoke.run() } ? 0 : 1)
}
if let previewIndex = CommandLine.arguments.firstIndex(of: "clipboard-window-preview"),
   CommandLine.arguments.indices.contains(previewIndex + 1) {
    _ = NSApplication.shared
    let path = CommandLine.arguments[previewIndex + 1]
    exit(MainActor.assumeIsolated {
        ClipboardHistorySmoke.renderPreview(to: path)
    } ? 0 : 1)
}
if let previewIndex = CommandLine.arguments.firstIndex(
    of: "mailbox-new-conversation-preview"
), CommandLine.arguments.indices.contains(previewIndex + 1) {
    _ = NSApplication.shared
    let path = CommandLine.arguments[previewIndex + 1]
    exit(MailboxPaneVisualSmoke.renderNewConversationPreview(to: path) ? 0 : 1)
}
if CommandLine.arguments.contains("global-hotkey-registration-smoke") {
    _ = NSApplication.shared
    let suite = "RimeBuffer.GlobalHotKeyRegistrationSmoke.\(UUID().uuidString)"
    guard let defaults = UserDefaults(suiteName: suite) else {
        print("FAILED: could not create isolated global hotkey defaults")
        exit(1)
    }
    let definitions = GlobalHotKeyRouting.definitions(defaults: defaults)
    let installed = GlobalHotKeyController.shared.install(defaults: defaults)
    if installed {
        let summary = definitions
            .map {
                let exclusive = $0.registrationOptions
                    == OptionBits(kEventHotKeyExclusive)
                return "\($0.action)=\($0.keyCode)/\($0.modifiers)"
                    + "/exclusive:\(exclusive)"
            }
            .joined(separator: " ")
        print("global hotkey registration smoke: OK \(summary)")
    }
    defaults.removePersistentDomain(forName: suite)
    exit(installed ? 0 : 1)
}
if CommandLine.arguments.contains("scheme-import-smoke") {
    exit(MacRimeSchemeLibrary.importSmoke(arguments: CommandLine.arguments) ? 0 : 1)
}
if CommandLine.arguments.contains("external-schema-engine-smoke") {
    exit(runExternalSchemaEngineSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("schema-smoke") {
    exit(runSchemaListStoreSmokeTest() && runExternalSchemaSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("marine-bridge-smoke") {
    exit(runMarineBridgeSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("marine-chrome-smoke") {
    exit(runMarineChromeSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("plugin-stream-smoke") {
    exit(runActionPluginStreamSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("plugin-smoke") {
    exit(runActionPluginSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("plugin-platform-smoke") {
    exit(runPluginPlatformSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("plugin-distribution-smoke") {
    exit(runPluginDistributionSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("plugin-configuration-smoke") {
    exit(runPluginConfigurationSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("translation-smoke") {
    exit(runTranslationPluginSmokeTest()
        && runTranslationLifecycleSmokeTest()
        && runBufferSpeechSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("ai-text-smoke") {
    exit(runAITextPluginSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("codex-image-live-smoke") {
    let passed: Bool = {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()
    let registry = AITextConnectorRegistry.shared
    let availabilityDeadline = Date().addingTimeInterval(45)
    while Date() < availabilityDeadline,
          registry.availability(for: .codexCLI) != .ready {
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
    }
    guard registry.availability(for: .codexCLI) == .ready else {
        fputs("codex image live smoke: connector unavailable\n", stderr)
        return false
    }
    let smokeRoot = FileManager.default.temporaryDirectory
        .appendingPathComponent("rimes-codex-image-live-\(UUID().uuidString)",
                                isDirectory: true)
    defer { try? FileManager.default.removeItem(at: smokeRoot) }
    let smokeStore: MailboxStore
    do {
        smokeStore = try MailboxStore(storageRoot: smokeRoot)
    } catch {
        fputs("codex image live smoke: Mailbox unavailable\n", stderr)
        return false
    }
    let coordinator = CodexImageGenerationCoordinator(store: smokeStore)
    let threadID: UUID
    do {
        threadID = try coordinator.start(
            prompt: "一颗简单的红色圆点，纯白背景，极简测试图。",
            modelID: "gpt-6-sol", reasoningEffort: "medium"
        )
    } catch {
        fputs("codex image live smoke: could not start \(error)\n", stderr)
        return false
    }
    let generationDeadline = Date().addingTimeInterval(660)
    while Date() < generationDeadline {
        switch coordinator.phase {
        case let .ready(completedID):
            guard completedID == threadID,
                  let message = smokeStore.thread(id: threadID)?
                    .messages.last,
                  smokeStore.imageData(for: message) != nil else {
                fputs("codex image live smoke: missing Mailbox image\n", stderr)
                return false
            }
            print("codex image live smoke: OK mailbox-thread=\(threadID)")
            return true
        case let .failed(message):
            fputs("codex image live smoke: \(message)\n", stderr)
            return false
        case .idle, .connecting, .generating, .saving:
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        }
    }
    fputs("codex image live smoke: timeout\n", stderr)
    return false
    }()
    exit(passed ? 0 : 1)
}
if CommandLine.arguments.contains("scholay-plugin-smoke") {
    exit(runScholayPluginSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("scholay-academic-smoke") {
    exit(runScholayAcademicSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("provider-profiles-smoke") {
    exit(runAIProviderProfilesSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("ai-text-terminal-receipt-smoke") {
    exit(runAITextTerminalDeliveryReceiptSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("ai-text-mailbox-smoke") {
    exit(runAITextMailboxGenerationSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("mailbox-store-smoke") {
    exit(runMailboxStoreSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("mailbox-window-smoke") {
    exit(runMailboxWindowSmokeTest() ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "mailbox-workspace-smoke") {
    let output = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(runMailboxWorkspaceSmoke(output: output) ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "capture-permission-preview-smoke"), CommandLine.arguments.indices.contains(index + 1) {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory); app.finishLaunching()
    do {
        try MainActor.assumeIsolated {
            let guide = CapturePermissionGuide(allowsSystemActions: false)
            try guide.renderForSmoke(to: URL(fileURLWithPath: CommandLine.arguments[index + 1]))
            withExtendedLifetime(guide) {}
        }
        exit(0)
    } catch { print("capture-permission-preview-smoke: FAILED \(error.localizedDescription)"); exit(1) }
}
if CommandLine.arguments.contains("capture-permission-smoke") {
    _ = NSApplication.shared
    Task { @MainActor in
        do { try await CapturePermissionSmoke.run(); exit(0) }
        catch { print("capture-permission-smoke: FAILED \(error.localizedDescription)"); exit(1) }
    }
    NSApplication.shared.run()
    exit(1)
}
if let index = CommandLine.arguments.firstIndex(of: "capture-selection-smoke") {
    _ = NSApplication.shared
    do {
        let preview = CommandLine.arguments.indices.contains(index + 1) ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
        try MainActor.assumeIsolated { try CaptureSelectionSmoke.run(previewDirectory: preview) }
        exit(0)
    } catch { print("capture-selection-smoke: FAILED \(error.localizedDescription)"); exit(1) }
}
if let index = CommandLine.arguments.firstIndex(of: "capture-chrome-smoke"), CommandLine.arguments.indices.contains(index + 1) {
    _ = NSApplication.shared
    do {
        try MainActor.assumeIsolated { try CaptureChromeSmoke.run(output: URL(fileURLWithPath: CommandLine.arguments[index + 1])) }
        exit(0)
    } catch { print("capture-chrome-smoke: FAILED \(error.localizedDescription)"); exit(1) }
}
if CommandLine.arguments.contains("capture-smoke") {
    _ = NSApplication.shared
    exit(CaptureSmoke.run() ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "window-chrome-smoke") {
    _ = NSApplication.shared
    do {
        let output = CommandLine.arguments.indices.contains(index + 1)
            ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
        try MainActor.assumeIsolated { try WindowChromeSmoke.run(output: output) }
        exit(0)
    } catch { print("window-chrome-smoke: FAILED \(error.localizedDescription)"); exit(1) }
}
if let index = CommandLine.arguments.firstIndex(of: "capture-media-smoke"), CommandLine.arguments.count > index + 1 {
    Task { exit(await CaptureSmoke.media(CommandLine.arguments[index + 1]) ? 0 : 1) }
    NSApplication.shared.run()
    exit(1)
}
if let index = CommandLine.arguments.firstIndex(of: "capture-preview"), CommandLine.arguments.indices.contains(index + 1) {
    _ = NSApplication.shared
    exit(MainActor.assumeIsolated { CaptureSmoke.preview(CommandLine.arguments[index + 1]) } ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "capture-live-smoke") {
    let app = NSApplication.shared
    let seconds = CommandLine.arguments.indices.contains(index + 1) ? Int(CommandLine.arguments[index + 1]) ?? 5 : 5
    Task { @MainActor in exit(await CaptureSmoke.live(seconds: seconds) ? 0 : 1) }
    app.run()
    exit(1)
}
if CommandLine.arguments.contains("capsule-window-smoke") {
    exit(runCapsuleWindowSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("capsule-module-smoke") {
    _ = NSApplication.shared
    exit(runCapsuleModuleSmokeTest() ? 0 : 1)
}
if let previewIndex = CommandLine.arguments.firstIndex(of: "capsule-manager-preview"),
   CommandLine.arguments.indices.contains(previewIndex + 1) {
    _ = NSApplication.shared
    let path = CommandLine.arguments[previewIndex + 1]
    exit(MainActor.assumeIsolated {
        CapsuleRailSmoke.renderManagerPreview(to: path)
    } ? 0 : 1)
}
if let previewIndex = CommandLine.arguments.firstIndex(of: "capsule-rail-preview"),
   CommandLine.arguments.indices.contains(previewIndex + 2) {
    _ = NSApplication.shared
    let path = CommandLine.arguments[previewIndex + 1]
    let tab = CommandLine.arguments[previewIndex + 2]
    exit(MainActor.assumeIsolated {
        CapsuleRailSmoke.renderPreview(to: path, tabName: tab)
    } ? 0 : 1)
}
if CommandLine.arguments.contains("capsule-rail-smoke") {
    _ = NSApplication.shared
    exit(MainActor.assumeIsolated { CapsuleRailSmoke.run() } ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "capsule-interaction-smoke") {
    _ = NSApplication.shared
    let output = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(MainActor.assumeIsolated { CapsuleInteractionSmoke.run(output: output) } ? 0 : 1)
}
if CommandLine.arguments.contains("capsule-sync-smoke") {
    exit(runCapsuleCloudSyncSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("mailbox-toast-smoke") {
    exit(runMailboxToastSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("stream-input-smoke") {
    exit(runStreamInputPluginSmokeTest() ? 0 : 1)
}
// Reports how much of a real aggregator catalog this build can actually
// route. Point it at a saved /api/models response to see recovery coverage
// before trusting the request pane against a live account.
if CommandLine.arguments.contains("aggregator-catalog"),
   let pathIndex = CommandLine.arguments.firstIndex(of: "aggregator-catalog"),
   CommandLine.arguments.count > pathIndex + 1 {
    let path = CommandLine.arguments[pathIndex + 1]
    guard let payload = try? Data(contentsOf: URL(fileURLWithPath: path)),
          let models = try? AggregatorCatalogParser.models(from: payload) else {
        print("could not read catalog at \(path)")
        exit(1)
    }
    let usable = models.filter(\.isUsable)
    var byAdapter: [AggregatorAdapter: Int] = [:]
    for model in usable {
        byAdapter[model.primaryAdapter, default: 0] += 1
    }
    print("models: \(models.count)  routable: \(usable.count)  "
        + "unroutable: \(models.count - usable.count)")
    for (adapter, count) in byAdapter.sorted(by: { $0.value > $1.value }) {
        print(String(format: "  %-22s %d", (adapter.rawValue as NSString).utf8String!, count))
    }
    let stranded = models.filter { !$0.isUsable }.map(\.id).prefix(8)
    if !stranded.isEmpty { print("unroutable sample: \(stranded.joined(separator: ", "))") }
    exit(0)
}
// Replays a real rollout through the same parser the pane uses, so the item
// vocabulary can be checked against sessions codex actually wrote.
if CommandLine.arguments.contains("codex-rollout"),
   let index = CommandLine.arguments.firstIndex(of: "codex-rollout"),
   CommandLine.arguments.count > index + 1 {
    let url = URL(fileURLWithPath: CommandLine.arguments[index + 1])
    guard var payload = try? Data(contentsOf: url) else {
        print("could not read rollout at \(url.path)")
        exit(1)
    }
    if let newline = payload.firstIndex(of: 0x0a),
       let header = CodexRolloutParser.header(
        from: payload[payload.startIndex..<newline]
       ) {
        print("session \(header.sessionID)  cwd=\(header.cwd ?? "?")  "
            + "cli=\(header.cliVersion ?? "?")")
    }
    let events = CodexRolloutWatcher.drain(&payload)
    var counts: [String: Int] = [:]
    var translatable = 0
    for event in events {
        counts[event.item.kindLabel, default: 0] += 1
        if CodexEventTranslationRules.isTranslatable(event.item) { translatable += 1 }
    }
    print("events: \(events.count)   translatable: \(translatable)   "
        + "verbatim: \(events.count - translatable)")
    for (kind, count) in counts.sorted(by: { $0.value > $1.value }) {
        print("  \(kind): \(count)")
    }
    exit(0)
}
// Reports what the session pane would launch and where it would read from,
// without opening a window. A GUI-launched agent does not inherit a shell
// PATH, so "it works in my terminal" proves nothing about what the pane sees.
if CommandLine.arguments.contains("codex-doctor") {
    if let executable = CodexExecutableLocator.resolve() {
        print("codex: \(executable.path)")
        let probe = Process()
        probe.executableURL = executable
        probe.arguments = ["--version"]
        // Probe through the same environment the pane spawns with, or this
        // reports the bug the environment exists to fix.
        let home = FileManager.default.homeDirectoryForCurrentUser
        var childEnvironment: [String: String] = [:]
        for entry in CodexProcessEnvironment.variables(
            base: [],
            inheritedPath: ProcessInfo.processInfo.environment["PATH"],
            executable: executable,
            workspace: home,
            homeDirectory: home,
            shell: ProcessInfo.processInfo.environment["SHELL"]
        ) {
            guard let separator = entry.firstIndex(of: "=") else { continue }
            childEnvironment[String(entry[entry.startIndex..<separator])] =
                String(entry[entry.index(after: separator)...])
        }
        probe.environment = childEnvironment
        let pipe = Pipe()
        probe.standardOutput = pipe
        probe.standardError = pipe
        if (try? probe.run()) != nil {
            probe.waitUntilExit()
            let output = String(
                decoding: pipe.fileHandleForReading.readDataToEndOfFile(),
                as: UTF8.self
            ).trimmingCharacters(in: .whitespacesAndNewlines)
            print("version: \(output)  exit=\(probe.terminationStatus)")
        }
    } else {
        print("codex: NOT FOUND (checked ~/.codex/bin, /opt/homebrew/bin, "
            + "/usr/local/bin, then PATH)")
    }
    let root = CodexRolloutWatcher.sessionsRoot
    let exists = FileManager.default.fileExists(atPath: root.path)
    print("sessions root: \(root.path) \(exists ? "(present)" : "(missing)")")
    print("workspace default: \(CodexSessionWorkspaceRules.preferredWorkspace().path)")
    if #available(macOS 15.0, *) {
        print("translation: AppleTranslationStringService available")
    } else {
        print("translation: unavailable below macOS 15")
    }
    // Run from inside the bundle this reports on the app's own identity, which
    // is the only way to see whether a recorded grant is actually in effect —
    // System Settings shows the checkbox either way.
    let identity = SystemPermissionAudit.identity()
    print("identity: \(identity.bundleIdentifier)")
    print("signing: \(SystemPermissionAudit.signingAuthority() ?? "ad-hoc (grants die on rebuild)")")
    for report in SystemPermissionAudit.reportAll() {
        print("permission \(report.permission.rawValue): \(report.status)")
    }
    exit(0)
}
if CommandLine.arguments.contains("codex-session-smoke") {
    exit(runCodexSessionSmokeTest() ? 0 : 1)
}
// Runs the real migration against explicit paths, so it can be proven on the
// actual data directory before a rename depends on it.
if let i = CommandLine.arguments.firstIndex(of: "rimes-migrate-probe"),
   i + 2 < CommandLine.arguments.count {
    let legacy = URL(fileURLWithPath: CommandLine.arguments[i + 1],
                     isDirectory: true)
    let destination = URL(fileURLWithPath: CommandLine.arguments[i + 2],
                          isDirectory: true)
    let outcome = RimesDataMigration.migrateIfNeeded(from: legacy,
                                                     to: destination)
    print("migration: \(outcome)")
    switch outcome {
    case .copied, .alreadyMigrated, .notNeeded: exit(0)
    case .destinationOccupied, .failed: exit(1)
    }
}
if CommandLine.arguments.contains("presentation-mode-smoke") {
    exit(runBufferPresentationModeSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("live-metrics-smoke") {
    exit(runBufferLiveTypingMetricsSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("rimes-migration-smoke") {
    exit(runRimesMigrationSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("clipboard-policy-smoke") {
    exit(runClipboardActivationPolicySmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("buffer-capture-smoke") {
    exit(runBufferCaptureRequestSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("aggregator-smoke") {
    exit(runAggregatorSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("my-prompt-smoke") {
    exit(runMyPromptPluginSmokeTest() ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "morse-smoke") {
    _ = NSApplication.shared
    let output = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(MainActor.assumeIsolated { runMorseCodeSmokeTest(output: output) } ? 0 : 1)
}
if CommandLine.arguments.contains("capsule-structure-smoke") {
    _ = NSApplication.shared
    exit(runCapsuleStructureSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("capsule-smoke")
    || CommandLine.arguments.contains("capsule-password-smoke") {
    exit(runCapsulePasswordSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("remarkable-plugin-smoke") {
    exit(runRemarkablePluginSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("settings-routing-smoke") {
    exit(runSettingsRoutingSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("status-menu-smoke") {
    exit(runStatusMenuSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("status-menu-preview-smoke") {
    exit(runStatusMaintenanceMenuPreview() ? 0 : 1)
}
if CommandLine.arguments.contains("history-heatmap-smoke") {
    exit(runHistoryHeatmapSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("typing-speed-smoke") {
    exit(runTypingSpeedStoreSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("daily-metrics-migration-smoke") {
    exit(runDailyMetricsMigrationSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("typing-test-smoke") {
    exit(runTypingTestSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("typing-practice-telemetry-smoke") {
    exit(runTypingPracticeTelemetrySmokeTest() ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "statistics-dashboard-smoke") {
    let output = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(runStatisticsDashboardSmokeTest(outputURL: output) ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "typing-test-ui-smoke") {
    let output = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1], isDirectory: true) : nil
    exit(runTypingTestUISmokeTest(outputDirectory: output) ? 0 : 1)
}
if CommandLine.arguments.contains("fly-chord-learning-smoke") {
    exit(runFlyChordLearningSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("chord-keymap-smoke") {
    exit(runChordKeymapSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("chord-keymap-engine-smoke") {
    exit(runChordKeymapEngineSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("native-chord-smoke") {
    exit(runNativeChordSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("yoyo-engine-smoke") {
    exit(runYoyoEngineSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("chord-ziranma-smoke") {
    exit(runChordZiranmaSmokeTest() ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "chord-ziranma-engine-smoke") {
    let report = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1], isDirectory: true) : nil
    exit(runChordZiranmaEngineSmokeTest(reportDirectory: report) ? 0 : 1)
}
if CommandLine.arguments.contains("chord-keymap-activation-smoke") {
    exit(runChordKeymapActivationSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("chord-unification-smoke") {
    exit(runChordUnificationSmokeTest() ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "chord-settings-smoke") {
    let output = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(runChordSettingsSmokeTest(outputURL: output) ? 0 : 1)
}
if let index = CommandLine.arguments.firstIndex(of: "chord-keymap-ui-smoke") {
    let output = CommandLine.arguments.indices.contains(index + 1)
        ? URL(fileURLWithPath: CommandLine.arguments[index + 1]) : nil
    exit(runChordKeymapEditorSmokeTest(outputURL: output) ? 0 : 1)
}
if CommandLine.arguments.contains("input-telemetry-smoke") {
    exit(runInputTelemetrySmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("user-lexicon-smoke") {
    exit(runUserLexiconServiceSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("user-lexicon-bridge-smoke") {
    exit(runRimeUserLexiconBridgeSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("matrix-smoke") {
    exit(runCandidateMatrixSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("origin-smoke") {
    exit(runOriginSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("inbound-smoke") {
    exit(runInboundBusSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("candidate-metrics-smoke") {
    exit(runCandidateMetricsSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("candidate-rendering-smoke") {
    let candidatesPassed = CandidateWindow.renderingPerformanceSmoke()
    let hiddenBufferPassed = BufferWindowController.hiddenFocusRefreshSmoke()
    exit(candidatesPassed && hiddenBufferPassed ? 0 : 1)
}
if CommandLine.arguments.contains("candidate-pagination-smoke") {
    exit(runCandidatePaginationSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("candidate-commit-smoke") {
    _ = NSApplication.shared
    exit(runCandidateCommitSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("activation-cache-smoke") {
    exit(runRimeActivationMetadataCacheSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("input-repair-smoke") {
    exit(runInputRepairSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("shift-punctuation-smoke") {
    exit(runShiftPunctuationEngineSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("theme-smoke") {
    exit(runThemeSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("theme-appkit-smoke") {
    exit(runThemeAppKitSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("input-source-install-smoke") {
    exit(runInputSourceInstallSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("update-package-metadata-smoke") {
    exit(runUpdatePackageMetadataSmokeTest() ? 0 : 1)
}
if CommandLine.arguments.contains("smoke") {
    exit(runEngineSmokeTest() ? 0 : 1)
}

// Self-install into the input-source list. TIS enable/select only persist when
// run INSIDE the login (Aqua) session — a detached CLI's writes can return
// success without updating that user's roster. The installer/build script runs
// this command through the user's launchd bootstrap. It then reconciles each
// parent/child/select boundary in a fresh subprocess before IMK startup.
if CommandLine.arguments.contains("--install") {
    exit(installInputSource() ? 0 : 1)
}
if CommandLine.arguments.contains("--repair-pending-install") {
    exit(repairPendingInputSourceActivation() ? 0 : 75)
}
if CommandLine.arguments.contains("--prepare-update") {
    exit(selectFallbackInputSourceForUpdate() ? 0 : 1)
}
// A misspelled smoke command must not fall through into a second IMK server.
if CommandLine.arguments.dropFirst().contains(where: { $0.hasSuffix("-smoke") }) {
    FileHandle.standardError.write(Data("unknown smoke command\n".utf8))
    exit(2)
}

// A standalone SettingsWindowController spins up its OWN RimeEngine. If that
// engine opened the live ~/Library/RIMES userdb while the installed IME is
// running, the two librime instances would fight over the LevelDB lock and break
// the user's live typing. So the dev GUI tools redirect to an isolated userdb
// (unless the caller already pinned RIMEBUFFER_USER_DIR).
func isolatePreviewUserDir() {
    guard let dir = isolateStandaloneRimeUserDir() else { return }
    let environment = ProcessInfo.processInfo.environment
    if environment["RIMEBUFFER_LOCAL_DATA_ROOT"] == nil {
        setenv("RIMEBUFFER_LOCAL_DATA_ROOT", dir, 1)
    }
    if environment["RIMEBUFFER_PLUGIN_ROOT"] == nil {
        setenv("RIMEBUFFER_PLUGIN_ROOT", dir + "/plugins", 1)
    }
}

// Dev-only: show the Settings window standalone (no IMK) so it can be reviewed
// or screenshotted without a live input session. Not wired into the shipped
// menus; invoked manually as `ETInput settings-preview`.
if CommandLine.arguments.contains("settings-preview") {
    isolatePreviewUserDir()
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    SettingsWindowController.shared.show()
    app.activate(ignoringOtherApps: true)
    app.run()
}
if let i = CommandLine.arguments.firstIndex(of: "gateway-settings-render"), i + 1 < CommandLine.arguments.count {
    isolatePreviewUserDir()
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()
    exit(SettingsWindowController.shared.renderGatewayForPreview(to: CommandLine.arguments[i + 1]) ? 0 : 1)
}
// Dev-only: render every stable route/subpage plus manifest.json.
if let i = CommandLine.arguments.firstIndex(of: "settings-render"),
   i + 1 < CommandLine.arguments.count {
    isolatePreviewUserDir()
    let dir = CommandLine.arguments[i + 1]
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()
    let rendered = SettingsWindowController.shared.renderAllForPreview(to: dir)
    print(rendered
        ? "rendered settings routes to \(dir)"
        : "failed to render one or more settings routes")
    exit(rendered ? 0 : 1)
}
// Dev-only: `ETInput panel-render <path>
// [translation|marine|candidate|linked|toolbar] [hover=<control>]
// [width=<points>] [long] [dark]` renders the actual compact workbench.
if let i = CommandLine.arguments.firstIndex(of: "popup-menu-render"),
   i + 1 < CommandLine.arguments.count {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()
    let rows: [BufferPopUpMenuRow] = [
        BufferPopUpMenuRow(itemIndex: 0, title: "Codex CLI", isSeparator: false,
                           isEnabled: true, isSelected: true),
        BufferPopUpMenuRow(itemIndex: 1, title: "Claude Code CLI",
                           isSeparator: false, isEnabled: true,
                           isSelected: false),
        BufferPopUpMenuRow(itemIndex: 2, title: "OpenAI 兼容 API",
                           isSeparator: false, isEnabled: true,
                           isSelected: false),
        .separator(itemIndex: 3),
        BufferPopUpMenuRow(itemIndex: 4, title: "未配置连接器",
                           isSeparator: false, isEnabled: false,
                           isSelected: false),
    ]
    // Auto-send no longer has a pull-down to preview here: the cycle button
    // and its adjacent switch render through the ordinary `panel-render`
    // workbench preview instead.
    let ok = renderBufferPopUpMenuPreview(
        to: CommandLine.arguments[i + 1],
        rows: rows
    )
    print(ok ? "rendered pull-down menu preview" : "FAILED: menu preview")
    exit(ok ? 0 : 1)
}

if let i = CommandLine.arguments.firstIndex(of: "panel-render"),
   i + 1 < CommandLine.arguments.count {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()
    let model = BufferModel.shared
    model.onChange = nil
    model.discardForPrivacy()
    model.stageExternal("这次", origin: .rime)
    model.append("做了", origin: .mcp(client: "preview"))
    model.append("缓冲工作台", origin: .remotePeer(deviceID: "preview"))
    let options = Array(CommandLine.arguments.dropFirst(i + 2))
    if options.contains("dark") { app.appearance = NSAppearance(named: .darkAqua) }
    let translation = options.contains("translation")
    let marine = options.contains("marine")
    let candidatePreview = options.contains("candidate")
    let linkedPreview = options.contains("linked")
    let toolbarExpanded = options.contains("toolbar")
    if marine { model.discardForPrivacy() }
    let scaleValue = options.first { Double($0) != nil }.flatMap { Double($0) }
    let scale = CGFloat(scaleValue ?? 2)
    let panelWidth = options.first { $0.hasPrefix("width=") }
        .flatMap { Double($0.dropFirst("width=".count)) }
        .map { CGFloat($0) } ?? 760
    let hoveredControl = options.compactMap { option -> BufferWorkbenchControl? in
        guard option.hasPrefix("hover=") else { return nil }
        return BufferWorkbenchControl(rawValue: String(option.dropFirst("hover=".count)))
    }.first
    let translationSnapshot: TranslationRailSnapshot?
    if marine {
        translationSnapshot = TranslationRailSnapshot(
            sourceText: "",
            showsSourceRail: false,
            outputBlocks: [],
            phase: .idle,
            targetRole: "答",
            targetEmptyText: "等待网页上下文"
        )
    } else if translation {
        let long = options.contains("long")
        translationSnapshot = TranslationRailSnapshot(
            sourceText: String(repeating: "今天终于把翻译缓冲区分成上下两个区域了。",
                               count: long ? 5 : 1),
            outputBlocks: [
                TranslationOutputBlock(id: UUID(),
                                       text: "Today the translation buffer is finally split"),
                TranslationOutputBlock(id: UUID(),
                                       text: String(repeating: "into two vertically stacked areas. ",
                                                    count: long ? 8 : 1)),
            ],
            phase: .ready
        )
    } else {
        translationSnapshot = nil
    }
    let previewStatuses: [WorkbenchStatusIndicator]? = marine
        ? MarineChromeStatusSnapshot(
            paired: true,
            contextOnline: false,
            platform: nil,
            sourceKind: nil,
            aiAvailability: .ready
        ).indicators
        : nil
    let rendered = BufferWindowController.shared.renderForPreview(
        to: CommandLine.arguments[i + 1],
        scale: scale,
        panelWidth: panelWidth,
        translationSnapshot: translationSnapshot,
        statusIndicators: previewStatuses,
        hoveredControl: hoveredControl,
        candidatePreview: candidatePreview,
        targetAssociationPreviewAppName: linkedPreview ? "Safari" : nil,
        toolbarExpanded: toolbarExpanded,
        liveMetricsPreview: options.contains("metrics")
            ? "42 字/分  ·  码长 1.83  ·  击键 3.2/秒  ·  回删 2"
            : nil,
        railFoldedPreview: options.contains("folded")
    )
    print(rendered
        ? "rendered \(candidatePreview ? "candidate " : (marine ? "marine " : (translation ? "translation " : "")))expanded workbench @\(scale)x"
        : "failed to render workbench")
    exit(rendered ? 0 : 1)
}
// Dev-only: `ETInput gateway-serve` runs the local gateway + a runloop and prints
// each inbound event, so the productized HTTP/MCP path can be exercised by curl
// and the real `claude` client without the full IMK bootstrap.
if CommandLine.arguments.contains("gateway-serve") {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()
    InboundBus.shared.onChange = {
        let p = InboundBus.shared.pending
        print("[inbound] pending=\(p.count) latest=\(p.last.map { "\($0.origin.tag):\($0.text.count)chars streaming=\($0.streaming)" } ?? "-")")
        fflush(stdout)
    }
    installMarineChromeGatewayAvailabilityObserver(
        registry: PluginRegistry.shared
    )
    installMarineChromePairingApprovalHandler()
    LocalGateway.shared.start()
    print("gateway-serve on 127.0.0.1:\(LocalGateway.shared.port) token=\(GatewayToken.current())")
    fflush(stdout)
    app.run()
}

let startupRimeUserDir = ProcessInfo.processInfo.environment["RIMEBUFFER_USER_DIR"]
    .flatMap { $0.isEmpty ? nil : $0 }
    .map { URL(fileURLWithPath: $0, isDirectory: true) }
    ?? URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
        .appendingPathComponent("Library/\(RimesPaths.directoryName)", isDirectory: true)
InputSchemaCatalog.prepareStartup(in: startupRimeUserDir)
retryPendingInputSourceActivationIfNeeded()

// IMK bootstrap. The connection name MUST match Info.plist; IMK finds our
// controller via InputMethodServerControllerClass = RIMESController.
// Start enabled built-in observers before the first controller can receive a
// key. The registry is discovery/enablement only; external Action Plugin
// execution and revocation remain owned by ActionPluginHost/Manager.
let pluginRegistry = PluginRegistry.shared
BufferPluginSelectionStore.shared.migrateDefaultIfNeeded(
    from: pluginRegistry.plugins(capability: .bufferAction)
)
let bufferPluginSelectionObserver = NotificationCenter.default.addObserver(
    forName: .activeBufferPluginDidChange,
    object: BufferPluginSelectionStore.shared,
    queue: .main
) { notification in
    BufferWindowController.shared.notePluginSelectionChanged()
    BufferModel.shared.clearAllContentSelection()
    if RimeInputSourceAuthority.currentSourceIsOwn(),
       notification.userInfo?["current"] as? PluginKey
        == StreamInputWorkspace.pluginKey {
        if let target = InputFocusCoordinator.shared.liveTarget(
            forceOverlayVisibilityRefresh: true
           ), BufferModel.shared.capturesInput(for: target.token) {
            // A pre-existing Rime composition belongs to the old workspace.
            // Settle it before the first stream key can be captured; the two
            // source models must never coexist as one invisible composition.
            target.controller?.resolveCompositionForWorkbenchTransition(
                target: target
            )
        }
    }
    ActionPluginHost.shared.bufferPluginSelectionDidChange()
    BufferWindowController.shared.refresh()
}
IMELog.reset("=== \(ProductIdentity.displayName) IME launch ===")
// Keep the preserved user profile's F4 catalog aligned with the product
// catalog before librime opens it. This is intentionally a narrow rewrite of
// `patch.schema_list`: SchemaListStore preserves every unrelated custom key,
// while the optional chord schema follows the extension's authoritative state.
let startupSchemaListURL = startupRimeUserDir.appendingPathComponent(
    "default.custom.yaml"
)
if MacRimeSchemeLibrary.shared.runtimePackage == nil {
// Recreate only the active custom schema from the durable snapshot. Updating
// the app or reseeding Rime data must never overwrite the user's keymap source.
let startupChordProfile = ChordKeymapStore.shared.activeProfile
if let error = ChordKeymapStore.shared.loadError {
    ChordKeymapActivationCoordinator.shared.suspendChordAfterFailure(
        "已应用键位方案无法读取，已暂停并击；文件未被覆盖。\(error.localizedDescription)"
    )
}
do {
    try ChordKeymapRuntimeFiles(root: startupRimeUserDir).writeSchema(
        for: startupChordProfile
    )
} catch {
    IMELog.write("startup: custom chord schema preparation failed \(error.localizedDescription)")
    ChordKeymapActivationCoordinator.shared.suspendChordAfterFailure(
        "无法恢复已应用的键位方案，已暂停并击并退回普通输入。请重新启用扩展重试。"
    )
}
let desiredStartupSchemaIDs = InputSchemaCatalog.enabledIDs(
    chordExtensionEnabled: ChordExtensionStore.shared.isEnabled,
    userDirectory: startupRimeUserDir
)
if SchemaListStore.enabledIDs(at: startupSchemaListURL)
    != desiredStartupSchemaIDs {
    do {
        try SchemaListStore.writeEnabledIDs(
            desiredStartupSchemaIDs,
            to: startupSchemaListURL
        )
        IMELog.write(
            "startup: F4 schemas reconciled -> "
                + desiredStartupSchemaIDs.joined(separator: ",")
        )
    } catch {
        // Continue booting with the last compiled deployment. Settings keeps a
        // transactional retry path; startup must not turn a writable-profile
        // issue into a selectable-but-dead input source.
        IMELog.write(
            "startup: F4 schema reconciliation failed "
                + error.localizedDescription
        )
    }
}
}
let connectionName = (Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String)
    ?? "\(Bundle.main.bundleIdentifier ?? RimesIdentity.bundleIdentifier)_Connection"

NSApplication.shared.setActivationPolicy(.accessory)   // background app; panels can float above others
CapsuleCloudSyncController.shared.start()

// Held for the process lifetime so the IMK connection stays up.
let imkServer = IMKServer(name: connectionName, bundleIdentifier: Bundle.main.bundleIdentifier)
if imkServer == nil {
    // No connection = a selectable-but-dead input source. Exit loudly; the
    // text-input system respawns us on demand once the cause is fixed.
    IMELog.write("FATAL: IMKServer(name: \(connectionName)) returned nil — exiting")
    exit(1)
}
// A Carbon hot key is process-global and remains independent from IMK's normal
// Command-key passthrough. Retain the controller for the entire server lifetime.
CaptureHotKey.shared.start()
let globalHotKeyController = GlobalHotKeyController.shared
_ = globalHotKeyController.setRuntimeEnabledForInputSource(
    RimeInputSourceAuthority.currentSourceIsOwn()
)
// Warm the engine so the first keystroke isn't slow / so failures surface early.
_ = rimeEngine.start()
if MacRimeSchemeLibrary.shared.runtimePackage == nil, ChordExtensionStore.shared.isEnabled,
   !ChordKeymapStore.shared.activeProfile.isBuiltIn {
    if ChordKeymapActivationCoordinator.verify(ChordKeymapStore.shared.activeProfile, engine: rimeEngine) {
        ChordKeymapActivationCoordinator.shared.clearRecoveryMessage()
    } else {
        ChordKeymapActivationCoordinator.shared.suspendChordAfterFailure(
            "已应用键位与 Rime 部署不一致，已暂停并击并退回普通输入。请重新启用扩展重试。"
        )
        try? SchemaListStore.writeEnabledIDs(InputSchemaCatalog.defaultEnabledIDs,
                                            to: startupSchemaListURL)
        _ = BBRimeDeploy()
        rimeEngine.invalidateSchemaListCacheAfterDeployment()
    }
}

// Mouse-selection routes to whichever controller currently owns focus — the
// shared window must never bind to one specific controller.
candidateWindow.onSelect = { owner, selection in
    InputFocusCoordinator.shared.controller(for: owner)?
        .selectCandidate(selection, owner: owner)
}
candidateWindow.onPage = { owner, delta in
    InputFocusCoordinator.shared.controller(for: owner)?
        .pageCandidates(delta: delta, owner: owner)
}
// Buffer presentation is independent from the caret-owned candidate panel.
var lastBufferControlsActive = BufferModel.shared.active
BufferModel.shared.onChange = {
    BufferWindowController.shared.refresh()
    let active = BufferModel.shared.active
    let shouldRefreshHostGuard = HostMarkedTextPresentationRules
        .shouldRefreshForActiveChange(previous: lastBufferControlsActive,
                                      current: active)
    lastBufferControlsActive = active
    if shouldRefreshHostGuard {
        // Model mutations can occur inside a commit/drain callback. Defer the
        // host-guard refresh to avoid re-entering getContext/setMarkedText.
        DispatchQueue.main.async {
            RIMESController.refreshActiveUI()
        }
    }
}
InputFocusCoordinator.shared.onChange = {
    ActionPluginHost.shared.focusDidChange()
    StreamInputWorkspace.shared.focusDidChange()
    BufferWindowController.shared.refreshForFocusChange()
    ClipboardHistoryWindowController.shared.focusDidChange()
}
InputFocusCoordinator.shared.onInvalidated = { owner in
    BufferModel.shared.clearAllContentSelection()
    candidateWindow.hide(owner: owner)
    BufferWindowController.shared.focusInvalidated(owner)
    ActionPluginHost.shared.focusInvalidated(owner)
    StreamInputWorkspace.shared.focusInvalidated(owner)
    ClipboardHistoryWindowController.shared.focusInvalidated(owner)
}
ActionPluginHost.shared.onChange = {
    BufferWindowController.shared.refresh()
}
BufferWindowController.shared.showOnLaunchIfNeeded()
ClipboardHistoryWindowController.shared.start()

// Local gateway: accept MCP / HTTP pushes from local agents into the inbound
// bus (loopback-only, token-gated). Off is a one-line setting.
MailboxInteractionBridge.shared.aiReplyCoordinator =
    AITextMailboxGenerationCoordinator.shared
_ = InboundToast.shared
InboundBus.shared.onChange = {
    MailboxWindowController.refreshIfOpen()
}
LocalGateway.shared.startIfEnabled()

// No standalone NSStatusItem: ETInput's commands are supplied by
// RIMESController.menu() under the system input-source icon.
StatusMenu.shared.setHealthy(rimeEngine.isHealthy)

// Auto-update: silently check GitHub Releases on launch + hourly, download in the
// background, and surface it through the input method's controls when ready.
UpdateManager.shared.startPeriodicUpdateCheck()

private enum InputSourceChangeDiagnosticRules {
    static func elapsedMilliseconds(previousUptime: TimeInterval?,
                                    now: TimeInterval) -> Int? {
        guard let previousUptime,
              previousUptime.isFinite,
              now.isFinite,
              now >= previousUptime else { return nil }
        let milliseconds = (now - previousUptime) * 1_000
        guard milliseconds <= Double(Int.max) else { return Int.max }
        return Int(milliseconds.rounded())
    }

    static func controlIsDown(_ modifierFlags: NSEvent.ModifierFlags) -> Bool {
        modifierFlags.contains(.control)
    }

}

// macOS 26 can omit deactivateServer when another process switches input
// sources through TISSelectInputSource. The distributed TIS notification still
// arrives, so finish the live controller before its client becomes stranded.
private func retireRimeInputSourceAuthority(observedSourceID: String?) {
    _ = globalHotKeyController.setRuntimeEnabledForInputSource(false)
    ClipboardHistoryWindowController.shared
        .inputSourceDidChangeAwayFromRIMES()
    let sourceDescription = observedSourceID ?? "unavailable"
    IMELog.write(
        "non-RIMES input source observed -> \(sourceDescription); "
            + "retiring IME authority and preserving utility windows"
    )
    if let lease = InputFocusCoordinator.shared.invalidateAll(
        reason: "input source changed"
    ) {
        // The newly selected input method already owns the client. Clean only
        // process-local Rime state and never call that retired IMK proxy.
        lease.controller?.finalizeProtectedSession(
            lease,
            reason: "input source changed"
        )
        candidateWindow.hide(owner: lease.token)
    } else {
        candidateWindow.hideAll()
    }
    if let captureToken = BufferModel.shared.captureFocusToken,
       !InputFocusCoordinator.shared.isCurrent(captureToken) {
        BufferModel.shared.routeDirectPreservingContent(
            reason: "input source changed"
        )
    }
    // `invalidateAll` and `routeDirectPreservingContent` can both be no-ops
    // when there is no current lease and Buffer was already direct. The
    // visible utility still has to enter detached clipboard presentation as
    // soon as another input source becomes authoritative.
    BufferWindowController.shared.refresh()
}

var lastObservedInputSourceID: String? = {
    guard let current = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue()
        else { return nil }
    return tisStringProperty(current, kTISPropertyInputSourceID)
}()
var lastObservedInputSourceUptime: TimeInterval? =
    ProcessInfo.processInfo.systemUptime
let inputSourceChangedObserver = DistributedNotificationCenter.default().addObserver(
    forName: Notification.Name(kTISNotifySelectedKeyboardInputSourceChanged as String),
    object: nil,
    queue: .main
) { _ in
    let now = ProcessInfo.processInfo.systemUptime
    let modifierFlags = NSEvent.modifierFlags
        .intersection(.deviceIndependentFlagsMask)
    let elapsed = InputSourceChangeDiagnosticRules.elapsedMilliseconds(
        previousUptime: lastObservedInputSourceUptime,
        now: now
    ).map(String.init) ?? "unknown"
    let previousInputSourceID = lastObservedInputSourceID
    let previousID = previousInputSourceID ?? "unknown"
    let controlDown = InputSourceChangeDiagnosticRules.controlIsDown(
        modifierFlags
    )
    guard let current = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue(),
          let currentID = tisStringProperty(current, kTISPropertyInputSourceID) else {
        IMELog.write("TIS source changed: previous=\(previousID) current=unavailable deltaMs=\(elapsed) controlDown=\(controlDown) modifiers=\(modifierFlags.rawValue)")
        lastObservedInputSourceID = nil
        lastObservedInputSourceUptime = now
        RIMESRecoveryStatusItem.shared.refresh(isSelected: false)
        // TIS can briefly return no current source during a handoff. Retire IMK
        // authority immediately; registration reconciliation keeps the four
        // standalone utility shortcuts and removes only RIMES-owned actions.
        retireRimeInputSourceAuthority(observedSourceID: nil)
        return
    }
    let frontmostID = NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? "unknown"
    IMELog.write("TIS source changed: previous=\(previousID) current=\(currentID) deltaMs=\(elapsed) controlDown=\(controlDown) modifiers=\(modifierFlags.rawValue) frontmost=\(frontmostID)")
    let ownID = Bundle.main.bundleIdentifier ?? RimesIdentity.bundleIdentifier
    let currentIsOwn = RimeInputSourceAuthority.isOwnInputSourceID(
        currentID,
        ownBundleID: ownID
    )
    lastObservedInputSourceID = currentID
    lastObservedInputSourceUptime = now
    if currentIsOwn {
        RIMESRecoveryStatusItem.shared.refresh(isSelected: true)
        _ = globalHotKeyController.setRuntimeEnabledForInputSource(true)
        // A detached Buffer can remain visible while no IMK client is active.
        // Refresh directly from the authoritative TIS transition so its
        // copy/send affordances do not keep the previous input source state.
        BufferWindowController.shared.refresh()
    } else {
        RIMESRecoveryStatusItem.shared.refresh(isSelected: false)
        retireRimeInputSourceAuthority(observedSourceID: currentID)
    }
}

MainActor.assumeIsolated {
    RIMESRecoveryStatusItem.shared.refresh(
        isSelected: RimeInputSourceAuthority.currentSourceIsOwn()
    )
}

let privacyOwnBundleID = Bundle.main.bundleIdentifier ?? RimesIdentity.bundleIdentifier
let privacyOwnProcessIdentifier = ProcessInfo.processInfo.processIdentifier
let initialFrontmostApplication = NSWorkspace.shared.frontmostApplication
var lastExternalForegroundIdentity = BufferPrivacyTransitionRules.externalIdentity(
    bundleID: initialFrontmostApplication?.bundleIdentifier,
    processIdentifier: initialFrontmostApplication?.processIdentifier ?? 0,
    ownBundleID: privacyOwnBundleID,
    ownProcessIdentifier: privacyOwnProcessIdentifier
)

NSWorkspace.shared.notificationCenter.addObserver(
    forName: NSWorkspace.didActivateApplicationNotification,
    object: nil, queue: .main) { note in
    let activatedApplication = note.userInfo?[NSWorkspace.applicationUserInfoKey]
        as? NSRunningApplication
    if let lease = InputFocusCoordinator.shared.invalidateIfFrontmostChanged(
        to: activatedApplication
    ) {
        lease.controller?.finalizeDisplacedFocus(lease)
        if BufferModel.shared.captureFocusToken == lease.token {
            BufferModel.shared.routeDirectPreservingContent(
                reason: "frontmost application changed"
            )
        }
        candidateWindow.hide(owner: lease.token)
    } else if InputFocusCoordinator.shared.owner == nil {
        candidateWindow.hideAll()
    }

    // Reset only on a real external A -> B transition. Activating our editor or
    // settings leaves the last external identity unchanged, so A -> ETInput -> A
    // cannot self-wipe. Mixed/external content is kept as a whole because it was
    // staged specifically for delivery to another application.
    let activatedExternal = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: activatedApplication?.bundleIdentifier,
        processIdentifier: activatedApplication?.processIdentifier ?? 0,
        ownBundleID: privacyOwnBundleID,
        ownProcessIdentifier: privacyOwnProcessIdentifier
    )
    if BufferPrivacyTransitionRules.shouldDiscard(
        previousExternal: lastExternalForegroundIdentity,
        activatedExternal: activatedExternal,
        resetOnSwitch: BufferModel.shared.resetOnAppSwitch,
        holdsExternalContent: BufferModel.shared.holdsExternalContent
    ) {
        BufferWindowController.shared.discardForPrivacyTransition()
    }
    lastExternalForegroundIdentity = BufferPrivacyTransitionRules.updatedPrevious(
        lastExternalForegroundIdentity,
        activatedExternal: activatedExternal
    )
}
NotificationCenter.default.addObserver(
    forName: NSApplication.willTerminateNotification,
    object: NSApp,
    queue: .main
) { _ in
    let clipboardFlushed = ClipboardHistoryWindowController.shared
        .flushPersistenceBeforeTermination()
    IMELog.write(
        "clipboard persistence termination flush="
            + (clipboardFlushed ? "complete" : "timed_out_or_failed")
    )
    InputMetricsPersistence.saveNow()
}

IMELog.write("bootstrap done: server=\(imkServer != nil) engineHealthy=\(rimeEngine.isHealthy)")

NSApplication.shared.run()

// MARK: - Input-source lifecycle

private func tisStringProperty(_ source: TISInputSource, _ key: CFString) -> String? {
    guard let pointer = TISGetInputSourceProperty(source, key) else { return nil }
    return Unmanaged<CFString>.fromOpaque(pointer).takeUnretainedValue() as String
}

private func tisBoolProperty(_ source: TISInputSource, _ key: CFString) -> Bool {
    guard let pointer = TISGetInputSourceProperty(source, key) else { return false }
    return Unmanaged<NSNumber>.fromOpaque(pointer).takeUnretainedValue().boolValue
}

/// Move the login session off ETInput before replacing its bundle or killing
/// its process. Long-lived clients (notably WeChat/Electron) otherwise retain a
/// dead IMK document connection and can immediately undo later source changes.
/// The legacy ETInput identity is included so an upgrade from older builds is
/// also handed off cleanly.
func selectFallbackInputSourceForUpdate() -> Bool {
    let ownBundleIDs = Set([
        Bundle.main.bundleIdentifier ?? "",
        RimesIdentity.bundleIdentifier,
        RimesIdentity.legacyBundleIdentifier,
        "com.isaac.inputmethod.ETInput",
    ].filter { !$0.isEmpty })

    func belongsToETInput(_ source: TISInputSource) -> Bool {
        if let bundleID = tisStringProperty(source, kTISPropertyBundleID),
           ownBundleIDs.contains(bundleID) {
            return true
        }
        guard let sourceID = tisStringProperty(source, kTISPropertyInputSourceID) else {
            return false
        }
        return ownBundleIDs.contains { sourceID == $0 || sourceID.hasPrefix($0 + ".") }
    }

    guard let current = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue() else {
        print("prepare-update: no current keyboard input source")
        return false
    }
    let currentID = tisStringProperty(current, kTISPropertyInputSourceID) ?? "(unknown)"
    guard belongsToETInput(current) else {
        print("prepare-update: current source is already safe: \(currentID)")
        return true
    }

    var candidates: [TISInputSource] = []
    if let layout = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue() {
        candidates.append(layout)
    }
    if let ascii = TISCopyCurrentASCIICapableKeyboardInputSource()?.takeRetainedValue() {
        candidates.append(ascii)
    }
    if let all = TISCreateInputSourceList(nil, false)?.takeRetainedValue() as? [TISInputSource] {
        candidates.append(contentsOf: all.filter {
            tisBoolProperty($0, kTISPropertyInputSourceIsASCIICapable)
        })
    }

    for fallback in candidates {
        guard !belongsToETInput(fallback),
              tisBoolProperty(fallback, kTISPropertyInputSourceIsEnabled),
              tisBoolProperty(fallback, kTISPropertyInputSourceIsSelectCapable) else {
            continue
        }
        let fallbackID = tisStringProperty(fallback, kTISPropertyInputSourceID) ?? "(unknown)"
        let status = TISSelectInputSource(fallback)
        print("prepare-update: select fallback=\(status) \(fallbackID)")
        guard status == noErr else { continue }

        for _ in 0..<20 {
            if let selected = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue(),
               !belongsToETInput(selected) {
                let selectedID = tisStringProperty(selected, kTISPropertyInputSourceID) ?? fallbackID
                print("prepare-update: fallback verified: \(selectedID)")
                return true
            }
            Thread.sleep(forTimeInterval: 0.1)
        }
    }

    print("prepare-update: could not select a non-\(ProductIdentity.displayName) ASCII fallback")
    return false
}

// MARK: - Engine smoke harness (bring-up only)

func runEngineSmokeTest() -> Bool {
    let environment = ProcessInfo.processInfo.environment
    guard let isolatedUserDir = environment["RIMEBUFFER_USER_DIR"],
          !isolatedUserDir.isEmpty,
          !StandaloneRimeCommandRules.isLiveUserDir(isolatedUserDir),
          StandaloneRimeCommandRules.requiresIsolatedUserDir(
            arguments: ["ETInput", "smoke"]
          ),
          StandaloneRimeCommandRules.requiresIsolatedUserDir(
            arguments: ["ETInput", "user-lexicon-bridge-smoke"]
          ),
          StandaloneRimeCommandRules.requiresIsolatedUserDir(
            arguments: ["ETInput", "settings-preview"]
          ),
          !StandaloneRimeCommandRules.requiresIsolatedUserDir(
            arguments: ["ETInput", "--install"]
          ),
          // The retired directory stays live: the migration copies rather
          // than moves, so it still holds dictionaries and Capsule records.
          StandaloneRimeCommandRules.isLiveUserDir(
            "/Users/smoke/Library/RimeBuffer/../RimeBuffer",
            homeDirectory: "/Users/smoke"
          ),
          StandaloneRimeCommandRules.isLiveUserDir(
            "/Users/smoke/Library/RimeBuffer/smoke-child",
            homeDirectory: "/Users/smoke"
          ),
          StandaloneRimeCommandRules.isLiveUserDir(
            "/users/smoke/library/rimebuffer",
            homeDirectory: "/Users/smoke"
          ),
          StandaloneRimeCommandRules.isLiveUserDir(
            "/Users/smoke/Library/RIMES",
            homeDirectory: "/Users/smoke"
          ),
          StandaloneRimeCommandRules.isLiveUserDir(
            "/Users/smoke/Library/RIMES/smoke-child",
            homeDirectory: "/Users/smoke"
          ),
          StandaloneRimeCommandRules.isLiveUserDir(
            "/users/smoke/library/rimes",
            homeDirectory: "/Users/smoke"
          ),
          standaloneIsolationAliasProbe(
            isolatedUserDir: isolatedUserDir
          ),
          !StandaloneRimeCommandRules.isLiveUserDir(
            "/tmp/rimebuffer-smoke",
            homeDirectory: "/Users/smoke"
          ) else {
        print("FAILED: standalone Rime userdir isolation")
        return false
    }
    let engine = RimeEngine()
    print("== \(ProductIdentity.displayName) engine smoke test ==")
    let ok = engine.start()
    print("start:", ok, "healthy:", engine.isHealthy)
    guard ok, engine.isHealthy, engine.hasOctagram else {
        print("FAILED: engine start:", engine.lastError())
        return false
    }

    let session = engine.createSession()
    print("session:", session)
    guard session != 0 else {
        print("FAILED: no session")
        return false
    }
    defer { engine.destroySession(session) }

    let expectedSchemas = InputSchemaCatalog.enabledIDs(
        chordExtensionEnabled: true
    )
    let deployedSchemas = engine.schemaList()
    let deployedIDs = deployedSchemas.map(\.id)
    print("deployed schemas:", deployedSchemas.map { "\($0.id)=\($0.name)" }.joined(separator: ", "))
    guard deployedIDs == expectedSchemas else {
        print("FAILED: schema_list expected=\(expectedSchemas) actual=\(deployedIDs)")
        return false
    }

    let defaultStatus = engine.getStatus(session: session)
    print("default schema: \(defaultStatus.schemaId) (\(defaultStatus.schemaName))")

    // Use a sequential Chinese schema for deterministic CLI key replay. The
    // default my_combo schema expects physical chord press/release timing.
    guard engine.selectSchema("rime_ice", session: session),
          engine.getStatus(session: session).schemaId == "rime_ice" else {
        print("FAILED: cannot select rime_ice")
        return false
    }
    engine.setOption("ascii_mode", false, session: session)
    engine.clearComposition(session: session)
    print("typing 'nihao' on rime_ice ...")
    for scalar in "nihao".unicodeScalars {
        _ = engine.processKey(Int32(scalar.value), session: session)
    }
    let ctx = engine.getContext(session: session)
    print("active=\(ctx.active) preedit='\(ctx.preedit)' page=\(ctx.pageNo) cands=\(ctx.candidates.count)")
    for c in ctx.candidates {
        print("  [\(c.label)] \(c.text)\(c.comment.isEmpty ? "" : "  · \(c.comment)")")
    }
    guard ctx.candidates.contains(where: { $0.text == "你好" }) else {
        print("FAILED: rime_ice did not produce 你好")
        return false
    }
    _ = engine.processKey(0x20, session: session)
    let chineseCommit = engine.takeCommit(session: session) ?? ""
    print("committed:", chineseCommit)
    guard chineseCommit == "你好" else {
        print("FAILED: unexpected Chinese commit '\(chineseCommit)'")
        return false
    }

    guard runWubiEngineSmokeTest(engine: engine, session: session),
          engine.selectSchema("rime_ice", session: session) else { return false }

    // The consciousness-stream module uses a hidden schema plus the atomic
    // set-input/copy-candidates bridge. It must decode without committing or
    // sharing bridge-owned candidate memory with the ordinary IMK session.
    let streamSession = engine.createSession()
    guard streamSession != 0 else {
        print("FAILED: no private stream inference session")
        return false
    }
    defer { engine.destroySession(streamSession) }
    guard engine.selectSchema("stream_input_local", session: streamSession),
          engine.getStatus(session: streamSession).schemaId
            == "stream_input_local",
          let streamCandidates = engine.decodeCandidateTexts(
            input: "nihao",
            maximumCount: 5,
            session: streamSession
          ),
          streamCandidates.contains("你好") else {
        print("FAILED: hidden stream schema or atomic decoder unavailable")
        return false
    }
    let usableStreamCandidates = streamCandidates.compactMap(
        RimeOctagramStreamInputEngine.usableCandidate
    )
    guard usableStreamCandidates == ["你好", "拟好"] else {
        print("FAILED: stream local filter accepted an unconverted raw tail")
        return false
    }

    // A menu candidate may translate only a prefix while raw input remains.
    // The atomic bridge must return the full highlighted commit preview; using
    // candidate.text here would silently truncate both fixtures to `你好`.
    guard let tailedCandidates = engine.decodeCandidateTexts(
        input: "nihaox",
        maximumCount: 5,
        session: streamSession
    ), let longerTailedCandidates = engine.decodeCandidateTexts(
        input: "nihaoabc",
        maximumCount: 5,
        session: streamSession
    ) else {
        print("FAILED: stream decoder could not decode tail fixtures")
        return false
    }
    guard tailedCandidates.first == "你好像",
          tailedCandidates.last == "你好x",
          !tailedCandidates.contains("你好"),
          longerTailedCandidates.first == "你好啊不错",
          longerTailedCandidates.contains("你好abc"),
          !longerTailedCandidates.contains("你好") else {
        print("tail candidates:", tailedCandidates, longerTailedCandidates)
        print("FAILED: stream decoder returned a partial candidate without its tail")
        return false
    }

    // Warm-query evidence is diagnostic rather than a brittle hosted-runner
    // threshold. Product code independently bounds raw input and candidate
    // count; this verifies the hot path stays a single atomic bridge call.
    var streamDurationsMicros: [UInt64] = []
    for _ in 0..<30 {
        let began = DispatchTime.now().uptimeNanoseconds
        guard engine.decodeCandidateTexts(
            input: "xiufuyigewenti",
            maximumCount: 5,
            session: streamSession
        )?.isEmpty == false else {
            print("FAILED: warm stream decode returned no candidates")
            return false
        }
        streamDurationsMicros.append(
            (DispatchTime.now().uptimeNanoseconds - began) / 1_000
        )
    }
    streamDurationsMicros.sort()
    let streamP50 = streamDurationsMicros[streamDurationsMicros.count / 2]
    let streamP95 = streamDurationsMicros[
        Int(Double(streamDurationsMicros.count - 1) * 0.95)
    ]
    print(
        "stream local decode: candidates=\(usableStreamCandidates) p50=\(streamP50)us p95=\(streamP95)us"
    )

    let ordinaryProbe = engine.createSession()
    guard ordinaryProbe != 0 else {
        print("FAILED: no ordinary post-stream session")
        return false
    }
    defer { engine.destroySession(ordinaryProbe) }
    guard engine.getStatus(session: ordinaryProbe).schemaId
            != "stream_input_local" else {
        print("FAILED: hidden stream schema leaked into ordinary session")
        return false
    }

    // The frontend's deferred Shift contract must protect a live composition:
    // a used/held gesture emits no mode-switch pair, while a proven standalone
    // tap still reaches librime and preserves its configured commit_code
    // semantics. This exercises the bundled engine rather than only the pure
    // gesture reducer below.
    let shiftSession = engine.createSession()
    guard shiftSession != 0,
          engine.selectSchema("rime_ice", session: shiftSession) else {
        print("FAILED: cannot create isolated Shift smoke session")
        return false
    }
    engine.clearComposition(session: shiftSession)
    engine.setOption("ascii_mode", false, session: shiftSession)
    _ = engine.takeCommit(session: shiftSession)
    for scalar in "ni".unicodeScalars {
        _ = engine.processKey(Int32(scalar.value), session: shiftSession)
    }
    let protectedShiftBefore = engine.getContext(session: shiftSession)
    var protectedShift = ShiftModifierGesture(
        beganAt: 1,
        rimeKeycode: RimeKey.shiftL,
        session: shiftSession,
        schemaID: "rime_ice"
    )
    protectedShift.noteModifierUse()
    guard protectedShift.releaseDecision(
            at: 1.1,
            currentSession: shiftSession,
            currentSchemaID: "rime_ice"
          ) == .discard else {
        print("FAILED: modified Shift should not replay into librime")
        return false
    }
    let protectedShiftAfter = engine.getContext(session: shiftSession)
    guard protectedShiftAfter.active == protectedShiftBefore.active,
          protectedShiftAfter.input == protectedShiftBefore.input,
          protectedShiftAfter.preedit == protectedShiftBefore.preedit,
          protectedShiftAfter.candidates.map(\.text)
            == protectedShiftBefore.candidates.map(\.text),
          engine.takeCommit(session: shiftSession) == nil,
          !engine.getStatus(session: shiftSession).asciiMode else {
        print("FAILED: modified Shift changed live composition")
        return false
    }

    let standaloneShift = ShiftModifierGesture(
        beganAt: 2,
        rimeKeycode: RimeKey.shiftL,
        session: shiftSession,
        schemaID: "rime_ice"
    )
    guard case let .replayStandaloneTap(rimeKeycode) = standaloneShift.releaseDecision(
        at: 2.1,
        currentSession: shiftSession,
        currentSchemaID: "rime_ice"
    ) else {
        print("FAILED: standalone Shift replay decision")
        return false
    }
    _ = engine.processKey(rimeKeycode,
                          mask: RimeKey.shiftMask,
                          session: shiftSession)
    _ = engine.processKey(rimeKeycode,
                          mask: RimeKey.releaseMask,
                          session: shiftSession)
    let standaloneShiftCommit = engine.takeCommit(session: shiftSession)
    guard standaloneShiftCommit == protectedShiftBefore.input,
          engine.getContext(session: shiftSession).input.isEmpty,
          engine.getStatus(session: shiftSession).asciiMode else {
        print("FAILED: standalone Shift did not preserve commit_code semantics",
              standaloneShiftCommit ?? "<nil>")
        return false
    }
    engine.setOption("ascii_mode", false, session: shiftSession)
    engine.clearComposition(session: shiftSession)
    engine.destroySession(shiftSession)

    guard engine.selectSchema("english", session: session),
          engine.getStatus(session: session).schemaId == "english" else {
        print("FAILED: cannot select english")
        return false
    }
    engine.setOption("ascii_mode", false, session: session)

    engine.clearComposition(session: session)
    for scalar in "hel".unicodeScalars {
        _ = engine.processKey(Int32(scalar.value), session: session)
    }
    let completionContext = engine.getContext(session: session)
    let completionSamples = completionContext.candidates.prefix(8).map(\.text)
    print("english 'hel' candidates:", completionSamples)
    guard completionSamples.contains(where: { $0.lowercased().hasPrefix("hel") && $0.count > 3 }) else {
        print("FAILED: English prefix completion is unavailable")
        return false
    }

    func typeAndCommitEnglish(_ text: String) -> String {
        engine.clearComposition(session: session)
        for scalar in text.unicodeScalars {
            _ = engine.processKey(Int32(scalar.value), session: session)
        }
        _ = engine.processKey(0x20, session: session)
        return engine.takeCommit(session: session) ?? ""
    }

    let helloCommit = typeAndCommitEnglish("hello")
    let worldCommit = typeAndCommitEnglish("world")
    let rawCommit = typeAndCommitEnglish("codexzzq")
    print("english commits:", [helloCommit, worldCommit, rawCommit])
    guard helloCommit == "hello",
          worldCommit == " world",
          rawCommit.trimmingCharacters(in: .whitespaces) == "codexzzq" else {
        print("FAILED: English commit/spacing/raw fallback")
        return false
    }

    guard engine.selectSchema("my_combo", session: session) else {
        print("FAILED: cannot return to my_combo")
        return false
    }
    engine.clearComposition(session: session)
    engine.setOption("ascii_mode", true, session: session)
    let asciiLetterHandled = engine.processKey(0x61, session: session)
    let asciiLetterContext = engine.getContext(session: session)
    let asciiLetterCommit = engine.takeCommit(session: session)
    guard !FlyChordRoutingRules.shouldStage(schemaID: "my_combo", asciiMode: true),
          !asciiLetterHandled,
          asciiLetterContext.input.isEmpty,
          asciiLetterCommit == nil else {
        print("FAILED: my_combo ASCII mode must pass Latin keys through immediately",
              asciiLetterHandled, asciiLetterContext.input,
              asciiLetterCommit ?? "<nil>")
        return false
    }
    engine.setOption("ascii_mode", false, session: session)

    // Reproduce switching into a fresh Chinese session with Caps Lock already
    // enabled. librime rejects Lock-modified letters even with ascii_mode off.
    // The frontend must let that rejection reach the host immediately, rather
    // than consuming the press and discovering it after the chord timer fires.
    let capsLetterHandled = engine.processKey(0x61,
                                              mask: RimeKey.lockMask,
                                              session: session)
    guard FlyChordRoutingRules.shouldStage(schemaID: "my_combo",
                                           asciiMode: false,
                                           extensionEnabled: true),
          !ChordPressRoutingRules.accepts(mask: RimeKey.lockMask),
          !capsLetterHandled,
          engine.getContext(session: session).input.isEmpty,
          engine.takeCommit(session: session) == nil,
          !engine.processKey(RimeKey.space,
                             mask: RimeKey.controlMask,
                             session: session),
          engine.processKey(0x6e, session: session),
          engine.processKey(0x6e, mask: RimeKey.releaseMask, session: session),
          !engine.getContext(session: session).candidates.isEmpty else {
        print("FAILED: Caps Lock pass-through, Ctrl+Space, or resumed Chinese chord")
        return false
    }
    engine.clearComposition(session: session)

    func typeFlyChordStrokes(
        _ strokes: [String],
        clearComposition: Bool = true,
        policy: FlyChordSettlementPolicy = ChordExtensionConfiguration(
            isEnabled: true, duration: 0.1
        ).settlementPolicy
    )
        -> (context: RimeContextModel, allPressesHandled: Bool) {
        if clearComposition {
            engine.clearComposition(session: session)
        }
        var allPressesHandled = true
        var pairing = FlyChordMutualPairingState()
        for stroke in strokes {
            let keys = stroke.unicodeScalars.map {
                FlyChordKeyEvent(keycode: Int32($0.value), mask: 0)
            }
            let shapedKeys = keys.map { (keycode: $0.keycode, mask: $0.mask) }
            guard let shape = FlyChordBatchShape(keys: shapedKeys) else {
                return (engine.getContext(session: session), false)
            }
            let contextBefore = engine.getContext(session: session)
            var engineKeys = keys
            var boundaryPlan = FlyChordBoundaryRules.plan(for: contextBefore)
            if let previousLeft = pairing.takeComplement(
                before: shape,
                currentKeyCount: keys.count,
                policy: policy,
                currentContext: contextBefore
            ) {
                for _ in 0..<previousLeft.insertedScalarCount {
                    if !engine.processKey(RimeKey.backspace, session: session) {
                        allPressesHandled = false
                    }
                }
                guard engine.getContext(session: session).input == previousLeft.baseInput else {
                    return (engine.getContext(session: session), false)
                }
                engineKeys = previousLeft.keys + keys
                boundaryPlan = previousLeft.boundaryPlan
            }
            let insertsBoundary = FlyChordBoundaryRules.shouldInsert(
                forKeyCount: engineKeys.count
            )
            if insertsBoundary,
               boundaryPlan.before,
               !engine.processKey(FlyChordBoundaryRules.delimiterKeycode,
                                  session: session) {
                allPressesHandled = false
            }
            for key in engineKeys {
                if !engine.processKey(key.keycode, mask: key.mask, session: session) {
                    allPressesHandled = false
                }
            }
            for key in engineKeys {
                if !engine.processKey(key.keycode,
                                      mask: key.mask | RimeKey.releaseMask,
                                      session: session) {
                    allPressesHandled = false
                }
            }
            if insertsBoundary,
               boundaryPlan.after,
               !engine.processKey(FlyChordBoundaryRules.delimiterKeycode,
                                  session: session) {
                allPressesHandled = false
            }
            pairing.recordSettledLeft(
                keys: keys,
                baseInput: contextBefore.input,
                settledContext: engine.getContext(session: session),
                boundaryPlan: boundaryPlan,
                policy: policy,
                shape: shape
            )
        }
        return (engine.getContext(session: session), allPressesHandled)
    }

    func collectCandidatePages(maxPages: Int = 128)
        -> (contexts: [RimeContextModel], texts: [String], pagingWorked: Bool) {
        var contexts = [engine.getContext(session: session)]
        var texts = contexts[0].candidates.map(\.text)
        var pagingWorked = true
        while let current = contexts.last,
              !current.isLastPage,
              contexts.count < maxPages {
            guard engine.processKey(0x3d, session: session) else {
                pagingWorked = false
                break
            }
            let next = engine.getContext(session: session)
            guard next.pageNo > current.pageNo else {
                pagingWorked = false
                break
            }
            contexts.append(next)
            texts.append(contentsOf: next.candidates.map(\.text))
        }
        return (contexts, texts, pagingWorked)
    }

    // Keep the internal no-pair primitive covered, while proving the single
    // product policy gives split strokes the same result as a combined chord.
    let sameBatchOneSided = typeFlyChordStrokes(["dv"], policy: .sameBatchOnly)
    let sameBatchCombined = typeFlyChordStrokes(["qkm"], policy: .sameBatchOnly)
    let sameBatchSplit = typeFlyChordStrokes(["q", "km"], policy: .sameBatchOnly)
    let unifiedSplit = typeFlyChordStrokes(["q", "km"])
    guard sameBatchOneSided.allPressesHandled,
          !sameBatchOneSided.context.input.isEmpty,
          !sameBatchOneSided.context.candidates.isEmpty,
          sameBatchCombined.context.candidates.map(\.text).contains("穷"),
          sameBatchSplit.allPressesHandled,
          !sameBatchSplit.context.candidates.isEmpty,
          sameBatchSplit.context.input != sameBatchCombined.context.input,
          unifiedSplit.context.input == sameBatchCombined.context.input,
          unifiedSplit.context.candidates.map(\.text)
            == sameBatchCombined.context.candidates.map(\.text) else {
        print("FAILED: unified chord split equivalence or internal no-pair guard",
              sameBatchOneSided.context.input,
              sameBatchOneSided.context.candidates.map(\.text),
              sameBatchCombined.context.input,
              sameBatchSplit.context.input,
              unifiedSplit.context.input)
        return false
    }

    // A genuinely unmapped chord must keep its transformed raw code available
    // for Return-to-commit even when abbreviation candidates occupy every page.
    let unmappedChord = typeFlyChordStrokes(["qs"], policy: .sameBatchOnly)
    let unmappedRaw = unmappedChord.context.input
    let unmappedReturnHandled = engine.processKey(RimeKey.return, session: session)
    let unmappedCommit = engine.takeCommit(session: session)
    guard unmappedChord.allPressesHandled,
          !unmappedRaw.isEmpty,
          unmappedReturnHandled,
          unmappedCommit == unmappedRaw else {
        print("FAILED: FlyYao unmapped chord raw commit",
              unmappedRaw, unmappedReturnHandled,
              unmappedCommit ?? "<nil>")
        return false
    }

    // Comma and period remain chord keys in multi-key mappings, but a batch
    // containing only one of them must fall through to Rime's punctuator.
    let punctuationCases: [(key: String, chinese: String, ascii: String)] = [
        (",", "，", ","),
        (".", "。", "."),
    ]
    for policy in [FlyChordSettlementPolicy.sameBatchOnly, .independentHalves] {
        for punctuation in punctuationCases {
            for asciiPunct in [false, true] {
                engine.clearComposition(session: session)
                _ = engine.takeCommit(session: session)
                engine.setOption("ascii_punct", asciiPunct, session: session)
                let outcome = typeFlyChordStrokes(
                    [punctuation.key],
                    clearComposition: false,
                    policy: policy
                )
                let commit = engine.takeCommit(session: session)
                let expected = asciiPunct ? punctuation.ascii : punctuation.chinese
                guard outcome.allPressesHandled, commit == expected else {
                    print("FAILED: FlyYao punctuation settlement",
                          policy, punctuation.key, asciiPunct,
                          commit ?? "<nil>", outcome.context.input,
                          outcome.context.candidates.map(\.text))
                    return false
                }
            }
        }
    }
    engine.setOption("ascii_punct", false, session: session)

    let flyYaoCases: [(name: String, combined: [String], split: [String], expected: String)] = [
        ("compound initial", ["dvi"], ["dv", "i"], "你"),
        ("contextual ong/iong", ["qkm"], ["q", "km"], "穷"),
        ("compound final", ["efuo"], ["ef", "uo"], "双"),
        ("literal period", ["qm."], ["q", "m."], "却"),
        ("zero initial", ["xvo"], ["xv", "o"], "哦"),
        ("contextual zero initial", ["eui"], ["e", "ui"], "而"),
        ("closed alias lve", ["sdm."], ["sd", "m."], "略"),
        ("multi-syllable boundary", ["qkm", "dvi"], ["q", "km", "dv", "i"], "穷你"),
    ]
    let forbiddenAlternateSegmentation: [String: Set<String>] = [
        "zero initial": ["欧", "偶", "呕"],
        "closed alias lve": ["路", "露", "卤鹅"],
    ]
    for test in flyYaoCases {
        let combined = typeFlyChordStrokes(test.combined)
        let split = typeFlyChordStrokes(test.split)
        let combinedTexts = combined.context.candidates.map(\.text)
        let splitTexts = split.context.candidates.map(\.text)
        print("FlyYao \(test.name): combined=\(combined.context.preedit) split=\(split.context.preedit)")
        guard combined.allPressesHandled,
              split.allPressesHandled,
              combined.context.input == split.context.input,
              combined.context.preedit == split.context.preedit,
              combinedTexts == splitTexts,
              forbiddenAlternateSegmentation[test.name, default: []]
                .isDisjoint(with: splitTexts),
              combinedTexts.contains(test.expected),
              splitTexts.contains(test.expected) else {
            print("FAILED: FlyYao combined/split mismatch",
                  test.combined, test.split, test.expected,
                  combinedTexts, splitTexts)
            return false
        }
    }

    // Two keys pressed in one timing batch remain a real chord even when each
    // physical half contains only one key. The same keys pressed separately
    // are intentionally literal so ordinary Latin typing stays possible.
    let simultaneousSingletonChords: [(stroke: String, expected: String)] = [
        ("fo", "佛"),
        ("gy", "怪"),
    ]
    for test in simultaneousSingletonChords {
        let combined = typeFlyChordStrokes([test.stroke])
        guard combined.allPressesHandled,
              combined.context.candidates.map(\.text).contains(test.expected) else {
            print("FAILED: FlyYao simultaneous singleton chord",
                  test.stroke, test.expected,
                  combined.context.input,
                  combined.context.candidates.map(\.text))
            return false
        }
    }

    let explicitTwoSyllables = typeFlyChordStrokes(["fu", "xvo"])
    guard explicitTwoSyllables.allPressesHandled,
          explicitTwoSyllables.context.preedit == "fu'o",
          !explicitTwoSyllables.context.input.contains("J"),
          !explicitTwoSyllables.context.candidates.map(\.text).contains("佛") else {
        print("FAILED: FlyYao preserved-stroke boundary for fu'o",
              explicitTwoSyllables.context.preedit,
              explicitTwoSyllables.context.candidates.map(\.text))
        return false
    }

    // `ni'hao` must expose both the full phrase and partial single-character
    // choices, with a real next page that Rime can navigate to.
    let candidatePagingProbe = typeFlyChordStrokes(["dvi", "xck"])
    let candidatePages = collectCandidatePages()
    guard candidatePagingProbe.allPressesHandled,
          candidatePagingProbe.context.input == "ni'hao",
          candidatePagingProbe.context.candidates.map(\.text).contains("你好"),
          !candidatePages.texts.contains(candidatePagingProbe.context.input),
          !candidatePagingProbe.context.isLastPage,
          candidatePages.pagingWorked,
          candidatePages.contexts.count > 1,
          candidatePages.contexts[1].pageNo > candidatePages.contexts[0].pageNo,
          candidatePages.texts.contains("你") else {
        print("FAILED: FlyYao partial candidate paging",
              candidatePagingProbe.context.input,
              candidatePages.contexts.map(\.pageNo),
              candidatePages.texts.prefix(30))
        return false
    }

    // In both product modes, separately pressed physical singleton keys are
    // literal Latin input. They must never acquire automatic syllable marks or
    // be recombined into a Chinese chord behind the user's back.
    for policy in [FlyChordSettlementPolicy.sameBatchOnly, .independentHalves] {
        let singletonEnglish = typeFlyChordStrokes(
            ["c", "o", "d", "e", "x"],
            policy: policy
        )
        guard singletonEnglish.allPressesHandled,
              singletonEnglish.context.input == "codex",
              singletonEnglish.context.preedit == "codex",
              !singletonEnglish.context.input.contains("'") else {
            print("FAILED: FlyYao singleton Latin passthrough",
                  policy,
                  singletonEnglish.context.input,
                  singletonEnglish.context.preedit,
                  singletonEnglish.context.candidates.map(\.text))
            return false
        }

        // rime_ice historically reserved lowercase v as a symbols prefix.
        // FlyYao must override that inherited route so a physical singleton v
        // can begin a normal English word in either product keying mode.
        let vPrefix = typeFlyChordStrokes(
            ["v", "a"],
            policy: policy
        )
        let vPrefixCandidateTexts = vPrefix.context.candidates.map(\.text)
        let inheritedVSymbols: Set<String> = ["ā", "á", "ǎ", "à"]
        guard vPrefix.allPressesHandled,
              vPrefix.context.input == "va",
              vPrefix.context.preedit == "va",
              vPrefixCandidateTexts.contains(where: {
                  ["van", "vain"].contains($0.lowercased())
              }),
              inheritedVSymbols.isDisjoint(with: vPrefixCandidateTexts) else {
            print("FAILED: FlyYao lowercase v entered inherited symbols mode",
                  policy,
                  vPrefix.context.input,
                  vPrefix.context.preedit,
                  vPrefixCandidateTexts)
            return false
        }

        let vEnglish = typeFlyChordStrokes(
            ["v", "i", "d", "e", "o"],
            policy: policy
        )
        let vCandidateTexts = vEnglish.context.candidates.map(\.text)
        let vReturnHandled = engine.processKey(RimeKey.return, session: session)
        let vRawCommit = engine.takeCommit(session: session)
        guard vEnglish.allPressesHandled,
              vEnglish.context.input == "video",
              vEnglish.context.preedit == "video",
              vCandidateTexts.contains(where: { $0.lowercased() == "video" }),
              vReturnHandled,
              vRawCommit == "video" else {
            print("FAILED: FlyYao lowercase v must stay literal",
                  policy,
                  vEnglish.context.input,
                  vEnglish.context.preedit,
                  vCandidateTexts,
                  vReturnHandled,
                  vRawCommit ?? "<nil>")
            return false
        }
    }

    let ambiguousSyllableBoundaries: [(combined: [String], expected: String)] = [
        (["dvi", "an"], "ni'an"),
        (["ah", "ah"], "ang'ang"),
    ]
    for test in ambiguousSyllableBoundaries {
        let combined = typeFlyChordStrokes(test.combined)
        guard combined.allPressesHandled,
              combined.context.input == test.expected,
              combined.context.preedit == test.expected else {
            print("FAILED: FlyYao explicit syllable boundary",
                  test.combined, test.expected,
                  combined.context.input, combined.context.preedit)
            return false
        }
    }

    _ = typeFlyChordStrokes(["an"])
    guard engine.processKey(RimeKey.home, session: session) else {
        print("FAILED: FlyYao cursor smoke cannot move Home")
        return false
    }
    let insertedAtStart = typeFlyChordStrokes(["dvi"], clearComposition: false)
    guard insertedAtStart.allPressesHandled,
          insertedAtStart.context.input == "ni'an",
          insertedAtStart.context.preedit == "ni'an" else {
        print("FAILED: FlyYao leading cursor syllable boundary",
              insertedAtStart.context.input, insertedAtStart.context.preedit)
        return false
    }

    _ = typeFlyChordStrokes(["an"])
    guard engine.processKey(RimeKey.home, session: session),
          engine.processKey(RimeKey.right, session: session) else {
        print("FAILED: FlyYao cursor smoke cannot move into raw input")
        return false
    }
    let insertedInMiddle = typeFlyChordStrokes(["dvi"], clearComposition: false)
    guard insertedInMiddle.allPressesHandled,
          insertedInMiddle.context.input == "a'ni'n",
          insertedInMiddle.context.preedit == "a'ni'n" else {
        print("FAILED: FlyYao two-sided cursor syllable boundary",
              insertedInMiddle.context.input, insertedInMiddle.context.preedit)
        return false
    }

    do {
        let schema = try FlyChordSchemaParser.loadDefault()
        let crossMappings = schema.mappings.filter { mapping in
            let halves = Set(mapping.chord.unicodeScalars.compactMap {
                FlyChordLayout.half(for: Int32($0.value))
            })
            return halves == Set([.left, .right])
        }
        guard crossMappings.count > 300 else {
            print("FAILED: FlyYao exhaustive mapping set is incomplete", crossMappings.count)
            return false
        }
        var pairableSplitCount = 0
        for mapping in crossMappings {
            let left = String(mapping.chord.filter { character in
                let scalar = String(character).unicodeScalars.first!
                return FlyChordLayout.half(for: Int32(scalar.value)) == .left
            })
            let right = String(mapping.chord.filter { character in
                let scalar = String(character).unicodeScalars.first!
                return FlyChordLayout.half(for: Int32(scalar.value)) == .right
            })
            let combined = typeFlyChordStrokes([mapping.chord])
            guard combined.allPressesHandled else {
                print("FAILED: FlyYao exhaustive combined chord",
                      mapping.chord, mapping.output)
                return false
            }

            // Two separately timed singleton halves are deliberately Latin.
            // Cross-batch equivalence applies only when at least one half is a
            // true multi-key half chord.
            guard left.count > 1 || right.count > 1 else { continue }
            pairableSplitCount += 1
            let split = typeFlyChordStrokes([left, right])
            let combinedTexts = combined.context.candidates.map(\.text)
            let splitTexts = split.context.candidates.map(\.text)
            guard split.allPressesHandled,
                  combined.context.input == split.context.input,
                  combined.context.preedit == split.context.preedit,
                  combinedTexts == splitTexts else {
                print("FAILED: FlyYao exhaustive combined/split mismatch",
                      mapping.chord, mapping.output,
                      combined.context.preedit, split.context.preedit,
                      combined.context.candidates.map(\.text),
                      split.context.candidates.map(\.text))
                return false
            }
        }
        guard pairableSplitCount > 250 else {
            print("FAILED: FlyYao pairable split coverage is incomplete",
                  pairableSplitCount)
            return false
        }
        print("FlyYao exhaustive combined mappings:", crossMappings.count,
              "pairable split mappings:", pairableSplitCount)
    } catch {
        print("FAILED: FlyYao exhaustive schema parse", error)
        return false
    }

    let splitBeforeBackspace = typeFlyChordStrokes(["q", "km"])
    let splitBackspaceHandled = engine.processKey(RimeKey.backspace, session: session)
    let splitAfterBackspace = engine.getContext(session: session)
    let combinedBeforeBackspace = typeFlyChordStrokes(["qkm"])
    let combinedBackspaceHandled = engine.processKey(RimeKey.backspace, session: session)
    let combinedAfterBackspace = engine.getContext(session: session)
    guard splitBeforeBackspace.context.input == combinedBeforeBackspace.context.input,
          splitBackspaceHandled == combinedBackspaceHandled,
          splitAfterBackspace.input == combinedAfterBackspace.input,
          splitAfterBackspace.preedit == combinedAfterBackspace.preedit,
          splitAfterBackspace.candidates.map(\.text)
            == combinedAfterBackspace.candidates.map(\.text) else {
        print("FAILED: FlyYao split/combo BackSpace semantics",
              splitAfterBackspace.input, combinedAfterBackspace.input)
        return false
    }

    // F4 owns schema switching. The configured members/order were asserted
    // above; this additionally proves the switcher key enters Rime's menu.
    engine.clearComposition(session: session)
    let f4Handled = engine.processKey(RimeKey.f1 + 3, session: session)
    let switcherContext = engine.getContext(session: session)
    print("F4 handled=\(f4Handled) candidates=\(switcherContext.candidates.map(\.text))")
    let switcherNames = Set(switcherContext.candidates.map(\.text))
    // Rime's schema metadata is the source of truth for F4 display names; the
    // product catalog may intentionally use a different extension-facing
    // label. The IDs/order were asserted above, so here we only require every
    // deployed schema to remain reachable from the switcher.
    let expectedNames = Set(deployedSchemas.map(\.name))
    guard f4Handled, expectedNames.isSubset(of: switcherNames) else {
        print("FAILED: F4 schema switcher is incomplete")
        return false
    }
    print("engine smoke: OK")
    return true
}

func runSchemaListStoreSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) schema-list store smoke test ==")
    let root = URL(fileURLWithPath: NSTemporaryDirectory())
        .appendingPathComponent("rimebuffer-schema-list-\(UUID().uuidString)")
    let config = root.appendingPathComponent("default.custom.yaml")
    defer { try? FileManager.default.removeItem(at: root) }

    let expectedProfiles: [(InputConfiguration, String, RuntimeInputProfile.LexiconFamily)] = [
        (.init(encoding: .fullPinyin, keyingMode: .sequential),
         "rime_ice", .chinese),
        (.init(encoding: .naturalDoublePinyin, keyingMode: .sequential),
         "double_pinyin", .chinese),
        (.init(encoding: .xiaoheDoublePinyin, keyingMode: .sequential),
         "double_pinyin_flypy", .chinese),
        (.init(encoding: .wubi86, keyingMode: .sequential),
         "wubi86", .wubi86),
        (.init(encoding: .lingxiWubi, keyingMode: .sequential),
         "lingxi_wubi", .wubi86),
        (.init(encoding: .fullPinyin, keyingMode: .chord),
         "my_combo", .chinese),
        (.init(encoding: .fullPinyin, keyingMode: .mutual),
         "my_combo", .chinese),
        (.init(encoding: .english, keyingMode: .sequential),
         "english", .english),
    ]
    for (configuration, schemaID, lexicon) in expectedProfiles {
        guard let profile = InputConfigurationResolver.profile(for: configuration),
              profile.schemaID == schemaID,
              profile.lexiconFamily == lexicon else {
            print("FAILED: input configuration mapping", configuration, schemaID)
            return false
        }
    }
    let chordSelection = InputConfigurationResolver.selecting(
        .chord,
        from: .init(encoding: .english, keyingMode: .sequential)
    )
    let naturalSelection = InputConfigurationResolver.selecting(
        .naturalDoublePinyin,
        from: .init(encoding: .fullPinyin, keyingMode: .chord)
    )
    let mutualSelection = InputConfigurationResolver.selecting(
        .mutual,
        from: .init(encoding: .english, keyingMode: .sequential)
    )
    guard chordSelection == .init(encoding: .fullPinyin, keyingMode: .chord),
          naturalSelection == .init(encoding: .naturalDoublePinyin,
                                    keyingMode: .sequential),
          mutualSelection == .init(encoding: .fullPinyin, keyingMode: .chord),
          InputConfigurationResolver.profile(schemaID: "my_combo")?.configuration
            == .init(encoding: .fullPinyin, keyingMode: .chord) else {
        print("FAILED: input configuration reducer")
        return false
    }

    // 0.4 moved the chord window from squirrel.yaml to UserDefaults. Existing
    // users must keep their tuned interval exactly once; an explicit new
    // preference always wins, and malformed legacy data freezes the default
    // instead of being imported unpredictably on a later launch.
    let chordMigrationRoot = root.appendingPathComponent(
        "chord-duration-migration",
        isDirectory: true
    )
    let legacyUserDirectory = chordMigrationRoot.appendingPathComponent(
        "RimeBuffer",
        isDirectory: true
    )
    let legacySquirrelDirectory = chordMigrationRoot.appendingPathComponent(
        "Rime",
        isDirectory: true
    )
    do {
        try FileManager.default.createDirectory(
            at: legacyUserDirectory,
            withIntermediateDirectories: true
        )
        try FileManager.default.createDirectory(
            at: legacySquirrelDirectory,
            withIntermediateDirectories: true
        )
    } catch {
        print("FAILED: could not create chord migration fixtures", error)
        return false
    }
    let legacyChordConfig = legacyUserDirectory.appendingPathComponent(
        "squirrel.custom.yaml"
    )
    let chordMigrationSuites = (0..<5).map {
        "RimeBuffer.ChordDurationMigrationSmoke.\($0).\(UUID().uuidString)"
    }
    let chordMigrationDefaults = chordMigrationSuites.compactMap {
        UserDefaults(suiteName: $0)
    }
    guard chordMigrationDefaults.count == chordMigrationSuites.count else {
        print("FAILED: could not create chord migration defaults")
        return false
    }
    defer {
        for (suite, defaults) in zip(
            chordMigrationSuites,
            chordMigrationDefaults
        ) {
            defaults.removePersistentDomain(forName: suite)
        }
    }
    do {
        try "patch:\n  chord_duration: 0.05 # legacy tuning\n".write(
            to: legacyChordConfig,
            atomically: true,
            encoding: .utf8
        )
        let migrated = ChordSettings.resolvedDuration(
            defaults: chordMigrationDefaults[0],
            userDirectory: legacyUserDirectory,
            squirrelDirectory: legacySquirrelDirectory
        )
        // Changing the legacy file after the first read must not change the
        // persisted migration result.
        try "chord_duration: 0.20\n".write(
            to: legacyChordConfig,
            atomically: true,
            encoding: .utf8
        )
        let migratedAgain = ChordSettings.resolvedDuration(
            defaults: chordMigrationDefaults[0],
            userDirectory: legacyUserDirectory,
            squirrelDirectory: legacySquirrelDirectory
        )

        chordMigrationDefaults[1].set(0.08, forKey: "chord.duration")
        let explicitPreference = ChordSettings.resolvedDuration(
            defaults: chordMigrationDefaults[1],
            userDirectory: legacyUserDirectory,
            squirrelDirectory: legacySquirrelDirectory
        )

        try "chord_duration: invalid\n".write(
            to: legacyChordConfig,
            atomically: true,
            encoding: .utf8
        )
        let invalidFallback = ChordSettings.resolvedDuration(
            defaults: chordMigrationDefaults[2],
            userDirectory: legacyUserDirectory,
            squirrelDirectory: legacySquirrelDirectory
        )
        try "chord_duration: 0.03\n".write(
            to: legacyChordConfig,
            atomically: true,
            encoding: .utf8
        )
        let invalidStillFrozen = ChordSettings.resolvedDuration(
            defaults: chordMigrationDefaults[2],
            userDirectory: legacyUserDirectory,
            squirrelDirectory: legacySquirrelDirectory
        )

        try "chord_duration: 0.80\n".write(
            to: legacyChordConfig,
            atomically: true,
            encoding: .utf8
        )
        let clampedLegacy = ChordSettings.resolvedDuration(
            defaults: chordMigrationDefaults[3],
            userDirectory: legacyUserDirectory,
            squirrelDirectory: legacySquirrelDirectory
        )

        chordMigrationDefaults[4].set(Double.nan, forKey: "chord.duration")
        let corruptPreference = ChordSettings.resolvedDuration(
            defaults: chordMigrationDefaults[4],
            userDirectory: legacyUserDirectory,
            squirrelDirectory: legacySquirrelDirectory
        )
        guard migrated == 0.05,
              migratedAgain == 0.05,
              explicitPreference == 0.08,
              invalidFallback == ChordSettings.defaultDuration,
              invalidStillFrozen == ChordSettings.defaultDuration,
              clampedLegacy == ChordSettings.range.upperBound,
              corruptPreference == ChordSettings.defaultDuration else {
            print(
                "FAILED: chord duration legacy migration",
                migrated,
                migratedAgain,
                explicitPreference,
                invalidFallback,
                invalidStillFrozen,
                clampedLegacy,
                corruptPreference
            )
            return false
        }
    } catch {
        print("FAILED: could not write chord migration fixtures", error)
        return false
    }

    func chordKey(_ character: Character) -> FlyChordKeyEvent {
        FlyChordKeyEvent(
            keycode: Int32(String(character).unicodeScalars.first!.value),
            mask: 0
        )
    }
    let leftQ = chordKey("q")
    let rightY = chordKey("y")
    var sameBatchLeftOnly = FlyChordBatchState()
    let sameBatchLeftDecision = sameBatchLeftOnly.stage(leftQ, policy: .sameBatchOnly)
    sameBatchLeftOnly.noteHandled(leftQ)
    let sameBatchLeftReplay = sameBatchLeftOnly.settle()

    var sameBatchCross = FlyChordBatchState()
    let sameBatchFirstDecision = sameBatchCross.stage(leftQ, policy: .sameBatchOnly)
    let sameBatchSecondDecision = sameBatchCross.stage(rightY, policy: .sameBatchOnly)
    sameBatchCross.noteHandled(leftQ)
    sameBatchCross.noteHandled(rightY)
    let sameBatchCrossReplay = sameBatchCross.settle()

    var mutualLeft = FlyChordBatchState()
    let mutualLeftDecision = mutualLeft.stage(leftQ, policy: .independentHalves)
    mutualLeft.noteHandled(leftQ)
    let mutualLeftReplay = mutualLeft.settle()

    var mutualRight = FlyChordBatchState()
    let mutualRightDecision = mutualRight.stage(rightY, policy: .independentHalves)
    mutualRight.noteHandled(rightY)
    let mutualRightReplay = mutualRight.settle()
    var pairingBoundary = FlyChordMutualPairingState()
    var settledLeftContext = RimeContextModel()
    settledLeftContext.input = "q"
    settledLeftContext.preedit = "q"
    settledLeftContext.cursorPos = 1
    settledLeftContext.selStart = 1
    settledLeftContext.selEnd = 1
    let expectedComplement = FlyChordMutualPairingState.SettledLeft(
        keys: [leftQ],
        baseInput: "",
        settledInput: "q",
        settledCursorPos: 1,
        settledSelStart: 1,
        settledSelEnd: 1,
        boundaryPlan: .init(before: false, after: false),
        insertedScalarCount: 1
    )
    pairingBoundary.recordSettledLeft(
        keys: [leftQ],
        baseInput: "",
        settledContext: settledLeftContext,
        boundaryPlan: .init(before: false, after: false),
        policy: .independentHalves,
        shape: .leftOnly
    )
    let singletonComplement = pairingBoundary.takeComplement(
        before: .rightOnly,
        currentKeyCount: 1,
        policy: .independentHalves,
        currentContext: settledLeftContext
    )
    pairingBoundary.recordSettledLeft(
        keys: [leftQ],
        baseInput: "",
        settledContext: settledLeftContext,
        boundaryPlan: .init(before: false, after: false),
        policy: .independentHalves,
        shape: .leftOnly
    )
    let multiKeyComplement = pairingBoundary.takeComplement(
        before: .rightOnly,
        currentKeyCount: 2,
        policy: .independentHalves,
        currentContext: settledLeftContext
    )
    pairingBoundary.recordSettledLeft(
        keys: [leftQ],
        baseInput: "",
        settledContext: settledLeftContext,
        boundaryPlan: .init(before: false, after: false),
        policy: .independentHalves,
        shape: .leftOnly
    )
    pairingBoundary.reset() // models apostrophe/edit/candidate/focus boundary
    let complementAfterBoundary = pairingBoundary.takeComplement(
        before: .rightOnly,
        currentKeyCount: 2,
        policy: .independentHalves,
        currentContext: settledLeftContext
    )
    var pairingAfterCursorMove = FlyChordMutualPairingState()
    pairingAfterCursorMove.recordSettledLeft(
        keys: [leftQ],
        baseInput: "",
        settledContext: settledLeftContext,
        boundaryPlan: .init(before: false, after: false),
        policy: .independentHalves,
        shape: .leftOnly
    )
    var movedCursorContext = settledLeftContext
    movedCursorContext.cursorPos = 0
    movedCursorContext.selStart = 0
    movedCursorContext.selEnd = 0
    let complementAfterCursorMove = pairingAfterCursorMove.takeComplement(
        before: .rightOnly,
        currentKeyCount: 2,
        policy: .independentHalves,
        currentContext: movedCursorContext
    )
    var sameBatchPairing = FlyChordMutualPairingState()
    sameBatchPairing.recordSettledLeft(
        keys: [leftQ],
        baseInput: "",
        settledContext: settledLeftContext,
        boundaryPlan: .init(before: false, after: false),
        policy: .sameBatchOnly,
        shape: .leftOnly
    )
    let sameBatchComplement = sameBatchPairing.takeComplement(
        before: .rightOnly,
        currentKeyCount: 2,
        policy: .sameBatchOnly,
        currentContext: settledLeftContext
    )

    guard FlyChordLayout.half(for: leftQ.keycode) == .left,
          FlyChordLayout.half(for: rightY.keycode) == .right,
          FlyChordLayout.half(for: RimeKey.space) == nil,
          sameBatchLeftDecision == .process([leftQ]),
          sameBatchLeftReplay == [leftQ],
          sameBatchFirstDecision == .process([leftQ]),
          sameBatchSecondDecision == .process([rightY]),
          sameBatchCrossReplay == [leftQ, rightY],
          mutualLeftDecision == .process([leftQ]),
          mutualLeftReplay == [leftQ],
          mutualRightDecision == .process([rightY]),
          mutualRightReplay == [rightY],
          singletonComplement == nil,
          multiKeyComplement == expectedComplement,
          sameBatchComplement == nil,
          complementAfterBoundary == nil,
          complementAfterCursorMove == nil,
          FlyChordRoutingRules.shouldStage(schemaID: "my_combo", asciiMode: false),
          FlyChordRoutingRules.shouldStage(schemaID: "my_combo",
                                           asciiMode: false,
                                           extensionEnabled: true),
          !FlyChordRoutingRules.shouldStage(schemaID: "my_combo",
                                            asciiMode: false,
                                            extensionEnabled: false),
          !FlyChordRoutingRules.shouldStage(schemaID: "my_combo", asciiMode: true),
          !FlyChordRoutingRules.shouldStage(schemaID: "english", asciiMode: false),
          ChordPressRoutingRules.accepts(mask: 0),
          [RimeKey.lockMask, RimeKey.shiftMask, RimeKey.controlMask,
           RimeKey.altMask, RimeKey.superMask, RimeKey.releaseMask,
           RimeKey.lockMask | RimeKey.controlMask].allSatisfy({ mask in
              !ChordPressRoutingRules.accepts(mask: mask)
          }),
          FlyChordBoundaryRules.plan(for: settledLeftContext)
            == .init(before: true, after: false),
          FlyChordBoundaryRules.plan(for: movedCursorContext)
            == .init(before: false, after: true),
          !FlyChordBoundaryRules.shouldInsert(forKeyCount: 1),
          FlyChordBoundaryRules.shouldInsert(forKeyCount: 2),
          FlyChordInputRollback.insertedScalarCount(before: "ni", after: "nshi") == 2,
          FlyChordInputRollback.insertedScalarCount(before: "ni", after: "ni") == 0,
          FlyChordInputRollback.insertedScalarCount(before: "ni", after: "n") == nil,
          FlyChordInputRollback.insertedScalarCount(before: "ni", after: "na") == nil else {
        print("FAILED: FlyYao same-batch/mutual batching policy")
        return false
    }

    let defaultsName = "RimeBuffer.InputConfigurationSmoke.\(UUID().uuidString)"
    guard let defaults = UserDefaults(suiteName: defaultsName) else {
        print("FAILED: input configuration defaults suite")
        return false
    }
    defer { defaults.removePersistentDomain(forName: defaultsName) }

    // A clean release profile starts on ordinary full pinyin with the chord
    // extension off. A stale F4 list cannot silently resurrect my_combo.
    defaults.removePersistentDomain(forName: defaultsName)
    let freshChordStore = ChordExtensionStore(defaults: defaults)
    let freshStore = InputConfigurationStore(
        defaults: defaults,
        chordExtensionStore: freshChordStore
    )
    guard !freshChordStore.isEnabled,
          freshStore.selectedSchemaID == "rime_ice",
          freshStore.lastOrdinarySchemaID == "rime_ice",
          freshStore.configuration == .defaultValue,
          !freshStore.adoptRuntimeSchema("my_combo"),
          !freshChordStore.isEnabled,
          freshStore.selectedSchemaID == "rime_ice" else {
        print("FAILED: fresh/default or stale runtime chord gate")
        return false
    }

    // A completed disable transaction remains authoritative even if a crash,
    // an older build, or a hand-edited preference leaves the v2 selected
    // schema pointing at my_combo. Reading the preference must retire that
    // residue, not interpret it as a new request to enable the extension.
    defaults.removePersistentDomain(forName: defaultsName)
    defaults.set(false, forKey: "chord.extension.enabled.v1")
    defaults.set("mutual",
                 forKey: "chord.extension.mode.v1")
    defaults.set("my_combo", forKey: "input.configuration.schemaID.v2")
    defaults.set("my_combo", forKey: "preferredSchema")
    defaults.set("wubi86",
                 forKey: "input.configuration.lastOrdinarySchemaID.v2")
    let disabledResidueChordStore = ChordExtensionStore(defaults: defaults)
    let disabledResidueStore = InputConfigurationStore(
        defaults: defaults,
        chordExtensionStore: disabledResidueChordStore
    )
    guard !disabledResidueChordStore.isEnabled,
          disabledResidueStore.selectedSchemaID == "wubi86",
          disabledResidueStore.lastOrdinarySchemaID == "wubi86",
          disabledResidueStore.configuration
            == .init(encoding: .wubi86, keyingMode: .sequential),
          defaults.string(forKey: "preferredSchema") == "wubi86" else {
        print("FAILED: explicitly disabled persisted chord residue")
        return false
    }

    defaults.removePersistentDomain(forName: defaultsName)
    defaults.set(false, forKey: "chord.extension.enabled.v1")
    defaults.set("mutual",
                 forKey: "chord.extension.mode.v1")
    defaults.set(InputEncoding.fullPinyin.rawValue,
                 forKey: "input.configuration.encoding.v1")
    defaults.set(KeyingMode.mutual.rawValue,
                 forKey: "input.configuration.keyingMode.v1")
    defaults.set("my_combo", forKey: "preferredSchema")
    defaults.set("double_pinyin",
                 forKey: "input.configuration.lastOrdinarySchemaID.v2")
    let disabledLegacyChordStore = ChordExtensionStore(defaults: defaults)
    let disabledLegacyStore = InputConfigurationStore(
        defaults: defaults,
        chordExtensionStore: disabledLegacyChordStore
    )
    guard !disabledLegacyChordStore.isEnabled,
          disabledLegacyStore.selectedSchemaID == "double_pinyin",
          disabledLegacyStore.configuration
            == .init(encoding: .naturalDoublePinyin,
                     keyingMode: .sequential) else {
        print("FAILED: explicitly disabled legacy chord residue")
        return false
    }

    // The removed fly-chord-learning switch governed only its learning page.
    // Its old default-enabled state must not opt an ordinary-schema user into
    // the new input feature.
    defaults.removePersistentDomain(forName: defaultsName)
    defaults.set([String](), forKey: "plugins.internal.disabledIDs")
    defaults.set("rime_ice", forKey: "preferredSchema")
    let legacyLearningOnlyChordStore = ChordExtensionStore(defaults: defaults)
    let legacyLearningOnlyStore = InputConfigurationStore(
        defaults: defaults,
        chordExtensionStore: legacyLearningOnlyChordStore
    )
    guard !legacyLearningOnlyChordStore.isEnabled,
          legacyLearningOnlyStore.selectedSchemaID == "rime_ice" else {
        print("FAILED: legacy learning-only switch enabled chord input")
        return false
    }

    let canonicalProfiles: [(String, InputConfiguration)] = [
        ("rime_ice", .init(encoding: .fullPinyin, keyingMode: .sequential)),
        ("double_pinyin", .init(encoding: .naturalDoublePinyin,
                                keyingMode: .sequential)),
        ("double_pinyin_flypy", .init(encoding: .xiaoheDoublePinyin,
                                      keyingMode: .sequential)),
        ("wubi86", .init(encoding: .wubi86, keyingMode: .sequential)),
        ("lingxi_wubi", .init(encoding: .lingxiWubi, keyingMode: .sequential)),
        ("english", .init(encoding: .english, keyingMode: .sequential)),
    ]
    for (schemaID, configuration) in canonicalProfiles {
        defaults.removePersistentDomain(forName: defaultsName)
        defaults.set(schemaID, forKey: "preferredSchema")
        let chordStore = ChordExtensionStore(defaults: defaults)
        let store = InputConfigurationStore(
            defaults: defaults,
            chordExtensionStore: chordStore
        )
        guard store.configuration == configuration,
              store.runtimeProfile.schemaID == schemaID,
              !chordStore.isEnabled else {
            print("FAILED: ordinary preferredSchema migration", schemaID)
            return false
        }
    }

    // Every legacy FlyYao mode now migrates to unified 并击. A previous strict
    // v2 setting must not resurrect the removed same-batch-only product mode.
    defaults.removePersistentDomain(forName: defaultsName)
    defaults.set(InputEncoding.fullPinyin.rawValue,
                 forKey: "input.configuration.encoding.v1")
    defaults.set(KeyingMode.chord.rawValue,
                 forKey: "input.configuration.keyingMode.v1")
    defaults.set("my_combo", forKey: "preferredSchema")
    defaults.set([ChordExtensionStore.pluginID],
                 forKey: "plugins.internal.disabledIDs")
    let migratedFlyYaoChordStore = ChordExtensionStore(defaults: defaults)
    let migratedFlyYaoStore = InputConfigurationStore(
        defaults: defaults,
        chordExtensionStore: migratedFlyYaoChordStore
    )
    guard migratedFlyYaoStore.configuration
            == .init(encoding: .fullPinyin, keyingMode: .chord),
          migratedFlyYaoStore.selectedSchemaID == "my_combo",
          migratedFlyYaoChordStore.isEnabled,
          migratedFlyYaoChordStore.settlementPolicy == .independentHalves else {
        print("FAILED: legacy FlyYao unified chord migration")
        return false
    }

    defaults.removePersistentDomain(forName: defaultsName)
    defaults.set(InputEncoding.fullPinyin.rawValue,
                 forKey: "input.configuration.encoding.v1")
    defaults.set(KeyingMode.chord.rawValue,
                 forKey: "input.configuration.keyingMode.v1")
    defaults.set(99, forKey: "input.configuration.keyingMode.semantics.v2")
    defaults.set("my_combo", forKey: "preferredSchema")
    let explicitChordExtension = ChordExtensionStore(defaults: defaults)
    weak var weakExplicitStore: InputConfigurationStore?
    let safeExplicitChordExtension = ChordExtensionStore(
        defaults: defaults,
        fallbackBeforeDisable: {
            _ = weakExplicitStore?.fallBackFromChordScheme()
        }
    )
    // Bootstrap the same defaults once through the store used by the runtime
    // transaction; constructing an unused projection must not change it.
    _ = explicitChordExtension.bootstrap()
    let explicitChordStore = InputConfigurationStore(
        defaults: defaults,
        chordExtensionStore: safeExplicitChordExtension
    )
    weakExplicitStore = explicitChordStore
    guard safeExplicitChordExtension.isEnabled,
          safeExplicitChordExtension.settlementPolicy == .independentHalves,
          explicitChordStore.configuration
            == .init(encoding: .fullPinyin, keyingMode: .chord),
          explicitChordStore.adoptRuntimeSchema("my_combo"),
          explicitChordStore.configuration
            == .init(encoding: .fullPinyin, keyingMode: .chord),
          defaults.integer(forKey: "input.configuration.keyingMode.semantics.v2") == 99,
          explicitChordStore.select(schemaID: "wubi86"),
          explicitChordStore.configuration
            == .init(encoding: .wubi86, keyingMode: .sequential),
          explicitChordStore.select(schemaID: "my_combo"),
          explicitChordStore.configuration
            == .init(encoding: .fullPinyin, keyingMode: .chord),
          safeExplicitChordExtension.setEnabled(false, source: .pluginLifecycle),
          !safeExplicitChordExtension.isEnabled,
          explicitChordStore.selectedSchemaID == "wubi86",
          explicitChordStore.lastOrdinarySchemaID == "wubi86",
          explicitChordStore.configuration
            == .init(encoding: .wubi86, keyingMode: .sequential),
          !explicitChordStore.adoptRuntimeSchema("my_combo"),
          !safeExplicitChordExtension.isEnabled else {
        print("FAILED: explicit FlyYao selection or safe-disable fallback")
        return false
    }

    defaults.removePersistentDomain(forName: defaultsName)
    defaults.set("unknown", forKey: "preferredSchema")
    let fallbackStore = InputConfigurationStore(defaults: defaults)
    guard fallbackStore.configuration == .defaultValue,
          !fallbackStore.select(schemaID: "unknown"),
          fallbackStore.configuration == .defaultValue else {
        print("FAILED: invalid configuration fallback")
        return false
    }

    do {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let initial = """
        patch:
          schema_list:
            - schema: my_serial
            - schema: melt_eng
          menu:
            page_size: 9
          custom_flag: keep-me
        """
        try initial.write(to: config, atomically: true, encoding: .utf8)
        let chordEnabledIDs = InputSchemaCatalog.enabledIDs(
            chordExtensionEnabled: true
        )
        try SchemaListStore.writeEnabledIDs(chordEnabledIDs, to: config)
        let ids = SchemaListStore.enabledIDs(at: config)
        let rewritten = try String(contentsOf: config, encoding: .utf8)
        guard ids == chordEnabledIDs,
              rewritten.contains("page_size: 9"),
              rewritten.contains("custom_flag: keep-me"),
              !rewritten.contains("my_serial"),
              !rewritten.contains("melt_eng") else {
            print("FAILED: schema-list rewrite", ids, rewritten)
            return false
        }

        try SchemaListStore.writeEnabledIDs(
            InputSchemaCatalog.defaultEnabledIDs,
            to: config
        )
        guard SchemaListStore.enabledIDs(at: config)
                == InputSchemaCatalog.defaultEnabledIDs,
              !((try String(contentsOf: config, encoding: .utf8))
                    .contains("- schema: my_combo")) else {
            print("FAILED: disabled chord schema remained in F4 list")
            return false
        }

        do {
            try SchemaListStore.writeEnabledIDs([], to: config)
            print("FAILED: empty schema selection was accepted")
            return false
        } catch SchemaListStore.StoreError.emptySelection {
            // Expected.
        }
    } catch {
        print("FAILED: schema-list store error:", error)
        return false
    }

    print("schema-list store smoke: OK")
    return true
}

func runStatsSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) key-frequency smoke test ==")
    let root = URL(fileURLWithPath: NSTemporaryDirectory())
        .appendingPathComponent("rimebuffer-key-frequency-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: root) }

    let calendar = Calendar(identifier: .gregorian)
    let day1 = calendar.date(from: DateComponents(year: 2026, month: 7, day: 5))!
    let day2 = calendar.date(from: DateComponents(year: 2026, month: 7, day: 6))!

    let store1 = KeyFrequencyStore(storageRoot: root, autosaveDelay: 999) { day1 }
    store1.record(keyCode: 0)       // A
    store1.record(keyCode: 0)       // A
    store1.record(keyCode: 8)       // C
    store1.recordModifierPress(keyCode: 55, flags: [.command])
    store1.recordModifierPress(keyCode: 55, flags: [])   // release, ignored
    store1.saveNow()

    let reloaded = KeyFrequencyStore(storageRoot: root, autosaveDelay: 999) { day1 }
    let day1Snapshot = reloaded.snapshot(for: day1)
    guard day1Snapshot.counts["KeyA"] == 2,
          day1Snapshot.counts["KeyC"] == 1,
          day1Snapshot.counts["LeftCommand"] == 1,
          day1Snapshot.total == 4 else {
        print("FAILED: day1 snapshot", day1Snapshot)
        return false
    }

    let store2 = KeyFrequencyStore(storageRoot: root, autosaveDelay: 999) { day2 }
    store2.record(keyCode: 11)      // B
    store2.saveNow()
    guard store2.snapshot(for: day1).total == 4,
          store2.snapshot(for: day2).counts["KeyB"] == 1 else {
        print("FAILED: day bucket isolation")
        return false
    }

    store2.clear(day: day1)
    guard store2.snapshot(for: day1).total == 0,
          store2.snapshot(for: day2).total == 1 else {
        print("FAILED: clear day")
        return false
    }

    store2.clear(day: nil)
    guard store2.snapshot(for: day2).total == 0 else {
        print("FAILED: clear all")
        return false
    }

    print("stats smoke: OK")
    return true
}

/// Pins the matrix viewport math. The three-row cap is visual only: the window
/// must slide far enough to reach the last page, never past the ends, and must
/// always keep the selected row on screen — a wrong base here would render one
/// row while selecting a different page's candidate.
/// Fast typing exposed a commit fade: candidates stayed nearly opaque beside
/// text the host already showed. A commit must remove the panel before the
/// host insertion. Fresh and continued compositions must both appear immediately.
func runCandidateCommitSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) candidate commit smoke test ==")
    guard let snapshot = CandidateWindow.commitRetirementSnapshotForSmoke() else {
        print("candidate-commit-smoke: SKIPPED (secure input forbids ordering the panel front)")
        return true
    }
    let checks: [(String, Bool)] = [
        ("panel presented", snapshot.presented),
        ("another owner's commit leaves the panel", snapshot.strangerRetirementIgnored),
        ("commit orders the panel out synchronously", snapshot.orderedOutSynchronously),
        ("commit retires the logical presentation", snapshot.retiredLogicalPresentation),
        ("same-keystroke composition continues without an entrance",
         snapshot.sameKeystrokeContinuedWithoutEntrance),
        ("ordinary hide is immediate and retires content", snapshot.ordinaryHideIsImmediate),
        ("fresh composition is immediately opaque", snapshot.laterEntranceIsImmediate),
    ]
    let failures = checks.filter { !$0.1 }.map(\.0)
    guard failures.isEmpty else {
        print("FAILED: \(failures.joined(separator: "; "))")
        return false
    }
    print("candidate-commit-smoke: OK (immediate show/hide, synchronous commit retirement, stale owner, 顶字 continuation)")
    return true
}

func runCandidateMatrixSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) candidate matrix smoke test ==")
    let maxRows = CandidateWindow.expandedMaxRows
    guard maxRows == 3 else {
        print("FAILED: expected a three-row viewport, got \(maxRows)")
        return false
    }

    // The third row is the boundary that used to retain a compact 24pt
    // document height. Pin all transitions and the three-row viewport cap.
    let rowHeight: CGFloat = 24
    let expectedViewportHeights: [CGFloat] = [24, 51, 78, 78]
    for (index, expectedHeight) in expectedViewportHeights.enumerated() {
        let rowCount = index + 1
        let height = CandidateWindow.matrixViewportHeight(rowHeight: rowHeight,
                                                          rowCount: rowCount)
        guard height == expectedHeight else {
            print("FAILED: \(rowCount)-row viewport expected \(expectedHeight)pt, got \(height)pt")
            return false
        }
    }

    // Walking ↓ through six pages: the window pins to the top until the
    // selection leaves it, then trails the selection one row at a time.
    let walkDown = [0, 0, 0, 1, 2, 3]
    var base = 0
    for selection in 0..<6 {
        base = CandidateWindow.windowBase(selection: selection, currentBase: base, pageCount: 6)
        guard base == walkDown[selection] else {
            print("FAILED: ↓ to row \(selection) expected base \(walkDown[selection]), got \(base)")
            return false
        }
    }
    // ...and walking back ↑ retraces it, so the first page stays reachable.
    let walkUp = [0, 1, 2, 3, 3, 3]
    for selection in (0..<6).reversed() {
        base = CandidateWindow.windowBase(selection: selection, currentBase: base, pageCount: 6)
        guard base == walkUp[selection] else {
            print("FAILED: ↑ to row \(selection) expected base \(walkUp[selection]), got \(base)")
            return false
        }
    }

    // Fewer pages than rows must never scroll.
    for pageCount in 1...maxRows {
        for selection in 0..<pageCount {
            let b = CandidateWindow.windowBase(selection: selection, currentBase: 0, pageCount: pageCount)
            guard b == 0 else {
                print("FAILED: \(pageCount) pages should not scroll, got base \(b)")
                return false
            }
        }
    }

    // Invariants over every reachable (pageCount, base, selection): the base is
    // in range and its window contains the selection — from any starting base,
    // so a click or a re-fetch can never strand the selection off screen.
    for pageCount in 1...12 {
        for startBase in 0..<pageCount {
            for selection in 0..<pageCount {
                let b = CandidateWindow.windowBase(selection: selection,
                                                   currentBase: startBase,
                                                   pageCount: pageCount)
                let maxBase = max(0, pageCount - maxRows)
                guard b >= 0, b <= maxBase else {
                    print("FAILED: base \(b) out of 0...\(maxBase) (pages=\(pageCount) sel=\(selection))")
                    return false
                }
                guard selection >= b, selection < b + maxRows else {
                    print("FAILED: row \(selection) outside window \(b)..<\(b + maxRows) (pages=\(pageCount))")
                    return false
                }
            }
        }
    }

    // The last page must be reachable: its window is the final one.
    for pageCount in 4...12 {
        let b = CandidateWindow.windowBase(selection: pageCount - 1, currentBase: 0, pageCount: pageCount)
        guard b == pageCount - maxRows else {
            print("FAILED: last page of \(pageCount) expected base \(pageCount - maxRows), got \(b)")
            return false
        }
    }

    // --- Column viewport ---------------------------------------------------
    // A page_size:9 row that fits (9*40 + 8 separators*8 = 424 <= 460): the
    // common case, where the viewport must never scroll.
    let roomy = Array(repeating: CGFloat(40), count: 9)
    guard CandidateWindow.fittedColumnCount(widths: roomy, separator: 8, available: 460, base: 0) == 9 else {
        print("FAILED: nine 40pt candidates should all fit in 460pt")
        return false
    }
    for selection in 0..<9 {
        let b = CandidateWindow.columnBase(selection: selection, currentBase: 0, widths: roomy,
                                           separator: 8, available: 460)
        guard b == 0 else {
            print("FAILED: a row that fits must not scroll, got base \(b) for column \(selection)")
            return false
        }
    }

    // Narrow row: only some columns fit, so the tail is reachable only if the
    // viewport scrolls — this is the case the fix exists for.
    let tight = Array(repeating: CGFloat(100), count: 9)
    let fits = CandidateWindow.fittedColumnCount(widths: tight, separator: 8, available: 320, base: 0)
    guard fits > 0, fits < 9 else {
        print("FAILED: expected a partially fitting row, got \(fits)/9")
        return false
    }
    // Every candidate must be reachable, and the window must contain it.
    var colBase = 0
    for selection in 0..<9 {
        colBase = CandidateWindow.columnBase(selection: selection, currentBase: colBase, widths: tight,
                                             separator: 8, available: 320)
        let count = CandidateWindow.fittedColumnCount(widths: tight, separator: 8,
                                                      available: 320, base: colBase)
        guard selection >= colBase, selection < colBase + count else {
            print("FAILED: column \(selection) unreachable — window \(colBase)..<\(colBase + count)")
            return false
        }
    }
    // ...and walking back left retraces to the first column.
    for selection in (0..<9).reversed() {
        colBase = CandidateWindow.columnBase(selection: selection, currentBase: colBase, widths: tight,
                                             separator: 8, available: 320)
        let count = CandidateWindow.fittedColumnCount(widths: tight, separator: 8,
                                                      available: 320, base: colBase)
        guard selection >= colBase, selection < colBase + count else {
            print("FAILED: column \(selection) unreachable going back — window \(colBase)..<\(colBase + count)")
            return false
        }
    }
    guard colBase == 0 else {
        print("FAILED: walking left should return to column 0, got base \(colBase)")
        return false
    }

    // A single candidate wider than the whole row still renders (and stays
    // selectable) rather than vanishing.
    guard CandidateWindow.fittedColumnCount(widths: [900, 50], separator: 8, available: 460, base: 0) == 1 else {
        print("FAILED: an over-wide candidate must still occupy one column")
        return false
    }

    // Every matrix row lays itself out independently. A long phrase in the
    // first row must neither widen cells nor reduce the visible count below it.
    let longRow = [CGFloat(250), 40, 40, 40, 40, 40]
    let shortRow = Array(repeating: CGFloat(40), count: 6)
    let longRange = CandidateWindow.fittedColumnRange(
        widths: longRow, separator: 8, available: 320, base: 0
    )
    let shortRange = CandidateWindow.fittedColumnRange(
        widths: shortRow, separator: 8, available: 320, base: 0
    )
    guard longRange == 0..<2, shortRange == 0..<6 else {
        print("FAILED: matrix rows did not fit independently", longRange, shortRange)
        return false
    }
    let scrolledShortRange = CandidateWindow.fittedColumnRange(
        widths: shortRow, separator: 8, available: 120, base: 3
    )
    guard scrolledShortRange == 3..<5 else {
        print("FAILED: per-row viewport base was not preserved", scrolledShortRange)
        return false
    }

    print("candidate matrix smoke OK")
    return true
}

/// Pins the echo guard — the workbench's rule that a peer's text is never
/// mirrored back. A wrong verdict here means two paired Macs bounce text
/// forever (or, the other way, that a legitimately-typed block silently fails
/// to reach the other Mac). Also checks the block-origin plumbing: appended
/// text carries its origin, and the send path can read it back.
/// Pins the inbound gating: trusted sources drop straight to the buffer, `ask`
/// sources wait as pending, blocked sources vanish, the pending cap holds, and
/// accept moves an item into the buffer carrying its origin. A wrong verdict
/// here would let unverified external text reach the buffer without review.
func runInboundBusSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) inbound bus smoke test ==")

    // DNS-rebinding defence: command-line clients omit Origin, while browser
    // origins must parse to an exact loopback host (prefix lookalikes are not local).
    let allowedOrigins: [String?] = [
        nil,
        "http://localhost",
        "https://localhost:47700",
        "http://127.0.0.1:47700",
        "http://[::1]:47700",
    ]
    guard allowedOrigins.allSatisfy({ LocalGateway.isAllowedOrigin($0) }) else {
        print("FAILED: a valid loopback Origin was rejected")
        return false
    }
    let rejectedOrigins = [
        "",
        "null",
        "file://localhost",
        "http://localhost.evil.example",
        "http://127.0.0.1.evil.example",
        "http://localhost@evil.example",
        "http://%6cocalhost",
        "http://localhost:99999",
        "http://localhost/",
        "https://example.com",
        "http://localhost?redirect=evil",
        "http://localhost/#fragment",
    ]
    guard rejectedOrigins.allSatisfy({ !LocalGateway.isAllowedOrigin($0) }) else {
        print("FAILED: a non-loopback or malformed Origin was accepted")
        return false
    }

    // A CLI smoke must not persist fixtures into the user's real Mailbox.
    let bus = InboundBus()
    let model = BufferModel.shared
    let oldEnabled = model.enabled
    defer { model.discardForPrivacy(); bus.clear(); model.enabled = oldEnabled }
    model.enabled = true
    model.discardForPrivacy(); bus.clear()

    // Trust defaults: mcp/http/plugin = ask, marine = trusted.
    guard bus.trust(for: .mcp(client: "x")) == .ask,
          bus.trust(for: .http(source: "s")) == .ask,
          bus.trust(for: .plugin(id: "example")) == .ask,
          bus.trust(for: .marine) == .trusted else {
        print("FAILED: trust defaults wrong")
        return false
    }

    // ask source → pending, NOT in the buffer yet.
    let id = bus.submit(
        origin: .mcp(client: "codex"),
        text: "## 草稿一",
        title: "t",
        format: .markdown
    )
    guard let id,
          bus.pendingCount == 1,
          bus.pending.first?.format == .markdown,
          model.blocks.isEmpty else {
        print("FAILED: ask source should wait in pending, not enter buffer")
        return false
    }
    // trusted source → straight to buffer, no pending.
    _ = bus.submit(origin: .marine, text: "marine 草稿")
    guard bus.pendingCount == 1, model.blocks.count == 1,
          model.blocks[0].origin == .marine else {
        print("FAILED: trusted source should drop into buffer")
        return false
    }
    // accept the pending mcp item → becomes a buffer block with mcp origin.
    bus.accept(id)
    guard bus.pendingCount == 0, model.blocks.count == 2,
          model.blocks.contains(where: { $0.origin == .mcp(client: "codex") }) else {
        print("FAILED: accept should move item into buffer with its origin")
        return false
    }
    // Externally-staged content must be marked so the switch-app reset spares it
    // (otherwise focusing the target field wipes what MCP/远端 just delivered).
    guard model.holdsExternalContent else {
        print("FAILED: buffer with external blocks must report holdsExternalContent")
        return false
    }
    // reject removes without touching the buffer.
    let rid = bus.submit(origin: .http(source: "s"), text: "拒绝我")
    bus.reject(rid!)
    guard bus.pendingCount == 0, model.blocks.count == 2 else {
        print("FAILED: reject should drop the item, not deliver it")
        return false
    }

    // Streaming: text updates in place, one pending item, endStream settles it.
    model.discardForPrivacy(); bus.clear()
    _ = bus.beginStream(
        origin: .mcp(client: "a"),
        streamID: "s1",
        format: .json
    )
    bus.appendStream(streamID: "s1", delta: "部分")
    bus.appendStream(streamID: "s1", delta: "文本")
    guard bus.pendingCount == 1,
          bus.pending[0].text == "部分文本",
          bus.pending[0].format == .json,
          bus.pending[0].streaming else {
        print("FAILED: streaming should update one item in place")
        return false
    }
    guard !bus.canAccept(bus.pending[0].id),
          !bus.accept(bus.pending[0].id),
          bus.pendingCount == 1,
          model.blocks.isEmpty else {
        print("FAILED: a streaming item must remain pending and cannot stage a partial snapshot")
        return false
    }
    bus.endStream(streamID: "s1")
    guard !bus.pending[0].streaming,
          bus.canAccept(bus.pending[0].id) else {
        print("FAILED: endStream should settle the item")
        return false
    }
    let settledID = bus.pending[0].id
    guard bus.accept(settledID),
          bus.pendingCount == 0,
          model.blocks.count == 1,
          model.blocks[0].text == "部分文本",
          model.blocks[0].origin == .mcp(client: "a"),
          !bus.accept(settledID),
          model.blocks.count == 1 else {
        print("FAILED: a settled stream must be accepted exactly once")
        return false
    }

    // Pending cap holds.
    bus.clear()
    for i in 0..<(InboundBus.maxPending + 10) { _ = bus.submit(origin: .mcp(client: "a"), text: "x\(i)") }
    guard bus.pendingCount == InboundBus.maxPending else {
        print("FAILED: pending cap not enforced, got \(bus.pendingCount)")
        return false
    }

    // Capacity limits reject atomically. A success response must never hide a
    // truncated complete push or streaming tail.
    bus.clear()
    let oversizedText = String(repeating: "x", count: InboundBus.maxTextCount + 1)
    guard bus.submitDetailed(
        origin: .http(source: "oversized"),
        text: oversizedText
    ) == .rejected(.tooLarge),
          bus.pending.isEmpty else {
        print("FAILED: oversized complete inbound text was not rejected atomically")
        return false
    }
    guard bus.beginStream(
        origin: .mcp(client: "oversized"),
        streamID: "oversized-stream"
    ) != nil,
          bus.appendStream(
            streamID: "oversized-stream",
            delta: String(repeating: "x", count: InboundBus.maxTextCount)
          ) == .appended,
          bus.appendStream(
            streamID: "oversized-stream",
            delta: "tail"
          ) == .tooLarge,
          bus.pending.first?.text.count == InboundBus.maxTextCount else {
        print("FAILED: oversized stream tail was not rejected atomically")
        return false
    }
    bus.clear()

    // Metadata-free plugin pushes (including restored Mailbox reviews) become
    // explicit local plain text. They carry no fabricated runtime/focus grant
    // and may safely enter both ordinary delivery and AI source preparation.
    model.discardForPrivacy(); bus.clear()
    guard let pluginReviewID = bus.submit(
        origin: .plugin(id: "restored-plugin"),
        text: "已审核插件文本"
    ),
          bus.accept(pluginReviewID),
          model.blocks.count == 1,
          model.blocks[0].pluginMetadata == nil,
          model.blocks[0].locallyReviewedAsPlainText,
          AITextSourcePolicy.accepts(model.blocks) else {
        print("FAILED: metadata-free plugin review was not safe plain text")
        return false
    }

    // Production-style buses must never claim an item is pending when its
    // Mailbox association could not be committed. This fixture fills the
    // durable thread capacity and covers complete plus streamed submissions.
    let persistenceRoot = FileManager.default.temporaryDirectory
        .appendingPathComponent(
            "rimebuffer-inbound-persistence-smoke-\(UUID().uuidString)",
            isDirectory: true
        )
    do {
        defer { try? FileManager.default.removeItem(at: persistenceRoot) }
        let persistenceStore = try MailboxStore(
            storageRoot: persistenceRoot,
            limits: MailboxStoreLimits(
                maximumThreads: 1,
                maximumMessagesPerThread: 4,
                maximumMessageCharacters: 4_096,
                maximumFileBytes: 1_048_576
            )
        )
        _ = try persistenceStore.createInboundThread(
            source: .http(source: "capacity fixture"),
            body: "occupy the only durable thread"
        )
        let durableBus = InboundBus(mailboxStore: persistenceStore)
        guard durableBus.submitDetailed(
            origin: .http(source: "must fail"),
            text: "not durable"
        ) == .rejected(.persistenceUnavailable),
              durableBus.pending.isEmpty else {
            print("FAILED: complete persistence failure left hidden pending data")
            return false
        }
        guard durableBus.beginStream(
            origin: .mcp(client: "must fail"),
            streamID: "persistence-failure"
        ) != nil else {
            print("FAILED: stream persistence fixture did not start")
            return false
        }
        durableBus.appendStream(
            streamID: "persistence-failure",
            delta: "not durable"
        )
        guard !durableBus.endStream(streamID: "persistence-failure"),
              durableBus.pending.isEmpty else {
            print("FAILED: stream persistence failure left hidden pending data")
            return false
        }
    } catch {
        print("FAILED: inbound persistence fixture threw \(error)")
        return false
    }

    // A purely locally-typed buffer must NOT count as external, so the switch-
    // app reset still clears normal typing.
    model.discardForPrivacy(); bus.clear()
    model.append("本地打字")   // origin defaults to .rime
    guard !model.holdsExternalContent else {
        print("FAILED: locally-typed buffer must not report holdsExternalContent")
        return false
    }

    print("inbound bus smoke OK")
    return true
}

private final class ActionPluginURLProtocolStub: URLProtocol {
    static var handler: ((URLRequest) throws -> (Int, Data))?

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        do {
            guard let handler = Self.handler,
                  let url = request.url else {
                throw ActionPluginHTTPError.invalidResponse
            }
            let (status, data) = try handler(request)
            let response = HTTPURLResponse(url: url,
                                           statusCode: status,
                                           httpVersion: "HTTP/1.1",
                                           headerFields: ["Content-Type": "application/json"])!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: data)
            client?.urlProtocolDidFinishLoading(self)
        } catch {
            client?.urlProtocol(self, didFailWithError: error)
        }
    }

    override func stopLoading() {}
}

private final class ActionPluginTransportStub: ActionPluginTransport {
    var statusResult: Result<ActionPluginStatus, Error>
    var invokeResult: Result<ActionPluginInvokeResponse, Error>
    var prepareResult: Result<ActionPluginPrepareResponse, Error>?
    var holdInvoke = false
    var holdStatus = false
    private(set) var statusRequestCount = 0
    private(set) var prepareRequestCount = 0
    private(set) var invokeRequestCount = 0
    private(set) var statusBindings: [ActionPluginRuntimeBinding?] = []
    private(set) var invokeBindings: [ActionPluginRuntimeBinding] = []
    let defaultBinding: ActionPluginRuntimeBinding
    private var pendingInvokes: [(
        completion: (Result<ActionPluginInvokeResponse, Error>) -> Void,
        result: Result<ActionPluginInvokeResponse, Error>
    )] = []
    private var pendingStatus: (
        completion: (Result<ActionPluginStatusSnapshot, Error>) -> Void,
        result: Result<ActionPluginStatusSnapshot, Error>
    )?

    init(status: ActionPluginStatus,
         response: ActionPluginInvokeResponse,
         binding: ActionPluginRuntimeBinding? = nil) {
        statusResult = .success(status)
        invokeResult = .success(response)
        defaultBinding = binding ?? ActionPluginRuntimeBinding(
            config: ActionPluginRuntimeConfig(pluginId: "example",
                                              apiBase: "http://127.0.0.1:47701/v1/plugin",
                                              token: "stub-token",
                                              updatedAt: 1,
                                              instanceId: "stub-instance",
                                              processId: 1)
        )
    }

    func fetchStatus(plugin: InstalledActionPlugin,
                     action: ActionPluginDefinition,
                     binding: ActionPluginRuntimeBinding?,
                     completion: @escaping (Result<ActionPluginStatusSnapshot, Error>) -> Void) {
        statusRequestCount += 1
        statusBindings.append(binding)
        let resolvedBinding = binding ?? defaultBinding
        let result = statusResult.map {
            ActionPluginStatusSnapshot(value: $0, binding: resolvedBinding)
        }
        if holdStatus {
            pendingStatus = (completion, result)
        } else {
            completion(result)
        }
    }

    func invoke(plugin: InstalledActionPlugin,
                action: ActionPluginDefinition,
                binding: ActionPluginRuntimeBinding,
                request payload: ActionPluginInvokeRequest,
                onStreamEvent: @escaping (ActionPluginStreamEvent) -> Void,
                completion: @escaping (Result<ActionPluginInvokeResponse, Error>) -> Void)
        -> ActionPluginInvocationCancellable? {
        invokeRequestCount += 1
        invokeBindings.append(binding)
        let result = invokeResult.map { template in
            ActionPluginInvokeResponse(requestId: payload.requestId,
                                       actionId: payload.actionId,
                                       contextId: payload.contextId,
                                       blocks: template.blocks,
                                       targetSummary: template.targetSummary)
        }
        if holdInvoke {
            pendingInvokes.append((completion, result))
        } else {
            completion(result)
        }
        return nil
    }

    func prepare(plugin: InstalledActionPlugin,
                 action: ActionPluginDefinition,
                 binding: ActionPluginRuntimeBinding,
                 request payload: ActionPluginInvokeRequest,
                 completion: @escaping (Result<ActionPluginPrepareResponse, Error>) -> Void)
        -> ActionPluginInvocationCancellable? {
        prepareRequestCount += 1
        guard let prepareResult else {
            completion(.failure(ActionPluginHTTPError.invalidResponse))
            return nil
        }
        completion(prepareResult.map { template in
            ActionPluginPrepareResponse(
                protocolVersion: template.protocolVersion,
                resultFormat: template.resultFormat,
                pluginId: payload.pluginId ?? template.pluginId,
                runtimeInstanceId: payload.runtimeInstanceId ?? template.runtimeInstanceId,
                requestId: payload.requestId,
                actionId: payload.actionId,
                contextId: payload.contextId,
                prompt: template.prompt,
                targetSummary: template.targetSummary
            )
        })
        return nil
    }

    func completeHeldInvoke() {
        guard !pendingInvokes.isEmpty else { return }
        let pending = pendingInvokes.removeFirst()
        pending.completion(pending.result)
    }

    func completeHeldStatus() {
        let pending = pendingStatus
        pendingStatus = nil
        guard let pending else { return }
        pending.completion(pending.result)
    }
}

private final class ActionPluginFocusBox {
    var token: FocusToken?
    var secureInput = false

    var access: ActionPluginFocusAccess {
        ActionPluginFocusAccess(
            currentToken: { [weak self] in self?.token },
            isValid: { [weak self] expected in self?.token == expected },
            secureInputEnabled: { [weak self] in self?.secureInput ?? true }
        )
    }
}

private final class PreparedActionConnectorStub: AITextProvider {
    let kind: AITextProviderKind = .codexCLI
    var availability: AITextProviderAvailability = .ready
    private(set) var requests: [AITextProviderRequest] = []
    var blocks = [AITextProviderBlock(index: 0,
                                      text: "连接器生成结果",
                                      title: "候选")]
    var holdsCompletion = false
    private var pending: (onEvent: (AITextProviderEvent) -> Void,
                          completion: (Result<[AITextProviderBlock], AITextProviderError>) -> Void)?

    func generate(_ request: AITextProviderRequest,
                  onEvent: @escaping (AITextProviderEvent) -> Void,
                  completion: @escaping (Result<[AITextProviderBlock], AITextProviderError>) -> Void)
        -> any AITextCancellable {
        requests.append(request)
        onEvent(.activity(AITextProviderActivity(
            kind: .reasoning,
            message: "正在分析页面话术"
        )))
        if holdsCompletion {
            pending = (onEvent, completion)
        } else {
            blocks.forEach { onEvent(.blockSnapshot($0)) }
            completion(.success(blocks))
        }
        return AITextNoopCancellation()
    }

    func complete() {
        guard let pending else { return }
        self.pending = nil
        blocks.forEach { pending.onEvent(.blockSnapshot($0)) }
        pending.completion(.success(blocks))
    }

    func emitHeld(_ block: AITextProviderBlock) {
        pending?.onEvent(.blockSnapshot(block))
    }
}

private final class ActionPluginLoaderBox {
    var plugins: [InstalledActionPlugin]

    init(_ plugins: [InstalledActionPlugin]) {
        self.plugins = plugins
    }

    func load(_: URL) -> [InstalledActionPlugin] { plugins }
}

private func runMainLoopUntil(timeout: TimeInterval = 1,
                              _ predicate: () -> Bool) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    repeat {
        if predicate() { return true }
        _ = RunLoop.main.run(mode: .default,
                             before: min(deadline, Date().addingTimeInterval(0.01)))
    } while Date() < deadline
    return predicate()
}

private func runContextualActionPresentationSmokeTest() -> Bool {
    let presentationId = "contextual.generate-comment"
    let presentationKey = ActionPluginPresentationKey(
        pluginId: "contextual",
        presentationId: presentationId
    )
    let directAction = ActionPluginDefinition(
        id: "contextual.generate-direct",
        title: "生成直评",
        symbol: "bubble.left.and.text.bubble.right",
        statusPath: "/status",
        preparePath: "/prepare",
        invokePath: "/invoke",
        modes: ["direct"],
        presentationId: presentationId,
        presentationTitle: "生成评论"
    )
    let replyAction = ActionPluginDefinition(
        id: "contextual.generate-reply",
        title: "生成回复",
        symbol: "arrowshape.turn.up.left",
        statusPath: "/status",
        preparePath: "/prepare",
        invokePath: "/invoke",
        modes: ["reply"],
        presentationId: presentationId,
        presentationTitle: "生成评论"
    )
    let manifest = ActionPluginManifest(
        schemaVersion: 1,
        id: "contextual",
        name: "Contextual",
        version: "1",
        runtimeConfigPaths: ["runtime.json"],
        actions: [directAction, replyAction]
    )
    guard ActionPluginManifestLoader.validate(manifest) else {
        print("FAILED: contextual presentation manifest validation")
        return false
    }
    let overlappingAction = ActionPluginDefinition(
        id: "contextual.generate-overlap",
        title: "错误重叠动作",
        symbol: "exclamationmark.triangle",
        statusPath: "/status",
        preparePath: "/prepare",
        invokePath: "/invoke",
        modes: ["direct"],
        presentationId: presentationId,
        presentationTitle: "生成评论"
    )
    let overlappingManifest = ActionPluginManifest(
        schemaVersion: manifest.schemaVersion,
        id: manifest.id,
        name: manifest.name,
        version: manifest.version,
        runtimeConfigPaths: manifest.runtimeConfigPaths,
        actions: [directAction, overlappingAction]
    )
    guard !ActionPluginManifestLoader.validate(overlappingManifest) else {
        print("FAILED: contextual presentation accepted overlapping modes")
        return false
    }
    let plugin = InstalledActionPlugin(
        manifest: manifest,
        directory: URL(fileURLWithPath: "/tmp/contextual.etplugin", isDirectory: true)
    )
    let binding = ActionPluginRuntimeBinding(config: ActionPluginRuntimeConfig(
        pluginId: "contextual",
        apiBase: "http://127.0.0.1:47701/v1/plugin",
        token: "contextual-token",
        updatedAt: 1,
        instanceId: "contextual-instance",
        processId: 1
    ))
    func status(available: Bool,
                contextId: String?,
                mode: String?,
                actionId: String,
                label: String) -> ActionPluginStatus {
        ActionPluginStatus(available: available,
                           contextId: contextId,
                           mode: mode,
                           actionId: actionId,
                           label: label,
                           targetSummary: "目标评论",
                           updatedAt: Date().timeIntervalSince1970)
    }
    let unavailable = status(available: false,
                             contextId: nil,
                             mode: nil,
                             actionId: directAction.id,
                             label: "等待评论框")
    let response = ActionPluginInvokeResponse(
        requestId: "template",
        actionId: directAction.id,
        contextId: "ctx-direct",
        blocks: [ActionPluginResultBlock(text: "生成内容", title: nil)],
        targetSummary: "目标评论"
    )
    let preparation = ActionPluginPrepareResponse(
        protocolVersion: ActionPluginPrepareContract.protocolVersion,
        resultFormat: ActionPluginPrepareContract.resultFormat,
        pluginId: "contextual",
        runtimeInstanceId: "contextual-instance",
        requestId: "template",
        actionId: directAction.id,
        contextId: "ctx-direct",
        prompt: "CONTEXTUAL PREPARED PROMPT\nReturn blocks-v1.",
        targetSummary: "目标评论"
    )
    let transport = ActionPluginTransportStub(status: unavailable,
                                              response: response,
                                              binding: binding)
    transport.prepareResult = .success(preparation)
    let connector = PreparedActionConnectorStub()
    connector.blocks = [
        AITextProviderBlock(index: 0, text: "生成内容", title: nil),
    ]
    let focusBox = ActionPluginFocusBox()
    var epochs = FocusEpochState()
    focusBox.token = epochs.activate()
    let model = BufferModel()
    let host = ActionPluginHost(
        rootURL: URL(fileURLWithPath: "/tmp/contextual-plugin-root", isDirectory: true),
        client: transport,
        focus: focusBox.access,
        bufferModel: model,
        inboundBus: InboundBus(),
        pluginLoader: { _ in [plugin] },
        connectorProvider: { connector },
        runtimeBindingIsCurrent: { _, candidate in candidate == binding }
    )
    var changeCount = 0
    host.onChange = { changeCount += 1 }
    host.refreshStatuses(force: true)
    guard runMainLoopUntil({ changeCount > 0 }),
          host.presentations.count == 1,
          host.presentations[0].title == "生成评论",
          host.presentations[0].presentationKey == presentationKey,
          host.presentations[0].key.actionId == directAction.id,
          host.presentations[0].canInvoke == false,
          host.presentations[0].isPreparedGeneration,
          host.presentations[0].preparedGenerationActionIDs
            == Set([directAction.id, replyAction.id]),
          host.primaryGenerationPresentation?.presentationKey == presentationKey,
          host.secondaryPresentations.isEmpty,
          host.generationStatusText == "等待评论框",
          host.primaryAction == .disabled else {
        print("FAILED: contextual action must remain one disabled control without a target")
        return false
    }

    transport.statusResult = .success(status(available: true,
                                             contextId: "ctx-direct",
                                             mode: "direct",
                                             actionId: directAction.id,
                                             label: "生成直评"))
    host.refreshStatuses(force: true)
    guard runMainLoopUntil({
        host.presentations.count == 1
            && host.presentations[0].canInvoke
            && host.presentations[0].presentationKey == presentationKey
            && host.presentations[0].key.actionId == directAction.id
            && host.primaryGenerationPresentation?.key.actionId == directAction.id
            && host.primaryAction == .requestGeneration
    }) else {
        print("FAILED: contextual presentation did not select direct action")
        return false
    }

    transport.statusResult = .success(status(available: true,
                                             contextId: "ctx-reply",
                                             mode: "reply",
                                             actionId: replyAction.id,
                                             label: "生成回复"))
    host.refreshStatuses(force: true)
    guard runMainLoopUntil({
        host.presentations.count == 1
            && host.presentations[0].title == "生成评论"
            && host.presentations[0].canInvoke
            && host.presentations[0].presentationKey == presentationKey
            && host.presentations[0].key.actionId == replyAction.id
            && host.primaryGenerationPresentation?.key.actionId == replyAction.id
            && host.generationStatusText == "生成回复"
            && host.primaryAction == .requestGeneration
    }) else {
        print("FAILED: contextual presentation did not switch its action id")
        return false
    }

    guard host.generate(), host.primaryAction == .generating else {
        print("FAILED: contextual prepared primary action did not start generation")
        return false
    }
    guard runMainLoopUntil({ model.blocks.count == 1 }),
          model.blocks[0].pluginMetadata?.actionId == replyAction.id,
          transport.prepareRequestCount == 1,
          transport.invokeRequestCount == 0,
          connector.requests.count == 1,
          host.primaryAction == .deliver else {
        print("FAILED: unified contextual control invoked the wrong action")
        return false
    }
    return true
}

private func runPreparedActionConnectorSmokeTest() -> Bool {
    let action = ActionPluginDefinition(
        id: "prepared.generate",
        title: "生成",
        symbol: "sparkles",
        statusPath: "/status",
        preparePath: "/prepare",
        invokePath: "/invoke",
        modes: ["direct"]
    )
    let manifest = ActionPluginManifest(
        schemaVersion: 1,
        id: "prepared",
        name: "Prepared",
        version: "1",
        runtimeConfigPaths: ["runtime.json"],
        actions: [action]
    )
    guard ActionPluginManifestLoader.validate(manifest) else {
        print("FAILED: prepared action manifest validation")
        return false
    }
    let groupedDirect = ActionPluginDefinition(
        id: "prepared.direct",
        title: "直评",
        symbol: "sparkles",
        statusPath: "/status",
        preparePath: "/prepare",
        invokePath: "/invoke",
        modes: ["direct"],
        presentationId: "prepared.generate",
        presentationTitle: "生成"
    )
    let groupedReply = ActionPluginDefinition(
        id: "prepared.reply",
        title: "回复",
        symbol: "sparkles",
        statusPath: "/status",
        preparePath: "/other-prepare",
        invokePath: "/invoke",
        modes: ["reply"],
        presentationId: "prepared.generate",
        presentationTitle: "生成"
    )
    let mismatchedPreparationManifest = ActionPluginManifest(
        schemaVersion: 1,
        id: "prepared-group",
        name: "Prepared Group",
        version: "1",
        runtimeConfigPaths: ["runtime.json"],
        actions: [groupedDirect, groupedReply]
    )
    guard !ActionPluginManifestLoader.validate(mismatchedPreparationManifest) else {
        print("FAILED: grouped actions accepted different prepare paths")
        return false
    }
    let binding = ActionPluginRuntimeBinding(config: ActionPluginRuntimeConfig(
        pluginId: "prepared",
        apiBase: "http://127.0.0.1:47701/v1/plugin",
        token: "prepared-token",
        updatedAt: 1,
        instanceId: "prepared-instance",
        processId: 1
    ))
    let status = ActionPluginStatus(
        available: true,
        contextId: "prepared-context",
        mode: "direct",
        actionId: action.id,
        label: "生成话术",
        targetSummary: "直评目标",
        updatedAt: Date().timeIntervalSince1970
    )
    let template = ActionPluginPrepareResponse(
        protocolVersion: ActionPluginPrepareContract.protocolVersion,
        resultFormat: ActionPluginPrepareContract.resultFormat,
        pluginId: "prepared",
        runtimeInstanceId: "prepared-instance",
        requestId: "template",
        actionId: action.id,
        contextId: "prepared-context",
        prompt: "MARINE PREPARED PROMPT\nReturn blocks-v1.",
        targetSummary: "直评目标"
    )
    let expectedIdentity = ActionPluginStreamIdentity(
        pluginId: "prepared",
        runtimeInstanceId: "prepared-instance",
        requestId: "template",
        actionId: action.id,
        contextId: "prepared-context"
    )
    guard ActionPluginPrepareContract.accepts(template,
                                              expectedIdentity: expectedIdentity),
          !ActionPluginPrepareContract.accepts(
            ActionPluginPrepareResponse(
                protocolVersion: template.protocolVersion,
                resultFormat: template.resultFormat,
                pluginId: template.pluginId,
                runtimeInstanceId: template.runtimeInstanceId,
                requestId: "wrong-request",
                actionId: template.actionId,
                contextId: template.contextId,
                prompt: template.prompt,
                targetSummary: template.targetSummary
            ),
            expectedIdentity: expectedIdentity
          ) else {
        print("FAILED: prepared action identity contract")
        return false
    }

    let dummyResponse = ActionPluginInvokeResponse(
        requestId: "unused",
        actionId: action.id,
        contextId: "prepared-context",
        blocks: [ActionPluginResultBlock(text: "legacy must not run", title: nil)],
        targetSummary: nil
    )
    let transport = ActionPluginTransportStub(status: status,
                                              response: dummyResponse,
                                              binding: binding)
    transport.prepareResult = .success(template)
    let connector = PreparedActionConnectorStub()
    connector.blocks = [
        AITextProviderBlock(index: 0, text: "连接器生成结果一", title: "候选一"),
        AITextProviderBlock(index: 1, text: "连接器生成结果二", title: "候选二"),
        AITextProviderBlock(index: 2, text: "连接器生成结果三", title: "候选三"),
    ]
    connector.holdsCompletion = true
    let focusBox = ActionPluginFocusBox()
    var epochs = FocusEpochState()
    focusBox.token = epochs.activate()
    let model = BufferModel()
    let plugin = InstalledActionPlugin(
        manifest: manifest,
        directory: URL(fileURLWithPath: "/tmp/prepared.etplugin", isDirectory: true)
    )
    let host = ActionPluginHost(
        rootURL: URL(fileURLWithPath: "/tmp/prepared-plugin-root", isDirectory: true),
        client: transport,
        focus: focusBox.access,
        bufferModel: model,
        inboundBus: InboundBus(),
        pluginLoader: { _ in [plugin] },
        connectorProvider: { connector },
        runtimeBindingIsCurrent: { _, candidate in candidate == binding }
    )
    host.refreshStatuses(force: true)
    guard runMainLoopUntil({ host.presentations.first?.canInvoke == true }),
          let presentation = host.presentations.first,
          let key = host.presentations.first?.key else {
        print("FAILED: prepared action did not become invokable")
        return false
    }
    guard presentation.isPreparedGeneration,
          presentation.preparedGenerationActionIDs == Set([action.id]),
          host.primaryGenerationPresentation == presentation,
          host.secondaryPresentations.isEmpty,
          host.canGenerate,
          host.generationProviderName == "Prepared",
          host.generationStatusText == "生成话术",
          host.primaryAction == .requestGeneration,
          ActionPluginPrimaryPresentationRules.primary(in: [presentation]) == presentation,
          ActionPluginPrimaryPresentationRules.primary(
            in: [presentation, presentation]
          ) == nil,
          ActionPluginPrimaryPresentationRules.secondary(
            in: [presentation, presentation]
          ).count == 2 else {
        print("FAILED: prepared primary presentation promotion was ambiguous")
        return false
    }

    let legacyPresentation = ActionPluginPresentation(
        key: ActionPluginKey(pluginId: "prepared", actionId: "prepared.legacy"),
        presentationKey: ActionPluginPresentationKey(
            pluginId: "prepared",
            presentationId: "prepared.legacy"
        ),
        pluginName: "Prepared",
        title: "旧动作",
        symbol: "bolt",
        label: "运行旧动作",
        targetSummary: nil,
        available: true,
        canInvoke: true,
        requiresFocus: true,
        preparedGenerationActionIDs: [],
        running: false,
        waitingForFirstContent: false
    )
    guard ActionPluginPrimaryPresentationRules.primary(
            in: [presentation, legacyPresentation]
          ) == nil,
          ActionPluginPrimaryPresentationRules.secondary(
            in: [presentation, legacyPresentation]
          ) == [presentation, legacyPresentation] else {
        print("FAILED: mixed prepared/legacy actions did not retain the explicit shelf")
        return false
    }

    model.stageExternal("ordinary source", origin: .rime)
    guard host.primaryAction == .requestGeneration else {
        print("FAILED: ordinary source promoted the prepared action to delivery")
        return false
    }

    let unrelatedMetadata = BufferModel.PluginMetadata(
        pluginId: "prepared",
        actionId: "prepared.other",
        requestId: "unrelated-request",
        contextId: "prepared-context",
        focusToken: focusBox.token,
        runtimeIdentity: binding.identity
    )
    model.stageExternal("other action result",
                        origin: .plugin(id: "prepared"),
                        pluginMetadata: unrelatedMetadata)
    guard host.primaryAction == .requestGeneration else {
        print("FAILED: unrelated plugin action promoted the prepared action to delivery")
        return false
    }

    let stalePrimaryMetadata = BufferModel.PluginMetadata(
        pluginId: "prepared",
        actionId: action.id,
        requestId: "stale-primary-request",
        contextId: "prepared-context",
        focusToken: focusBox.token,
        runtimeIdentity: binding.identity,
        stale: true
    )
    model.stageExternal("stale primary result",
                        origin: .plugin(id: "prepared"),
                        pluginMetadata: stalePrimaryMetadata)
    guard host.primaryAction == .requestGeneration else {
        print("FAILED: stale prepared result was exposed as ready delivery")
        return false
    }

    guard host.generate(), host.primaryAction == .generating else {
        print("FAILED: prepared primary action did not enter generating state")
        return false
    }
    guard runMainLoopUntil({
        connector.requests.count == 1
            && model.loadingMessage?.contains("正在分析页面话术") == true
    }),
          model.blocks.count == 3,
          model.transientLoadingActive,
          host.presentations.first?.waitingForFirstContent == true else {
        print("FAILED: prepared connector activity was not visible before first content")
        return false
    }
    connector.complete()
    guard runMainLoopUntil({
        model.blocks.count == 6
            && model.blocks.contains {
                $0.text == "连接器生成结果一"
                    && $0.pluginMetadata?.incomplete == false
            }
    }),
          transport.prepareRequestCount == 1,
          transport.invokeRequestCount == 0,
          connector.requests.count == 1,
          connector.requests[0].preparedPrompt == template.prompt,
          connector.requests[0].sourceText.isEmpty,
          host.deliveryPendingBlocks.map(\.text) == [
            "连接器生成结果一",
            "连接器生成结果二",
            "连接器生成结果三",
          ],
          host.deliveryPendingBlocks[0].pluginMetadata?.pluginId == "prepared",
          host.deliveryPendingBlocks[0].pluginMetadata?.contextId == "prepared-context",
          !host.hasIncompleteDeliveryBlocks,
          host.primaryAction == .deliver,
          !host.generate() else {
        print("FAILED: prepared action did not stay on the connector/plugin authority path")
        return false
    }

    let invalidProvisionalBlocks = [
        AITextProviderBlock(index: ActionPluginStreamParser.maximumBlocks,
                            text: "越界索引",
                            title: nil),
        AITextProviderBlock(index: 0,
                            text: String(repeating: "x",
                                         count: AITextRuntimeLimits.maximumWireBytes + 1),
                            title: nil),
        AITextProviderBlock(index: 0,
                            text: "标题越界",
                            title: String(repeating: "题",
                                          count: ActionPluginStreamParser.maximumTitleBytes + 1)),
    ]
    for (offset, invalidBlock) in invalidProvisionalBlocks.enumerated() {
        connector.holdsCompletion = true
        host.invoke(key)
        guard runMainLoopUntil({
            connector.requests.count == offset + 2 && model.transientLoadingActive
        }) else {
            print("FAILED: invalid prepared connector case did not start")
            return false
        }
        connector.emitHeld(invalidBlock)
        guard runMainLoopUntil({ !model.transientLoadingActive }),
              model.blocks.count == 6,
              host.presentations.first?.waitingForFirstContent == false else {
            print("FAILED: invalid provisional connector block reached the workbench")
            return false
        }
    }

    guard let deliveryFocus = focusBox.token else {
        print("FAILED: prepared delivery focus disappeared")
        return false
    }
    var deliveredTexts: [String] = []
    var completedDeliveries: [BufferDeliveryCoordinator.SendResult] = []
    var terminalDeliveryContexts: [
        BufferDeliveryCoordinator.TerminalDrainContext
    ] = []
    var workbenchSessionEpoch: UInt64 = 73
    var pendingValidations: [(ActionPluginDeliveryDecision) -> Void] = []
    var mutateModelAfterDeliveredText: String?
    let preparedCoordinator = BufferDeliveryCoordinator(
        model: model,
        dependencies: .init(
            resolveTarget: { expected in
                guard expected == nil || expected == deliveryFocus else { return nil }
                return .init(
                    token: deliveryFocus,
                    compositionActive: false,
                    resolveComposition: {},
                    deliver: { block in
                        deliveredTexts.append(block.text)
                        if mutateModelAfterDeliveredText == block.text {
                            mutateModelAfterDeliveredText = nil
                            model.stageExternal(
                                "terminal concurrent ordinary input",
                                origin: .rime
                            )
                        }
                        return true
                    }
                )
            },
            secureInputEnabled: { false },
            validatePlugin: { _, _, completion in
                pendingValidations.append(completion)
            },
            refreshUI: {},
            workbenchSessionEpoch: { workbenchSessionEpoch },
            terminalDrainDelivered: {
                terminalDeliveryContexts.append($0)
            }
        ),
        contentSourceResolver: { host }
    )
    let delivery = preparedCoordinator.sendNext(
        expectedToken: deliveryFocus,
        completion: { completedDeliveries.append($0) }
    )
    guard delivery.deferred,
          pendingValidations.count == 1,
          completedDeliveries.isEmpty,
          deliveredTexts.isEmpty else {
        print("FAILED: prepared sendNext did not wait for target validation")
        return false
    }
    let firstValidation = pendingValidations.removeFirst()
    firstValidation(.allowed)
    guard
          completedDeliveries == [.init(sentCount: 1, blockedReason: nil)],
          deliveredTexts == ["连接器生成结果一"],
          host.deliveryPendingBlocks.map(\.text) == [
            "连接器生成结果二",
            "连接器生成结果三",
          ],
          host.primaryAction == .deliver else {
        print("FAILED: prepared sendNext did not select only the first generated block")
        return false
    }
    let partialDelivery = preparedCoordinator.sendAll(
        expectedToken: deliveryFocus,
        completion: { completedDeliveries.append($0) }
    )
    guard partialDelivery.deferred,
          pendingValidations.count == 1 else {
        print("FAILED: prepared sendAll did not validate its first remaining block")
        return false
    }
    let secondValidation = pendingValidations.removeFirst()
    secondValidation(.allowed)
    guard pendingValidations.count == 1,
          completedDeliveries.count == 1 else {
        print("FAILED: prepared sendAll did not advance validation one block at a time")
        return false
    }
    // Simulate ordinary typing while the last target check is held. The
    // already-inserted generated block must still be consumed by stable UUID;
    // advancing BufferModel.changeCount cannot make it deliverable twice.
    model.stageExternal("concurrent ordinary input", origin: .rime)
    let thirdValidation = pendingValidations.removeFirst()
    thirdValidation(.rejected(.targetChanged))
    guard
          completedDeliveries == [
            .init(sentCount: 1, blockedReason: nil),
            .init(sentCount: 1, blockedReason: .pluginTargetChanged),
          ],
          deliveredTexts == [
            "连接器生成结果一",
            "连接器生成结果二",
          ],
          model.blocks.map(\.text) == [
            "ordinary source",
            "other action result",
            "stale primary result",
            "连接器生成结果三",
            "concurrent ordinary input",
          ],
          model.blocks.first(where: { $0.text == "连接器生成结果三" })?
            .pluginMetadata?.stale == true,
          host.deliveryPendingBlocks.isEmpty,
          host.primaryAction == .requestGeneration,
          completedDeliveries.allSatisfy({
            $0.terminalDrain == nil
          }),
          terminalDeliveryContexts.isEmpty else {
        print("FAILED: prepared sendAll did not atomically consume accepted and stale rejected blocks")
        return false
    }

    // A prepared Action result still consumes accepted stable UUIDs when
    // unrelated BufferModel text arrives during async target validation, but
    // that drift must suppress the terminal auto-close receipt.
    connector.blocks = [
        AITextProviderBlock(index: 0,
                            text: "连接器漂移终态结果",
                            title: "漂移候选"),
    ]
    connector.holdsCompletion = false
    guard host.generate(),
          runMainLoopUntil({
            host.deliveryPendingBlocks.map(\.text) == ["连接器漂移终态结果"]
          }) else {
        print("FAILED: prepared generation-drift receipt fixture did not regenerate")
        return false
    }
    let generationDriftDelivery = preparedCoordinator.sendAll(
        expectedToken: deliveryFocus,
        completion: { completedDeliveries.append($0) }
    )
    guard generationDriftDelivery.deferred,
          pendingValidations.count == 1 else {
        print("FAILED: prepared generation-drift delivery did not await validation")
        return false
    }
    mutateModelAfterDeliveredText = "连接器漂移终态结果"
    pendingValidations.removeFirst()(.allowed)
    guard let generationDriftResult = completedDeliveries.last,
          generationDriftResult.sentCount == 1,
          generationDriftResult.blockedReason == nil,
          generationDriftResult.terminalDrain == nil,
          deliveredTexts.last == "连接器漂移终态结果",
          host.deliveryPendingBlocks.isEmpty,
          terminalDeliveryContexts.isEmpty else {
        print("FAILED: concurrent generation drift requested terminal auto-close")
        return false
    }

    connector.blocks = [
        AITextProviderBlock(index: 0,
                            text: "连接器终态结果",
                            title: "最终候选"),
    ]
    connector.holdsCompletion = false
    guard host.generate(),
          runMainLoopUntil({
            host.deliveryPendingBlocks.map(\.text) == ["连接器终态结果"]
          }) else {
        print("FAILED: prepared terminal receipt fixture did not regenerate")
        return false
    }
    let terminalSourceGeneration = host.deliveryGeneration
    let terminalDelivery = preparedCoordinator.sendAll(
        expectedToken: deliveryFocus,
        completion: { completedDeliveries.append($0) }
    )
    guard terminalDelivery.deferred,
          pendingValidations.count == 1 else {
        print("FAILED: prepared terminal delivery did not await validation")
        return false
    }
    workbenchSessionEpoch = 74
    pendingValidations.removeFirst()(.allowed)
    guard let terminalResult = completedDeliveries.last,
          terminalResult.sentCount == 1,
          terminalResult.blockedReason == nil,
          terminalResult.terminalDrain?.workspaceID
            == host.deliveryWorkspaceID,
          terminalResult.terminalDrain?.sourceGeneration
            == terminalSourceGeneration,
          terminalResult.terminalDrain?
            .sourceGenerationAfterConsumption == host.deliveryGeneration,
          terminalResult.terminalDrain?
            .matchesCurrentSource(host) == true,
          terminalResult.terminalDrain?.workbenchSessionEpoch == 73,
          terminalDeliveryContexts.isEmpty,
          runMainLoopUntil({ terminalDeliveryContexts.count == 1 }),
          terminalDeliveryContexts.first
            == terminalResult.terminalDrain,
          host.deliveryPendingBlocks.isEmpty else {
        print("FAILED: prepared terminal result did not emit one deferred close context")
        return false
    }
    return true
}

private func runContextOnlyPreparedActionSmokeTest() -> Bool {
    let legacyJSON = #"""
    {
      "id": "legacy.generate",
      "title": "生成",
      "symbol": "sparkles",
      "statusPath": "/status",
      "invokePath": "/invoke",
      "modes": ["direct"]
    }
    """#
    let contextOnlyJSON = #"""
    {
      "id": "context-only.generate",
      "title": "生成",
      "symbol": "sparkles",
      "statusPath": "/status",
      "preparePath": "/prepare",
      "invokePath": "/invoke",
      "modes": ["direct"],
      "requiresFocus": false
    }
    """#
    guard let legacyAction = try? JSONDecoder().decode(
        ActionPluginDefinition.self,
        from: Data(legacyJSON.utf8)
    ), legacyAction.requiresFocus,
       let action = try? JSONDecoder().decode(
        ActionPluginDefinition.self,
        from: Data(contextOnlyJSON.utf8)
       ), !action.requiresFocus else {
        print("FAILED: requiresFocus manifest compatibility")
        return false
    }

    let groupedBound = ActionPluginDefinition(
        id: "context-only.bound",
        title: "直评",
        symbol: "sparkles",
        statusPath: "/status",
        preparePath: "/prepare",
        invokePath: "/invoke",
        modes: ["direct"],
        requiresFocus: true,
        presentationId: "context-only.generate",
        presentationTitle: "生成"
    )
    let groupedUnbound = ActionPluginDefinition(
        id: "context-only.unbound",
        title: "回复",
        symbol: "sparkles",
        statusPath: "/status",
        preparePath: "/prepare",
        invokePath: "/invoke",
        modes: ["reply"],
        requiresFocus: false,
        presentationId: "context-only.generate",
        presentationTitle: "生成"
    )
    let mismatchedFocusManifest = ActionPluginManifest(
        schemaVersion: 1,
        id: "context-only-group",
        name: "Context Only Group",
        version: "1",
        runtimeConfigPaths: ["runtime.json"],
        actions: [groupedBound, groupedUnbound]
    )
    guard !ActionPluginManifestLoader.validate(mismatchedFocusManifest) else {
        print("FAILED: grouped actions accepted different focus contracts")
        return false
    }

    let manifest = ActionPluginManifest(
        schemaVersion: 1,
        id: "context-only",
        name: "Context Only",
        version: "1",
        runtimeConfigPaths: ["runtime.json"],
        actions: [action]
    )
    guard ActionPluginManifestLoader.validate(manifest) else {
        print("FAILED: context-only manifest validation")
        return false
    }
    let binding = ActionPluginRuntimeBinding(config: ActionPluginRuntimeConfig(
        pluginId: manifest.id,
        apiBase: "http://127.0.0.1:47701/v1/plugin",
        token: "context-only-token",
        updatedAt: 1,
        instanceId: "context-only-instance",
        processId: 1
    ))
    func status(contextId: String) -> ActionPluginStatus {
        ActionPluginStatus(
            available: true,
            contextId: contextId,
            mode: "direct",
            actionId: action.id,
            label: "生成评论",
            targetSummary: "页面评论目标",
            updatedAt: Date().timeIntervalSince1970
        )
    }
    let initialContextID = "context-only-target-a"
    let template = ActionPluginPrepareResponse(
        protocolVersion: ActionPluginPrepareContract.protocolVersion,
        resultFormat: ActionPluginPrepareContract.resultFormat,
        pluginId: manifest.id,
        runtimeInstanceId: "context-only-instance",
        requestId: "template",
        actionId: action.id,
        contextId: initialContextID,
        prompt: "CONTEXT ONLY PROMPT\nReturn blocks-v1.",
        targetSummary: "页面评论目标"
    )
    let response = ActionPluginInvokeResponse(
        requestId: "unused",
        actionId: action.id,
        contextId: initialContextID,
        blocks: [ActionPluginResultBlock(text: "legacy must not run", title: nil)],
        targetSummary: nil
    )
    let transport = ActionPluginTransportStub(
        status: status(contextId: initialContextID),
        response: response,
        binding: binding
    )
    transport.prepareResult = .success(template)
    let connector = PreparedActionConnectorStub()
    connector.holdsCompletion = true
    let focusBox = ActionPluginFocusBox()
    let model = BufferModel()
    let bus = InboundBus()
    let plugin = InstalledActionPlugin(
        manifest: manifest,
        directory: URL(fileURLWithPath: "/tmp/context-only.etplugin", isDirectory: true)
    )
    let host = ActionPluginHost(
        rootURL: URL(fileURLWithPath: "/tmp/context-only-plugin-root", isDirectory: true),
        client: transport,
        focus: focusBox.access,
        bufferModel: model,
        inboundBus: bus,
        pluginLoader: { _ in [plugin] },
        connectorProvider: { connector },
        runtimeBindingIsCurrent: { _, candidate in candidate == binding }
    )

    host.refreshStatuses(force: true)
    guard runMainLoopUntil({ host.presentations.first?.canInvoke == true }),
          focusBox.token == nil,
          host.presentations.first?.available == true,
          let key = host.presentations.first?.key else {
        print("FAILED: context-only action stayed disabled without IMK focus")
        return false
    }
    connector.availability = .unavailable("连接器不可用")
    guard host.presentations.first?.canInvoke == false else {
        print("FAILED: context-only action ignored connector availability")
        return false
    }
    connector.availability = .ready
    focusBox.secureInput = true
    guard host.presentations.first?.canInvoke == false else {
        print("FAILED: context-only action ignored secure input")
        return false
    }
    focusBox.secureInput = false

    host.invoke(key)
    guard runMainLoopUntil({
        connector.requests.count == 1
            && model.transientLoadingActive
            && host.presentations.first?.waitingForFirstContent == true
    }), transport.prepareRequestCount == 1,
       transport.invokeRequestCount == 0 else {
        print("FAILED: context-only action did not invoke its prepared connector")
        return false
    }
    connector.emitHeld(AITextProviderBlock(index: 0,
                                           text: "无焦点流式候选",
                                           title: "候选"))
    guard runMainLoopUntil({ model.blocks.count == 1 }),
          let provisional = model.blocks[0].pluginMetadata,
          provisional.focusToken == nil,
          provisional.reviewedAsPlainText,
          provisional.incomplete,
          model.blocks[0].text == "无焦点流式候选" else {
        print("FAILED: context-only stream was not staged as unbound reviewed text")
        return false
    }

    var deliveryEpochs = FocusEpochState()
    let temporaryDeliveryFocus = deliveryEpochs.activate()
    var deliveryFocus: FocusToken? = temporaryDeliveryFocus
    var deliveredTexts: [String] = []
    var validationCalled = false
    let deliveryCoordinator = BufferDeliveryCoordinator(
        model: model,
        dependencies: .init(
            resolveTarget: { expected in
                guard let deliveryFocus,
                      expected == nil || expected == deliveryFocus else { return nil }
                return .init(
                    token: deliveryFocus,
                    compositionActive: false,
                    resolveComposition: {},
                    deliver: { block in
                        deliveredTexts.append(block.text)
                        return true
                    }
                )
            },
            secureInputEnabled: { false },
            validatePlugin: { _, _, completion in
                validationCalled = true
                completion(.rejected(.targetChanged))
            },
            refreshUI: {}
        )
    )
    guard deliveryCoordinator.availability() == .blocked(.pluginResultIncomplete) else {
        print("FAILED: context-only partial result became deliverable")
        return false
    }
    deliveryFocus = nil
    connector.complete()
    guard runMainLoopUntil({
        model.blocks.count == 1
            && model.blocks[0].pluginMetadata?.incomplete == false
    }), let finalMetadata = model.blocks[0].pluginMetadata,
       finalMetadata.focusToken == nil,
       finalMetadata.reviewedAsPlainText,
       !finalMetadata.stale,
       focusBox.token == nil else {
        print("FAILED: context-only final result acquired a synthetic focus lease")
        return false
    }
    guard deliveryCoordinator.availability() == .blocked(.noFocusedField),
          deliveryCoordinator.sendNext().blockedReason == .noFocusedField,
          deliveredTexts.isEmpty else {
        print("FAILED: unbound result delivered without a fresh target")
        return false
    }

    let freshDeliveryFocus = deliveryEpochs.activate()
    deliveryFocus = freshDeliveryFocus
    let delivered = deliveryCoordinator.sendNext(expectedToken: freshDeliveryFocus)
    guard delivered.succeeded,
          deliveredTexts == ["连接器生成结果"],
          !validationCalled,
          model.blocks.isEmpty else {
        print("FAILED: reviewed context-only result did not use the fresh target boundary")
        return false
    }

    transport.statusResult = .success(status(contextId: initialContextID))
    host.refreshStatuses(force: true)
    guard runMainLoopUntil({ host.presentations.first?.canInvoke == true }) else {
        print("FAILED: context-only target-change fixture did not refresh")
        return false
    }
    host.invoke(key)
    guard runMainLoopUntil({ connector.requests.count == 2 }) else {
        print("FAILED: second context-only invocation did not start")
        return false
    }
    let staleLargeResult = Array(repeating: "review-word", count: 2_200)
        .joined(separator: " ")
    guard staleLargeResult.utf8.count > InboundBus.maxTextCount else {
        print("FAILED: large stale review fixture is too small")
        return false
    }
    connector.blocks = [
        AITextProviderBlock(index: 0,
                            text: staleLargeResult,
                            title: "长结果"),
    ]
    connector.emitHeld(AITextProviderBlock(index: 0,
                                           text: "目标变化前的部分结果",
                                           title: nil))
    guard runMainLoopUntil({ model.blocks.count == 1 }) else {
        print("FAILED: target-change fixture did not stage its partial result")
        return false
    }
    transport.statusResult = .success(status(contextId: "context-only-target-b"))
    host.refreshStatuses(force: true)
    guard runMainLoopUntil({
        model.blocks.isEmpty && host.presentations.first?.running == false
    }) else {
        print("FAILED: context change did not revoke visible partial output")
        return false
    }
    connector.complete()
    guard runMainLoopUntil({ bus.pendingCount == 1 }),
          bus.pending[0].text == staleLargeResult,
          bus.pending[0].pluginMetadata?.stale == true,
          bus.pending[0].pluginMetadata?.focusToken == nil,
          bus.pending[0].pluginMetadata?.reviewedAsPlainText == false,
          model.blocks.isEmpty else {
        print("FAILED: context-changed result did not route to the review inbox")
        return false
    }
    let staleReviewID = bus.pending[0].id
    let reviewedModel = BufferModel.shared
    reviewedModel.discardForPrivacy()
    defer { reviewedModel.discardForPrivacy() }
    bus.accept(staleReviewID)
    guard bus.pending.isEmpty,
          reviewedModel.stagedText == staleLargeResult,
          reviewedModel.blocks.count > 1,
          reviewedModel.blocks.allSatisfy({
              $0.text.utf8.count <= ActionPluginStreamParser.maximumBlockBytes
          }) else {
        print("FAILED: large stale prepared result was not preserved through review",
              bus.pendingCount,
              reviewedModel.stagedText.count,
              staleLargeResult.count,
              reviewedModel.blocks.count,
              reviewedModel.blocks.map { $0.text.utf8.count }.max() ?? 0)
        return false
    }
    return true
}

func runActionPluginSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) action plugin smoke test ==")
    guard runActionPluginManagerSmokeTest() else { return false }
    guard runActionPluginStreamSmokeTest() else { return false }
    guard runContextualActionPresentationSmokeTest() else { return false }
    guard runPreparedActionConnectorSmokeTest() else { return false }
    guard runContextOnlyPreparedActionSmokeTest() else { return false }
    let fileManager = FileManager.default
    let root = fileManager.temporaryDirectory
        .appendingPathComponent("rimebuffer-plugin-smoke-\(UUID().uuidString)",
                                isDirectory: true)
    let pluginDirectory = root.appendingPathComponent("example.etplugin", isDirectory: true)
    let fakeHome = root.appendingPathComponent("home", isDirectory: true)
    do {
        try fileManager.createDirectory(at: pluginDirectory,
                                        withIntermediateDirectories: true)
        try fileManager.createDirectory(at: fakeHome,
                                        withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: root) }

        let manifest = #"""
        {
          "schemaVersion": 1,
          "id": "example",
          "name": "Example",
          "version": "1.0.0",
          "runtimeConfigPaths": ["symlink-runtime.json", "borrowed-runtime.json", "runtime.json", "old-runtime.json"],
          "actions": [{
            "id": "example.generate",
            "title": "生成",
            "symbol": "sparkles",
            "statusPath": "/status",
            "invokePath": "/invoke",
            "modes": ["direct", "reply"]
          }]
        }
        """#
        try Data(manifest.utf8).write(
            to: pluginDirectory.appendingPathComponent("manifest.json"),
            options: .atomic
        )
        try Data(#"{"pluginId":"example","apiBase":"http://localhost:47701/v1/plugin","token":"old","updatedAt":1}"#.utf8)
            .write(to: fakeHome.appendingPathComponent("old-runtime.json"), options: .atomic)
        try Data(#"{"pluginId":"example","apiBase":"http://localhost:47701/v1/plugin","token":"old","updatedAt":1}"#.utf8)
            .write(to: pluginDirectory.appendingPathComponent("old-runtime.json"), options: .atomic)
        try Data(#"{"pluginId":"example","apiBase":"http://127.0.0.1:47701/v1/plugin","token":"new","updatedAt":2}"#.utf8)
            .write(to: pluginDirectory.appendingPathComponent("runtime.json"), options: .atomic)
        try Data(#"{"pluginId":"other","apiBase":"http://127.0.0.1:47701/v1/plugin","token":"borrowed","updatedAt":3}"#.utf8)
            .write(to: pluginDirectory.appendingPathComponent("borrowed-runtime.json"), options: .atomic)
        try fileManager.createSymbolicLink(
            at: pluginDirectory.appendingPathComponent("symlink-runtime.json"),
            withDestinationURL: pluginDirectory.appendingPathComponent("runtime.json")
        )
        let invalidDirectory = root.appendingPathComponent("invalid.etplugin", isDirectory: true)
        try fileManager.createDirectory(at: invalidDirectory, withIntermediateDirectories: true)
        try Data(#"{"schemaVersion":2,"id":"invalid","name":"Invalid","runtimeConfigPaths":["runtime.json"],"actions":[{"id":"invalid.action","title":"Invalid","symbol":"xmark","statusPath":"/status","invokePath":"/invoke","modes":[]}] }"#.utf8)
            .write(to: invalidDirectory.appendingPathComponent("manifest.json"), options: .atomic)

        let loaded = ActionPluginManifestLoader.load(from: root)
        guard loaded.count == 1,
              loaded[0].manifest.id == "example",
              loaded[0].manifest.actions.count == 1,
              let config = ActionPluginManifestLoader.runtimeConfig(
                for: loaded[0], homeDirectory: fakeHome
              ),
              config.pluginId == "example",
              config.token == "new",
              config.updatedAt == 2 else {
            print("FAILED: manifest discovery/runtime config precedence")
            return false
        }

        guard ActionPluginManifestLoader.expandedPath(
                "~/old-runtime.json",
                pluginDirectory: pluginDirectory,
                homeDirectory: fakeHome
              ) == fakeHome.appendingPathComponent("old-runtime.json"),
              ActionPluginManifestLoader.expandedPath(
                "../escaped-runtime.json",
                pluginDirectory: pluginDirectory,
                homeDirectory: fakeHome
              ) == nil,
              ActionPluginManifestLoader.expandedPath(
                "nested/../runtime.json",
                pluginDirectory: pluginDirectory,
                homeDirectory: fakeHome
              ) == pluginDirectory.appendingPathComponent("runtime.json").standardizedFileURL,
              ActionPluginHTTPClient.isAllowedLoopbackBase(config.apiBase),
              !ActionPluginHTTPClient.isAllowedLoopbackBase("https://example.com/plugin"),
              !ActionPluginHTTPClient.isAllowedLoopbackBase("http://localhost.evil.test/plugin"),
              !ActionPluginHTTPClient.isAllowedLoopbackBase("http://localhost:99999/plugin"),
              !ActionPluginHTTPClient.isAllowedLoopbackBase("http://localhost/plugin?redirect=evil"),
              let request = ActionPluginHTTPClient.makeRequest(
                config: config,
                path: "/status",
                method: "GET",
                timeout: 1.5
              ),
              request.url?.absoluteString == "http://127.0.0.1:47701/v1/plugin/status",
              request.cachePolicy == .reloadIgnoringLocalCacheData,
              request.httpShouldHandleCookies == false,
              request.value(forHTTPHeaderField: "Cache-Control") == "no-store",
              request.value(forHTTPHeaderField: "Authorization") == "Bearer new" else {
            print("FAILED: tilde expansion/loopback/Bearer request contract")
            return false
        }

        var responseBuffer = ActionPluginResponseBuffer()
        let maximumResponseBytes = ActionPluginHTTPClient.maximumResponseBytes
        guard responseBuffer.append(Data(repeating: 0x61,
                                         count: maximumResponseBytes),
                                    maximumBytes: maximumResponseBytes),
              !responseBuffer.append(Data([0x62]),
                                     maximumBytes: maximumResponseBytes),
              responseBuffer.data.count == maximumResponseBytes else {
            print("FAILED: streaming response cap must reject before appending overflow")
            return false
        }

        let status = try JSONDecoder().decode(
            ActionPluginStatus.self,
            from: Data(#"{"available":true,"contextId":"ctx-1","mode":"reply","actionId":"example.generate","label":"回复 @用户","targetSummary":"原评论","updatedAt":3}"#.utf8)
        )
        let action = loaded[0].manifest.actions[0]
        var focusEpochs = FocusEpochState()
        let firstFocus = focusEpochs.activate()
        let secondFocus = focusEpochs.activate()
        guard ActionPluginRoutingRules.statusMatches(status,
                                                     action: action,
                                                     contextId: "ctx-1"),
              !ActionPluginRoutingRules.statusMatches(status,
                                                      action: action,
                                                      contextId: "ctx-2"),
              ActionPluginRoutingRules.focusBindingMatches(
                bound: secondFocus,
                current: secondFocus
              ),
              !ActionPluginRoutingRules.focusBindingMatches(
                bound: firstFocus,
                current: secondFocus
              ),
              !ActionPluginRoutingRules.focusBindingMatches(
                bound: nil,
                current: secondFocus
              ) else {
            print("FAILED: status/action/context/focus matching")
            return false
        }

        let response = try JSONDecoder().decode(
            ActionPluginInvokeResponse.self,
            from: Data(#"{"requestId":"req-1","actionId":"example.generate","contextId":"ctx-1","blocks":[{"text":"生成的话术","title":"回复 @用户"}],"targetSummary":"原评论"}"#.utf8)
        )
        guard response.blocks.first?.text == "生成的话术",
              ActionPluginRoutingRules.shouldStageDirect(
                responseMatches: true,
                focusValid: true,
                currentStatusMatches: true,
                invocationMarkedStale: false
              ),
              !ActionPluginRoutingRules.shouldStageDirect(
                responseMatches: true,
                focusValid: false,
                currentStatusMatches: true,
                invocationMarkedStale: false
              ),
              !ActionPluginRoutingRules.shouldStageDirect(
                responseMatches: true,
                focusValid: true,
                currentStatusMatches: false,
                invocationMarkedStale: false
              ),
              !ActionPluginRoutingRules.shouldStageDirect(
                responseMatches: false,
                focusValid: true,
                currentStatusMatches: true,
                invocationMarkedStale: false
              ) else {
            print("FAILED: safe result routing predicate")
            return false
        }

        let sessionConfiguration = URLSessionConfiguration.ephemeral
        sessionConfiguration.protocolClasses = [ActionPluginURLProtocolStub.self]
        let httpClient = ActionPluginHTTPClient(
            session: URLSession(configuration: sessionConfiguration)
        )
        func bodyData(from request: URLRequest) -> Data? {
            if let body = request.httpBody { return body }
            guard let stream = request.httpBodyStream else { return nil }
            stream.open()
            defer { stream.close() }
            var data = Data()
            var buffer = [UInt8](repeating: 0, count: 4096)
            while stream.hasBytesAvailable {
                let count = stream.read(&buffer, maxLength: buffer.count)
                guard count >= 0 else { return nil }
                if count == 0 { break }
                data.append(buffer, count: count)
            }
            return data
        }
        let requestLock = NSLock()
        var observedHTTPRequests: [String] = []
        ActionPluginURLProtocolStub.handler = { request in
            let authorization = request.value(forHTTPHeaderField: "Authorization") ?? ""
            requestLock.lock()
            observedHTTPRequests.append("\(request.httpMethod ?? "") \(request.url?.host ?? "") \(authorization)")
            requestLock.unlock()
            switch (request.httpMethod, request.url?.path) {
            case ("GET", "/v1/plugin/status"):
                if authorization == "Bearer new" {
                    return (401, Data(#"{"error":"stale runtime"}"#.utf8))
                }
                guard authorization == "Bearer old" else {
                    return (401, Data(#"{"error":"unauthorized"}"#.utf8))
                }
                return (200, Data(#"{"available":true,"contextId":"ctx-1","mode":"reply","actionId":"example.generate","label":"回复 @用户","targetSummary":"原评论","updatedAt":3}"#.utf8))
            case ("POST", "/v1/plugin/invoke"):
                guard authorization == "Bearer old",
                      request.url?.host == "localhost",
                      let body = bodyData(from: request),
                      let payload = try? JSONDecoder().decode(ActionPluginInvokeRequest.self,
                                                              from: body),
                      payload == ActionPluginInvokeRequest(requestId: "req-1",
                                                           actionId: "example.generate",
                                                           contextId: "ctx-1") else {
                    return (400, Data(#"{"error":"bad request"}"#.utf8))
                }
                return (200, Data(#"{"requestId":"req-1","actionId":"example.generate","contextId":"ctx-1","blocks":[{"text":"生成的话术","title":"回复 @用户"}],"targetSummary":"原评论"}"#.utf8))
            default:
                return (404, Data(#"{"error":"not found"}"#.utf8))
            }
        }
        defer { ActionPluginURLProtocolStub.handler = nil }

        let statusSemaphore = DispatchSemaphore(value: 0)
        var fetchedSnapshot: ActionPluginStatusSnapshot?
        httpClient.fetchStatus(plugin: loaded[0], action: action, binding: nil) { result in
            fetchedSnapshot = try? result.get()
            statusSemaphore.signal()
        }
        guard statusSemaphore.wait(timeout: .now() + 2) == .success,
              fetchedSnapshot?.value == status,
              fetchedSnapshot?.binding.config.token == "old",
              observedHTTPRequests.prefix(2).elementsEqual([
                "GET 127.0.0.1 Bearer new",
                "GET localhost Bearer old",
              ]) else {
            print("FAILED: status must fall back newest-to-oldest and retain successful binding")
            return false
        }

        // A new runtime file appearing after status cannot redirect the invoke
        // to another Marine instance; the successful status binding is exact.
        try Data(#"{"pluginId":"example","apiBase":"http://127.0.0.1:47701/v1/plugin","token":"newest","updatedAt":99}"#.utf8)
            .write(to: pluginDirectory.appendingPathComponent("runtime.json"), options: .atomic)

        let invokeSemaphore = DispatchSemaphore(value: 0)
        var fetchedResponse: ActionPluginInvokeResponse?
        _ = httpClient.invoke(
            plugin: loaded[0],
            action: action,
            binding: fetchedSnapshot!.binding,
            request: ActionPluginInvokeRequest(requestId: "req-1",
                                               actionId: "example.generate",
                                               contextId: "ctx-1")
        ) { _ in
            // Legacy JSON invokes must not produce incremental events.
        } completion: { result in
            fetchedResponse = try? result.get()
            invokeSemaphore.signal()
        }
        guard invokeSemaphore.wait(timeout: .now() + 2) == .success,
              fetchedResponse == response,
              observedHTTPRequests.last == "POST localhost Bearer old" else {
            print("FAILED: invoke must use the exact successful status binding")
            return false
        }

        // Exercise the real host lifecycle with injectable focus/transport.
        // No request may run merely because the workbench or browser target
        // appeared; invocation remains an explicit user action.
        let isolatedModel = BufferModel()
        let isolatedBus = InboundBus()
        let focusBox = ActionPluginFocusBox()
        let transport = ActionPluginTransportStub(status: status, response: response)
        let workbenchFirst = ActionPluginHost(rootURL: root,
                                              client: transport,
                                              focus: focusBox.access,
                                              bufferModel: isolatedModel,
                                              inboundBus: isolatedBus,
                                              runtimeBindingIsCurrent: { _, candidate in
                                                  candidate == transport.defaultBinding
                                              })
        var hostChangeCount = 0
        workbenchFirst.onChange = { hostChangeCount += 1 }

        workbenchFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, { hostChangeCount > 0 }),
              workbenchFirst.presentations.first?.canInvoke == false,
              workbenchFirst.presentations.first?.isPreparedGeneration == false,
              workbenchFirst.presentations.first?.preparedGenerationActionIDs.isEmpty == true,
              workbenchFirst.primaryGenerationPresentation == nil,
              workbenchFirst.secondaryPresentations == workbenchFirst.presentations,
              workbenchFirst.primaryAction == .disabled,
              transport.invokeRequestCount == 0 else {
            print("FAILED: workbench-first must wait for a focused target")
            return false
        }

        focusBox.token = firstFocus
        workbenchFirst.focusDidChange()
        workbenchFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            workbenchFirst.presentations.first?.canInvoke == true
        }), transport.invokeRequestCount == 0 else {
            print("FAILED: workbench-first target should enable without auto-invoking")
            return false
        }

        // Target-first: constructing/showing the host after focus exists must
        // converge to the same enabled action.
        let targetFirstModel = BufferModel()
        let targetFirstBus = InboundBus()
        let targetFirstTransport = ActionPluginTransportStub(status: status,
                                                             response: response)
        var targetRuntimeIsCurrent = true
        var targetFirstPluginSelected = true
        let targetFirst = ActionPluginHost(rootURL: root,
                                           client: targetFirstTransport,
                                           focus: focusBox.access,
                                           bufferModel: targetFirstModel,
                                           inboundBus: targetFirstBus,
                                           pluginIsSelected: { pluginID in
                                               targetFirstPluginSelected
                                                   && pluginID == loaded[0].manifest.id
                                           },
                                           runtimeBindingIsCurrent: { _, candidate in
                                               targetRuntimeIsCurrent
                                                   && candidate == targetFirstTransport.defaultBinding
                                           })
        targetFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == true
        }), targetFirstTransport.invokeRequestCount == 0,
           let targetFirstKey = targetFirst.presentations.first?.key else {
            print("FAILED: target-first should enable without auto-invoking")
            return false
        }

        // A cached context observed under focus A is unusable under focus B,
        // even before the next status poll completes.
        focusBox.token = secondFocus
        targetFirst.focusDidChange()
        let invokesBeforeStaleTap = targetFirstTransport.invokeRequestCount
        targetFirst.invoke(targetFirstKey)
        guard targetFirst.presentations.first?.canInvoke == false,
              targetFirstTransport.invokeRequestCount == invokesBeforeStaleTap else {
            print("FAILED: cached status crossed a focus epoch")
            return false
        }

        targetFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == true
        }) else {
            print("FAILED: fresh status did not bind to the new focus")
            return false
        }

        targetFirst.invoke(targetFirstKey)
        guard runMainLoopUntil(timeout: 1, { targetFirstModel.blocks.count == 1 }),
              targetFirstTransport.invokeRequestCount == invokesBeforeStaleTap + 1,
              targetFirstModel.blocks[0].text == "生成的话术",
              targetFirstModel.blocks[0].pluginMetadata?.stale == false,
              targetFirstBus.pendingCount == 0 else {
            print("FAILED: matching invoke/final-status should stage one buffer block")
            return false
        }

        guard let generatedMetadata = targetFirstModel.blocks[0].pluginMetadata else {
            print("FAILED: generated plugin block is missing target metadata")
            return false
        }
        var deliveryDecision: ActionPluginDeliveryDecision?
        targetFirst.validateForDelivery(metadata: generatedMetadata,
                                        expectedFocusToken: secondFocus) {
            deliveryDecision = $0
        }
        let lastValidationBinding = targetFirstTransport.statusBindings.last ?? nil
        guard runMainLoopUntil(timeout: 1, { deliveryDecision != nil }),
              deliveryDecision == .allowed,
              lastValidationBinding == targetFirstTransport.defaultBinding else {
            print("FAILED: send-time validation must re-check the original runtime binding")
            return false
        }

        // Changing the current workbench owner hides and cancels the old
        // plugin's controls, but a completed target-bound block still carries
        // a valid authority record and must remain deliverable. Selection is
        // not installation or runtime revocation.
        targetFirstPluginSelected = false
        targetFirst.bufferPluginSelectionDidChange()
        guard targetFirst.presentations.isEmpty else {
            print("FAILED: deselected plugin remained visible in workbench")
            return false
        }
        var switchedOwnerDeliveryDecision: ActionPluginDeliveryDecision?
        targetFirst.validateForDelivery(metadata: generatedMetadata,
                                        expectedFocusToken: secondFocus) {
            switchedOwnerDeliveryDecision = $0
        }
        guard runMainLoopUntil(timeout: 1, { switchedOwnerDeliveryDecision != nil }),
              switchedOwnerDeliveryDecision == .allowed else {
            print("FAILED: owner switch revoked a completed plugin block")
            return false
        }
        targetFirst.pluginConfigurationDidChange(changedPluginID: "new-owner")
        var enabledOwnerDeliveryDecision: ActionPluginDeliveryDecision?
        targetFirst.validateForDelivery(metadata: generatedMetadata,
                                        expectedFocusToken: secondFocus) {
            enabledOwnerDeliveryDecision = $0
        }
        guard runMainLoopUntil(timeout: 1, { enabledOwnerDeliveryDecision != nil }),
              enabledOwnerDeliveryDecision == .allowed else {
            print("FAILED: enabling another plugin revoked completed authority")
            return false
        }
        targetFirstPluginSelected = true
        targetFirst.bufferPluginSelectionDidChange()

        // A Settings mutation for inactive plugin B changes only B's
        // enablement/authority. It must not cancel selected plugin A's held
        // generation or remove A's partial/final workbench state.
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == true
        }) else {
            print("FAILED: selected plugin did not recover before unrelated mutation")
            return false
        }
        targetFirst.pluginConfigurationDidChange(
            changedPluginID: "inactive-plugin"
        )
        guard targetFirst.presentations.first?.canInvoke == true else {
            print("FAILED: inactive plugin mutation cleared selected idle status")
            return false
        }
        let blocksBeforeUnrelatedMutation = targetFirstModel.blocks.count
        targetFirstTransport.holdInvoke = true
        targetFirst.invoke(targetFirstKey)
        guard targetFirstModel.loadingMessage != nil,
              targetFirst.presentations.first?.running == true else {
            print("FAILED: unrelated-mutation fixture did not hold generation")
            return false
        }
        targetFirst.pluginConfigurationDidChange(
            changedPluginID: "inactive-plugin"
        )
        guard targetFirstModel.loadingMessage != nil,
              targetFirst.presentations.first?.running == true else {
            print("FAILED: inactive plugin mutation cancelled selected generation")
            return false
        }
        targetFirstTransport.holdInvoke = false
        targetFirstTransport.completeHeldInvoke()
        guard runMainLoopUntil(timeout: 1, {
            targetFirstModel.blocks.count == blocksBeforeUnrelatedMutation + 1
                && targetFirstModel.loadingMessage == nil
        }) else {
            print("FAILED: selected generation did not survive inactive plugin mutation")
            return false
        }

        // Runtime rotation revokes legacy JSON results too. The old local
        // service may still answer, but its binding can no longer authorize a
        // block minted by the previous instance.
        targetRuntimeIsCurrent = false
        var rotatedDeliveryDecision: ActionPluginDeliveryDecision?
        targetFirst.validateForDelivery(metadata: generatedMetadata,
                                        expectedFocusToken: secondFocus) {
            rotatedDeliveryDecision = $0
        }
        guard runMainLoopUntil(timeout: 1, { rotatedDeliveryDecision != nil }),
              rotatedDeliveryDecision == .rejected(.stale) else {
            print("FAILED: legacy delivery survived runtime rotation")
            return false
        }
        targetRuntimeIsCurrent = true

        // A disable/uninstall notification must revoke a validation that is
        // already waiting on the plugin's status response.
        let validationRoot = root.appendingPathComponent("validation-race", isDirectory: true)
        let validationLoader = ActionPluginLoaderBox([loaded[0]])
        let validationModel = BufferModel()
        let validationBus = InboundBus()
        let validationFocus = ActionPluginFocusBox()
        validationFocus.token = secondFocus
        let validationTransport = ActionPluginTransportStub(status: status,
                                                             response: response)
        let validationHost = ActionPluginHost(
            rootURL: validationRoot,
            client: validationTransport,
            focus: validationFocus.access,
            bufferModel: validationModel,
            inboundBus: validationBus,
            pluginLoader: validationLoader.load,
            runtimeBindingIsCurrent: { _, candidate in
                candidate == validationTransport.defaultBinding
            }
        )
        validationHost.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            validationHost.presentations.first?.canInvoke == true
        }), let validationKey = validationHost.presentations.first?.key else {
            print("FAILED: validation-race host did not become ready")
            return false
        }
        validationHost.invoke(validationKey)
        guard runMainLoopUntil(timeout: 1, { validationModel.blocks.count == 1 }),
              let validationMetadata = validationModel.blocks.first?.pluginMetadata else {
            print("FAILED: validation-race fixture did not stage a bound block")
            return false
        }
        validationTransport.holdStatus = true
        var revokedValidation: ActionPluginDeliveryDecision?
        validationHost.validateForDelivery(metadata: validationMetadata,
                                             expectedFocusToken: secondFocus) {
            revokedValidation = $0
        }
        guard revokedValidation == nil else {
            print("FAILED: held delivery validation completed synchronously")
            return false
        }
        validationLoader.plugins = []
        NotificationCenter.default.post(
            name: ActionPluginManager.didChangeNotification,
            object: nil,
            userInfo: [
                ActionPluginManager.rootPathUserInfoKey:
                    validationRoot.standardizedFileURL.path,
            ]
        )
        validationTransport.completeHeldStatus()
        guard runMainLoopUntil(timeout: 1, { revokedValidation != nil }),
              revokedValidation == .rejected(.stale),
              validationModel.blocks.count == 1 else {
            print("FAILED: disable during held send-validation must reject as stale")
            return false
        }

        func upgradedPlugin(version: String, title: String) -> InstalledActionPlugin {
            let original = loaded[0]
            let originalAction = original.manifest.actions[0]
            let upgradedAction = ActionPluginDefinition(
                id: originalAction.id,
                title: title,
                symbol: originalAction.symbol,
                statusPath: originalAction.statusPath,
                invokePath: originalAction.invokePath,
                modes: originalAction.modes
            )
            let upgradedManifest = ActionPluginManifest(
                schemaVersion: original.manifest.schemaVersion,
                id: original.manifest.id,
                name: original.manifest.name,
                version: version,
                runtimeConfigPaths: original.manifest.runtimeConfigPaths,
                actions: [upgradedAction]
            )
            return InstalledActionPlugin(manifest: upgradedManifest,
                                         directory: original.directory)
        }

        // Keeping plugin/action IDs stable across an upgrade must not let the
        // old generation's invoke callback or final-status callback stage text.
        let upgradeRoot = root.appendingPathComponent("upgrade-race", isDirectory: true)
        let upgradeLoader = ActionPluginLoaderBox([loaded[0]])
        let upgradeModel = BufferModel()
        let upgradeBus = InboundBus()
        let upgradeFocus = ActionPluginFocusBox()
        upgradeFocus.token = secondFocus
        let upgradeTransport = ActionPluginTransportStub(status: status,
                                                          response: response)
        let upgradeHost = ActionPluginHost(
            rootURL: upgradeRoot,
            client: upgradeTransport,
            focus: upgradeFocus.access,
            bufferModel: upgradeModel,
            inboundBus: upgradeBus,
            pluginLoader: upgradeLoader.load,
            runtimeBindingIsCurrent: { _, candidate in
                candidate == upgradeTransport.defaultBinding
            }
        )
        upgradeHost.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            upgradeHost.presentations.first?.canInvoke == true
        }), let upgradeKey = upgradeHost.presentations.first?.key else {
            print("FAILED: upgrade-race host did not become ready")
            return false
        }
        upgradeTransport.holdInvoke = true
        upgradeHost.invoke(upgradeKey)
        guard upgradeTransport.invokeRequestCount == 1,
              upgradeModel.loadingMessage != nil else {
            print("FAILED: upgrade-race invoke was not held")
            return false
        }
        let version2 = upgradedPlugin(version: "2.0.0", title: "生成 v2")
        upgradeLoader.plugins = [version2]
        NotificationCenter.default.post(
            name: ActionPluginManager.didChangeNotification,
            object: nil,
            userInfo: [
                ActionPluginManager.rootPathUserInfoKey:
                    upgradeRoot.standardizedFileURL.path,
                ActionPluginManager.changedPluginIDUserInfoKey:
                    loaded[0].manifest.id,
            ]
        )
        upgradeTransport.completeHeldInvoke()
        guard runMainLoopUntil(timeout: 1, {
            upgradeHost.presentations.first?.title == "生成 v2"
                && upgradeHost.presentations.first?.canInvoke == true
        }), upgradeModel.loadingMessage == nil,
           upgradeModel.blocks.isEmpty,
           upgradeBus.pendingCount == 0 else {
            print("FAILED: same-key manifest upgrade accepted an old invoke callback")
            return false
        }

        // Also cover the narrower window after invoke succeeded but its final
        // target status is still in flight. Here the management notification is
        // deliberately delayed; the loader read-through must still reject v2.
        upgradeTransport.holdInvoke = false
        upgradeTransport.holdStatus = true
        let statusRequestsBeforeFinalRace = upgradeTransport.statusRequestCount
        upgradeHost.invoke(upgradeKey)
        guard runMainLoopUntil(timeout: 1, {
            upgradeTransport.statusRequestCount > statusRequestsBeforeFinalRace
                && upgradeModel.loadingMessage != nil
        }) else {
            print("FAILED: final-status upgrade race was not held")
            return false
        }
        upgradeLoader.plugins = [upgradedPlugin(version: "3.0.0", title: "生成 v3")]
        upgradeTransport.completeHeldStatus()
        guard runMainLoopUntil(timeout: 1, { upgradeModel.loadingMessage == nil }),
              upgradeModel.blocks.isEmpty,
              upgradeBus.pendingCount == 0 else {
            print("FAILED: same-key upgrade accepted an old final-status callback")
            return false
        }

        // Exercise the real coordinator gate without an IMK client: validation
        // begins for focus A, focus changes to B, then even an "allowed" late
        // callback must not reach the injected delivery closure.
        var deliveryEpochs = FocusEpochState()
        let deliveryFocusA = deliveryEpochs.activate()
        let deliveryFocusB = deliveryEpochs.activate()
        var currentDeliveryFocus: FocusToken? = deliveryFocusA
        var insertedTexts: [String] = []
        var heldDeliveryValidation: ((ActionPluginDeliveryDecision) -> Void)?
        var asyncSendResult: BufferDeliveryCoordinator.SendResult?
        let deliveryCoordinator = BufferDeliveryCoordinator(
            model: targetFirstModel,
            dependencies: .init(
                resolveTarget: { expected in
                    guard let currentDeliveryFocus,
                          expected == nil || expected == currentDeliveryFocus else { return nil }
                    return .init(
                        token: currentDeliveryFocus,
                        compositionActive: false,
                        resolveComposition: {},
                        deliver: { block in
                            insertedTexts.append(block.text)
                            return true
                        }
                    )
                },
                secureInputEnabled: { false },
                validatePlugin: { _, _, completion in
                    heldDeliveryValidation = completion
                },
                refreshUI: {}
            )
        )
        // The isolated block was generated under secondFocus; copy it with the
        // test focus token so the coordinator reaches the asynchronous gate.
        targetFirstModel.discardForPrivacy()
        let deliveryMetadata = BufferModel.PluginMetadata(
            pluginId: generatedMetadata.pluginId,
            actionId: generatedMetadata.actionId,
            requestId: generatedMetadata.requestId,
            contextId: generatedMetadata.contextId,
            focusToken: deliveryFocusA,
            runtimeIdentity: generatedMetadata.runtimeIdentity,
            title: generatedMetadata.title,
            targetSummary: generatedMetadata.targetSummary,
            stale: false
        )
        targetFirstModel.stageExternal("生成的话术",
                                       origin: .plugin(id: generatedMetadata.pluginId),
                                       pluginMetadata: deliveryMetadata)
        let deferredResult = deliveryCoordinator.sendNext(
            expectedToken: deliveryFocusA,
            completion: { asyncSendResult = $0 }
        )
        guard deferredResult.deferred,
              heldDeliveryValidation != nil else {
            print("FAILED: plugin delivery must defer for fresh target validation")
            return false
        }
        currentDeliveryFocus = deliveryFocusB
        heldDeliveryValidation?(.allowed)
        guard insertedTexts.isEmpty,
              targetFirstModel.blocks.count == 1,
              targetFirstModel.blocks[0].pluginMetadata?.stale == true,
              asyncSendResult?.blockedReason == .pluginTargetChanged else {
            print("FAILED: focus A result must never insert after switching to focus B")
            return false
        }

        // If focus changes while generation is in flight, a byte-for-byte
        // matching response still must wait in the inbox for review.
        targetFirstModel.discardForPrivacy()
        targetFirstTransport.holdInvoke = true
        targetFirst.invoke(targetFirstKey)
        var moreFocusEpochs = FocusEpochState()
        let replacementFocus = moreFocusEpochs.activate()
        focusBox.token = replacementFocus
        targetFirst.focusDidChange()
        targetFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == true
        }) else {
            print("FAILED: stale legacy invocation still occupied the foreground slot")
            return false
        }
        targetFirstTransport.completeHeldInvoke()
        guard runMainLoopUntil(timeout: 1, { targetFirstBus.pendingCount == 1 }),
              targetFirstModel.blocks.isEmpty,
              targetFirstBus.pending[0].pluginMetadata?.stale == true else {
            print("FAILED: focus-changed result must route to the inbox")
            return false
        }

        // The same revocation applies before a legacy terminal callback is
        // routed. A still-live old endpoint cannot place its late result into
        // either the workbench or the review inbox after runtime rotation.
        targetFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == true
        }) else {
            print("FAILED: legacy runtime-rotation fixture did not become ready")
            return false
        }
        let inboxCountBeforeRotation = targetFirstBus.pendingCount
        targetFirst.invoke(targetFirstKey)
        targetRuntimeIsCurrent = false
        targetFirstTransport.completeHeldInvoke()
        guard runMainLoopUntil(timeout: 1, {
            targetFirstModel.loadingMessage == nil
        }), targetFirstModel.blocks.isEmpty,
           targetFirstBus.pendingCount == inboxCountBeforeRotation else {
            print("FAILED: legacy terminal result survived runtime rotation")
            return false
        }
        targetRuntimeIsCurrent = true

        // A full inbox must become a visible workbench notice rather than
        // silently dropping a stale result. Keep a newer foreground request B
        // running while parked A completes, so A cannot reuse or overwrite
        // B's transient-loading ownership.
        targetFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == true
        }) else {
            print("FAILED: replacement focus did not refresh before inbox-cap test")
            return false
        }
        while targetFirstBus.pendingCount < InboundBus.maxPending {
            _ = targetFirstBus.submit(origin: .mcp(client: "fill"),
                                      text: "占位 \(targetFirstBus.pendingCount)")
        }
        targetFirstTransport.holdInvoke = true
        targetFirst.invoke(targetFirstKey)
        let postCapacityFocus = moreFocusEpochs.activate()
        focusBox.token = postCapacityFocus
        targetFirst.focusDidChange()
        targetFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == true
        }) else {
            print("FAILED: full-inbox foreground B did not become ready")
            return false
        }
        targetFirst.invoke(targetFirstKey)
        targetFirstTransport.completeHeldInvoke()
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.workbenchFailureMessage?.contains("收信箱已满") == true
        }), targetFirstBus.pendingCount == InboundBus.maxPending,
           targetFirstModel.blocks.isEmpty,
           targetFirstModel.transientLoadingActive,
           targetFirstModel.loadingMessage?.contains("收信箱已满") != true else {
            print("FAILED: full inbox notice was lost behind foreground B")
            return false
        }
        targetFirstTransport.completeHeldInvoke()
        guard runMainLoopUntil(timeout: 1, { targetFirstModel.blocks.count == 1 }),
              targetFirst.workbenchFailureMessage?.contains("收信箱已满") == true,
              !targetFirstModel.transientLoadingActive else {
            print("FAILED: foreground B completion erased the background retention notice")
            return false
        }
        targetFirstTransport.holdInvoke = false
        targetFirstModel.discardForPrivacy()
        targetFirst.cancelActiveInvocationForWorkbench()

        // Marine/runtime unavailable disables only this action host; existing
        // Rime buffer state remains intact and no generation starts.
        targetFirstTransport.statusResult = .failure(ActionPluginHTTPError.runtimeUnavailable)
        targetFirst.refreshStatuses(force: true)
        guard runMainLoopUntil(timeout: 1, {
            targetFirst.presentations.first?.canInvoke == false
        }) else {
            print("FAILED: unavailable plugin runtime should disable its action")
            return false
        }

        let bus = InboundBus.shared
        let model = BufferModel.shared
        let oldEnabled = model.enabled
        defer {
            model.discardForPrivacy()
            bus.clear()
            model.enabled = oldEnabled
        }
        model.discardForPrivacy()
        bus.clear()
        let metadata = BufferModel.PluginMetadata(
            pluginId: "example",
            actionId: "example.generate",
            requestId: "req-1",
            contextId: "ctx-1",
            focusToken: secondFocus,
            runtimeIdentity: "instance:stub-instance",
            title: "回复 @用户",
            targetSummary: "原评论",
            stale: true
        )
        let pendingID = bus.submit(origin: .plugin(id: "example"),
                                   text: "迟到结果",
                                   title: "回复 @用户",
                                   pluginMetadata: metadata)
        guard let pendingID,
              bus.pendingCount == 1,
              model.blocks.isEmpty,
              bus.pending[0].pluginMetadata == metadata else {
            print("FAILED: stale plugin result must wait in inbound inbox")
            return false
        }
        bus.accept(pendingID)
        let reviewedMetadata = model.blocks.first?.pluginMetadata
        guard bus.pendingCount == 0,
              model.blocks.count == 1,
              model.blocks[0].origin == .plugin(id: "example"),
              reviewedMetadata?.pluginId == metadata.pluginId,
              reviewedMetadata?.targetSummary == metadata.targetSummary,
              reviewedMetadata?.stale == false,
              reviewedMetadata?.reviewedAsPlainText == true else {
            print("FAILED: accepting a stale plugin result must explicitly downgrade its binding")
            return false
        }
        var reviewedValidationCalled = false
        var reviewedInserted: [String] = []
        let reviewedCoordinator = BufferDeliveryCoordinator(
            model: model,
            dependencies: .init(
                resolveTarget: { expected in
                    guard expected == nil || expected == secondFocus else { return nil }
                    return .init(token: secondFocus,
                                 compositionActive: false,
                                 resolveComposition: {},
                                 deliver: { block in
                                     reviewedInserted.append(block.text)
                                     return true
                                 })
                },
                secureInputEnabled: { false },
                validatePlugin: { _, _, completion in
                    reviewedValidationCalled = true
                    completion(.rejected(.stale))
                },
                refreshUI: {}
            ),
            contentSourceResolver: { model }
        )
        let reviewedSend = reviewedCoordinator.sendNext(expectedToken: secondFocus)
        guard reviewedSend.succeeded,
              reviewedInserted == ["迟到结果"],
              !reviewedValidationCalled,
              model.blocks.isEmpty else {
            print("FAILED: a manually reviewed stale result must send only as ordinary text")
            return false
        }
    } catch {
        print("FAILED: action plugin smoke threw \(error)")
        return false
    }

    print("action plugin smoke OK")
    return true
}

func runOriginSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) origin/echo smoke test ==")

    // Only remote-peer origins are barred from mirroring; every other source
    // (local typing, agent drafts, network inbound) mirrors as before.
    let mirrors: [Origin] = [.rime, .marine, .plugin(id: "example"), .mcp(client: "x"),
                             .http(source: "s"), .sse(feed: "f"), .ssh(host: "h")]
    for o in mirrors where !o.allowsRemoteMirror {
        print("FAILED: \(o.tag) should mirror to remote")
        return false
    }
    guard !Origin.remotePeer(deviceID: "mac-b").allowsRemoteMirror else {
        print("FAILED: remotePeer must NOT mirror back (echo loop)")
        return false
    }

    // Origin identity is by-case AND by-payload, so two peers stay distinct.
    guard Origin.remotePeer(deviceID: "a") != Origin.remotePeer(deviceID: "b"),
          Origin.mcp(client: "a") != Origin.rime else {
        print("FAILED: origin equality is wrong")
        return false
    }

    // Block plumbing: default origin is rime; explicit origin is preserved and
    // readable where the send path reads it.
    let model = BufferModel.shared
    let oldEnabled = model.enabled
    defer { model.discardForPrivacy(); model.enabled = oldEnabled }
    model.enabled = true
    model.discardForPrivacy()
    model.append("你")
    model.append("hi", origin: .mcp(client: "codex"))
    guard model.blocks.count == 2,
          model.blocks[0].origin == .rime,
          model.blocks[1].origin == .mcp(client: "codex") else {
        print("FAILED: append did not carry origin: \(model.blocks.map(\.origin.tag))")
        return false
    }

    print("origin/echo smoke OK")
    return true
}

func runBufferSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) buffer smoke test ==")
    guard RimeKey.fromVirtualKeyCode(22) == 0x36,
          RimeKey.fromVirtualKeyCode(27) == 0x2d,
          RIMESController.shouldConsumeCodexBufferControlText(
              [0x1e, 0x1f], bundleId: "com.openai.codex", bufferActive: true
          ),
          !RIMESController.shouldConsumeCodexBufferControlText(
              [0x1e], bundleId: "com.apple.Terminal", bufferActive: true
          ),
          !RIMESController.shouldConsumeCodexBufferControlText(
              [0x1f], bundleId: "com.openai.codex", bufferActive: false
          ),
          !RIMESController.shouldConsumeCodexBufferControlText(
              [0x01], bundleId: "com.openai.codex", bufferActive: true
          ),
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x61, mask: RimeKey.controlMask
          ) == .selectAll,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x61, mask: RimeKey.superMask
          ) == .selectAll,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x76, mask: RimeKey.controlMask
          ) == .paste,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x76, mask: RimeKey.superMask
          ) == .paste,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x61,
            mask: RimeKey.superMask | RimeKey.shiftMask
          ) == nil,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x76,
            mask: RimeKey.superMask | RimeKey.controlMask
          ) == nil,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x63, mask: RimeKey.superMask
          ) == .copyGeneratedResult,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x63, mask: RimeKey.controlMask
          ) == nil,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x63,
            mask: RimeKey.superMask | RimeKey.shiftMask
          ) == nil,
          BufferClipboardShortcutRules.shortcut(
            keycode: 0x63,
            mask: RimeKey.superMask | RimeKey.lockMask
          ) == nil,
          BufferClipboardPhysicalShortcutRules.shortcut(
            aKeyDown: false,
            vKeyDown: true,
            mask: RimeKey.controlMask
          ) == .paste,
          BufferClipboardPhysicalShortcutRules.shortcut(
            aKeyDown: true,
            vKeyDown: false,
            mask: RimeKey.controlMask
          ) == .selectAll,
          BufferClipboardPhysicalShortcutRules.shortcut(
            aKeyDown: true,
            vKeyDown: true,
            mask: RimeKey.controlMask
          ) == nil,
          BufferClipboardPhysicalShortcutRules.shortcut(
            aKeyDown: false,
            vKeyDown: true,
            mask: RimeKey.controlMask | RimeKey.shiftMask
          ) == nil,
          BufferClipboardPhysicalShortcutRules.shortcut(
            aKeyDown: false,
            vKeyDown: false,
            cKeyDown: true,
            mask: RimeKey.superMask
          ) == .copyGeneratedResult,
          BufferClipboardPhysicalShortcutRules.shortcut(
            aKeyDown: true,
            vKeyDown: false,
            cKeyDown: true,
            mask: RimeKey.superMask
          ) == nil,
          BufferClipboardCommandRules.shortcut(
            selectorName: "pageDown:",
            physicalShortcut: .paste
          ) == .paste,
          BufferClipboardCommandRules.shortcut(
            selectorName: "pageDown:",
            physicalShortcut: nil
          ) == nil,
          BufferClipboardCommandRules.shortcut(
            selectorName: "pageDown:",
            physicalShortcut: .selectAll
          ) == nil,
          BufferClipboardCommandRules.shortcut(
            selectorName: "moveToBeginningOfParagraph:",
            physicalShortcut: .selectAll
          ) == .selectAll,
          BufferClipboardCommandRules.shortcut(
            selectorName: "paste:",
            physicalShortcut: nil
          ) == .paste,
          BufferClipboardCommandRules.shortcut(
            selectorName: "copy:",
            physicalShortcut: .copyGeneratedResult
          ) == .copyGeneratedResult,
          BufferClipboardCommandRules.shortcut(
            selectorName: "copy:",
            physicalShortcut: nil
          ) == nil,
          BufferClipboardTextRules.validated("可粘贴") == "可粘贴",
          BufferClipboardTextRules.validated("") == nil,
          BufferClipboardTextRules.validated("a\0b") == nil,
          runBufferGeneratedResultCopyRulesProbe(),
          BufferPluginKeyboardShortcutRules.direction(
            keycode: RimeKey.up,
            mask: RimeKey.superMask | RimeKey.shiftMask
          ) == -1,
          BufferPluginKeyboardShortcutRules.direction(
            keycode: RimeKey.down,
            mask: RimeKey.superMask | RimeKey.shiftMask
          ) == 1,
          BufferPluginKeyboardShortcutRules.direction(
            keycode: RimeKey.up,
            mask: RimeKey.superMask | RimeKey.shiftMask | RimeKey.controlMask
          ) == nil,
          BufferPluginKeyboardShortcutRules.direction(
            keycode: RimeKey.up, mask: RimeKey.altMask
          ) == nil,
          BufferPluginKeyboardShortcutRules.direction(
            keycode: RimeKey.left,
            mask: RimeKey.superMask | RimeKey.shiftMask
          ) == nil,
          BufferPluginKeyboardShortcutRules.commandDirection(
            selectorName: "moveToBeginningOfDocumentAndModifySelection:",
            physicalDirection: -1
          ) == -1,
          BufferPluginKeyboardShortcutRules.commandDirection(
            selectorName: "moveToEndOfDocumentAndModifySelection:",
            physicalDirection: 1
          ) == 1,
          BufferPluginKeyboardShortcutRules.commandDirection(
            selectorName: "moveDown:",
            physicalDirection: -1
          ) == nil,
          BufferLogicalNavigationRules.action(
            keycode: RimeKey.home,
            compositionSettled: true
          ) == .moveToStart,
          BufferLogicalNavigationRules.action(
            keycode: RimeKey.deleteForward,
            compositionSettled: true
          ) == .deleteForward,
          BufferLogicalNavigationRules.action(
            keycode: RimeKey.down,
            compositionSettled: true
          ) == .consume,
          BufferLogicalNavigationRules.action(
            keycode: RimeKey.down,
            compositionSettled: false
          ) == nil,
          BufferLogicalNavigationRules.commandAction(
            selectorName: "insertTab:"
          ) == .consume,
          BufferLogicalNavigationRules.commandAction(
            selectorName: "moveToEndOfDocument:"
          ) == .moveToEnd,
          BufferWorkbenchEscapeRoutingRules.disposition(
            isUnmodifiedEscape: true,
            workbenchVisible: true,
            ownClient: false,
            exactExternalFocus: true
          ) == .closeWorkbench,
          BufferWorkbenchEscapeRoutingRules.disposition(
            isUnmodifiedEscape: true,
            workbenchVisible: true,
            ownClient: false,
            exactExternalFocus: false
          ) == .consumeOnly,
          BufferWorkbenchEscapeRoutingRules.disposition(
            isUnmodifiedEscape: true,
            workbenchVisible: false,
            ownClient: false,
            exactExternalFocus: true
          ) == .passThrough,
          BufferWorkbenchEscapeRoutingRules.disposition(
            isUnmodifiedEscape: true,
            workbenchVisible: true,
            ownClient: true,
            exactExternalFocus: true
          ) == .passThrough,
          BufferWorkbenchEscapeRoutingRules.disposition(
            isUnmodifiedEscape: false,
            workbenchVisible: true,
            ownClient: false,
            exactExternalFocus: true
          ) == .passThrough else {
        print("FAILED: buffer control-key escape gate")
        return false
    }
    let printableFlags: NSEvent.ModifierFlags = []
    let blockedPrintableModifiers: [NSEvent.ModifierFlags] = [
        .command, .control, .option, .function,
    ]
    guard BufferUnhandledPrintableRules.capturedText(
              characters: "a Z0 .,!?",
              modifierFlags: printableFlags,
              bufferEnabled: true,
              exactExternalFocus: true,
              secureInputEnabled: false
          ) == "a Z0 .,!?",
          BufferUnhandledPrintableRules.capturedText(
              characters: "A",
              modifierFlags: [.shift, .capsLock],
              bufferEnabled: true,
              exactExternalFocus: true,
              secureInputEnabled: false
          ) == "A",
          BufferUnhandledPrintableRules.capturedText(
              characters: "a",
              modifierFlags: [],
              bufferEnabled: false,
              exactExternalFocus: true,
              secureInputEnabled: false
          ) == nil,
          BufferUnhandledPrintableRules.capturedText(
              characters: "a",
              modifierFlags: [],
              bufferEnabled: true,
              exactExternalFocus: false,
              secureInputEnabled: false
          ) == nil,
          BufferUnhandledPrintableRules.capturedText(
              characters: "a",
              modifierFlags: [],
              bufferEnabled: true,
              exactExternalFocus: true,
              secureInputEnabled: true
          ) == nil,
          BufferUnhandledPrintableRules.capturedText(
              characters: "a",
              modifierFlags: [.command],
              bufferEnabled: true,
              exactExternalFocus: true,
              secureInputEnabled: false
          ) == nil,
          blockedPrintableModifiers.allSatisfy({ flags in
              BufferUnhandledPrintableRules.capturedText(
                  characters: "a",
                  modifierFlags: flags,
                  bufferEnabled: true,
                  exactExternalFocus: true,
                  secureInputEnabled: false
              ) == nil
          }),
          BufferUnhandledPrintableRules.capturedText(
              characters: "é",
              modifierFlags: [],
              bufferEnabled: true,
              exactExternalFocus: true,
              secureInputEnabled: false
          ) == nil,
          BufferUnhandledPrintableRules.capturedText(
              characters: "\t",
              modifierFlags: [],
              bufferEnabled: true,
              exactExternalFocus: true,
              secureInputEnabled: false
          ) == nil,
          BufferUnhandledPrintableRules.shouldConsumeRejectedEvent(
              characters: "a Z0 .,! ?",
              modifierFlags: [.shift, .capsLock],
              bufferEnabled: true,
              externalClient: true,
              secureInputEnabled: false
          ),
          !BufferUnhandledPrintableRules.shouldConsumeRejectedEvent(
              characters: "a",
              modifierFlags: [],
              bufferEnabled: false,
              externalClient: true,
              secureInputEnabled: false
          ),
          !BufferUnhandledPrintableRules.shouldConsumeRejectedEvent(
              characters: "a",
              modifierFlags: [],
              bufferEnabled: true,
              externalClient: false,
              secureInputEnabled: false
          ),
          !BufferUnhandledPrintableRules.shouldConsumeRejectedEvent(
              characters: "a",
              modifierFlags: [],
              bufferEnabled: true,
              externalClient: true,
              secureInputEnabled: true
          ),
          blockedPrintableModifiers.allSatisfy({ flags in
              !BufferUnhandledPrintableRules.shouldConsumeRejectedEvent(
                  characters: "a",
                  modifierFlags: flags,
                  bufferEnabled: true,
                  externalClient: true,
                  secureInputEnabled: false
              )
          }) else {
        print("FAILED: unhandled printable buffer capture gate")
        return false
    }
    let model = BufferModel.shared
    let oldEnabled = model.enabled
    let oldOnChange = model.onChange
    defer {
        model.discardForPrivacy()
        model.enabled = oldEnabled
        model.onChange = oldOnChange
    }

    model.onChange = nil
    model.enabled = true
    model.discardForPrivacy()

    let directOwner = DirectInputRunOwner.testing(1)
    let directText = "Hello world, this is a phrase and more."
    var firstDirectID: UUID?
    for character in directText {
        let id = model.appendDirectInputFragment(String(character), owner: directOwner)
        if firstDirectID == nil { firstDirectID = id }
    }
    guard model.stagedText == directText,
          model.blocks.map(\.text) == [
              "Hello world, ",
              "this is a phrase ",
              "and more.",
          ],
          model.blocks.first?.id == firstDirectID else {
        print("FAILED: direct English phrase capture", model.blocks.map(\.text))
        return false
    }
    let tailID = model.blocks.last?.id
    guard model.deleteBackwardInDirectInput(owner: directOwner),
          model.stagedText == String(directText.dropLast()),
          model.blocks.last?.id == tailID else {
        print("FAILED: direct English character backspace")
        return false
    }
    let remainingTailCount = model.blocks.last?.text.count ?? 0
    for _ in 0..<remainingTailCount {
        guard model.deleteBackwardInDirectInput(owner: directOwner) else {
            print("FAILED: direct input tail removal")
            return false
        }
    }
    let previousTail = model.blocks.last?.text ?? ""
    guard !previousTail.isEmpty,
          model.deleteBackwardInDirectInput(owner: directOwner),
          model.blocks.last?.text == String(previousTail.dropLast()) else {
        print("FAILED: direct input character backspace across block boundary")
        return false
    }
    _ = model.appendDirectInputFragment("!", owner: directOwner)
    let countBeforeFocusChange = model.blocks.count
    _ = model.appendDirectInputFragment("X", owner: .testing(2))
    guard model.blocks.count == countBeforeFocusChange + 1,
          model.blocks.last?.text == "X" else {
        print("FAILED: direct input focus boundary")
        return false
    }
    model.append("中")
    _ = model.appendDirectInputFragment("Y", owner: .testing(2))
    guard model.blocks.suffix(2).map(\.text) == ["中", "Y"] else {
        print("FAILED: Rime commit must end direct input run")
        return false
    }
    model.discardForPrivacy()

    model.append("甲")
    model.append("乙")
    model.append("丙")
    guard model.setInsertionPoint(1),
          model.insertionIndex == 1,
          model.deleteForwardAtInsertionPoint(),
          model.blocks.map(\.text) == ["甲", "丙"],
          model.insertionIndex == 1,
          model.setInsertionPoint(99),
          model.insertionIndex == 2 else {
        print("FAILED: logical Buffer caret and forward deletion")
        return false
    }
    model.routeDirectPreservingContent(reason: "route smoke")
    guard model.inputRoute == .directToHost,
          !model.active,
          model.processingActive,
          model.blocks.map(\.text) == ["甲", "丙"] else {
        print("FAILED: direct route must preserve staged Buffer content")
        return false
    }
    let contentGenerationBeforeRouteSwitch = model.changeCount
    var routeEpochs = FocusEpochState()
    let captureToken = routeEpochs.activate()
    let unrelatedToken = routeEpochs.activate()
    model.activateCapture(for: captureToken)
    guard model.inputRoute == .captureToBuffer,
          model.active,
          model.processingActive,
          model.capturesInput(for: captureToken),
          !model.capturesInput(for: unrelatedToken),
          model.changeCount == contentGenerationBeforeRouteSwitch else {
        print("FAILED: exact-focus Buffer capture route")
        return false
    }
    model.discardForPrivacy()

    model.append("旧内容")
    let generationBeforeSelection = model.changeCount
    guard model.selectAllContent(),
          model.allContentSelected,
          model.changeCount == generationBeforeSelection,
          model.insertPastedText("New useful words and another phrase."),
          !model.allContentSelected,
          model.stagedText == "New useful words and another phrase.",
          model.blocks.count > 1,
          model.changeCount == generationBeforeSelection + 1 else {
        print("FAILED: select-all paste replacement and semantic segmentation")
        return false
    }
    _ = model.selectAllContent()
    model.append("替换")
    guard model.blocks.map(\.text) == ["替换"],
          !model.allContentSelected else {
        print("FAILED: local typing must replace selected workbench content")
        return false
    }
    _ = model.selectAllContent()
    model.beginTransientLoading(requestId: "selection-heartbeat", message: "处理中")
    model.updateTransientLoading(requestId: "selection-heartbeat", message: "仍在处理")
    guard model.allContentSelected else {
        print("FAILED: plugin status heartbeat must preserve select-all state")
        return false
    }
    model.finishTransientLoading(requestId: "selection-heartbeat")
    model.stageExternal("外部", origin: .mcp(client: "selection-smoke"))
    guard model.blocks.map(\.text) == ["替换", "外部"],
          !model.allContentSelected else {
        print("FAILED: asynchronous external content must invalidate, not replace, selection")
        return false
    }
    _ = model.selectAllContent()
    guard model.removeLastBlock(), model.blocks.isEmpty else {
        print("FAILED: backspace semantics must delete an all-selected buffer")
        return false
    }
    guard model.insertPastedText(" \n "),
          model.stagedText == " \n " else {
        print("FAILED: clipboard paste must preserve whitespace-only text")
        return false
    }
    model.discardForPrivacy()
    let detachedClipboardModel = BufferModel()
    guard detachedClipboardModel.insertPastedText(
            "detached clipboard",
            origin: .clipboard
          ),
          detachedClipboardModel.blocks.allSatisfy({ $0.origin == .clipboard }),
          detachedClipboardModel.inputRoute == .directToHost else {
        print("FAILED: detached clipboard provenance or input route")
        return false
    }

    // Accepted delivery attempts disappear from the live workbench without
    // retaining a plaintext delivery history.
    model.append("你")
    model.append("好")
    let firstID = model.blocks[0].id
    let secondID = model.blocks[1].id
    model.consumeDelivered(blockIDs: [firstID])
    guard model.blocks.map(\.text) == ["好"],
          model.pendingDeliveryBlocks.map(\.id) == [secondID],
          model.insertionIndex == 1,
          model.enabled else {
        print("FAILED: accepted delivery must consume only the accepted block",
              model.blocks.map(\.text),
              model.insertionIndex,
              model.enabled)
        return false
    }
    model.consumeDelivered(blockIDs: [secondID])
    guard model.blocks.isEmpty,
          model.pendingDeliveryBlocks.isEmpty,
          model.insertionIndex == 0 else {
        print("FAILED: full accepted delivery should empty the live workbench")
        return false
    }

    // Consuming non-adjacent accepted blocks must preserve the order of every
    // unsent block and keep the insertion point valid.
    model.append("甲")
    model.append("乙")
    model.append("丙")
    model.append("丁")
    let originalOrder = model.blocks.map(\.id)
    model.consumeDelivered(blockIDs: [originalOrder[1], originalOrder[3]])
    guard model.blocks.map(\.id) == [originalOrder[0], originalOrder[2]],
          model.pendingDeliveryBlocks.map(\.id) == [originalOrder[0], originalOrder[2]],
          model.insertionIndex == 2 else {
        print("FAILED: non-adjacent delivery consumption order")
        return false
    }

    model.discardForPrivacy()
    model.append("前")
    model.append("后")
    guard model.removeLastBlock(),
          model.blocks.map(\.text) == ["前"] else {
        print("FAILED: removeLastBlock should drop newest block",
              "blocks=\(model.blocks.map(\.text))")
        return false
    }
    guard model.removeLastBlock(),
          model.blocks.isEmpty,
          model.removeLastBlock() == false else {
        print("FAILED: removeLastBlock empty semantics",
              "blocks=\(model.blocks.map(\.text))")
        return false
    }

    model.discardForPrivacy()
    model.append("一")
    model.append("三")
    guard model.moveInsertionPoint(delta: -1),
          model.insertionIndex == 1 else {
        print("FAILED: moveInsertionPoint should move left",
              "index=\(model.insertionIndex)",
              "blocks=\(model.blocks.map(\.text))")
        return false
    }
    model.append("二")
    guard model.blocks.map(\.text) == ["一", "二", "三"],
          model.insertionIndex == 2 else {
        print("FAILED: append should insert at insertion point",
              "index=\(model.insertionIndex)",
              "blocks=\(model.blocks.map(\.text))")
        return false
    }
    _ = model.moveInsertionPoint(delta: 99)
    guard model.insertionIndex == model.blocks.count else {
        print("FAILED: insertion point should clamp to end",
              "index=\(model.insertionIndex)",
              "blocks=\(model.blocks.map(\.text))")
        return false
    }

    guard model.captureFocusToken == captureToken else {
        print("FAILED: pause fixture must begin with an exact capture token")
        return false
    }
    model.beginTransientLoading(requestId: "pause-smoke", message: "处理中")
    model.pauseCapturePreservingContent()
    guard !model.enabled,
          !model.transientEnabled,
          model.loadingMessage == nil,
          model.inputRoute == .directToHost,
          model.captureFocusToken == nil,
          !model.blocks.isEmpty else {
        print("FAILED: pause must preserve blocks and clear transient loading state")
        return false
    }

    model.discardForPrivacy()
    guard model.blocks.isEmpty,
          model.loadingMessage == nil,
          !model.transientEnabled else {
        print("FAILED: privacy discard must erase all live state")
        return false
    }

    print("buffer smoke: OK")
    return true
}

private func runWorkbenchShelfAlignmentProbe() -> Bool {
    let host = NSView(frame: NSRect(x: 0, y: 0, width: 760, height: 33))
    let shelf = NSStackView()
    shelf.translatesAutoresizingMaskIntoConstraints = false
    host.addSubview(shelf)

    let status = NSTextField(labelWithString: "")
    status.lineBreakMode = .byTruncatingTail
    status.setContentHuggingPriority(.defaultLow, for: .horizontal)
    status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    status.isHidden = true

    let selector = NSView()
    selector.translatesAutoresizingMaskIntoConstraints = false
    let selectorWidth = selector.widthAnchor.constraint(equalToConstant: 64)
    selectorWidth.isActive = true
    selector.heightAnchor.constraint(equalToConstant: 18).isActive = true

    let spinner = NSView()
    spinner.translatesAutoresizingMaskIntoConstraints = false
    spinner.widthAnchor.constraint(equalToConstant: 12).isActive = true
    spinner.heightAnchor.constraint(equalToConstant: 12).isActive = true
    spinner.isHidden = true

    let actionRow = NSStackView()
    actionRow.orientation = .horizontal
    actionRow.spacing = 2
    actionRow.detachesHiddenViews = false
    actionRow.isHidden = true
    actionRow.setContentHuggingPriority(.required, for: .horizontal)
    actionRow.setContentCompressionResistancePriority(.required, for: .horizontal)

    let pluginActions = NSStackView(views: [selector, spinner, actionRow])
    pluginActions.orientation = .horizontal
    pluginActions.alignment = .centerY
    pluginActions.distribution = .fill
    pluginActions.spacing = 4
    pluginActions.edgeInsets = NSEdgeInsets(top: 1, left: 5, bottom: 1, right: 3)
    pluginActions.detachesHiddenViews = true
    pluginActions.setContentHuggingPriority(.required, for: .horizontal)
    pluginActions.setContentCompressionResistancePriority(.defaultHigh, for: .horizontal)

    let flexibleSpace = NSView()
    let statusIndicators = NSStackView()
    let functionMenu = NSView()
    let exchangeEdit = NSView()
    let targetAssociation = NSView()
    let autoSend = NSView()
    let autoSendCloseAfterLast = NSView()
    let close = NSView()
    for control in [functionMenu, exchangeEdit, autoSend,
                    autoSendCloseAfterLast, targetAssociation, close] {
        control.translatesAutoresizingMaskIntoConstraints = false
        control.widthAnchor.constraint(equalToConstant: 22).isActive = true
        control.heightAnchor.constraint(equalToConstant: 22).isActive = true
    }
    BufferWorkbenchShelfLayout.configure(
        shelf,
        status: status,
        functionMenu: functionMenu,
        pluginActions: pluginActions,
        flexibleSpace: flexibleSpace,
        statusIndicators: statusIndicators,
        exchangeEdit: exchangeEdit,
        autoSend: autoSend,
        autoSendCloseAfterLast: autoSendCloseAfterLast,
        targetAssociation: targetAssociation,
        close: close
    )
    NSLayoutConstraint.activate([
        shelf.leadingAnchor.constraint(equalTo: host.leadingAnchor),
        shelf.trailingAnchor.constraint(equalTo: host.trailingAnchor),
        shelf.topAnchor.constraint(equalTo: host.topAnchor),
        shelf.bottomAnchor.constraint(equalTo: host.bottomAnchor),
    ])

    func geometry() -> (
        selector: CGFloat,
        targetAssociation: CGFloat,
        close: CGFloat,
        pluginWidth: CGFloat
    ) {
        host.layoutSubtreeIfNeeded()
        return (
            selector.convert(selector.bounds, to: host).minX,
            targetAssociation.convert(targetAssociation.bounds, to: host).maxX,
            close.convert(close.bounds, to: host).minX,
            pluginActions.bounds.width
        )
    }
    func trailingGap(after view: NSView) -> CGFloat {
        host.layoutSubtreeIfNeeded()
        let pluginFrame = pluginActions.convert(pluginActions.bounds, to: host)
        let lastVisibleFrame = view.convert(view.bounds, to: host)
        return pluginFrame.maxX - lastVisibleFrame.maxX
    }
    let baseline = geometry()
    let baselineTrailingGap = trailingGap(after: selector)

    spinner.isHidden = false
    let loadingOnly = geometry()
    let loadingTrailingGap = trailingGap(after: spinner)
    spinner.isHidden = true

    let option = NSView()
    option.translatesAutoresizingMaskIntoConstraints = false
    option.widthAnchor.constraint(equalToConstant: 116).isActive = true
    option.heightAnchor.constraint(equalToConstant: 18).isActive = true
    let action = NSView()
    action.translatesAutoresizingMaskIntoConstraints = false
    action.widthAnchor.constraint(equalToConstant: 80).isActive = true
    action.heightAnchor.constraint(equalToConstant: 18).isActive = true
    actionRow.addArrangedSubview(option)
    actionRow.addArrangedSubview(action)
    actionRow.isHidden = false
    let actionsOnly = geometry()
    let actionsTrailingGap = trailingGap(after: actionRow)

    status.stringValue = "可生成 · 发送前点选输入框"
    status.isHidden = false
    selectorWidth.constant = 108
    spinner.isHidden = false
    let dynamic = geometry()
    let dynamicTrailingGap = trailingGap(after: actionRow)

    status.stringValue = ""
    status.isHidden = true
    selectorWidth.constant = 64
    spinner.isHidden = true
    actionRow.isHidden = true
    let restoredEmpty = geometry()

    let epsilon: CGFloat = 0.5
    let expectedEmptySelectorX = BufferWorkbenchMetrics.shelfHorizontalInset
        + 22
        + BufferWorkbenchMetrics.shelfSpacing
        + pluginActions.edgeInsets.left
    let expectedEmptyPluginWidth = selectorWidth.constant
        + pluginActions.edgeInsets.left
        + pluginActions.edgeInsets.right
    let stable = abs(baseline.selector - expectedEmptySelectorX) <= epsilon
        && abs(loadingOnly.selector - baseline.selector) <= epsilon
        && abs(actionsOnly.selector - baseline.selector) <= epsilon
        && abs(dynamic.selector - baseline.selector) <= epsilon
        && abs(restoredEmpty.selector - baseline.selector) <= epsilon
        && abs(baseline.pluginWidth - expectedEmptyPluginWidth) <= epsilon
        && abs(baselineTrailingGap - pluginActions.edgeInsets.right) <= epsilon
        && loadingOnly.pluginWidth > baseline.pluginWidth
        && abs(loadingTrailingGap - pluginActions.edgeInsets.right) <= epsilon
        && actionsOnly.pluginWidth > baseline.pluginWidth
        && abs(actionsTrailingGap - pluginActions.edgeInsets.right) <= epsilon
        && dynamic.pluginWidth > baseline.pluginWidth
        && abs(dynamicTrailingGap - pluginActions.edgeInsets.right) <= epsilon
        && abs(restoredEmpty.pluginWidth - baseline.pluginWidth) <= epsilon
        && abs(baseline.close - loadingOnly.close) <= epsilon
        && abs(baseline.close - actionsOnly.close) <= epsilon
        && abs(baseline.close - dynamic.close) <= epsilon
        && abs(baseline.close - restoredEmpty.close) <= epsilon
        && abs(baseline.close - baseline.targetAssociation
            - BufferWorkbenchMetrics.shelfSpacing) <= epsilon
        && abs(loadingOnly.targetAssociation
            - baseline.targetAssociation) <= epsilon
        && abs(actionsOnly.targetAssociation
            - baseline.targetAssociation) <= epsilon
        && abs(dynamic.targetAssociation
            - baseline.targetAssociation) <= epsilon
        && abs(restoredEmpty.targetAssociation
            - baseline.targetAssociation) <= epsilon
    if !stable {
        print("FAILED: workbench shelf alignment moved",
              "baseline=\(baseline)",
              "dynamic=\(dynamic)",
              "restored=\(restoredEmpty)")
    }
    return stable
}

/// Blocks age on their own clocks: a newcomer starts at zero while its
/// elders keep their progress, a pause preserves every age rather than
/// resetting it, and only the head block can leave.
func runBufferAutoSendLifecycleProbe() -> Bool {
    runBufferAutoSendClockSmoke()
}

func runBufferWindowSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) buffer window smoke test ==")

    guard BufferTargetAssociationRules.state(
            rimeOwnsInput: true,
            contentProtected: false,
            captureActive: true,
            capturedTargetIsLive: true,
            hasLiveTarget: true,
            targetBox: .locked
          ) == .capturing,
          BufferTargetAssociationRules.state(
            rimeOwnsInput: true,
            contentProtected: false,
            captureActive: true,
            capturedTargetIsLive: false,
            hasLiveTarget: true,
            targetBox: .locked
          ) == .targetChanged,
          BufferTargetAssociationRules.state(
            rimeOwnsInput: true,
            contentProtected: false,
            captureActive: false,
            capturedTargetIsLive: false,
            hasLiveTarget: true,
            targetBox: .locked
          ) == .ready,
          BufferTargetAssociationRules.state(
            rimeOwnsInput: true,
            contentProtected: false,
            captureActive: false,
            capturedTargetIsLive: false,
            hasLiveTarget: false,
            targetBox: .unidentified
          ) == .unavailable,
          BufferTargetAssociationRules.state(
            rimeOwnsInput: false,
            contentProtected: false,
            captureActive: true,
            capturedTargetIsLive: true,
            hasLiveTarget: true,
            targetBox: .locked
          ) == .detached,
          BufferTargetAssociationRules.state(
            rimeOwnsInput: false,
            contentProtected: true,
            captureActive: true,
            capturedTargetIsLive: true,
            hasLiveTarget: true,
            targetBox: .locked
          ) == .protected,
          // Lock = application + box. A live application whose box cannot be
          // named is never presented as a send target, and a capture whose
          // box has lost focus reads as a focus change.
          BufferTargetAssociationRules.state(
            rimeOwnsInput: true,
            contentProtected: false,
            captureActive: true,
            capturedTargetIsLive: true,
            hasLiveTarget: true,
            targetBox: .unidentified
          ) == .capturingWithoutBox,
          BufferTargetAssociationRules.state(
            rimeOwnsInput: true,
            contentProtected: false,
            captureActive: true,
            capturedTargetIsLive: true,
            hasLiveTarget: true,
            targetBox: .changed
          ) == .targetChanged,
          BufferTargetAssociationRules.state(
            rimeOwnsInput: true,
            contentProtected: false,
            captureActive: false,
            capturedTargetIsLive: false,
            hasLiveTarget: true,
            targetBox: .unidentified
          ) == .boxUnidentified,
          BufferTargetAssociationRules.shouldClearCue(
            state: .capturingWithoutBox,
            hasPresentedCue: true,
            cueMatchesLiveTarget: true
          ),
          BufferTargetAssociationRules.shouldClearCue(
            state: .boxUnidentified,
            hasPresentedCue: true,
            cueMatchesLiveTarget: true
          ),
          // Capturing: only the box capture was granted for counts. Not
          // capturing: any identified box, and no box means no target.
          BufferTargetBoxRules.state(capturing: true, lockedBox: 1,
                                     currentBox: 1) == .locked,
          BufferTargetBoxRules.state(capturing: true, lockedBox: 1,
                                     currentBox: 2) == .changed,
          BufferTargetBoxRules.state(capturing: true, lockedBox: 1,
                                     currentBox: nil as Int?) == .changed,
          BufferTargetBoxRules.state(capturing: true, lockedBox: nil as Int?,
                                     currentBox: 1) == .unidentified,
          BufferTargetBoxRules.state(capturing: false, lockedBox: nil as Int?,
                                     currentBox: 1) == .locked,
          BufferTargetBoxRules.state(capturing: false, lockedBox: 1,
                                     currentBox: nil as Int?) == .unidentified,
          // A send needs a locked box vouched for by a recent sample; a stale
          // or missing sample refuses rather than assuming nothing moved.
          BufferTargetBoxRules.deliveryAllowed(state: .locked, sampleAge: 0.2),
          !BufferTargetBoxRules.deliveryAllowed(state: .locked, sampleAge: 5),
          !BufferTargetBoxRules.deliveryAllowed(state: .locked, sampleAge: nil),
          !BufferTargetBoxRules.deliveryAllowed(state: .locked, sampleAge: -1),
          !BufferTargetBoxRules.deliveryAllowed(state: .unidentified, sampleAge: 0.1),
          // An app that exposes no boxes locks to its input session; a box
          // appearing in it later is a change, not the same lock.
          BufferTargetBoxRules.state(capturing: true,
                                     lockedBox: BufferTargetBox.wholeSession,
                                     currentBox: .wholeSession) == .locked,
          BufferTargetBoxRules.state(capturing: true,
                                     lockedBox: BufferTargetBox.wholeSession,
                                     currentBox: .element(AXUIElementCreateApplication(getpid()))) == .changed,
          BufferTargetBoxRules.state(capturing: false,
                                     lockedBox: nil as BufferTargetBox?,
                                     currentBox: .wholeSession) == .locked,
          BufferTargetBox.element(AXUIElementCreateApplication(getpid()))
            == .element(AXUIElementCreateApplication(getpid())),
          !BufferTargetAssociationRules.shouldClearCue(
            state: .capturing,
            hasPresentedCue: true,
            cueMatchesLiveTarget: true
          ),
          BufferTargetAssociationRules.shouldClearCue(
            state: .ready,
            hasPresentedCue: true,
            cueMatchesLiveTarget: false
          ),
          !BufferTargetAssociationRules.shouldClearCue(
            state: .ready,
            hasPresentedCue: false,
            cueMatchesLiveTarget: false
          ),
          BufferTargetAssociationRules.shouldClearCue(
            state: .targetChanged,
            hasPresentedCue: false,
            cueMatchesLiveTarget: false
          ) else {
        print("FAILED: Buffer target-association state contract")
        return false
    }

    let associationPanel = NSRect(x: 100, y: 100, width: 200, height: 80)
    let topMarker = BufferTargetAssociationGeometry.marker(
        panelFrame: associationPanel,
        targetRect: NSRect(x: 199, y: 220, width: 2, height: 20)
    )
    let bottomMarker = BufferTargetAssociationGeometry.marker(
        panelFrame: associationPanel,
        targetRect: NSRect(x: 199, y: 40, width: 2, height: 20)
    )
    let leftMarker = BufferTargetAssociationGeometry.marker(
        panelFrame: associationPanel,
        targetRect: NSRect(x: 20, y: 139, width: 2, height: 2)
    )
    let rightMarker = BufferTargetAssociationGeometry.marker(
        panelFrame: associationPanel,
        targetRect: NSRect(x: 379, y: 139, width: 2, height: 2)
    )
    let clampedTopMarker = BufferTargetAssociationGeometry.marker(
        panelFrame: associationPanel,
        targetRect: NSRect(x: 100, y: 220, width: 2, height: 20)
    )
    guard topMarker == BufferTargetAssociationMarker(edge: .top, position: 0.5),
          bottomMarker == BufferTargetAssociationMarker(edge: .bottom, position: 0.5),
          leftMarker == BufferTargetAssociationMarker(edge: .left, position: 0.5),
          rightMarker == BufferTargetAssociationMarker(edge: .right, position: 0.5),
          clampedTopMarker?.edge == .top,
          abs((clampedTopMarker?.position ?? 0) - 0.07) < 0.0001,
          BufferTargetAssociationGeometry.marker(
            panelFrame: associationPanel,
            targetRect: NSRect(x: 180, y: 120, width: 10, height: 10)
          ) == nil,
          BufferTargetAssociationGeometry.marker(
            panelFrame: NSRect(x: CGFloat.nan, y: 0, width: 200, height: 80),
            targetRect: NSRect(x: 0, y: 0, width: 2, height: 20)
          ) == nil else {
        print("FAILED: Buffer target-association geometry contract")
        return false
    }

    let targetCue = TargetAssociationCueController()
    guard targetCue.hasSafeWindowContractForSmokeTest(),
          !targetCue.show(
            caretScreenRect: NSRect(x: CGFloat.nan, y: 0, width: 1, height: 20),
            accentColor: .systemBlue,
            level: .popUpMenu,
            reduceMotion: false
          ) else {
        print("FAILED: target-association cue window contract")
        return false
    }

    guard BufferDetachedClipboardRules.acceptedText("clipboard text")
            == "clipboard text",
          BufferDetachedClipboardRules.acceptedText(nil) == nil,
          BufferDetachedClipboardRules.acceptedText("") == nil,
          BufferDetachedClipboardRules.acceptedText("unsafe\0text") == nil,
          BufferDetachedClipboardRules.acceptedText(
            String(
                repeating: "a",
                count: BufferDetachedClipboardRules.maximumUTF8Bytes + 1
            )
          ) == nil else {
        print("FAILED: detached Buffer clipboard validation")
        return false
    }

    let pasteboard = NSPasteboard(
        name: .init("com.rimes.buffer-window-smoke.\(UUID().uuidString)")
    )
    defer { pasteboard.releaseGlobally() }
    pasteboard.clearContents()
    guard pasteboard.setString("preserve me", forType: .string),
          !BufferTextPasteboardWriter.write("", to: pasteboard),
          pasteboard.string(forType: .string) == "preserve me",
          BufferTextPasteboardWriter.write("detached result", to: pasteboard),
          pasteboard.string(forType: .string) == "detached result" else {
        print("FAILED: detached Buffer pasteboard write contract")
        return false
    }

    let preferenceSuiteName = "RimeBuffer.BufferWindowSmoke.\(UUID().uuidString)"
    guard let preferenceDefaults = UserDefaults(suiteName: preferenceSuiteName) else {
        print("FAILED: could not create isolated workbench preferences")
        return false
    }
    defer {
        preferenceDefaults.removePersistentDomain(forName: preferenceSuiteName)
    }
    guard BufferWorkbenchPreferences.closeAfterLastDelivery(
            defaults: preferenceDefaults
          ) else {
        print("FAILED: close-after-last-delivery did not default on")
        return false
    }
    BufferWorkbenchPreferences.setCloseAfterLastDelivery(
        false,
        defaults: preferenceDefaults
    )
    guard !BufferWorkbenchPreferences.closeAfterLastDelivery(
            defaults: preferenceDefaults
          ) else {
        print("FAILED: close-after-last-delivery preference did not persist off")
        return false
    }

    var commitRetirementOrder: [String] = []
    CommitPresentationRetirement.perform(
        owner: "focus-1",
        clearInline: { owner in
            commitRetirementOrder.append("inline:\(owner)")
        },
        hideCandidates: { owner in
            commitRetirementOrder.append("candidates:\(owner)")
        }
    )
    // Models the synchronous Delivery.insert boundary. Both projections must
    // already be retired when host code can re-enter the controller.
    commitRetirementOrder.append("insert")
    var missingOwnerRetirement: [String] = []
    CommitPresentationRetirement.perform(
        owner: Optional<String>.none,
        clearInline: { _ in missingOwnerRetirement.append("inline") },
        hideCandidates: { _ in missingOwnerRetirement.append("candidates") }
    )
    guard commitRetirementOrder
            == ["inline:focus-1", "candidates:focus-1", "insert"],
          missingOwnerRetirement.isEmpty else {
        print("FAILED: commit presentation was not retired before insertion")
        return false
    }

    guard InputSourceChangeDiagnosticRules.elapsedMilliseconds(
            previousUptime: 10.0,
            now: 10.025
          ) == 25,
          InputSourceChangeDiagnosticRules.elapsedMilliseconds(
            previousUptime: nil,
            now: 10.0
          ) == nil,
          InputSourceChangeDiagnosticRules.elapsedMilliseconds(
            previousUptime: 10.1,
            now: 10.0
          ) == nil,
          InputSourceChangeDiagnosticRules.controlIsDown([.control, .shift]),
          !InputSourceChangeDiagnosticRules.controlIsDown([.option]),
          RimeInputSourceAuthority.isOwnInputSourceID(
            RimesIdentity.bundleIdentifier,
            ownBundleID: RimesIdentity.bundleIdentifier
          ),
          RimeInputSourceAuthority.isOwnInputSourceID(
            RimesIdentity.inputSourceID,
            ownBundleID: RimesIdentity.bundleIdentifier
          ),
          // The retired identifier must still be recognised as ours: an
          // installation left under the old name is still this app.
          RimeInputSourceAuthority.isOwnInputSourceID(
            RimesIdentity.legacyBundleIdentifier + ".Hans",
            ownBundleID: RimesIdentity.legacyBundleIdentifier
          ),
          !RimeInputSourceAuthority.isOwnInputSourceID(
            "im.rime.inputmethod.Squirrel.Hans",
            ownBundleID: RimesIdentity.bundleIdentifier
          ),
          !RimeInputSourceAuthority.isOwnInputSourceID(
            "com.apple.keylayout.ABC",
            ownBundleID: RimesIdentity.bundleIdentifier
          ) else {
        print("FAILED: input-source transition diagnostics")
        return false
    }

    // Physical flagsChanged events close stream mutual pairing even when the
    // aggregate modifier bitset has no delta (for example, pressing the second
    // Shift/Command/Option/Control key). Ordinary key events are not boundaries.
    guard StreamInputModifierBoundaryRules.closesPairing(
        eventType: .flagsChanged
    ),
    !StreamInputModifierBoundaryRules.closesPairing(
        eventType: .keyDown
    ) else {
        print("FAILED: physical modifier transition stream boundary routing")
        return false
    }

    let candidateMatrix = CandidateWindow.matrixLayoutSnapshotForSmoke(rowCount: 3)
    let candidateLayoutEpsilon: CGFloat = 0.5
    guard candidateMatrix.initialPanelLevel == NSWindow.Level.popUpMenu.rawValue,
          candidateMatrix.iShotPanelLevel == Int(CGShieldingWindowLevel()),
          candidateMatrix.iShotPanelLevel > candidateMatrix.initialPanelLevel,
          candidateMatrix.hiddenPanelLevel == NSWindow.Level.popUpMenu.rawValue,
          CandidatePanelLevelRules.level(
            bundleID: "app.a",
            hostKind: .frontmostApplication
          ) == .popUpMenu,
          CandidatePanelLevelRules.level(
            bundleID: "com.apple.Spotlight",
            hostKind: .nonactivatingSystemOverlay
          ) == .popUpMenu,
          CandidatePanelLevelRules.level(
            bundleID: FocusHostRules.iShotBundleID,
            hostKind: .frontmostApplication
          ) == .popUpMenu,
          CandidatePanelSecurityRules.mayOrderFront(secureInputEnabled: false),
          !CandidatePanelSecurityRules.mayOrderFront(secureInputEnabled: true),
          candidateMatrix.rowCount == 3,
          candidateMatrix.rowHeights.allSatisfy({
              abs($0 - candidateMatrix.expectedRowHeight) <= candidateLayoutEpsilon
          }),
          abs(candidateMatrix.documentHeight
              - candidateMatrix.expectedDocumentHeight) <= candidateLayoutEpsilon,
          abs(candidateMatrix.scrollHeight
              - candidateMatrix.expectedDocumentHeight) <= candidateLayoutEpsilon,
          abs(candidateMatrix.panelHeight
              - candidateMatrix.expectedPanelHeight) <= candidateLayoutEpsilon,
          !candidateMatrix.documentTranslatesAutoresizingMaskIntoConstraints,
          candidateMatrix.documentAutoresizingMaskConstraintCount == 0 else {
        print("FAILED: three-row candidate matrix geometry",
              "panelLevels=\(candidateMatrix.initialPanelLevel)/\(candidateMatrix.iShotPanelLevel)/\(candidateMatrix.hiddenPanelLevel)",
              "rows=\(candidateMatrix.rowCount)",
              "rowHeights=\(candidateMatrix.rowHeights)",
              "document=\(candidateMatrix.documentHeight)/\(candidateMatrix.expectedDocumentHeight)",
              "scroll=\(candidateMatrix.scrollHeight)",
              "panel=\(candidateMatrix.panelHeight)/\(candidateMatrix.expectedPanelHeight)",
              "translatesMask=\(candidateMatrix.documentTranslatesAutoresizingMaskIntoConstraints)",
              "maskConstraints=\(candidateMatrix.documentAutoresizingMaskConstraintCount)")
        return false
    }

    let hotKeyDefaultsSuite = "RimeBuffer.GlobalHotKeySmoke.\(UUID().uuidString)"
    guard let hotKeyDefaults = UserDefaults(suiteName: hotKeyDefaultsSuite) else {
        print("FAILED: could not create isolated global hotkey preferences")
        return false
    }
    defer { hotKeyDefaults.removePersistentDomain(forName: hotKeyDefaultsSuite) }

    // Upgrade fixture: preserve a pre-existing custom binding on Capsule's
    // default Command-Shift-V and give the rail the deterministic first free
    // fallback instead of registering a duplicate hot key.
    let migrationDefaultsSuite =
        "RimeBuffer.ClipboardShortcutMigrationSmoke.\(UUID().uuidString)"
    guard let migrationDefaults = UserDefaults(suiteName: migrationDefaultsSuite),
          let customCommandShiftV = try? JSONEncoder().encode(
            RimeKeyboardShortcut(
                keyCode: UInt16(kVK_ANSI_V),
                modifiers: [.command, .shift]
            )
          ) else {
        print("FAILED: could not create Clipboard shortcut migration fixture")
        return false
    }
    defer {
        migrationDefaults.removePersistentDomain(forName: migrationDefaultsSuite)
    }
    migrationDefaults.set(
        customCommandShiftV,
        forKey: "keyboardShortcut.v1.\(RimeShortcutAction.openSettings.rawValue)"
    )
    let migratedClipboardShortcut = RimeShortcutPreferences.shortcut(
        for: .toggleClipboardHistory,
        defaults: migrationDefaults
    )
    let migratedClipboardHotKey = GlobalHotKeyRouting.definition(
        for: .toggleClipboardHistory,
        defaults: migrationDefaults
    )
    // With Capsule's manager shortcut retired, Command-Shift-C is free again
    // and is the first fallback.
    guard migratedClipboardShortcut == RimeKeyboardShortcut(
            keyCode: UInt16(kVK_ANSI_C),
            modifiers: [.command, .shift]
          ),
          RimeShortcutPreferences.shortcut(
            for: .openSettings,
            defaults: migrationDefaults
          ) == RimeKeyboardShortcut(
            keyCode: UInt16(kVK_ANSI_V),
            modifiers: [.command, .shift]
          ),
          migratedClipboardHotKey.keyCode == UInt32(kVK_ANSI_C),
          migratedClipboardHotKey.modifiers == UInt32(cmdKey | shiftKey) else {
        print("FAILED: Clipboard shortcut migration overwrote an existing binding")
        return false
    }

    let mailboxMigrationSuite =
        "RimeBuffer.MailboxShortcutMigrationSmoke.\(UUID().uuidString)"
    guard let mailboxMigrationDefaults = UserDefaults(suiteName: mailboxMigrationSuite),
          let legacyCommandShiftM = try? JSONEncoder().encode(
            RimeKeyboardShortcut(
                keyCode: UInt16(kVK_ANSI_M),
                modifiers: [.command, .shift]
            )
          ) else {
        print("FAILED: could not create Mailbox shortcut migration fixture")
        return false
    }
    defer {
        mailboxMigrationDefaults.removePersistentDomain(
            forName: mailboxMigrationSuite
        )
    }
    mailboxMigrationDefaults.set(
        legacyCommandShiftM,
        forKey: "keyboardShortcut.v1.\(RimeShortcutAction.openSettings.rawValue)"
    )
    let migratedMailboxShortcut = RimeShortcutPreferences.shortcut(
        for: .openMailbox,
        defaults: mailboxMigrationDefaults
    )
    guard migratedMailboxShortcut == RimeKeyboardShortcut(
            keyCode: UInt16(kVK_ANSI_N),
            modifiers: [.command, .shift]
          ),
          RimeShortcutPreferences.shortcut(
            for: .openSettings,
            defaults: mailboxMigrationDefaults
          ) == RimeKeyboardShortcut(
            keyCode: UInt16(kVK_ANSI_M),
            modifiers: [.command, .shift]
          ) else {
        print("FAILED: Mailbox shortcut migration overwrote an existing binding")
        return false
    }

    // Every component shortcut works under any input source: calling one up
    // under another input method selects RIMES before the component opens.
    guard GlobalHotKeyRouting.registeredActions() == Set(GlobalHotKeyAction.allCases),
          GlobalHotKeyRouting.registeredActions().contains(.openSettings) else {
        print("FAILED: global utility registration scope")
        return false
    }
    // A component waits for RIMES to own input, and for a field-bound one
    // (Buffer, Capsule rail) to take the focused field, unless time runs out.
    guard !RimeInputSourceSelectionRules.mayOpen(
            sourceIsOwn: false, hasFocusTarget: false, awaitsFocus: false, timedOut: false
          ),
          !RimeInputSourceSelectionRules.mayOpen(
            sourceIsOwn: true, hasFocusTarget: false, awaitsFocus: true, timedOut: false
          ),
          RimeInputSourceSelectionRules.mayOpen(
            sourceIsOwn: true, hasFocusTarget: true, awaitsFocus: true, timedOut: false
          ),
          RimeInputSourceSelectionRules.mayOpen(
            sourceIsOwn: true, hasFocusTarget: false, awaitsFocus: false, timedOut: false
          ),
          RimeInputSourceSelectionRules.mayOpen(
            sourceIsOwn: false, hasFocusTarget: false, awaitsFocus: true, timedOut: true
          ),
          RimeInputSourceSelection.handoffTimeout > 0,
          RimeInputSourceSelection.handoffTimeout <= 0.5 else {
        print("FAILED: component opening must wait for the RIMES input handoff")
        return false
    }

    let workbenchHotKey = GlobalHotKeyRouting.definition(
        for: .toggleWorkbench,
        defaults: hotKeyDefaults
    )
    let clipboardHotKey = GlobalHotKeyRouting.definition(
        for: .toggleClipboardHistory,
        defaults: hotKeyDefaults
    )
    let settingsHotKey = GlobalHotKeyRouting.definition(
        for: .openSettings,
        defaults: hotKeyDefaults
    )
    let mailboxHotKey = GlobalHotKeyRouting.definition(
        for: .openMailbox,
        defaults: hotKeyDefaults
    )
    let workbenchHotKeyID = workbenchHotKey.identifier
    let clipboardHotKeyID = clipboardHotKey.identifier
    let settingsHotKeyID = settingsHotKey.identifier
    let mailboxHotKeyID = mailboxHotKey.identifier
    let unrelatedHotKeyID = EventHotKeyID(
        signature: GlobalHotKeyRouting.signature,
        id: UInt32.max
    )
    let customSettingsShortcut = RimeKeyboardShortcut(
        keyCode: UInt16(kVK_ANSI_G),
        modifiers: [.control, .option]
    )
    do {
        try RimeShortcutPreferences.set(
            customSettingsShortcut,
            for: .openSettings,
            defaults: hotKeyDefaults
        )
    } catch {
        print("FAILED: could not persist isolated global hotkey preference \(error)")
        return false
    }
    let reloadedSettingsHotKey = GlobalHotKeyRouting.definition(
        for: .openSettings,
        defaults: hotKeyDefaults
    )
    let customClipboardShortcut = RimeKeyboardShortcut(
        keyCode: UInt16(kVK_ANSI_H),
        modifiers: [.control, .option]
    )
    do {
        try RimeShortcutPreferences.set(
            customClipboardShortcut,
            for: .toggleClipboardHistory,
            defaults: hotKeyDefaults
        )
    } catch {
        print("FAILED: could not persist isolated Clipboard shortcut \(error)")
        return false
    }
    let reloadedClipboardHotKey = GlobalHotKeyRouting.definition(
        for: .toggleClipboardHistory,
        defaults: hotKeyDefaults
    )
    let defaultDeliveryShortcut = RimeShortcutPreferences.shortcut(
        for: .deliverBuffer,
        defaults: hotKeyDefaults
    )
    let customDeliveryShortcut = RimeKeyboardShortcut(
        keyCode: UInt16(kVK_F6),
        modifiers: []
    )
    do {
        try RimeShortcutPreferences.set(
            customDeliveryShortcut,
            for: .deliverBuffer,
            defaults: hotKeyDefaults
        )
    } catch {
        print("FAILED: could not persist isolated delivery shortcut \(error)")
        return false
    }
    let commandDeliveryShortcut = RimeKeyboardShortcut(
        keyCode: UInt16(kVK_ANSI_D),
        modifiers: [.command]
    )
    do {
        try RimeShortcutPreferences.set(
            commandDeliveryShortcut,
            for: .deliverBuffer,
            defaults: hotKeyDefaults
        )
    } catch {
        print("FAILED: could not persist Command delivery shortcut \(error)")
        return false
    }
    var rejectedUnsafeBareShortcut = false
    do {
        try RimeShortcutPreferences.set(
            RimeKeyboardShortcut(keyCode: UInt16(kVK_ANSI_A), modifiers: []),
            for: .deliverBuffer,
            defaults: hotKeyDefaults
        )
    } catch RimeShortcutPreferenceError.unsafeBareKey {
        rejectedUnsafeBareShortcut = true
    } catch {
        print("FAILED: wrong unsafe delivery shortcut error \(error)")
        return false
    }
    var rejectedReservedEditingShortcut = false
    do {
        try RimeShortcutPreferences.set(
            RimeKeyboardShortcut(
                keyCode: UInt16(kVK_ANSI_A),
                modifiers: [.command]
            ),
            for: .previousPlugin,
            defaults: hotKeyDefaults
        )
    } catch RimeShortcutPreferenceError.reservedBufferEditingShortcut {
        rejectedReservedEditingShortcut = true
    } catch {
        print("FAILED: wrong reserved editing shortcut error \(error)")
        return false
    }
    var rejectedShortcutConflict = false
    do {
        try RimeShortcutPreferences.set(
            customSettingsShortcut,
            for: .previousPlugin,
            defaults: hotKeyDefaults
        )
    } catch RimeShortcutPreferenceError.conflict(.openSettings) {
        rejectedShortcutConflict = true
    } catch {
        print("FAILED: wrong shortcut conflict error \(error)")
        return false
    }
    let customWorkbenchShortcut = RimeKeyboardShortcut(
        keyCode: UInt16(kVK_ANSI_X),
        modifiers: [.command, .shift]
    )
    do {
        try RimeShortcutPreferences.set(
            customWorkbenchShortcut,
            for: .toggleWorkbench,
            defaults: hotKeyDefaults
        )
        try RimeShortcutPreferences.set(
            RimeShortcutAction.toggleWorkbench.defaultShortcut,
            for: .openSettings,
            defaults: hotKeyDefaults
        )
    } catch {
        print("FAILED: could not prepare reset conflict \(error)")
        return false
    }
    var rejectedResetConflict = false
    do {
        try RimeShortcutPreferences.reset(
            .toggleWorkbench,
            defaults: hotKeyDefaults
        )
    } catch RimeShortcutPreferenceError.conflict(.openSettings) {
        rejectedResetConflict = true
    } catch {
        print("FAILED: wrong reset conflict error \(error)")
        return false
    }

    guard workbenchHotKey.keyCode == UInt32(kVK_ANSI_B),
          workbenchHotKey.modifiers == UInt32(cmdKey | shiftKey),
          workbenchHotKey.registrationOptions == OptionBits(kEventHotKeyNoOptions),
          clipboardHotKey.keyCode == UInt32(kVK_ANSI_V),
          clipboardHotKey.modifiers == UInt32(cmdKey | shiftKey),
          clipboardHotKey.registrationOptions
            == OptionBits(kEventHotKeyExclusive),
          settingsHotKey.keyCode == UInt32(kVK_ANSI_S),
          settingsHotKey.modifiers == UInt32(cmdKey | shiftKey),
          settingsHotKey.registrationOptions == OptionBits(kEventHotKeyNoOptions),
          mailboxHotKey.keyCode == UInt32(kVK_ANSI_M),
          mailboxHotKey.modifiers == UInt32(cmdKey | shiftKey),
          mailboxHotKey.registrationOptions == OptionBits(kEventHotKeyExclusive),
          workbenchHotKeyID.id != settingsHotKeyID.id,
          clipboardHotKeyID.id != workbenchHotKeyID.id,
          clipboardHotKeyID.id != settingsHotKeyID.id,
          mailboxHotKeyID.id != workbenchHotKeyID.id,
          mailboxHotKeyID.id != clipboardHotKeyID.id,
          mailboxHotKeyID.id != settingsHotKeyID.id,
          reloadedSettingsHotKey.keyCode == UInt32(kVK_ANSI_G),
          reloadedSettingsHotKey.modifiers == UInt32(controlKey | optionKey),
          reloadedClipboardHotKey.keyCode == UInt32(kVK_ANSI_H),
          reloadedClipboardHotKey.modifiers == UInt32(controlKey | optionKey),
          defaultDeliveryShortcut
            == RimeShortcutAction.deliverBuffer.defaultShortcut,
          RimeShortcutPreferences.shortcut(
            for: .deliverBuffer,
            defaults: hotKeyDefaults
          ) == commandDeliveryShortcut,
          rejectedUnsafeBareShortcut,
          rejectedReservedEditingShortcut,
          rejectedShortcutConflict,
          rejectedResetConflict,
          RimeShortcutPreferences.shortcut(
            for: .toggleWorkbench,
            defaults: hotKeyDefaults
          ) == customWorkbenchShortcut,
          GlobalHotKeyRouting.route(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed),
            identifier: workbenchHotKeyID
          ) == .toggleWorkbench,
          GlobalHotKeyRouting.route(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed),
            identifier: clipboardHotKeyID
          ) == .toggleClipboardHistory,
          GlobalHotKeyRouting.route(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed),
            identifier: settingsHotKeyID
          ) == .openSettings,
          GlobalHotKeyRouting.route(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed),
            identifier: mailboxHotKeyID
          ) == .openMailbox,
          GlobalHotKeyRouting.route(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyReleased),
            identifier: workbenchHotKeyID
          ) == .ignore,
          GlobalHotKeyRouting.route(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed),
            identifier: unrelatedHotKeyID
          ) == .ignore else {
        print("FAILED: global workbench/Capsule/Mailbox/settings hotkey routing")
        return false
    }

    let ready = BufferDeliveryCoordinator.Availability.ready
    let readyText = BufferWorkbenchStatusText.text(for: ready, secureInput: false)
    let readyHelp = BufferWorkbenchStatusText.help(for: ready, secureInput: false)
    let composing = BufferDeliveryCoordinator.Availability.blocked(.composing)
    let composingText = BufferWorkbenchStatusText.text(
        for: composing,
        secureInput: false
    )
    let composingHelp = BufferWorkbenchStatusText.help(
        for: composing,
        secureInput: false
    )
    let contextOnlyNoFocusText = BufferWorkbenchStatusText.text(
        for: .blocked(.noFocusedField),
        secureInput: false,
        canGenerateWithoutFocus: true
    )
    let contextOnlyNoFocusHelp = BufferWorkbenchStatusText.help(
        for: .blocked(.noFocusedField),
        secureInput: false,
        canGenerateWithoutFocus: true
    )
    let standardSourceOffset = BufferWorkbenchMetrics.mainControlYOffset(
        row: .source, mode: .standard
    )
    let standardTargetOffset = BufferWorkbenchMetrics.mainControlYOffset(
        row: .target, mode: .standard
    )
    let translationSourceOffset = BufferWorkbenchMetrics.mainControlYOffset(
        row: .source, mode: .translation
    )
    let translationTargetOffset = BufferWorkbenchMetrics.mainControlYOffset(
        row: .target, mode: .translation
    )
    let compactDerivedTargetOffset = BufferWorkbenchMetrics.mainControlYOffset(
        row: .target, mode: .singleDerived
    )
    let streamSourceOffset = BufferWorkbenchMetrics.mainControlYOffset(
        row: .source, mode: .derived(targetRows: 3)
    )
    let streamTargetOffset = BufferWorkbenchMetrics.mainControlYOffset(
        row: .target, mode: .derived(targetRows: 3)
    )
    let statusReadySnapshot = TranslationRailSnapshot(
        sourceText: "status-source",
        outputBlocks: [TranslationOutputBlock(id: UUID(), text: "status-result")],
        phase: .ready
    )
    let statusWaitingSnapshot = TranslationRailSnapshot(
        sourceText: "status-source",
        outputBlocks: [],
        phase: .waiting
    )
    let statusFailedSnapshot = TranslationRailSnapshot(
        sourceText: "status-source",
        outputBlocks: [],
        phase: .failed,
        message: "生成失败"
    )
    let retainedOutputFailedSnapshot = TranslationRailSnapshot(
        sourceText: "status-source",
        outputBlocks: [TranslationOutputBlock(id: UUID(), text: "旧结果")],
        phase: .failed,
        message: "更新失败"
    )
    let exchangeSourceSnapshot = TranslationRailSnapshot(
        sourceText: "exchange-source",
        outputBlocks: [],
        phase: .idle
    )
    let emptyLiveSnapshot = TranslationRailSnapshot(
        sourceText: "",
        outputBlocks: [],
        phase: .idle,
        sourceEmptyText: "输入内容",
        targetEmptyText: "等待结果"
    )
    let emptyLiveWaitingSnapshot = TranslationRailSnapshot(
        sourceText: "",
        outputBlocks: [],
        phase: .waiting,
        sourceEmptyText: "输入内容",
        targetEmptyText: "等待结果"
    )
    let standaloneMessageSnapshot = TranslationRailSnapshot(
        sourceText: "",
        outputBlocks: [],
        phase: .failed,
        message: "处理失败",
        sourceEmptyText: "输入内容",
        targetEmptyText: "等待结果"
    )
    let targetOnlyOutputSnapshot = TranslationRailSnapshot(
        sourceText: "",
        outputBlocks: [TranslationOutputBlock(id: UUID(), text: "独立结果")],
        phase: .ready
    )
    guard BufferWorkbenchLayout.mainBar == [.bufferRail],
          BufferWorkbenchLayout.railOverlay
            == [.clipboardImport, .copyResult, .send],
          BufferWorkbenchLayout.toolbar
            == [.functionMenu, .pluginActions, .status, .exchangeEdit,
                .autoSend, .autoSendCloseAfterLast, .targetAssociation, .close],
          BufferWorkbenchLayout.hoverControls
            == [.copyResult, .send, .clipboardImport, .functionMenu,
                .pluginActions, .exchangeEdit, .autoSend, .close],
          BufferWorkbenchLayout.passiveControls
            == [.bufferRail, .status, .targetAssociation],
          BufferWorkbenchLayout.toolbarInitiallyExpanded,
          BufferWorkbenchLayout.toolbarEmptySpaceDraggable,
          BufferWorkbenchToolbarDragRules.disposition(
            hitIsInteractiveControl: false
          ) == .dragWindow,
          BufferWorkbenchToolbarDragRules.disposition(
            hitIsInteractiveControl: true
          ) == .interactWithControl,
          runBufferWorkbenchToolbarHitTestProbe(),
          runBufferPopUpMenuGeometryProbe(),
          BufferWindowController.defaultAutoSendLifetime == 1,
          BufferWindowController.autoSendLifetimeChoices == [1, 2, 3, 5],
          // Automatic delivery is the Default buffer's alone: any selected
          // plugin, and the music surface, own their own delivery rhythm.
          BufferAutoSendAvailabilityRules.isAvailable(
            pluginSelected: false, musicSelected: false
          ),
          !BufferAutoSendAvailabilityRules.isAvailable(
            pluginSelected: true, musicSelected: false
          ),
          !BufferAutoSendAvailabilityRules.isAvailable(
            pluginSelected: false, musicSelected: true
          ),
          !BufferAutoSendAvailabilityRules.isAvailable(
            pluginSelected: true, musicSelected: true
          ),
          runBufferAutoSendLifecycleProbe(),
          runBufferInlineNoLeadingControlProbe(),
          runBufferInlineTrailingActionExclusionProbe(),
          BufferWorkbenchPointerRules.state(
            enabled: true, hovered: false, pressed: false
          ) == .idle,
          BufferWorkbenchPointerRules.state(
            enabled: true, hovered: true, pressed: false
          ) == .hovered,
          BufferWorkbenchPointerRules.state(
            enabled: true, hovered: true, pressed: true
          ) == .pressed,
          BufferWorkbenchPointerRules.state(
            enabled: false, hovered: true, pressed: true
          ) == .disabled,
          BufferWorkbenchPointerRules.cursor(enabled: true) == .pointingHand,
          BufferWorkbenchPointerRules.cursor(enabled: false) == .arrow,
          BufferWorkbenchMetrics.controlSize == 22,
          BufferWorkbenchMetrics.primaryControlWidth
              == BufferWorkbenchMetrics.controlSize,
          BufferWorkbenchMetrics.primaryControlHeight
              == BufferWorkbenchMetrics.controlSize,
          BufferWorkbenchLayoutMode.derived(targetRows: 99).targetRows == 5,
          BufferAlternativePagerRules.maximumCount == 5,
          ProductIdentity.displayName == ProductBrand.name,
          BufferWorkbenchMetrics.mainSpacing == 3,
          BufferWorkbenchMetrics.shelfSpacing == 4,
          BufferWorkbenchMetrics.shelfStatusWidth == 88,
          BufferWorkbenchStatusPresentation.text(
            fallback: readyText,
            snapshot: nil
          ).isEmpty,
          BufferWorkbenchStatusPresentation.text(
            fallback: contextOnlyNoFocusText,
            snapshot: nil
          ) == contextOnlyNoFocusText,
          BufferWorkbenchStatusPresentation.text(
            fallback: "AI 已就绪",
            snapshot: statusReadySnapshot
          ).isEmpty,
          BufferWorkbenchStatusPresentation.text(
            fallback: "正在生成",
            snapshot: statusWaitingSnapshot,
            style: .singleExchange
          ).isEmpty,
          BufferWorkbenchStatusPresentation.text(
            fallback: "生成失败",
            snapshot: statusFailedSnapshot,
            style: .singleExchange
          ) == "生成失败",
          BufferWorkbenchStatusPresentation.text(
            fallback: "正在翻译",
            snapshot: emptyLiveWaitingSnapshot,
            style: .liveExpand
          ) == "正在翻译",
          BufferWorkbenchStatusPresentation.text(
            fallback: "更新失败",
            snapshot: retainedOutputFailedSnapshot,
            style: .singleExchange
          ) == "更新失败",
          BufferWorkbenchStatusPresentation.tone(
            snapshot: statusFailedSnapshot,
            text: "生成失败"
          ) == .danger,
          AITextProviderKind.allCases.allSatisfy({
            BufferDerivedPresentationRules.style(for: $0.pluginKey) == .singleExchange
          }),
          BufferDerivedPresentationRules.style(
            for: AITextBuiltInPluginID.retiredUnifiedKey
          ) == .liveExpand,
          BufferDerivedPresentationRules.style(
            for: MarineChromeWorkspace.pluginKey
          ) == .singleExchange,
          AITextProviderKind.allCases.allSatisfy({
            BufferDerivedPresentationRules.nativeContract(for: $0.pluginKey)
                == .derived(.singleExchange)
          }),
          BufferDerivedPresentationRules.nativeContract(
            for: MarineChromeWorkspace.pluginKey
          ) == .derived(.singleExchange),
          BufferDerivedPresentationRules.nativeContract(
            for: RemarkableWorkspace.pluginKey
          ) == .standardBufferImport,
          BufferDerivedPresentationRules.style(for: nil) == .liveExpand,
          BufferDerivedPresentationRules.visibleRails(
            style: .liveExpand,
            snapshot: emptyLiveSnapshot
          ) == BufferDerivedRailVisibility(showsSource: true, showsTarget: false),
          BufferDerivedPresentationRules.visibleRails(
            style: .liveExpand,
            snapshot: emptyLiveWaitingSnapshot
          ) == BufferDerivedRailVisibility(showsSource: true, showsTarget: false),
          BufferDerivedPresentationRules.visibleRails(
            style: .liveExpand,
            snapshot: standaloneMessageSnapshot
          ) == BufferDerivedRailVisibility(showsSource: false, showsTarget: true),
          BufferDerivedPresentationRules.visibleRails(
            style: .liveExpand,
            snapshot: targetOnlyOutputSnapshot
          ) == BufferDerivedRailVisibility(showsSource: false, showsTarget: true),
          BufferDerivedPresentationRules.visibleRails(
            style: .liveExpand,
            snapshot: statusWaitingSnapshot
          ) == BufferDerivedRailVisibility(showsSource: true, showsTarget: true),
          BufferDerivedPresentationRules.layoutMode(
            style: .singleExchange,
            snapshot: exchangeSourceSnapshot
          ) == .singleDerived,
          BufferDerivedPresentationRules.layoutMode(
            style: .singleExchange,
            snapshot: statusReadySnapshot
          ) == .singleDerived,
          BufferDerivedPresentationRules.layoutMode(
            style: .liveExpand,
            snapshot: statusReadySnapshot
          ) == .derived(targetRows: 1),
          BufferDerivedPresentationRules.layoutMode(
            style: .liveExpand,
            snapshot: emptyLiveSnapshot
          ) == .singleDerived,
          BufferDerivedPresentationRules.layoutMode(
            style: .liveExpand,
            snapshot: standaloneMessageSnapshot
          ) == .singleDerived,
          BufferDerivedPresentationRules.showsExchangeActions(
            style: .singleExchange,
            snapshot: statusReadySnapshot
          ),
          !BufferDerivedPresentationRules.showsExchangeActions(
            style: .singleExchange,
            snapshot: statusWaitingSnapshot
          ),
          BufferWorkbenchShelfLayout.flexiblePriority.rawValue == 1,
          BufferWorkbenchShelfLayout.statusWidthPriority.rawValue == 749,
          runWorkbenchShelfAlignmentProbe(),
          BufferInlineMetrics.blockSpacing == 5,
          BufferInlineMetrics.railHorizontalInset == 5,
          BufferInlineMetrics.itemHeight == 20,
          BufferInlineMetrics.blockUnderlineWidth == 1,
          BufferInlineMetrics.selectedBlockUnderlineWidth == 2,
          BufferInlineMetrics.messageCornerRadius == 5,
          BufferInlineMetrics.contentSpacing == 3,
          BufferInlineMetrics.originBadgeSize == 5,
          BufferInlineMetrics.messageHorizontalInset == 5,
          // Underlined blocks need no padding: three blocks, two badged,
          // spend 26pt on chrome where bubbles spent 46pt.
          BufferInlineMetrics.packedBlockChromeWidth(
            blockCount: 3,
            badgedBlockCount: 2
          ) == 26,
          standardSourceOffset == 0,
          standardTargetOffset == 0,
          compactDerivedTargetOffset == 0,
          translationSourceOffset == -16,
          translationTargetOffset == 16,
          translationSourceOffset < 0,
          translationTargetOffset > 0,
          translationSourceOffset == -translationTargetOffset,
          streamSourceOffset == -16,
          streamTargetOffset == 16,
          streamSourceOffset == -streamTargetOffset,
          !BufferWorkbenchLayout.windowBackgroundDraggable,
          FirstMouseButton(frame: .zero).acceptsFirstMouse(for: nil),
          readyText == "可发送",
          composingText.isEmpty,
          !composing.canSend,
          composing.blocksManualGenerationRequest,
          composingHelp == BufferDeliveryCoordinator.BlockedReason.composing.message,
          contextOnlyNoFocusText == "可生成 · 发送前点选输入框",
          contextOnlyNoFocusHelp.contains("可以先生成内容"),
          contextOnlyNoFocusHelp.contains("发送前"),
          BufferWorkbenchStatusText.text(
            for: .blocked(.noFocusedField),
            secureInput: false
          ) == "等待输入框",
          BufferWorkbenchStatusText.text(
            for: .blocked(.noFocusedField),
            secureInput: true,
            canGenerateWithoutFocus: true
          ) == "安全输入，内容已隐藏",
          !readyText.contains("ChatGPT"),
          !readyText.contains("→"),
          !readyHelp.contains("ChatGPT"),
          !readyHelp.contains("发送到"),
          BufferWorkbenchStatusText.text(for: ready, secureInput: true)
            == "安全输入，内容已隐藏",
          BufferWorkbenchStatusText.text(
            for: .blocked(.validatingPluginTarget), secureInput: false
          ) == "正在确认目标",
          BufferWorkbenchStatusText.text(
            for: .blocked(.stalePluginResult), secureInput: false
          ) == "插件结果已过期",
          BufferWorkbenchStatusText.text(
            for: .blocked(.pluginTargetChanged), secureInput: false
          ) == "评论目标已变化",
          BufferWorkbenchStatusText.text(
            for: .blocked(.pluginUnavailable), secureInput: false
          ) == "插件暂不可用" else {
        print("FAILED: simplified workbench control/status contract")
        return false
    }

    guard BufferControlRoutingRules.disposition(
            bufferActive: false, ownClient: false, exactFocus: true
          ) == .passThrough,
          BufferControlRoutingRules.disposition(
            bufferActive: true, ownClient: true, exactFocus: true
          ) == .passThrough,
          BufferControlRoutingRules.disposition(
            bufferActive: true, ownClient: false, exactFocus: true
          ) == .executeBufferAction,
          BufferControlRoutingRules.disposition(
            bufferActive: true, ownClient: false, exactFocus: false
          ) == .consumeOnly,
          BufferEnterSecureInputRules.disposition(secureInputEnabled: false)
            == .normal,
          BufferEnterSecureInputRules.disposition(secureInputEnabled: true)
            == .consumeWithoutGuardOrGeneration else {
        print("FAILED: buffer Return/Backspace isolation disposition")
        return false
    }

    guard WorkbenchManualGenerationPrimaryActionRules.resolve(
            isGenerating: false,
            hasReadyDelivery: false,
            canGenerate: false
          ) == .disabled,
          WorkbenchManualGenerationPrimaryActionRules.resolve(
            isGenerating: false,
            hasReadyDelivery: false,
            canGenerate: true
          ) == .requestGeneration,
          WorkbenchManualGenerationPrimaryActionRules.resolve(
            isGenerating: true,
            hasReadyDelivery: true,
            canGenerate: true
          ) == .generating,
          WorkbenchManualGenerationPrimaryActionRules.resolve(
            isGenerating: false,
            hasReadyDelivery: true,
            canGenerate: true
          ) == .deliver,
          !WorkbenchManualGenerationPrimaryAction.disabled.beginsDeliveryGesture,
          !WorkbenchManualGenerationPrimaryAction.requestGeneration.beginsDeliveryGesture,
          !WorkbenchManualGenerationPrimaryAction.generating.beginsDeliveryGesture,
          WorkbenchManualGenerationPrimaryAction.deliver.beginsDeliveryGesture,
          BufferDeliveryCoordinator.Availability.blocked(.composing)
            .blocksManualGenerationRequest,
          !BufferDeliveryCoordinator.Availability.blocked(.nothingPending)
            .blocksManualGenerationRequest,
          !BufferDeliveryCoordinator.Availability.ready
            .blocksManualGenerationRequest else {
        print("FAILED: AI primary action state/Return routing contract")
        return false
    }

    let shortShiftTap = ShiftModifierGesture(
        beganAt: 10,
        rimeKeycode: RimeKey.shiftL,
        session: 7,
        schemaID: "rime_ice"
    )
    let shortRightShiftTap = ShiftModifierGesture(
        beganAt: 10,
        rimeKeycode: RimeKey.shiftR,
        session: 7,
        schemaID: "rime_ice"
    )
    var usedShift = shortShiftTap
    usedShift.noteModifierUse()
    let premodifiedShift = ShiftModifierGesture(
        beganAt: 10,
        rimeKeycode: RimeKey.shiftL,
        session: 7,
        schemaID: "rime_ice",
        beganWithOtherModifier: true
    )
    var overlappingShift = shortShiftTap
    overlappingShift.noteModifierUse()
    var cancelledShift = shortShiftTap
    cancelledShift.cancelForFocusChange()
    let physicalLeftShift = ShiftModifierEventRules.rimeKeycode(
        forHardwareKeyCode: UInt16(kVK_Shift)
    )
    let physicalRightShift = ShiftModifierEventRules.rimeKeycode(
        forHardwareKeyCode: UInt16(kVK_RightShift)
    )
    let commandReleaseWithShiftMask = ShiftModifierEventRules.rimeKeycode(
        forHardwareKeyCode: UInt16(kVK_Command)
    )
    let primaryHotKeyReleaseWithShiftMask = ShiftModifierEventRules.rimeKeycode(
        forHardwareKeyCode: UInt16(kVK_ANSI_V)
    )
    var shiftInterruptedByAggregateDelta = shortShiftTap
    if commandReleaseWithShiftMask == nil {
        // Mirror handleFlags' fail-closed path: an aggregate Shift delta on a
        // non-Shift callback must make an existing gesture non-replayable.
        shiftInterruptedByAggregateDelta.noteModifierUse()
    }
    // The real Carbon/IMK failure is not limited to one optional gesture. A
    // process-wide tombstone must catch a real Shift gesture spanning the hot
    // key callback and releases delivered to multiple controller instances,
    // while non-Shift modifier callbacks cannot synthesize a replacement.
    var hotKeyShiftTombstone = GlobalHotKeyShiftTombstone()
    let armedHotKeyShiftTombstone = hotKeyShiftTombstone.record(
        route: .toggleWorkbench,
        eventTimestamp: 20.05,
        shortcutUsesShift: true
    )
    let completedBeforeHotKeyWasSuppressed =
        hotKeyShiftTombstone.suppressesRelease(
            beganAt: 20.0,
            releasedAt: 20.04
        )
    let crossedHotKeyGestureWasSuppressed =
        hotKeyShiftTombstone.suppressesRelease(
        beganAt: 20.0,
        releasedAt: 20.1
    )
    let duplicateControllerReleaseWasSuppressed =
        hotKeyShiftTombstone.suppressesRelease(
            beganAt: 20.0,
            releasedAt: 20.11
        )
    let nextStandaloneReleaseWasSuppressed =
        hotKeyShiftTombstone.suppressesRelease(
            beganAt: 20.12,
            releasedAt: 20.2
        )
    var freshHotKeyTombstone = GlobalHotKeyShiftTombstone()
    _ = freshHotKeyTombstone.record(
        route: .toggleWorkbench,
        eventTimestamp: 30.0,
        shortcutUsesShift: true
    )
    let freshNextStandaloneWasSuppressed =
        freshHotKeyTombstone.suppressesRelease(
            beganAt: 30.01,
            releasedAt: 30.1
        )
    let ignoredTombstone = GlobalHotKeyShiftTombstone()
    var ignoredRouteTombstone = ignoredTombstone
    let armedIgnoredTombstone = ignoredRouteTombstone.record(
        route: .ignore,
        eventTimestamp: 20.05,
        shortcutUsesShift: true
    )
    var noShiftTombstone = ignoredTombstone
    let armedNoShiftTombstone = noShiftTombstone.record(
        route: .openSettings,
        eventTimestamp: 20.05,
        shortcutUsesShift: false
    )
    var monotonicTombstone = hotKeyShiftTombstone
    let rejectedOlderTombstone = monotonicTombstone.record(
        route: .openSettings,
        eventTimestamp: 20.04,
        shortcutUsesShift: true
    )
    var primaryKeyTombstone = GlobalHotKeyPrimaryKeyTombstone()
    let armedPrimaryKeyTombstone = primaryKeyTombstone.record(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 40.05
    )
    let exactPrimaryKeyDown = primaryKeyTombstone.observeRegisteredKeyDown(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 40.05
    )
    let duplicatePrimaryKeyDown = primaryKeyTombstone.observeRegisteredKeyDown(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 40.05
    )
    let recordedPrimaryKeyRelease = primaryKeyTombstone.recordRelease(
        action: .toggleClipboardHistory,
        eventTimestamp: 40.10
    )
    let intervalPrimaryKeyUp = primaryKeyTombstone.evaluate(
        eventType: .keyUp,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 40.10
    )
    let otherPrimaryKey = primaryKeyTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_A),
        eventTimestamp: 40.08
    )
    let nextPhysicalPrimaryKeyDown = primaryKeyTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 40.11
    )
    var earlierPrimaryKeyTombstone = GlobalHotKeyPrimaryKeyTombstone()
    _ = earlierPrimaryKeyTombstone.record(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 50.0
    )
    let earlierPrimaryKeyEvent = earlierPrimaryKeyTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 49.99
    )
    var provisionalPrimaryKeyTombstone = GlobalHotKeyPrimaryKeyTombstone()
    _ = provisionalPrimaryKeyTombstone.record(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 60.0
    )
    _ = provisionalPrimaryKeyTombstone.observeRegisteredKeyDown(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 60.0
    )
    let provisionalPrimaryKeyUp = provisionalPrimaryKeyTombstone.evaluate(
        eventType: .keyUp,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 60.05
    )
    let duplicateWithinProvisionalInterval = provisionalPrimaryKeyTombstone
        .evaluate(
            eventType: .keyDown,
            keyCode: UInt16(kVK_ANSI_V),
            eventTimestamp: 60.01
        )
    let registeredClipboardDefinition = GlobalHotKeyDefinition(
        action: .toggleClipboardHistory,
        keyCode: UInt32(kVK_ANSI_V),
        modifiers: UInt32(cmdKey | shiftKey)
    )
    let registeredClipboardMatch = GlobalHotKeyRouting.primaryKeyMatch(
        definitions: [registeredClipboardDefinition],
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        modifierFlags: [.command, .shift]
    )
    let ordinaryVMatch = GlobalHotKeyRouting.primaryKeyMatch(
        definitions: [registeredClipboardDefinition],
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        modifierFlags: []
    )
    let extraModifierVMatch = GlobalHotKeyRouting.primaryKeyMatch(
        definitions: [registeredClipboardDefinition],
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        modifierFlags: [.command, .shift, .option]
    )
    let registeredClipboardKeyUpMatch = GlobalHotKeyRouting.primaryKeyMatch(
        definitions: [registeredClipboardDefinition],
        eventType: .keyUp,
        keyCode: UInt16(kVK_ANSI_V),
        modifierFlags: [.command, .shift]
    )
    guard let imkFirstDownIdentity = GlobalHotKeyPrimaryKeyEventIdentity(
            keyCode: UInt16(kVK_ANSI_V),
            phase: .keyDown,
            cgTimestamp: 70_000
          ),
          let imkFirstUpIdentity = GlobalHotKeyPrimaryKeyEventIdentity(
            keyCode: UInt16(kVK_ANSI_V),
            phase: .keyUp,
            cgTimestamp: 70_050
          ),
          let ordinaryVAfterIMKFirstIdentity =
            GlobalHotKeyPrimaryKeyEventIdentity(
                keyCode: UInt16(kVK_ANSI_V),
                phase: .keyDown,
                cgTimestamp: 70_060
            ),
          let carbonFirstDownIdentity = GlobalHotKeyPrimaryKeyEventIdentity(
            keyCode: UInt16(kVK_ANSI_V),
            phase: .keyDown,
            cgTimestamp: 80_000
          ),
          let carbonFirstUpIdentity = GlobalHotKeyPrimaryKeyEventIdentity(
            keyCode: UInt16(kVK_ANSI_V),
            phase: .keyUp,
            cgTimestamp: 80_050
          ),
          let ordinaryVAfterCarbonFirstIdentity =
            GlobalHotKeyPrimaryKeyEventIdentity(
                keyCode: UInt16(kVK_ANSI_V),
                phase: .keyDown,
                cgTimestamp: 80_060
            ),
          let strippedCarbonFirstIdentity = GlobalHotKeyPrimaryKeyEventIdentity(
            keyCode: UInt16(kVK_ANSI_V),
            phase: .keyDown,
            cgTimestamp: 85_000
          ),
          let ordinaryVAfterStrippedCarbonIdentity =
            GlobalHotKeyPrimaryKeyEventIdentity(
                keyCode: UInt16(kVK_ANSI_V),
                phase: .keyDown,
                cgTimestamp: 85_100
            ),
          let missingReleaseDownIdentity = GlobalHotKeyPrimaryKeyEventIdentity(
            keyCode: UInt16(kVK_ANSI_V),
            phase: .keyDown,
            cgTimestamp: 90_000
          ),
          let ordinaryVAfterMissingReleaseIdentity =
            GlobalHotKeyPrimaryKeyEventIdentity(
                keyCode: UInt16(kVK_ANSI_V),
                phase: .keyDown,
                cgTimestamp: 90_100
            ) else {
        print("FAILED: global hotkey primary-key event identities")
        return false
    }

    // IMK-first: the exact live registration owns the key before Carbon has
    // armed a tombstone. Carbon subsequently attaches its press/release, and
    // the first ordinary V after release must pass and retire the record.
    var imkFirstPrimaryKeyTombstone = GlobalHotKeyPrimaryKeyTombstone()
    let imkFirstPrimaryKeyDown = imkFirstPrimaryKeyTombstone
        .observeRegisteredKeyDown(
            action: .toggleClipboardHistory,
            route: .toggleClipboardHistory,
            keyCode: UInt16(kVK_ANSI_V),
            eventTimestamp: 7_000.0,
            eventIdentity: imkFirstDownIdentity
        )
    let imkFirstCarbonPress = imkFirstPrimaryKeyTombstone.record(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 70.0,
        eventIdentity: imkFirstDownIdentity
    )
    let imkFirstCarbonRelease = imkFirstPrimaryKeyTombstone.recordRelease(
        action: .toggleClipboardHistory,
        eventTimestamp: 70.05,
        eventIdentity: imkFirstUpIdentity
    )
    let imkFirstPrimaryKeyUp = imkFirstPrimaryKeyTombstone.evaluate(
        eventType: .keyUp,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 7_000.05,
        eventIdentity: imkFirstUpIdentity
    )
    let ordinaryVAfterIMKFirst = imkFirstPrimaryKeyTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 70.0,
        eventIdentity: ordinaryVAfterIMKFirstIdentity
    )
    let delayedIMKFirstDuplicate = imkFirstPrimaryKeyTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 12_000.0,
        eventIdentity: imkFirstDownIdentity
    )

    // Carbon-first: the framework-local floating clocks deliberately disagree,
    // while the shared CG identity proves ownership. Missing release debt also
    // retires on the next different physical keyDown.
    var carbonFirstPrimaryKeyTombstone = GlobalHotKeyPrimaryKeyTombstone()
    let carbonFirstPress = carbonFirstPrimaryKeyTombstone.record(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 80.0,
        eventIdentity: carbonFirstDownIdentity
    )
    let carbonFirstPrimaryKeyDown = carbonFirstPrimaryKeyTombstone
        .observeRegisteredKeyDown(
            action: .toggleClipboardHistory,
            route: .toggleClipboardHistory,
            keyCode: UInt16(kVK_ANSI_V),
            eventTimestamp: 8_000.0,
            eventIdentity: carbonFirstDownIdentity
        )
    let carbonFirstRelease = carbonFirstPrimaryKeyTombstone.recordRelease(
        action: .toggleClipboardHistory,
        eventTimestamp: 80.05,
        eventIdentity: carbonFirstUpIdentity
    )
    let carbonFirstPrimaryKeyUp = carbonFirstPrimaryKeyTombstone.evaluate(
        eventType: .keyUp,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 8_000.05,
        eventIdentity: carbonFirstUpIdentity
    )
    let ordinaryVAfterCarbonFirst = carbonFirstPrimaryKeyTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 80.0,
        eventIdentity: ordinaryVAfterCarbonFirstIdentity
    )
    let delayedCarbonFirstDuplicate = carbonFirstPrimaryKeyTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 16_000.0,
        eventIdentity: carbonFirstDownIdentity
    )
    // A Carbon-first callback whose modifiers were stripped bypasses the exact
    // registration preflight, but the common CG identity still consumes it.
    // Equal floating timestamps with a different CG identity must pass.
    var strippedCarbonFirstTombstone = GlobalHotKeyPrimaryKeyTombstone()
    _ = strippedCarbonFirstTombstone.record(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 85.0,
        eventIdentity: strippedCarbonFirstIdentity
    )
    let strippedCarbonFirstKeyDown = strippedCarbonFirstTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 8_500.0,
        eventIdentity: strippedCarbonFirstIdentity
    )
    let ordinaryVAfterStrippedCarbon = strippedCarbonFirstTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 85.0,
        eventIdentity: ordinaryVAfterStrippedCarbonIdentity
    )
    let delayedStrippedCarbonDuplicate = strippedCarbonFirstTombstone.evaluate(
        eventType: .keyDown,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 17_000.0,
        eventIdentity: strippedCarbonFirstIdentity
    )
    var missingReleasePrimaryKeyTombstone = GlobalHotKeyPrimaryKeyTombstone()
    _ = missingReleasePrimaryKeyTombstone.record(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 90.0,
        eventIdentity: missingReleaseDownIdentity
    )
    _ = missingReleasePrimaryKeyTombstone.observeRegisteredKeyDown(
        action: .toggleClipboardHistory,
        route: .toggleClipboardHistory,
        keyCode: UInt16(kVK_ANSI_V),
        eventTimestamp: 9_000.0,
        eventIdentity: missingReleaseDownIdentity
    )
    let ordinaryVAfterMissingRelease = missingReleasePrimaryKeyTombstone
        .evaluate(
            eventType: .keyDown,
            keyCode: UInt16(kVK_ANSI_V),
            eventTimestamp: 90.0,
            eventIdentity: ordinaryVAfterMissingReleaseIdentity
        )
    guard physicalLeftShift == RimeKey.shiftL,
          physicalRightShift == RimeKey.shiftR,
          commandReleaseWithShiftMask == nil,
          primaryHotKeyReleaseWithShiftMask == nil,
          shiftInterruptedByAggregateDelta.releaseDecision(
            at: 10.2,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .discard,
          ShiftModifierGesture.standaloneTapLimit == 0.5,
          // macOS redelivers flagsChanged for one physical Shift press: same
          // key, no aggregate delta. Only the *other* Shift key is a real
          // second transition. Reading a redelivery as one voided the gesture
          // its own press had just created, so no standalone tap ever reached
          // librime and Chinese/English could not be switched at all.
          !ShiftModifierEventRules.isSecondShiftTransition(
            aggregateDeltaIsEmpty: true,
            shiftIsDown: true,
            hardwareKeyCode: 56,
            inFlightGestureKeycode: RimeKey.shiftL
          ),
          ShiftModifierEventRules.isSecondShiftTransition(
            aggregateDeltaIsEmpty: true,
            shiftIsDown: true,
            hardwareKeyCode: 60,
            inFlightGestureKeycode: RimeKey.shiftL
          ),
          ShiftModifierEventRules.isSecondShiftTransition(
            aggregateDeltaIsEmpty: true,
            shiftIsDown: true,
            hardwareKeyCode: 56,
            inFlightGestureKeycode: nil
          ),
          !ShiftModifierEventRules.isSecondShiftTransition(
            aggregateDeltaIsEmpty: false,
            shiftIsDown: true,
            hardwareKeyCode: 60,
            inFlightGestureKeycode: RimeKey.shiftL
          ),
          !ShiftModifierEventRules.isSecondShiftTransition(
            aggregateDeltaIsEmpty: true,
            shiftIsDown: false,
            hardwareKeyCode: 60,
            inFlightGestureKeycode: RimeKey.shiftL
          ),
          !ShiftModifierEventRules.isSecondShiftTransition(
            aggregateDeltaIsEmpty: true,
            shiftIsDown: true,
            hardwareKeyCode: 55,
            inFlightGestureKeycode: RimeKey.shiftL
          ),
          shortShiftTap.releaseDecision(
            at: 10.499,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .replayStandaloneTap(rimeKeycode: RimeKey.shiftL),
          shortRightShiftTap.releaseDecision(
            at: 10.1,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .replayStandaloneTap(rimeKeycode: RimeKey.shiftR),
          shortShiftTap.releaseDecision(
            at: 10.5,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .discard,
          usedShift.releaseDecision(
            at: 10.1,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .discard,
          premodifiedShift.releaseDecision(
            at: 10.1,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .discard,
          overlappingShift.releaseDecision(
            at: 10.1,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .discard,
          cancelledShift.releaseDecision(
            at: 10.1,
            currentSession: 7,
            currentSchemaID: "rime_ice"
          ) == .discard,
          armedHotKeyShiftTombstone,
          !completedBeforeHotKeyWasSuppressed,
          crossedHotKeyGestureWasSuppressed,
          duplicateControllerReleaseWasSuppressed,
          !nextStandaloneReleaseWasSuppressed,
          !freshNextStandaloneWasSuppressed,
          !armedIgnoredTombstone,
          ignoredRouteTombstone.hotKeyAt == nil,
          !armedNoShiftTombstone,
          noShiftTombstone.hotKeyAt == nil,
          !rejectedOlderTombstone,
          monotonicTombstone.hotKeyAt == 20.05,
          monotonicTombstone.route == .toggleWorkbench,
          armedPrimaryKeyTombstone,
          exactPrimaryKeyDown.disposition == .consume,
          exactPrimaryKeyDown.deltaMicroseconds == 0,
          duplicatePrimaryKeyDown.disposition == .consume,
          recordedPrimaryKeyRelease,
          intervalPrimaryKeyUp.disposition == .consume,
          otherPrimaryKey.disposition == .passThrough,
          nextPhysicalPrimaryKeyDown.disposition == .passThrough,
          primaryKeyTombstone.keyCode == nil,
          earlierPrimaryKeyEvent.disposition == .passThrough,
          earlierPrimaryKeyEvent.deltaMicroseconds == nil,
          earlierPrimaryKeyTombstone.keyCode == nil,
          provisionalPrimaryKeyUp.disposition == .consume,
          duplicateWithinProvisionalInterval.disposition == .consume,
          provisionalPrimaryKeyTombstone.imkReleasedAtMicroseconds
            == 60_050_000,
          registeredClipboardMatch == GlobalHotKeyPrimaryKeyMatch(
            action: .toggleClipboardHistory,
            route: .toggleClipboardHistory,
            keyCode: UInt16(kVK_ANSI_V)
          ),
          ordinaryVMatch == nil,
          extraModifierVMatch == nil,
          registeredClipboardKeyUpMatch == nil,
          imkFirstPrimaryKeyDown.disposition == .consume,
          imkFirstPrimaryKeyDown.deltaMicroseconds == 0,
          imkFirstCarbonPress,
          imkFirstCarbonRelease,
          imkFirstPrimaryKeyUp.disposition == .consume,
          ordinaryVAfterIMKFirst.disposition == .passThrough,
          imkFirstPrimaryKeyTombstone.keyCode == nil,
          delayedIMKFirstDuplicate.disposition == .consume,
          carbonFirstPress,
          carbonFirstPrimaryKeyDown.disposition == .consume,
          carbonFirstPrimaryKeyDown.deltaMicroseconds == 0,
          carbonFirstRelease,
          carbonFirstPrimaryKeyUp.disposition == .consume,
          ordinaryVAfterCarbonFirst.disposition == .passThrough,
          carbonFirstPrimaryKeyTombstone.keyCode == nil,
          delayedCarbonFirstDuplicate.disposition == .consume,
          strippedCarbonFirstKeyDown.disposition == .consume,
          ordinaryVAfterStrippedCarbon.disposition == .passThrough,
          strippedCarbonFirstTombstone.keyCode == nil,
          delayedStrippedCarbonDuplicate.disposition == .consume,
          ordinaryVAfterMissingRelease.disposition == .passThrough,
          missingReleasePrimaryKeyTombstone.keyCode == nil,
          usedShift.releaseDecision(
            at: 10.1,
            currentSession: 8,
            currentSchemaID: "rime_ice"
          ) == .discard,
          usedShift.releaseDecision(
            at: 10.1,
            currentSession: 7,
            currentSchemaID: "my_combo"
          ) == .discard else {
        print("FAILED: deferred standalone/modified/held Shift routing")
        return false
    }

    // Host isolation and semantic composition are separate. In particular an
    // idle buffer must keep Chromium in IME-composing mode without making our
    // Return state machine think there is Rime input left to settle.
    guard HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: true,
            capturesRimeCommits: true,
            rimeComposing: false,
            secureInput: false
          ) == .bufferGuard(rimeComposing: false),
          HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: true,
            capturesRimeCommits: true,
            rimeComposing: true,
            secureInput: false
          ) == .bufferGuard(rimeComposing: true),
          // Transient external blocks own idle Return, but local Rime preedit
          // remains inline because transient mode does not capture typing.
          HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: true,
            capturesRimeCommits: false,
            rimeComposing: false,
            secureInput: false
          ) == .bufferGuard(rimeComposing: false),
          HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: true,
            capturesRimeCommits: false,
            rimeComposing: true,
            secureInput: false
          ) == .normalPreedit,
          HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: false,
            capturesRimeCommits: true,
            rimeComposing: false,
            secureInput: false
          ) == .normalPreedit,
          HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: false,
            capturesRimeCommits: false,
            rimeComposing: true,
            secureInput: false,
            stagedChordGuardActive: true
          ) == .bufferGuard(rimeComposing: true),
          HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: false,
            capturesRimeCommits: false,
            rimeComposing: true,
            secureInput: true,
            stagedChordGuardActive: true
          ) == .none,
          HostMarkedTextPresentationRules.presentation(
            bufferControlsActive: true,
            capturesRimeCommits: true,
            rimeComposing: false,
            secureInput: true
          ) == .none,
          HostMarkedTextPresentationRules.shouldRefreshForActiveChange(
            previous: false, current: true
          ),
          !HostMarkedTextPresentationRules.shouldRefreshForActiveChange(
            previous: true, current: true
          ) else {
        print("FAILED: buffer host marked-text isolation presentation")
        return false
    }

    let halfHoldDecision = BufferEnterGestureRules.pollDecision(
        isPhysicalDown: true,
        elapsed: 0.3,
        holdDelay: 0.6
    )
    let halfHoldProgress: Double
    if case let .wait(progress) = halfHoldDecision {
        halfHoldProgress = progress
    } else {
        halfHoldProgress = -1
    }
    guard BufferEnterGestureRules.pollDecision(
            isPhysicalDown: false,
            elapsed: 0.2,
            holdDelay: 0.6
          ) == .sendNext,
          // A delayed poll after a quick release must remain a tap, never an
          // accidental send-all just because wall time crossed the threshold.
          BufferEnterGestureRules.pollDecision(
            isPhysicalDown: false,
            elapsed: 2.0,
            holdDelay: 0.6
          ) == .sendNext,
          abs(halfHoldProgress - 0.5) < 0.000_001,
          BufferEnterGestureRules.pollDecision(
            isPhysicalDown: true,
            elapsed: 0.6,
            holdDelay: 0.6
          ) == .sendAll else {
        print("FAILED: buffer Enter tap/hold poll decision")
        return false
    }

    // Sending the final transient block makes buffer routing pass-through
    // immediately, but the keyUp and newline command from that already-owned
    // physical press must remain consumed independently of model state.
    var lateCommandOwnership = BufferEnterCallbackOwnership()
    lateCommandOwnership.claimPress()
    let inactiveAfterFinalBlock = BufferControlRoutingRules.disposition(
        bufferActive: false,
        ownClient: false,
        exactFocus: true
    )
    guard inactiveAfterFinalBlock == .passThrough,
          lateCommandOwnership.consumeKeyUp() == .consumeOwned,
          lateCommandOwnership.suppressesNewlineCommand,
          lateCommandOwnership.routeNewlineCommand() == .consumeOwned,
          // Duplicate/stale callbacks from the same generation remain hidden;
          // command delivery itself must not consume the protection.
          lateCommandOwnership.routeNewlineCommand() == .consumeOwned,
          lateCommandOwnership.prepareForKeyDown(isRepeat: false) == .routeFresh,
          !lateCommandOwnership.ownsCallbacks else {
        print("FAILED: final-block Enter callbacks must stay owned after buffer deactivation")
        return false
    }

    // Some hosts deliver insertNewline: before keyUp. Consuming the command
    // must retain both keyUp ownership and duplicate-command suppression until
    // a definite fresh press starts.
    var commandFirstOwnership = BufferEnterCallbackOwnership()
    commandFirstOwnership.claimPress()
    guard commandFirstOwnership.routeNewlineCommand() == .consumeOwned,
          commandFirstOwnership.suppressesKeyUp,
          commandFirstOwnership.suppressesNewlineCommand,
          commandFirstOwnership.consumeKeyUp() == .consumeOwned,
          commandFirstOwnership.ownsCallbacks,
          commandFirstOwnership.prepareForKeyDown(isRepeat: false) == .routeFresh,
          !commandFirstOwnership.ownsCallbacks else {
        print("FAILED: command-before-keyUp Enter ownership")
        return false
    }

    // A host may omit didCommand entirely. The next real non-repeat keyDown
    // retires that old debt and routes the same new press immediately. Repeats
    // from a still-owned press remain consumed.
    var nextPressOwnership = BufferEnterCallbackOwnership()
    nextPressOwnership.claimPress()
    _ = nextPressOwnership.consumeKeyUp()
    guard nextPressOwnership.prepareForKeyDown(isRepeat: false) == .routeFresh,
          !nextPressOwnership.ownsCallbacks else {
        print("FAILED: fresh Enter must retire omitted-command debt without press-twice")
        return false
    }
    var repeatOwnership = BufferEnterCallbackOwnership()
    repeatOwnership.claimPress()
    guard repeatOwnership.prepareForKeyDown(isRepeat: true) == .consumeOwned,
          repeatOwnership.suppressesKeyUp,
          repeatOwnership.suppressesNewlineCommand else {
        print("FAILED: Enter auto-repeat must remain attached to owned press")
        return false
    }
    var duplicateKeyUpOwnership = BufferEnterCallbackOwnership()
    duplicateKeyUpOwnership.claimPress()
    guard duplicateKeyUpOwnership.consumeKeyUp() == .consumeOwned,
          duplicateKeyUpOwnership.suppressesKeyUp,
          duplicateKeyUpOwnership.consumeKeyUp() == .consumeOwned,
          duplicateKeyUpOwnership.prepareForKeyDown(isRepeat: false) == .routeFresh,
          !duplicateKeyUpOwnership.ownsCallbacks else {
        print("FAILED: duplicate/late keyUp suppression must survive until fresh press")
        return false
    }

    // A defensive didCommand callback never retires a generation, even if a
    // newer Return is physically down. Only the authoritative NSEvent keyDown
    // may declare and route a fresh press.
    var commandDuringNextPress = BufferEnterCallbackOwnership()
    commandDuringNextPress.claimPress()
    _ = commandDuringNextPress.consumeKeyUp()
    guard commandDuringNextPress.routeNewlineCommand() == .consumeOwned,
          commandDuringNextPress.prepareForKeyDown(isRepeat: false) == .routeFresh,
          !commandDuringNextPress.ownsCallbacks else {
        print("FAILED: didCommand must not retire Enter ownership")
        return false
    }

    guard BufferWindowVisibilityRules.isVisibleOnActiveSpace(
            isOrdered: true,
            isOnActiveSpace: true
          ),
          !BufferWindowVisibilityRules.isVisibleOnActiveSpace(
            isOrdered: true,
            isOnActiveSpace: false
          ),
          !BufferWindowVisibilityRules.isVisibleOnActiveSpace(
            isOrdered: false,
            isOnActiveSpace: true
          ) else {
        print("FAILED: workbench visibility must be scoped to the active Space")
        return false
    }
    guard BufferWindowFocusFollowRules.shouldRelocate(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: false,
            hasTrustedExternalFocus: true,
            panelVisibleOnActiveSpace: false,
            targetScreenMatchesPanel: true
          ),
          BufferWindowFocusFollowRules.shouldRelocate(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: false,
            hasTrustedExternalFocus: true,
            panelVisibleOnActiveSpace: true,
            targetScreenMatchesPanel: false
          ),
          !BufferWindowFocusFollowRules.shouldRelocate(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: false,
            hasTrustedExternalFocus: true,
            panelVisibleOnActiveSpace: true,
            targetScreenMatchesPanel: true
          ),
          !BufferWindowFocusFollowRules.shouldRelocate(
            workbenchVisible: false,
            presentationProtected: false,
            secureInput: false,
            hasTrustedExternalFocus: true,
            panelVisibleOnActiveSpace: false,
            targetScreenMatchesPanel: true
          ),
          !BufferWindowFocusFollowRules.shouldRelocate(
            workbenchVisible: true,
            presentationProtected: true,
            secureInput: false,
            hasTrustedExternalFocus: true,
            panelVisibleOnActiveSpace: false,
            targetScreenMatchesPanel: true
          ),
          !BufferWindowFocusFollowRules.shouldRelocate(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: true,
            hasTrustedExternalFocus: true,
            panelVisibleOnActiveSpace: false,
            targetScreenMatchesPanel: true
          ),
          !BufferWindowFocusFollowRules.shouldRelocate(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: false,
            hasTrustedExternalFocus: false,
            panelVisibleOnActiveSpace: false,
            targetScreenMatchesPanel: true
          ) else {
        print("FAILED: workbench focus-follow visibility policy")
        return false
    }
    // An Electron host names its box only after the first opening has already
    // been placed by the caret. That opening is aligned once, and only while
    // it is still the untouched opening of the same workbench session.
    func lateBoxAlignment(
        visible: Bool = true,
        transient: Bool = true,
        sameSession: Bool = true,
        elapsed: TimeInterval = 0.4,
        sameHost: Bool? = true,
        boxNamed: Bool = true
    ) -> BufferLateBoxAlignmentRules.Decision {
        BufferLateBoxAlignmentRules.decision(
            workbenchVisible: visible,
            openingStillTransient: transient,
            sameWorkbenchSession: sameSession,
            elapsed: elapsed,
            targetInSameHostProcess: sameHost,
            boxNamed: boxNamed
        )
    }
    guard lateBoxAlignment() == .align,
          lateBoxAlignment(boxNamed: false) == .wait,
          // The lease is briefly absent while an input-source switch
          // re-activates the same field under a new token.
          lateBoxAlignment(sameHost: nil) == .wait,
          lateBoxAlignment(sameHost: false) == .abandon,
          lateBoxAlignment(visible: false) == .abandon,
          lateBoxAlignment(transient: false) == .abandon,
          lateBoxAlignment(sameSession: false) == .abandon,
          lateBoxAlignment(elapsed: BufferLateBoxAlignmentRules.window)
            == .align,
          lateBoxAlignment(elapsed: BufferLateBoxAlignmentRules.window + 0.1)
            == .abandon,
          lateBoxAlignment(elapsed: -1) == .abandon,
          BufferLateBoxAlignmentRules.window
            >= ElectronAccessibilityTree.buildAllowance else {
        print("FAILED: late input-box alignment of a caret-only opening")
        return false
    }
    let unpinnedBehavior = BufferWindowCollectionBehaviorRules.behavior(
        pinned: false
    )
    let pinnedBehavior = BufferWindowCollectionBehaviorRules.behavior(
        pinned: true
    )
    guard unpinnedBehavior.contains(.moveToActiveSpace),
          unpinnedBehavior.contains(.fullScreenAuxiliary),
          !unpinnedBehavior.contains(.canJoinAllSpaces),
          pinnedBehavior.contains(.canJoinAllSpaces),
          pinnedBehavior.contains(.fullScreenAuxiliary),
          !pinnedBehavior.contains(.moveToActiveSpace) else {
        print("FAILED: workbench Space/full-screen collection behavior")
        return false
    }
    let candidateBehavior = CandidatePanelSpaceRules.collectionBehavior
    guard candidateMatrix.panelCollectionBehavior == candidateBehavior,
          candidateBehavior.contains(.moveToActiveSpace),
          candidateBehavior.contains(.fullScreenAuxiliary),
          !candidateBehavior.contains(.canJoinAllSpaces),
          !candidateBehavior.contains(.stationary),
          CandidatePanelSpaceRules.shouldOrderOutBeforeShow(
            isVisible: true,
            isOnActiveSpace: false
          ),
          !CandidatePanelSpaceRules.shouldOrderOutBeforeShow(
            isVisible: true,
            isOnActiveSpace: true
          ),
          !CandidatePanelSpaceRules.shouldOrderOutBeforeShow(
            isVisible: false,
            isOnActiveSpace: false
          ) else {
        print("FAILED: candidate panel Space/reordering policy")
        return false
    }
    guard BufferWindowOrderingRules.shouldOrderOutBeforeMoving(
            isOrdered: true,
            isOnActiveSpace: false,
            pinned: false
          ),
          !BufferWindowOrderingRules.shouldOrderOutBeforeMoving(
            isOrdered: true,
            isOnActiveSpace: true,
            pinned: false
          ),
          !BufferWindowOrderingRules.shouldOrderOutBeforeMoving(
            isOrdered: false,
            isOnActiveSpace: false,
            pinned: false
          ),
          !BufferWindowOrderingRules.shouldOrderOutBeforeMoving(
            isOrdered: true,
            isOnActiveSpace: false,
            pinned: true
          ) else {
        print("FAILED: workbench old-Space reordering policy")
        return false
    }
    guard CandidatePanelInteractionRules.mayInteract(
            hasLogicalCandidates: true,
            hasInteractionTarget: true,
            panelIsVisible: true,
            panelIsOnActiveSpace: true
          ),
          !CandidatePanelInteractionRules.mayInteract(
            hasLogicalCandidates: true,
            hasInteractionTarget: true,
            panelIsVisible: false,
            panelIsOnActiveSpace: true
          ),
          !CandidatePanelInteractionRules.mayInteract(
            hasLogicalCandidates: true,
            hasInteractionTarget: true,
            panelIsVisible: true,
            panelIsOnActiveSpace: false
          ),
          !CandidatePanelInteractionRules.mayInteract(
            hasLogicalCandidates: true,
            hasInteractionTarget: false,
            panelIsVisible: true,
            panelIsOnActiveSpace: true
          ),
          !CandidatePanelInteractionRules.mayInteract(
            hasLogicalCandidates: true,
            hasInteractionTarget: true,
            panelIsVisible: true,
            panelIsOnActiveSpace: true,
            transitionAllowsInteraction: false
          ) else {
        print("FAILED: hidden candidate panel must not capture local interaction")
        return false
    }

    var epochs = FocusEpochState()
    let tokenA = epochs.activate()
    let tokenB = epochs.activate()
    guard tokenA != tokenB,
          !epochs.isCurrent(tokenA),
          epochs.isCurrent(tokenB),
          epochs.deactivate(tokenA) == false,
          epochs.isCurrent(tokenB),
          epochs.deactivate(tokenB),
          epochs.current == nil else {
        print("FAILED: focus epoch must reject stale deactivate")
        return false
    }

    func targetAllowed(tokenIsCurrent: Bool = true,
                       expectedTokenMatches: Bool = true,
                       externalTarget: Bool = true,
                       deliveryTrusted: Bool = true,
                       controllerAlive: Bool = true,
                       clientAlive: Bool = true,
                       clientIdentityMatches: Bool = true,
                       controllerClientIdentityMatches: Bool = true,
                       clientBundleMatches: Bool = true,
                       frontmostApplicationMatches: Bool = true,
                       frontmostProcessMatches: Bool = true) -> Bool {
        FocusTargetRules.allows(
            tokenIsCurrent: tokenIsCurrent,
            expectedTokenMatches: expectedTokenMatches,
            externalTarget: externalTarget,
            deliveryTrusted: deliveryTrusted,
            controllerAlive: controllerAlive,
            clientAlive: clientAlive,
            clientIdentityMatches: clientIdentityMatches,
            controllerClientIdentityMatches: controllerClientIdentityMatches,
            clientBundleMatches: clientBundleMatches,
            frontmostApplicationMatches: frontmostApplicationMatches,
            frontmostProcessMatches: frontmostProcessMatches
        )
    }
    let eligible = targetAllowed()
    let rejectedVariants = [
        targetAllowed(tokenIsCurrent: false),
        targetAllowed(expectedTokenMatches: false),
        targetAllowed(externalTarget: false),
        targetAllowed(deliveryTrusted: false),
        targetAllowed(controllerAlive: false),
        targetAllowed(clientAlive: false),
        targetAllowed(clientIdentityMatches: false),
        targetAllowed(controllerClientIdentityMatches: false),
        targetAllowed(clientBundleMatches: false),
        targetAllowed(frontmostApplicationMatches: false),
        targetAllowed(frontmostProcessMatches: false),
    ]
    guard eligible, rejectedVariants.allSatisfy({ !$0 }) else {
        print("FAILED: live target eligibility gate")
        return false
    }
    guard !FocusTargetRules.shouldPruneExpiredLease(
            controllerAlive: true,
            clientAlive: true
          ),
          FocusTargetRules.shouldPruneExpiredLease(
            controllerAlive: false,
            clientAlive: true
          ),
          FocusTargetRules.shouldPruneExpiredLease(
            controllerAlive: true,
            clientAlive: false
          ),
          FocusTargetRules.requiresNoClientCleanup(
            controllerAlive: true,
            clientAlive: false
          ),
          !FocusTargetRules.requiresNoClientCleanup(
            controllerAlive: false,
            clientAlive: false
          ) else {
        print("FAILED: expired lease must clear a surviving controller session")
        return false
    }

    let ordinaryHost = FocusHostRules.resolveKnownFrontmost(
        incomingBundleID: "app.a",
        frontmostBundleID: "app.a",
        frontmostProcessIdentifier: 101,
        trustedOverlayProcessIdentifier: nil
    )
    let spotlightHost = FocusHostRules.resolveKnownFrontmost(
        incomingBundleID: "com.apple.Spotlight",
        frontmostBundleID: "app.a",
        frontmostProcessIdentifier: 101,
        trustedOverlayProcessIdentifier: 202
    )
    let pasteBundleID = "com.wiheads.paste"
    let pastePath = "/Applications/Paste.app"
    let pasteHost = FocusHostRules.resolveKnownFrontmost(
        incomingBundleID: pasteBundleID,
        frontmostBundleID: "app.a",
        frontmostProcessIdentifier: 101,
        trustedOverlayProcessIdentifier: 303
    )
    let pasteLease = FocusHostResolution(
        kind: .nonactivatingSystemOverlay,
        clientProcessIdentifier: 303,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101
    )
    let iShotBundleID = FocusHostRules.iShotBundleID
    let iShotPath = FocusHostRules.iShotBundlePath
    let iShotHost = FocusHostRules.resolveKnownFrontmost(
        incomingBundleID: iShotBundleID,
        frontmostBundleID: "app.a",
        frontmostProcessIdentifier: 101,
        trustedOverlayProcessIdentifier: 304
    )
    let iShotLease = FocusHostResolution(
        kind: .nonactivatingSystemOverlay,
        clientProcessIdentifier: 304,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101
    )
    let savePanelBundleID = "com.apple.appkit.xpc.openAndSavePanelService"
    let savePanelPath =
        "/System/Library/Frameworks/AppKit.framework/Versions/C/XPCServices/com.apple.appkit.xpc.openAndSavePanelService.xpc"
    let savePanelHost = FocusHostRules.resolveKnownFrontmost(
        incomingBundleID: savePanelBundleID,
        frontmostBundleID: "app.a",
        frontmostProcessIdentifier: 101,
        trustedOverlayProcessIdentifier: nil,
        trustedSystemPanelAvailable: true
    )
    let spotlightLease = FocusHostResolution(
        kind: .nonactivatingSystemOverlay,
        clientProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101
    )
    let savePanelLease = FocusHostResolution(
        kind: .appKitOpenSavePanel,
        clientProcessIdentifier: 101,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101
    )
    let ordinaryAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .frontmostApplication,
        leaseBundleID: "app.a",
        leaseProcessIdentifier: 101,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: nil,
        trustedSurfaceAuthority: false
    )
    let ordinaryNilBundleAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .frontmostApplication,
        leaseBundleID: "app.a",
        leaseProcessIdentifier: 101,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: nil,
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: nil,
        trustedSurfaceAuthority: false
    )
    let ordinaryBundleMismatchAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .frontmostApplication,
        leaseBundleID: "app.a",
        leaseProcessIdentifier: 101,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.b",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: nil,
        trustedSurfaceAuthority: false
    )
    let ordinaryPIDMismatchAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .frontmostApplication,
        leaseBundleID: "app.a",
        leaseProcessIdentifier: 101,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 303,
        currentTrustedOverlayProcessIdentifier: nil,
        trustedSurfaceAuthority: false
    )
    let spotlightAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .nonactivatingSystemOverlay,
        leaseBundleID: "com.apple.Spotlight",
        leaseProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: 202,
        trustedSurfaceAuthority: true
    )
    let nilAnchorBundleAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .nonactivatingSystemOverlay,
        leaseBundleID: "com.apple.Spotlight",
        leaseProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: nil,
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: 202,
        trustedSurfaceAuthority: true
    )
    let changedAnchorBundleAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .nonactivatingSystemOverlay,
        leaseBundleID: "com.apple.Spotlight",
        leaseProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.b",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: 202,
        trustedSurfaceAuthority: true
    )
    let changedAnchorPIDAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .nonactivatingSystemOverlay,
        leaseBundleID: "com.apple.Spotlight",
        leaseProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 303,
        currentTrustedOverlayProcessIdentifier: 202,
        trustedSurfaceAuthority: true
    )
    let deadOverlayAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .nonactivatingSystemOverlay,
        leaseBundleID: "com.apple.Spotlight",
        leaseProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: nil,
        trustedSurfaceAuthority: true
    )
    let restartedOverlayAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .nonactivatingSystemOverlay,
        leaseBundleID: "com.apple.Spotlight",
        leaseProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: 203,
        trustedSurfaceAuthority: true
    )
    let hiddenOverlayAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .nonactivatingSystemOverlay,
        leaseBundleID: "com.apple.Spotlight",
        leaseProcessIdentifier: 202,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: 202,
        trustedSurfaceAuthority: false
    )
    let savePanelAuthority = FocusHostRules.applicationAuthorityMatches(
        kind: .appKitOpenSavePanel,
        leaseBundleID: savePanelBundleID,
        leaseProcessIdentifier: 101,
        foregroundAnchorBundleID: "app.a",
        foregroundAnchorProcessIdentifier: 101,
        currentFrontmostBundleID: "app.a",
        currentFrontmostProcessIdentifier: 101,
        currentTrustedOverlayProcessIdentifier: nil,
        trustedSurfaceAuthority: true
    )
    let savePanelChangedBundleAuthority =
        FocusHostRules.applicationAuthorityMatches(
            kind: .appKitOpenSavePanel,
            leaseBundleID: savePanelBundleID,
            leaseProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            currentFrontmostBundleID: "app.b",
            currentFrontmostProcessIdentifier: 101,
            currentTrustedOverlayProcessIdentifier: nil,
            trustedSurfaceAuthority: true
        )
    let savePanelChangedPIDAuthority =
        FocusHostRules.applicationAuthorityMatches(
            kind: .appKitOpenSavePanel,
            leaseBundleID: savePanelBundleID,
            leaseProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            currentFrontmostBundleID: "app.a",
            currentFrontmostProcessIdentifier: 303,
            currentTrustedOverlayProcessIdentifier: nil,
            trustedSurfaceAuthority: true
        )
    let savePanelMissingSurfaceAuthority =
        FocusHostRules.applicationAuthorityMatches(
            kind: .appKitOpenSavePanel,
            leaseBundleID: savePanelBundleID,
            leaseProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            currentFrontmostBundleID: "app.a",
            currentFrontmostProcessIdentifier: 101,
            currentTrustedOverlayProcessIdentifier: nil,
            trustedSurfaceAuthority: false
        )
    let orderedPanelWindows = [
        FocusWindowSnapshot(
            windowNumber: 900,
            ownerProcessIdentifier: 303,
            layer: 0,
            alpha: 1
        ),
        FocusWindowSnapshot(
            windowNumber: 42,
            ownerProcessIdentifier: 101,
            layer: 0,
            alpha: 1
        ),
        FocusWindowSnapshot(
            windowNumber: 41,
            ownerProcessIdentifier: 101,
            layer: 0,
            alpha: 1
        ),
    ]
    let frozenPanelWindow = FocusSystemPanelWindowRules.frontmostWindowNumber(
        anchorProcessIdentifier: 101,
        orderedWindows: orderedPanelWindows
    )
    let initialPanelWindowAuthority =
        FocusSystemPanelWindowRules.authorityForExistingLease(
            frozenWindowNumber: nil,
            frozenWindowRemainsTrusted: false,
            deliverySuspended: true,
            eventCanEstablishTransientSurface: true,
            candidateWindowNumber: 42
        )
    let retainedPanelWindowAuthority =
        FocusSystemPanelWindowRules.authorityForExistingLease(
            frozenWindowNumber: 42,
            frozenWindowRemainsTrusted: true,
            deliverySuspended: true,
            eventCanEstablishTransientSurface: true,
            candidateWindowNumber: 43
        )
    let vanishedPanelWindowAuthority =
        FocusSystemPanelWindowRules.authorityForExistingLease(
            frozenWindowNumber: 42,
            frozenWindowRemainsTrusted: false,
            deliverySuspended: true,
            eventCanEstablishTransientSurface: true,
            candidateWindowNumber: 43
        )
    let continuingPanelWindowAuthority =
        FocusSystemPanelWindowRules.authorityForExistingLease(
            frozenWindowNumber: 42,
            frozenWindowRemainsTrusted: true,
            deliverySuspended: false,
            eventCanEstablishTransientSurface: false,
            candidateWindowNumber: 43
        )
    let freshEpochPanelWindowAuthority =
        FocusSystemPanelWindowRules.authorityForExistingLease(
            frozenWindowNumber: 42,
            frozenWindowRemainsTrusted: true,
            deliverySuspended: false,
            eventCanEstablishTransientSurface: true,
            candidateWindowNumber: 43
        )
    let nonKeyPanelWindowAuthority =
        FocusSystemPanelWindowRules.authorityForExistingLease(
            frozenWindowNumber: nil,
            frozenWindowRemainsTrusted: false,
            deliverySuspended: true,
            eventCanEstablishTransientSurface: false,
            candidateWindowNumber: 42
        )
    let unsuspendedPanelWindowAuthority =
        FocusSystemPanelWindowRules.authorityForExistingLease(
            frozenWindowNumber: nil,
            frozenWindowRemainsTrusted: false,
            deliverySuspended: false,
            eventCanEstablishTransientSurface: true,
            candidateWindowNumber: 42
        )
    guard FocusHostRules.isAppKitOpenSavePanelBundle(savePanelBundleID),
          FocusHostRules.isTransientSystemSurfaceBundle(savePanelBundleID),
          FocusHostRules.isTransientSystemSurfaceBundle("com.apple.Spotlight"),
          FocusHostRules.isTransientSystemSurfaceBundle(pasteBundleID),
          !FocusHostRules.isTransientSystemSurfaceBundle("app.a"),
          FocusHostRules.mayUseOrdinaryProcessBoundFallback("app.a"),
          !FocusHostRules.mayUseOrdinaryProcessBoundFallback(
            savePanelBundleID
          ),
          !FocusHostRules.mayUseOrdinaryProcessBoundFallback(
            "com.apple.Spotlight"
          ),
          !FocusHostRules.mayUseOrdinaryProcessBoundFallback(pasteBundleID),
          savePanelHost == savePanelLease,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: savePanelBundleID,
            frontmostBundleID: "app.a",
            frontmostProcessIdentifier: 101,
            trustedOverlayProcessIdentifier: nil,
            trustedSystemPanelAvailable: false
          ) == nil,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: savePanelBundleID,
            frontmostBundleID: savePanelBundleID,
            frontmostProcessIdentifier: 204,
            trustedOverlayProcessIdentifier: nil,
            trustedSystemPanelAvailable: true
          ) == nil,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: "com.apple.Spotlight",
            frontmostBundleID: "com.apple.Spotlight",
            frontmostProcessIdentifier: 202,
            trustedOverlayProcessIdentifier: 202
          ) == nil,
          FocusHostRules.resolutionMatchesLease(
            savePanelLease,
            hostKind: .appKitOpenSavePanel,
            clientProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101
          ),
          savePanelAuthority.bundle,
          savePanelAuthority.process,
          !savePanelChangedBundleAuthority.bundle,
          savePanelChangedBundleAuthority.process,
          savePanelChangedPIDAuthority.bundle,
          !savePanelChangedPIDAuthority.process,
          !savePanelMissingSurfaceAuthority.bundle,
          !savePanelMissingSurfaceAuthority.process,
          frozenPanelWindow == 42,
          FocusSystemPanelWindowRules.windowRemainsTrusted(
            42,
            anchorProcessIdentifier: 101,
            onScreenWindows: orderedPanelWindows
          ),
          !FocusSystemPanelWindowRules.windowRemainsTrusted(
            42,
            anchorProcessIdentifier: 101,
            onScreenWindows: [orderedPanelWindows[2]]
          ),
          !FocusSystemPanelWindowRules.windowRemainsTrusted(
            42,
            anchorProcessIdentifier: 101,
            onScreenWindows: [
                FocusWindowSnapshot(
                    windowNumber: 43,
                    ownerProcessIdentifier: 101,
                    layer: 0,
                    alpha: 1
                ),
                orderedPanelWindows[2],
            ]
          ),
          !FocusSystemPanelWindowRules.windowRemainsTrusted(
            42,
            anchorProcessIdentifier: 101,
            onScreenWindows: [
                FocusWindowSnapshot(
                    windowNumber: 42,
                    ownerProcessIdentifier: 303,
                    layer: 0,
                    alpha: 1
                ),
            ]
          ),
          !FocusSystemPanelWindowRules.windowRemainsTrusted(
            42,
            anchorProcessIdentifier: 101,
            onScreenWindows: [
                FocusWindowSnapshot(
                    windowNumber: 42,
                    ownerProcessIdentifier: 101,
                    layer: 1,
                    alpha: 1
                ),
            ]
          ),
          !FocusSystemPanelWindowRules.windowRemainsTrusted(
            42,
            anchorProcessIdentifier: 101,
            onScreenWindows: [
                FocusWindowSnapshot(
                    windowNumber: 42,
                    ownerProcessIdentifier: 101,
                    layer: 0,
                    alpha: 0
                ),
            ]
          ),
          FocusHostRules.callbackMayUseResolution(
            kind: .appKitOpenSavePanel,
            explicitActivation: true,
            eventCanEstablishTransientSurface: false,
            continuesExactLease: false,
            trustedSurfaceAuthority: false
          ),
          FocusHostRules.callbackMayUseResolution(
            kind: .appKitOpenSavePanel,
            explicitActivation: false,
            eventCanEstablishTransientSurface: true,
            continuesExactLease: false,
            trustedSurfaceAuthority: true
          ),
          !FocusHostRules.callbackMayUseResolution(
            kind: .appKitOpenSavePanel,
            explicitActivation: false,
            eventCanEstablishTransientSurface: false,
            continuesExactLease: false,
            trustedSurfaceAuthority: true
          ),
          FocusHostRules.callbackMayUseResolution(
            kind: .appKitOpenSavePanel,
            explicitActivation: false,
            eventCanEstablishTransientSurface: false,
            continuesExactLease: true,
            trustedSurfaceAuthority: true
          ),
          !FocusHostRules.callbackMayUseResolution(
            kind: .appKitOpenSavePanel,
            explicitActivation: false,
            eventCanEstablishTransientSurface: true,
            continuesExactLease: true,
            trustedSurfaceAuthority: false
          ),
          !FocusHostRules.shouldSuspendNewLease(
            kind: .frontmostApplication,
            explicitActivation: true
          ),
          FocusHostRules.shouldSuspendNewLease(
            kind: .nonactivatingSystemOverlay,
            explicitActivation: true
          ),
          FocusHostRules.shouldSuspendNewLease(
            kind: .appKitOpenSavePanel,
            explicitActivation: true
          ),
          !FocusHostRules.shouldSuspendNewLease(
            kind: .appKitOpenSavePanel,
            explicitActivation: false
          ),
          FocusHostRules.shouldPreservePendingPreheatAfterRejectedCallback(
            kind: .appKitOpenSavePanel,
            reusesExactOwner: true,
            resolutionMatchesOwner: true,
            deliverySuspended: true,
            awaitingKeyDown: true
          ),
          FocusHostRules.shouldPreservePendingPreheatAfterRejectedCallback(
            kind: .nonactivatingSystemOverlay,
            reusesExactOwner: true,
            resolutionMatchesOwner: true,
            deliverySuspended: true,
            awaitingKeyDown: true
          ),
          !FocusHostRules.shouldPreservePendingPreheatAfterRejectedCallback(
            kind: .frontmostApplication,
            reusesExactOwner: true,
            resolutionMatchesOwner: true,
            deliverySuspended: true,
            awaitingKeyDown: true
          ),
          !FocusHostRules.shouldPreservePendingPreheatAfterRejectedCallback(
            kind: .appKitOpenSavePanel,
            reusesExactOwner: true,
            resolutionMatchesOwner: false,
            deliverySuspended: true,
            awaitingKeyDown: true
          ),
          !FocusHostRules.shouldPreservePendingPreheatAfterRejectedCallback(
            kind: .appKitOpenSavePanel,
            reusesExactOwner: true,
            resolutionMatchesOwner: true,
            deliverySuspended: true,
            awaitingKeyDown: false
          ),
          initialPanelWindowAuthority == FocusSystemPanelWindowAuthority(
            trusted: true,
            pendingWindowNumber: 42
          ),
          retainedPanelWindowAuthority == FocusSystemPanelWindowAuthority(
            trusted: true,
            pendingWindowNumber: 42
          ),
          vanishedPanelWindowAuthority == FocusSystemPanelWindowAuthority(
            trusted: false,
            pendingWindowNumber: nil
          ),
          continuingPanelWindowAuthority == FocusSystemPanelWindowAuthority(
            trusted: true,
            pendingWindowNumber: nil
          ),
          freshEpochPanelWindowAuthority == FocusSystemPanelWindowAuthority(
            trusted: true,
            pendingWindowNumber: 42
          ),
          nonKeyPanelWindowAuthority == FocusSystemPanelWindowAuthority(
            trusted: false,
            pendingWindowNumber: nil
          ),
          unsuspendedPanelWindowAuthority == FocusSystemPanelWindowAuthority(
            trusted: false,
            pendingWindowNumber: nil
          ),
          !FocusHostKind.frontmostApplication.requiresTransientSurfaceAuthority,
          FocusHostKind.nonactivatingSystemOverlay.requiresTransientSurfaceAuthority,
          FocusHostKind.appKitOpenSavePanel.requiresTransientSurfaceAuthority,
          FocusHostKind.nonactivatingSystemOverlay.permitsPendingModifierBaselineSync,
          !FocusHostKind.appKitOpenSavePanel.permitsPendingModifierBaselineSync,
          FocusHostRules.frontmostChangeInvalidatesLease(
            hostKind: .appKitOpenSavePanel,
            leaseBundleID: savePanelBundleID,
            leaseProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            activatedBundleID: "app.a",
            activatedProcessIdentifier: 101
          ),
          FocusHostRules.displacedLeaseRequiresNoClientCleanup(
            hostKind: .appKitOpenSavePanel
          ),
          targetAllowed(
            frontmostApplicationMatches: savePanelAuthority.bundle,
            frontmostProcessMatches: savePanelAuthority.process
          ) else {
        print("FAILED: trusted AppKit open/save panel focus gate")
        return false
    }
    let ordinaryUnknownAnchorMatches = ordinaryHost.map {
        FocusHostRules.resolutionMatchesLease(
            $0,
            hostKind: .frontmostApplication,
            clientProcessIdentifier: 101,
            foregroundAnchorBundleID: nil,
            foregroundAnchorProcessIdentifier: 101
        )
    } ?? false
    guard ordinaryHost == FocusHostResolution(
            kind: .frontmostApplication,
            clientProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101
          ),
          spotlightHost == spotlightLease,
          pasteHost == pasteLease,
          iShotHost == iShotLease,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: pasteBundleID,
            frontmostBundleID: pasteBundleID,
            frontmostProcessIdentifier: 303,
            trustedOverlayProcessIdentifier: 303
          ) == nil,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: iShotBundleID,
            frontmostBundleID: iShotBundleID,
            frontmostProcessIdentifier: 304,
            trustedOverlayProcessIdentifier: 304
          ) == nil,
          savePanelHost == savePanelLease,
          FocusHostRules.isTrustedNonactivatingSystemOverlay(
            bundleID: "com.apple.Spotlight",
            bundlePath: "/System/Library/CoreServices/Spotlight.app"
          ),
          FocusHostRules.isTrustedNonactivatingSystemOverlay(
            bundleID: pasteBundleID,
            bundlePath: pastePath
          ),
          FocusHostRules.isTrustedNonactivatingSystemOverlay(
            bundleID: iShotBundleID,
            bundlePath: iShotPath
          ),
          FocusHostRules.isTransientSystemSurfaceBundle(iShotBundleID),
          !FocusHostRules.mayUseOrdinaryProcessBoundFallback(iShotBundleID),
          FocusHostRules.isTrustedAppKitOpenSavePanel(
            bundleID: savePanelBundleID,
            bundlePath: savePanelPath
          ),
          !FocusHostRules.isTrustedNonactivatingSystemOverlay(
            bundleID: "com.apple.Spotlight",
            bundlePath: "/Applications/FakeSpotlight.app"
          ),
          !FocusHostRules.isTrustedNonactivatingSystemOverlay(
            bundleID: pasteBundleID,
            bundlePath: "/Applications/FakePaste.app"
          ),
          !FocusHostRules.isTrustedNonactivatingSystemOverlay(
            bundleID: iShotBundleID,
            bundlePath: "/Applications/FakeiShot.app"
          ),
          !FocusHostRules.isTrustedAppKitOpenSavePanel(
            bundleID: savePanelBundleID,
            bundlePath: "/Applications/FakeSavePanel.xpc"
          ),
          !FocusHostRules.isNonactivatingSystemOverlayBundle("com.apple.SearchUI"),
          FocusHostRules.uniqueTrustedOverlayProcessIdentifier(
            bundleID: "com.apple.Spotlight",
            runningCandidates: [
                (202, "/System/Library/CoreServices/Spotlight.app"),
            ]
          ) == 202,
          FocusHostRules.uniqueTrustedOverlayProcessIdentifier(
            bundleID: "com.apple.Spotlight",
            runningCandidates: [
                (202, "/System/Library/CoreServices/Spotlight.app"),
                (203, "/Applications/FakeSpotlight.app"),
            ]
          ) == nil,
          FocusHostRules.uniqueTrustedOverlayProcessIdentifier(
            bundleID: pasteBundleID,
            runningCandidates: [(303, pastePath)]
          ) == 303,
          FocusHostRules.uniqueTrustedOverlayProcessIdentifier(
            bundleID: iShotBundleID,
            runningCandidates: [(304, iShotPath)]
          ) == 304,
          FocusHostRules.uniqueTrustedOverlayProcessIdentifier(
            bundleID: iShotBundleID,
            runningCandidates: [
                (304, iShotPath),
                (305, iShotPath),
            ]
          ) == nil,
          FocusHostRules.uniqueTrustedOverlayProcessIdentifier(
            bundleID: pasteBundleID,
            runningCandidates: [
                (303, pastePath),
                (304, pastePath),
            ]
          ) == nil,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: pasteBundleID,
            frontmostBundleID: "app.a",
            frontmostProcessIdentifier: 101,
            trustedOverlayProcessIdentifier: nil
          ) == nil,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: iShotBundleID,
            frontmostBundleID: "app.a",
            frontmostProcessIdentifier: 101,
            trustedOverlayProcessIdentifier: nil
          ) == nil,
          FocusHostRules.resolutionMatchesLease(
            pasteLease,
            hostKind: .nonactivatingSystemOverlay,
            clientProcessIdentifier: 303,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101
          ),
          FocusHostRules.resolutionMatchesLease(
            iShotLease,
            hostKind: .nonactivatingSystemOverlay,
            clientProcessIdentifier: 304,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101
          ),
          FocusHostRules.allSystemPanelProcessesAreTrusted(
            bundleID: savePanelBundleID,
            runningCandidates: [
                (204, savePanelPath),
            ]
          ),
          FocusHostRules.allSystemPanelProcessesAreTrusted(
            bundleID: savePanelBundleID,
            runningCandidates: [
                (204, savePanelPath),
                (205, savePanelPath),
            ]
          ),
          !FocusHostRules.allSystemPanelProcessesAreTrusted(
            bundleID: savePanelBundleID,
            runningCandidates: []
          ),
          !FocusHostRules.allSystemPanelProcessesAreTrusted(
            bundleID: savePanelBundleID,
            runningCandidates: [
                (204, savePanelPath),
                (205, "/Applications/FakeSavePanel.xpc"),
            ]
          ),
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: "com.apple.Spotlight",
            frontmostBundleID: "app.a",
            frontmostProcessIdentifier: 101,
            trustedOverlayProcessIdentifier: nil
          ) == nil,
          FocusHostRules.resolveKnownFrontmost(
            incomingBundleID: "app.background",
            frontmostBundleID: "app.a",
            frontmostProcessIdentifier: 101,
            trustedOverlayProcessIdentifier: 202
          ) == nil,
          FocusHostRules.resolutionMatchesLease(
            spotlightLease,
            hostKind: .nonactivatingSystemOverlay,
            clientProcessIdentifier: 202,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101
          ),
          !FocusHostRules.resolutionMatchesLease(
            spotlightLease,
            hostKind: .nonactivatingSystemOverlay,
            clientProcessIdentifier: 203,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101
          ),
          !FocusHostRules.resolutionMatchesLease(
            spotlightLease,
            hostKind: .nonactivatingSystemOverlay,
            clientProcessIdentifier: 202,
            foregroundAnchorBundleID: "app.b",
            foregroundAnchorProcessIdentifier: 101
          ),
          !FocusHostRules.resolutionMatchesLease(
            spotlightLease,
            hostKind: .nonactivatingSystemOverlay,
            clientProcessIdentifier: 202,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 303
          ),
          ordinaryUnknownAnchorMatches,
          ordinaryAuthority.bundle,
          ordinaryAuthority.process,
          ordinaryNilBundleAuthority.bundle,
          ordinaryNilBundleAuthority.process,
          !ordinaryBundleMismatchAuthority.bundle,
          ordinaryBundleMismatchAuthority.process,
          ordinaryPIDMismatchAuthority.bundle,
          !ordinaryPIDMismatchAuthority.process,
          spotlightAuthority.bundle,
          spotlightAuthority.process,
          nilAnchorBundleAuthority.bundle,
          nilAnchorBundleAuthority.process,
          !changedAnchorBundleAuthority.bundle,
          changedAnchorBundleAuthority.process,
          changedAnchorPIDAuthority.bundle,
          !changedAnchorPIDAuthority.process,
          !deadOverlayAuthority.bundle,
          !deadOverlayAuthority.process,
          !restartedOverlayAuthority.bundle,
          !restartedOverlayAuthority.process,
          !hiddenOverlayAuthority.bundle,
          !hiddenOverlayAuthority.process,
          FocusHostRules.callbackMayUseResolution(
            kind: .nonactivatingSystemOverlay,
            explicitActivation: true,
            eventCanEstablishTransientSurface: false,
            continuesExactLease: false,
            trustedSurfaceAuthority: false
          ),
          FocusHostRules.callbackMayUseResolution(
            kind: .nonactivatingSystemOverlay,
            explicitActivation: false,
            eventCanEstablishTransientSurface: true,
            continuesExactLease: false,
            trustedSurfaceAuthority: true
          ),
          !FocusHostRules.callbackMayUseResolution(
            kind: .nonactivatingSystemOverlay,
            explicitActivation: false,
            eventCanEstablishTransientSurface: false,
            continuesExactLease: false,
            trustedSurfaceAuthority: true
          ),
          FocusHostRules.callbackMayUseResolution(
            kind: .nonactivatingSystemOverlay,
            explicitActivation: false,
            eventCanEstablishTransientSurface: false,
            continuesExactLease: true,
            trustedSurfaceAuthority: true
          ),
          !FocusHostRules.callbackMayUseResolution(
            kind: .nonactivatingSystemOverlay,
            explicitActivation: false,
            eventCanEstablishTransientSurface: true,
            continuesExactLease: true,
            trustedSurfaceAuthority: false
          ),
          !FocusHostRules.frontmostChangeInvalidatesLease(
            hostKind: .frontmostApplication,
            leaseBundleID: "app.a",
            leaseProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            activatedBundleID: "app.a",
            activatedProcessIdentifier: 101
          ),
          FocusHostRules.frontmostChangeInvalidatesLease(
            hostKind: .frontmostApplication,
            leaseBundleID: "app.a",
            leaseProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            activatedBundleID: "app.b",
            activatedProcessIdentifier: 101
          ),
          FocusHostRules.frontmostChangeInvalidatesLease(
            hostKind: .frontmostApplication,
            leaseBundleID: "app.a",
            leaseProcessIdentifier: 101,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            activatedBundleID: "app.a",
            activatedProcessIdentifier: 303
          ),
          FocusHostRules.frontmostChangeInvalidatesLease(
            hostKind: .nonactivatingSystemOverlay,
            leaseBundleID: "com.apple.Spotlight",
            leaseProcessIdentifier: 202,
            foregroundAnchorBundleID: "app.a",
            foregroundAnchorProcessIdentifier: 101,
            activatedBundleID: "app.a",
            activatedProcessIdentifier: 101
          ),
          FocusHostRules.displacedLeaseRequiresNoClientCleanup(
            hostKind: .nonactivatingSystemOverlay
          ),
          !FocusHostRules.displacedLeaseRequiresNoClientCleanup(
            hostKind: .frontmostApplication
          ),
          targetAllowed(
            frontmostApplicationMatches: spotlightAuthority.bundle,
            frontmostProcessMatches: spotlightAuthority.process
          ) else {
        print("FAILED: trusted nonactivating focus overlay gate")
        return false
    }

    guard FocusEventRules.isOrdered(12.0, activationFloor: 11.5, lastAccepted: 11.9),
          !FocusEventRules.isOrdered(11.4, activationFloor: 11.5, lastAccepted: nil),
          !FocusEventRules.isOrdered(9.70, activationFloor: 9.75, lastAccepted: nil),
          !FocusEventRules.isOrdered(11.8, activationFloor: nil, lastAccepted: 11.9),
          FocusEventRules.isFreshNonactivatingOverlayEvent(20.25, now: 20.50),
          !FocusEventRules.isFreshNonactivatingOverlayEvent(19.0, now: 20.50),
          !FocusEventRules.isFreshNonactivatingOverlayEvent(20.75, now: 20.50),
          FocusEventRules.mayTakeOwnership(incomingBundleID: "app.b",
                                           currentOwnerBundleID: "app.a",
                                           frontmostBundleID: "app.b",
                                           incomingHostKind: .frontmostApplication),
          !FocusEventRules.mayTakeOwnership(incomingBundleID: "app.a",
                                            currentOwnerBundleID: "app.b",
                                            frontmostBundleID: "app.b",
                                            incomingHostKind: .frontmostApplication),
          !FocusEventRules.mayTakeOwnership(incomingBundleID: "app.c",
                                            currentOwnerBundleID: "app.a",
                                            frontmostBundleID: "app.b",
                                            incomingHostKind: .frontmostApplication),
          !FocusEventRules.mayTakeOwnership(incomingBundleID: "app.b",
                                            currentOwnerBundleID: "app.a",
                                            frontmostBundleID: nil,
                                            incomingHostKind: .frontmostApplication),
          FocusEventRules.mayTakeOwnership(incomingBundleID: "app.a",
                                           currentOwnerBundleID: "app.a",
                                           frontmostBundleID: nil,
                                           incomingHostKind: .frontmostApplication),
          FocusEventRules.mayTakeOwnership(
            incomingBundleID: "com.apple.Spotlight",
            currentOwnerBundleID: "app.a",
            frontmostBundleID: "app.a",
            incomingHostKind: .nonactivatingSystemOverlay
          ),
          FocusEventRules.mayTakeOwnership(
            incomingBundleID: savePanelBundleID,
            currentOwnerBundleID: "app.a",
            frontmostBundleID: "app.a",
            incomingHostKind: .appKitOpenSavePanel
          ),
          FocusEventRules.mayUseOrdinaryProcessBoundResolution(
            ownerExists: true,
            reusesExactOwner: true,
            frontmostProcessIdentifier: nil,
            knownClientProcessIdentifier: nil
          ),
          FocusEventRules.mayUseOrdinaryProcessBoundResolution(
            ownerExists: false,
            reusesExactOwner: false,
            frontmostProcessIdentifier: 101,
            knownClientProcessIdentifier: nil
          ),
          FocusEventRules.mayUseOrdinaryProcessBoundResolution(
            ownerExists: false,
            reusesExactOwner: false,
            frontmostProcessIdentifier: 101,
            knownClientProcessIdentifier: 101
          ),
          !FocusEventRules.mayUseOrdinaryProcessBoundResolution(
            ownerExists: true,
            reusesExactOwner: false,
            frontmostProcessIdentifier: 101,
            knownClientProcessIdentifier: nil
          ),
          !FocusEventRules.mayUseOrdinaryProcessBoundResolution(
            ownerExists: false,
            reusesExactOwner: false,
            frontmostProcessIdentifier: 101,
            knownClientProcessIdentifier: 202
          ),
          FocusEventRules.mayCacheClientProcess(
            hostKind: .frontmostApplication,
            resolvedIdentityWasVerified: true
          ),
          !FocusEventRules.mayCacheClientProcess(
            hostKind: .frontmostApplication,
            resolvedIdentityWasVerified: false
          ),
          FocusEventRules.mayCacheClientProcess(
            hostKind: .nonactivatingSystemOverlay,
            resolvedIdentityWasVerified: true
          ),
          !FocusEventRules.mayCacheClientProcess(
            hostKind: .appKitOpenSavePanel,
            resolvedIdentityWasVerified: true
          ),
          FocusEventRules.verifiedIdentityRequiresFreshEpoch(
            reusesExactOwner: true,
            ownerProcessIdentityWasVerified: false,
            resolvedIdentityWasVerified: true
          ),
          !FocusEventRules.verifiedIdentityRequiresFreshEpoch(
            reusesExactOwner: true,
            ownerProcessIdentityWasVerified: false,
            resolvedIdentityWasVerified: false
          ),
          !FocusEventRules.verifiedIdentityRequiresFreshEpoch(
            reusesExactOwner: true,
            ownerProcessIdentityWasVerified: true,
            resolvedIdentityWasVerified: true
          ),
          FocusEventRules.mayRebindKnownClientProcess(
            hostKind: .frontmostApplication,
            resolvedIdentityWasVerified: true
          ),
          !FocusEventRules.mayRebindKnownClientProcess(
            hostKind: .frontmostApplication,
            resolvedIdentityWasVerified: false
          ),
          !FocusEventRules.mayRebindKnownClientProcess(
            hostKind: .nonactivatingSystemOverlay,
            resolvedIdentityWasVerified: true
          ) else {
        print("FAILED: focus event ordering/background callback gate")
        return false
    }
    guard FocusActivationRules.shouldConfirmProvisional(
            isProvisional: true,
            sameControllerAndClient: true,
            age: 0.10
          ),
          !FocusActivationRules.shouldConfirmProvisional(
            isProvisional: true,
            sameControllerAndClient: true,
            age: 0.30
          ),
          FocusActivationRules.shouldConfirmProvisional(
            isProvisional: true,
            sameControllerAndClient: true,
            age: 0.75,
            hostKind: .nonactivatingSystemOverlay
          ),
          !FocusActivationRules.shouldConfirmProvisional(
            isProvisional: true,
            sameControllerAndClient: true,
            age: 2.10,
            hostKind: .nonactivatingSystemOverlay
          ),
          FocusActivationRules.shouldConfirmProvisional(
            isProvisional: true,
            sameControllerAndClient: true,
            age: 0.75,
            hostKind: .appKitOpenSavePanel
          ),
          !FocusActivationRules.shouldConfirmProvisional(
            isProvisional: true,
            sameControllerAndClient: true,
            age: 2.10,
            hostKind: .appKitOpenSavePanel
          ),
          !FocusActivationRules.lifecycleCallbackMayApply(
            now: 10.05,
            suppressionUntil: 10.10,
            leaseAge: 0.20,
            senderIsExplicit: true,
            clientIdentityWasReused: false
          ),
          FocusActivationRules.lifecycleCallbackMayApply(
            now: 10.20,
            suppressionUntil: 10.10,
            leaseAge: 0.20,
            senderIsExplicit: true,
            clientIdentityWasReused: false
          ),
          !FocusActivationRules.lifecycleCallbackMayApply(
            now: 10.20,
            suppressionUntil: 10.00,
            leaseAge: 0.04,
            senderIsExplicit: false,
            clientIdentityWasReused: false
          ),
          FocusActivationRules.lifecycleCallbackMayApply(
            now: 10.20,
            suppressionUntil: 10.00,
            leaseAge: 0.10,
            senderIsExplicit: false,
            clientIdentityWasReused: false
          ),
          !FocusActivationRules.lifecycleCallbackMayApply(
            now: 99.00,
            suppressionUntil: 10.00,
            leaseAge: 89.00,
            senderIsExplicit: true,
            clientIdentityWasReused: true
          ),
          FocusActivationRules.acceptedEventConfirmsReusedClientLifecycle(
            eventType: .keyDown
          ),
          !FocusActivationRules.acceptedEventConfirmsReusedClientLifecycle(
            eventType: .keyUp
          ),
          !FocusActivationRules.acceptedEventConfirmsReusedClientLifecycle(
            eventType: .flagsChanged
          ),
          abs(FocusActivationRules.suppressionDeadlineAfterConfirmedKeyDown(
            current: 10.25,
            confirmationUptime: 10.05
          ) - 10.55) < 0.000_001,
          FocusActivationRules.suppressionDeadlineAfterConfirmedKeyDown(
            current: 11.0,
            confirmationUptime: 10.05
          ) == 11.0,
          !FocusActivationRules.lifecycleCallbackMayApply(
            now: 10.31,
            suppressionUntil: 10.55,
            leaseAge: 0.31,
            senderIsExplicit: true,
            clientIdentityWasReused: false
          ),
          FocusActivationRules.lifecycleCallbackMayApply(
            now: 99.00,
            suppressionUntil: 10.00,
            leaseAge: 89.00,
            senderIsExplicit: true,
            clientIdentityWasReused: false
          ),
          FocusActivationRules.currentControllerClientMayApply(
            clientExists: true,
            identityMatches: true
          ),
          !FocusActivationRules.currentControllerClientMayApply(
            clientExists: false,
            identityMatches: false
          ),
          !FocusActivationRules.currentControllerClientMayApply(
            clientExists: true,
            identityMatches: false
          ),
          FocusActivationRules.mayContinueExactLeaseWithoutBundle(
            forceNewEpoch: false,
            eventRequiresFreshEpoch: false
          ),
          !FocusActivationRules.mayContinueExactLeaseWithoutBundle(
            forceNewEpoch: true,
            eventRequiresFreshEpoch: false
          ),
          !FocusActivationRules.mayContinueExactLeaseWithoutBundle(
            forceNewEpoch: false,
            eventRequiresFreshEpoch: true
          ),
          FocusActivationRules.eventRevealsFieldChange(
            hasEvent: true,
            reusesExactOwner: true,
            compositionActive: true,
            markedRangeReliable: true,
            markedRangeWasObservable: true,
            markedRangeIsMissing: true
          ),
          !FocusActivationRules.eventRevealsFieldChange(
            hasEvent: true,
            reusesExactOwner: true,
            compositionActive: true,
            markedRangeReliable: true,
            markedRangeWasObservable: false,
            markedRangeIsMissing: true
          ),
          !FocusActivationRules.eventRevealsFieldChange(
            hasEvent: true,
            reusesExactOwner: true,
            compositionActive: true,
            markedRangeReliable: false,
            markedRangeWasObservable: true,
            markedRangeIsMissing: true
          ) else {
        print("FAILED: provisional/lifecycle focus rules")
        return false
    }
    guard FocusTargetRules.identifiesExternalTarget(
            bundleID: "app.external",
            processIdentifier: 101,
            ownBundleID: "ime.own",
            ownProcessIdentifier: 999
          ),
          !FocusTargetRules.identifiesExternalTarget(
            bundleID: "unknown",
            processIdentifier: 999,
            ownBundleID: "ime.own",
            ownProcessIdentifier: 999
          ),
          !FocusTargetRules.identifiesExternalTarget(
            bundleID: "ime.own",
            processIdentifier: 999,
            ownBundleID: "ime.own",
            ownProcessIdentifier: 999
          ) else {
        print("FAILED: own process must never become an external delivery target")
        return false
    }

    let routingGate = ChordClientRoutingGate()
    var callbackRoutingStates: [Bool] = []
    let simulatedChordFlushCallback = {
        callbackRoutingStates.append(routingGate.allowsClientRouting)
    }
    routingGate.withIsolatedClientRouting {
        simulatedChordFlushCallback()
    }
    simulatedChordFlushCallback()
    guard callbackRoutingStates == [false, true],
          routingGate.allowsClientRouting else {
        print("FAILED: suspended focus must isolate pending chord client routing")
        return false
    }

    let appA = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: "app.a", processIdentifier: 101,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    let appB = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: "app.b", processIdentifier: 202,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    let relaunchedAppA = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: "app.a", processIdentifier: 303,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    let recycledAppPID = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: "app.c", processIdentifier: 101,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    let anonymousProcess = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: nil, processIdentifier: 404,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    let sameAnonymousProcess = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: nil, processIdentifier: 404,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    let knownAnonymousProcess = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: "app.anonymous", processIdentifier: 404,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    let ownWindow = BufferPrivacyTransitionRules.externalIdentity(
        bundleID: "ime.own", processIdentifier: 999,
        ownBundleID: "ime.own", ownProcessIdentifier: 999
    )
    guard let appA, let appB, let relaunchedAppA, let recycledAppPID,
          let anonymousProcess, let sameAnonymousProcess,
          let knownAnonymousProcess,
          ownWindow == nil,
          !BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: nil, activatedExternal: appA,
            resetOnSwitch: true, holdsExternalContent: false
          ),
          !BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: appA, activatedExternal: appA,
            resetOnSwitch: true, holdsExternalContent: false
          ),
          !BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: appA, activatedExternal: relaunchedAppA,
            resetOnSwitch: true, holdsExternalContent: false
          ),
          BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: appA, activatedExternal: recycledAppPID,
            resetOnSwitch: true, holdsExternalContent: false
          ),
          !BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: anonymousProcess,
            activatedExternal: sameAnonymousProcess,
            resetOnSwitch: true, holdsExternalContent: false
          ),
          !BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: anonymousProcess,
            activatedExternal: knownAnonymousProcess,
            resetOnSwitch: true, holdsExternalContent: false
          ),
          BufferPrivacyTransitionRules.updatedPrevious(
            anonymousProcess, activatedExternal: knownAnonymousProcess
          ) == knownAnonymousProcess,
          BufferPrivacyTransitionRules.updatedPrevious(
            knownAnonymousProcess, activatedExternal: anonymousProcess
          ) == knownAnonymousProcess,
          BufferPrivacyTransitionRules.updatedPrevious(
            appA, activatedExternal: ownWindow
          ) == appA,
          BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: appA, activatedExternal: appB,
            resetOnSwitch: true, holdsExternalContent: false
          ),
          !BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: appA, activatedExternal: appB,
            resetOnSwitch: false, holdsExternalContent: false
          ),
          !BufferPrivacyTransitionRules.shouldDiscard(
            previousExternal: appA, activatedExternal: appB,
            resetOnSwitch: true, holdsExternalContent: true
          ) else {
        print("FAILED: external app privacy transition rules")
        return false
    }

    let model = BufferModel.shared
    let oldEnabled = model.enabled
    let oldOnChange = model.onChange
    model.onChange = nil
    model.discardForPrivacy()
    model.enabled = false
    let rail = BufferInlineView()
    _ = rail.renderStandardForPreview()
    let renderedPassiveEmptyPlaceholder = rail.renderedInputPlaceholderVisible
        && !rail.renderedInputCaretVisible
        && rail.renderedLeadingInputControlCount == 0
        && rail.renderedTextFragments.contains("等待暂存内容")
    model.enabled = true
    _ = rail.renderStandardForPreview()
    let renderedActiveEmptyCaret = !rail.renderedInputPlaceholderVisible
        && rail.renderedInputCaretVisible
        && !rail.renderedTextFragments.contains("等待暂存内容")
    model.beginTransientLoading(requestId: "empty-status",
                                message: "正在接收插件内容")
    _ = rail.renderStandardForPreview()
    let renderedActiveEmptyStatus = !rail.renderedInputPlaceholderVisible
        && rail.renderedInputCaretVisible
        && rail.renderedTextFragments.contains("正在接收插件内容")
    model.finishTransientLoading(requestId: "empty-status")
    model.append("shield-smoke")
    _ = rail.renderStandardForPreview()
    let renderedBeforeShield = !rail.isHidden && rail.renderedBlockCount == 1
    _ = model.selectAllContent()
    _ = rail.renderStandardForPreview()
    let renderedStandardSelection = rail.renderedSelectedStandardBlockCount == 1
    model.clearAllContentSelection()
    model.failTransientLoading(requestId: "visible-error",
                               message: "收信箱已满，插件结果未保存")
    _ = rail.renderStandardForPreview()
    let renderedErrorBesideContent = rail.renderedTextFragments.contains {
        $0.contains("收信箱已满")
    }
    model.finishTransientLoading(requestId: "visible-error")
    _ = rail.renderStandardForPreview()
    let stableRenderPass = rail.renderPassCount
    let rebuiltUnchangedRail = rail.renderStandardForPreview()
    let skippedUnchangedRail = !rebuiltUnchangedRail && rail.renderPassCount == stableRenderPass
    rail.setEnterHoldProgress(0.6)
    let showedEnterHoldProgress = rail.isEnterHoldProgressVisible
    _ = rail.renderStandardForPreview(shielded: true)
    let scrubbedByShield = !rail.isHidden
        && rail.renderedBlockCount == 0
        && rail.renderedLeadingInputControlCount == 0
        && rail.renderedTextFragments == ["内容已隐藏"]
        && !rail.isEnterHoldProgressVisible
    model.enabled = oldEnabled
    model.discardForPrivacy()
    model.onChange = oldOnChange
    guard renderedPassiveEmptyPlaceholder, renderedActiveEmptyCaret,
          renderedActiveEmptyStatus,
          renderedBeforeShield, renderedStandardSelection, renderedErrorBesideContent,
          skippedUnchangedRail, showedEnterHoldProgress,
          scrubbedByShield else {
        print("FAILED: buffer rail secure-input shielding behavior")
        return false
    }

    let translationRail = BufferInlineView(
        frame: NSRect(
            x: 0,
            y: 0,
            width: 760,
            height: BufferInlineView.translationPreferredHeight(targetRows: 5)
        )
    )
    translationRail.layoutSubtreeIfNeeded()
    let compactMarinePreview = TranslationRailSnapshot(
        sourceText: "",
        showsSourceRail: false,
        outputBlocks: [],
        phase: .idle,
        targetRole: "答",
        targetEmptyText: "等待网页上下文"
    )
    translationRail.setFrameSize(NSSize(
        width: 760,
        height: BufferInlineView.translationPreferredHeight(
            targetRows: 1,
            showsSourceRail: false
        )
    ))
    _ = translationRail.renderTranslationForPreview(
        compactMarinePreview,
        active: false
    )
    translationRail.reconcileTranslationDocumentGeometry()
    let compactMarineProbe = translationRail.translationLayoutProbe
    let renderedCompactMarine = translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 1
        && translationRail.preferredHeight == BufferInlineView.standardPreferredHeight
        && compactMarineProbe.rails.count == 1
        && !translationRail.renderedInputPlaceholderVisible
        && !translationRail.renderedInputCaretVisible
        && translationRail.renderedTargetStatusDescriptions.contains("等待网页上下文")
        && !translationRail.renderedTextFragments.contains("等待网页上下文")
        && !translationRail.renderedTextFragments.contains("等待原文")

    let thoughtPreview = TranslationRailSnapshot(
        sourceText: "请分析",
        outputBlocks: [],
        phase: .translating,
        transientThought: "正在分析上下文",
        targetRole: "答"
    )
    _ = translationRail.renderTranslationForPreview(thoughtPreview)
    let showedTransientThought = translationRail.renderedTextFragments
        .contains("正在分析上下文")
    let answeredPreview = TranslationRailSnapshot(
        sourceText: "请分析",
        outputBlocks: [TranslationOutputBlock(id: UUID(), text: "正式回答")],
        phase: .ready,
        targetRole: "答"
    )
    _ = translationRail.renderTranslationForPreview(answeredPreview)
    let clearedTransientThought = translationRail.renderedTextFragments
        .contains("正式回答")
        && !translationRail.renderedTextFragments.contains("正在分析上下文")

    let emptyLivePreview = TranslationRailSnapshot(
        sourceText: "",
        outputBlocks: [],
        phase: .idle,
        sourceEmptyText: "输入内容",
        targetEmptyText: "等待结果"
    )
    let renderedPassiveEmptyLive = translationRail.renderTranslationForPreview(
        emptyLivePreview,
        active: false,
        presentationStyle: .liveExpand
    ) && translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 0
        && translationRail.preferredHeight == BufferInlineView.standardPreferredHeight
        && translationRail.renderedInputPlaceholderVisible
        && !translationRail.renderedInputCaretVisible
        && translationRail.renderedTextFragments.contains("输入内容")
        && !translationRail.renderedTextFragments.contains("等待结果")
    let renderedActiveEmptyLive = translationRail.renderTranslationForPreview(
        emptyLivePreview,
        active: true,
        presentationStyle: .liveExpand
    ) && translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 0
        && translationRail.preferredHeight == BufferInlineView.standardPreferredHeight
        && !translationRail.renderedInputPlaceholderVisible
        && translationRail.renderedInputCaretVisible
        && !translationRail.renderedTextFragments.contains("输入内容")
        && !translationRail.renderedTextFragments.contains("等待结果")
    let emptyLiveWaitingPreview = TranslationRailSnapshot(
        sourceText: "",
        outputBlocks: [],
        phase: .waiting,
        sourceEmptyText: "输入内容",
        targetEmptyText: "等待结果"
    )
    let renderedCompactEmptyWaiting = translationRail.renderTranslationForPreview(
        emptyLiveWaitingPreview,
        active: false,
        presentationStyle: .liveExpand
    ) && translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 0
        && translationRail.renderedTextFragments.contains("输入内容")
    let standaloneMessagePreview = TranslationRailSnapshot(
        sourceText: "",
        outputBlocks: [],
        phase: .failed,
        message: "处理失败",
        sourceEmptyText: "输入内容",
        targetEmptyText: "等待结果"
    )
    let renderedStandaloneMessage = translationRail.renderTranslationForPreview(
        standaloneMessagePreview,
        active: false,
        presentationStyle: .liveExpand
    ) && translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 1
        && translationRail.preferredHeight == BufferInlineView.standardPreferredHeight
        && !translationRail.renderedInputPlaceholderVisible
        && !translationRail.renderedInputCaretVisible
        && translationRail.renderedTargetStatusDescriptions.contains("处理失败")
        && !translationRail.renderedTextFragments.contains("处理失败")
        && !translationRail.renderedTextFragments.contains("输入内容")

    translationRail.setFrameSize(NSSize(
        width: 760,
        height: BufferInlineView.translationPreferredHeight(targetRows: 5)
    ))
    let sourcePreview = "上方原文缓冲"
    let targetPreviewA = "下方译文"
    let targetPreviewB = "第二块"
    let targetPreviewAID = UUID()
    let targetPreviewBID = UUID()
    let translationPreview = TranslationRailSnapshot(
        sourceText: sourcePreview,
        sourceSelected: true,
        outputBlocks: [
            TranslationOutputBlock(id: targetPreviewAID, text: targetPreviewA),
            TranslationOutputBlock(id: targetPreviewBID, text: targetPreviewB),
        ],
        phase: .ready
    )
    let renderedLiveExpand = translationRail.renderTranslationForPreview(
        translationPreview
    ) && translationRail.renderedTranslationSourceSelected
        && translationRail.translationRailCount == 2
        && translationRail.renderedTranslationTargetRowCount == 1
    let translationFragments = translationRail.renderedTextFragments
    let sourcePosition = translationFragments.firstIndex(of: sourcePreview)
    let targetPosition = translationFragments.firstIndex(of: targetPreviewA)
    let stableTargetViews = translationRail.renderedTranslationTargetViewIdentities
    let continuedPreview = TranslationRailSnapshot(
        sourceText: "\(sourcePreview)续",
        outputBlocks: [
            TranslationOutputBlock(
                id: targetPreviewAID,
                text: "\(targetPreviewA)续",
                retainedTailStart: targetPreviewA.utf16.count
            ),
            TranslationOutputBlock(id: targetPreviewBID, text: targetPreviewB),
        ],
        phase: .translating,
        message: "继续全局猜测"
    )
    _ = translationRail.renderTranslationForPreview(continuedPreview)
    let reusedTargetViews = stableTargetViews
        == translationRail.renderedTranslationTargetViewIdentities
        && stableTargetViews.count == 2
        && translationRail.renderedTextFragments.contains("\(targetPreviewA)续")

    let alternativeTexts = ["方案", "翻案", "凡干", "反感", "返岗", "第六项"]
    let alternativeIDs = alternativeTexts.map { _ in UUID() }
    let alternativePreview: (Int, Int) -> TranslationRailSnapshot = { selected, count in
        let blocks = (0..<count).map { index in
            TranslationOutputBlock(
                id: alternativeIDs[index],
                text: alternativeTexts[index],
                ordinal: index + 1,
                selected: index == selected
            )
        }
        return TranslationRailSnapshot(
            sourceText: "fangan",
            outputBlocks: blocks,
            outputRows: blocks.enumerated().map { index, block in
                TranslationOutputRow(key: index, blocks: [block])
            },
            phase: .ready,
            sourceRole: "拼",
            targetRole: "文"
        )
    }
    let fiveAlternativePreview = alternativePreview(2, 5)
    let renderedFiveAlternativePager = translationRail.renderTranslationForPreview(
        fiveAlternativePreview
    ) && translationRail.renderedAlternativeCount == 5
        && translationRail.renderedAlternativeIndex == 2
        && translationRail.renderedTranslationTargetRowCount == 1
        && translationRail.translationRailCount == 2
        && translationRail.preferredHeight
            == BufferInlineView.translationPreferredHeight(targetRows: 5)
        && translationRail.renderedTextFragments.contains("凡干")
        && !translationRail.renderedTextFragments.contains("方案")
        && !translationRail.renderedTextFragments.contains("返岗")
        && translationRail.renderedTranslationTargetViewIdentities.count == 1
    translationRail.reconcileTranslationDocumentGeometry()
    let pagerProbe = translationRail.translationLayoutProbe
    let pagerFrames = pagerProbe.rails
        .map(\.viewportFrame)
        .sorted { $0.minY < $1.minY }
    let renderedPagerGeometry = pagerProbe.boundsHeight == 64
        && pagerProbe.containerHeight == 54
        && pagerProbe.rails.count == 2
        && pagerProbe.rails.allSatisfy {
            abs($0.viewportHeight - 25) < 0.5
                && abs($0.documentHeight - $0.viewportHeight) < 0.5
        }
        && zip(pagerFrames, pagerFrames.dropFirst()).allSatisfy {
            abs(($1.minY - $0.maxY) - 4) < 0.5
        }
    let renderedFifthAlternative = translationRail.renderTranslationForPreview(
        alternativePreview(4, 5)
    ) && translationRail.renderedAlternativeIndex == 4
        && translationRail.renderedAlternativeCount == 5
        && translationRail.renderedTextFragments.contains("返岗")
        && !translationRail.renderedTextFragments.contains("凡干")
    let cappedSixthAlternative = translationRail.renderTranslationForPreview(
        alternativePreview(5, 6)
    ) && translationRail.renderedAlternativeCount == 5
        && translationRail.renderedAlternativeIndex == 0
        && translationRail.renderedTextFragments.contains("方案")
        && !translationRail.renderedTextFragments.contains("第六项")

    let retainedChildID = UUID()
    let segmentedPreview = TranslationRailSnapshot(
        sourceText: "fangan",
        outputBlocks: [
            TranslationOutputBlock(id: alternativeIDs[2], text: "修复"),
            TranslationOutputBlock(
                id: retainedChildID,
                text: "一个问题",
                retainedTailStart: 0
            ),
        ],
        outputRows: [
            TranslationOutputRow(
                key: 2,
                blocks: [
                    TranslationOutputBlock(id: alternativeIDs[2], text: "修复"),
                    TranslationOutputBlock(
                        id: retainedChildID,
                        text: "一个问题",
                        retainedTailStart: 0
                    ),
                ]
            ),
        ],
        phase: .translating,
        sourceRole: "拼",
        targetRole: "文"
    )
    let renderedSegmentedRail = translationRail.renderTranslationForPreview(
        segmentedPreview
    ) && translationRail.renderedAlternativeCount == 1
        && translationRail.renderedTranslationTargetRowCount == 1
        && translationRail.translationRailCount == 2
        && translationRail.renderedTranslationRetainedTailStarts[retainedChildID] == 0
        && translationRail.renderedTextFragments.contains("修复")
        && translationRail.renderedTextFragments.contains("一个问题")

    let exchangeIdlePreview = TranslationRailSnapshot(
        sourceText: "仍由工作区保留的原文",
        outputBlocks: [],
        phase: .idle,
        sourceRole: "原",
        targetRole: "答"
    )
    let renderedExchangeSource = translationRail.renderTranslationForPreview(
        exchangeIdlePreview,
        presentationStyle: .singleExchange
    ) && translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 0
        && translationRail.preferredHeight == BufferInlineView.standardPreferredHeight
        && translationRail.renderedTextFragments.contains("仍由工作区保留的原文")
    let exchangeWaitingPreview = TranslationRailSnapshot(
        sourceText: exchangeIdlePreview.sourceText,
        outputBlocks: [],
        phase: .waiting,
        sourceRole: "原",
        targetRole: "答",
        waitingText: "正在生成"
    )
    let renderedExchangeWaiting = translationRail.renderTranslationForPreview(
        exchangeWaitingPreview,
        active: false,
        presentationStyle: .singleExchange
    ) && translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 1
        && translationRail.preferredHeight == BufferInlineView.standardPreferredHeight
        && !translationRail.renderedInputPlaceholderVisible
        && !translationRail.renderedInputCaretVisible
        && translationRail.renderedTargetStatusDescriptions.contains("正在生成")
        && !translationRail.renderedTextFragments.contains("正在生成")
        && !translationRail.renderedTextFragments.contains(exchangeIdlePreview.sourceText)
    let exchangeResultID = UUID()
    let exchangeReadyPreview = TranslationRailSnapshot(
        sourceText: exchangeIdlePreview.sourceText,
        outputBlocks: [
            TranslationOutputBlock(id: exchangeResultID, text: "尚未投递的结果"),
        ],
        phase: .ready,
        sourceRole: "原",
        targetRole: "答"
    )
    let renderedExchangeResult = translationRail.renderTranslationForPreview(
        exchangeReadyPreview,
        presentationStyle: .singleExchange
    ) && translationRail.translationRailCount == 1
        && translationRail.renderedTranslationTargetRowCount == 1
        && translationRail.preferredHeight == BufferInlineView.standardPreferredHeight
        && translationRail.renderedTextFragments.contains("尚未投递的结果")
        && !translationRail.renderedTextFragments.contains(exchangeIdlePreview.sourceText)
        && exchangeReadyPreview.sourceText == exchangeIdlePreview.sourceText
        && exchangeReadyPreview.outputBlocks.first?.id == exchangeResultID
    let returnedExchangeSource = translationRail.renderTranslationForPreview(
        exchangeIdlePreview,
        presentationStyle: .singleExchange
    ) && translationRail.renderedTextFragments.contains(exchangeIdlePreview.sourceText)
        && !translationRail.renderedTextFragments.contains("尚未投递的结果")

    _ = translationRail.refresh(shielded: true)
    let translationShielded = translationRail.translationRailCount == 0
        && !translationRail.isHidden
        && translationRail.renderedLeadingInputControlCount == 0
        && translationRail.renderedTextFragments == ["内容已隐藏"]
        && !translationRail.renderedTextFragments.contains(sourcePreview)
        && !translationRail.renderedTextFragments.contains("方案")
        && !translationRail.renderedTextFragments.contains("尚未投递的结果")
        && !translationRail.renderedTextFragments.contains(exchangeIdlePreview.sourceText)
    guard renderedLiveExpand,
          renderedPassiveEmptyLive,
          renderedActiveEmptyLive,
          renderedCompactEmptyWaiting,
          renderedStandaloneMessage,
          sourcePosition != nil,
          targetPosition != nil,
          sourcePosition! < targetPosition!,
          !translationFragments.contains("→"),
          !translationFragments.contains("原"),
          !translationFragments.contains("译"),
          TranslationRailRoleSymbolRules.resolve("原", target: false)
            == .init(name: "text.bubble", accessibilityLabel: "原始内容"),
          TranslationRailRoleSymbolRules.resolve("答", target: true)
            == .init(name: "sparkles", accessibilityLabel: "AI 回答"),
          renderedCompactMarine,
          showedTransientThought,
          clearedTransientThought,
          reusedTargetViews,
          renderedFiveAlternativePager,
          renderedPagerGeometry,
          renderedFifthAlternative,
          cappedSixthAlternative,
          renderedSegmentedRail,
          renderedExchangeSource,
          renderedExchangeWaiting,
          renderedExchangeResult,
          returnedExchangeSource,
          translationShielded else {
        print("FAILED: buffer derived rail presentation contract",
              "live=\(renderedLiveExpand)",
              "empty=\(renderedPassiveEmptyLive)/\(renderedActiveEmptyLive)/\(renderedCompactEmptyWaiting)/\(renderedStandaloneMessage)",
              "compact=\(renderedCompactMarine)",
              "thinking=\(showedTransientThought)/\(clearedTransientThought)",
              "reused=\(reusedTargetViews)",
              "pager=\(renderedFiveAlternativePager)",
              "pagerGeometry=\(renderedPagerGeometry)",
              "fifth=\(renderedFifthAlternative)",
              "cap=\(cappedSixthAlternative)",
              "segmented=\(renderedSegmentedRail)",
              "exchange=\(renderedExchangeSource)/\(renderedExchangeWaiting)/\(renderedExchangeResult)",
              "return=\(returnedExchangeSource)",
              "shielded=\(translationShielded)",
              "probe=\(pagerProbe.boundsHeight)/\(pagerProbe.containerHeight)",
              "rails=\(pagerProbe.rails.map { ($0.key, $0.viewportHeight, $0.documentHeight) })")
        return false
    }

    var forwardedCaretIndex: Int?
    let caretIndexProbe = InputCaretGeometryRules.queryAtInlineSessionAnchor { index in
        forwardedCaretIndex = index
        return index
    }
    guard caretIndexProbe == 0, forwardedCaretIndex == 0 else {
        print("FAILED: IMK inline-session caret index contract")
        return false
    }

    let clipboardEligible = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: true,
        captureEnabled: true,
        secureInput: false,
        screenLocked: false,
        sessionInactive: false,
        sleeping: false
    )
    let clipboardOff = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: true,
        captureEnabled: false,
        secureInput: false,
        screenLocked: false,
        sessionInactive: false,
        sleeping: false
    )
    let clipboardHidden = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: false,
        captureEnabled: true,
        secureInput: false,
        screenLocked: false,
        sessionInactive: false,
        sleeping: false
    )
    let clipboardSecure = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: true,
        captureEnabled: true,
        secureInput: true,
        screenLocked: false,
        sessionInactive: false,
        sleeping: false
    )
    let clipboardLocked = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: true,
        captureEnabled: true,
        secureInput: false,
        screenLocked: true,
        sessionInactive: false,
        sleeping: false
    )
    let clipboardSessionInactive = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: true,
        captureEnabled: true,
        secureInput: false,
        screenLocked: false,
        sessionInactive: true,
        sleeping: false
    )
    let clipboardSleeping = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: true,
        captureEnabled: true,
        secureInput: false,
        screenLocked: false,
        sessionInactive: false,
        sleeping: true
    )
    let clipboardStackedProtection = ClipboardHistoryWindowLifecycleRules.captureState(
        windowVisibleOnActiveSpace: true,
        captureEnabled: true,
        secureInput: true,
        screenLocked: true,
        sessionInactive: true,
        sleeping: true
    )
    guard clipboardEligible.allowsClipboardObservation,
          clipboardEligible.allowsContentPresentation,
          !clipboardOff.allowsClipboardObservation,
          clipboardHidden.allowsClipboardObservation,
          !clipboardHidden.allowsContentPresentation,
          !clipboardSecure.allowsClipboardObservation,
          !clipboardLocked.allowsClipboardObservation,
          !clipboardSessionInactive.allowsClipboardObservation,
          !clipboardSleeping.allowsClipboardObservation,
          clipboardSecure.protection == [.secureInput],
          clipboardLocked.protection == [.screenLocked],
          clipboardSessionInactive.protection == [.sessionInactive],
          clipboardSleeping.protection == [.sessionInactive],
          clipboardStackedProtection.protection
            == [.secureInput, .screenLocked, .sessionInactive],
          !clipboardStackedProtection.allowsContentPresentation else {
        print("FAILED: standalone Clipboard capture/presentation gates")
        return false
    }

    let inlineComposition = BufferInlineView.inlineCompositionSnapshotForSmoke(
        text: "wai'mian",
        cursorPosUTF8: 3
    )
    let multibyteInlineComposition = BufferInlineView.inlineCompositionSnapshotForSmoke(
        text: "中a",
        cursorPosUTF8: 3
    )
    let invalidBoundaryInlineComposition = BufferInlineView.inlineCompositionSnapshotForSmoke(
        text: "中a",
        cursorPosUTF8: 1
    )
    let derivedInlineComposition = BufferInlineView
        .derivedInlineCompositionSnapshotForSmoke()
    let bufferCandidateSnapshot = CandidateWindow.bufferCaretSnapshotForSmoke()
    let candidateActionSurface = CandidateWindow.actionSurfaceSnapshotForSmoke()
    // Blocks are marked by an underline below the text, never a bubble; the
    // selected ordinary block and the selected result get the thick accent.
    let blockUnderlines = BufferInlineView.blockUnderlineSnapshotForSmoke()
    guard blockUnderlines.bubbleFree == [true, true, true, true],
          blockUnderlines.underlineAlongBottom == [true, true, true, true],
          blockUnderlines.textClearsUnderline == [true, true, true, true],
          blockUnderlines.thickness == [1, 2, 1, 2],
          blockUnderlines.accent == [false, true, false, true] else {
        print("FAILED: buffer blocks must be underlined, not bubbled: \(blockUnderlines)")
        return false
    }
    guard inlineComposition.renderedText == "wai'mian",
          inlineComposition.renderedPrefix == "wai",
          inlineComposition.renderedSuffix == "'mian",
          inlineComposition.caretBetweenRuns,
          inlineComposition.height == BufferInlineMetrics.itemHeight,
          multibyteInlineComposition.renderedPrefix == "中",
          multibyteInlineComposition.renderedSuffix == "a",
          invalidBoundaryInlineComposition.renderedPrefix.isEmpty,
          invalidBoundaryInlineComposition.renderedSuffix == "中a",
          derivedInlineComposition.renderedText == "zhong'jian",
          derivedInlineComposition.sourceRailVisible,
          derivedInlineComposition.attachedToSourceRail,
          derivedInlineComposition.placeholderAbsent,
          derivedInlineComposition.caretVisibleInSourceClip,
          bufferCandidateSnapshot.contentStayedInPanel,
          bufferCandidateSnapshot.renderedCandidateViews > 0,
          bufferCandidateSnapshot.preeditHidden,
          abs(bufferCandidateSnapshot.stripOnlyHeight
                - bufferCandidateSnapshot.expectedStripOnlyHeight) < 0.5,
          bufferCandidateSnapshot.rejectedCachedHostAnchor,
          bufferCandidateSnapshot.scrubbedCandidateViews,
          bufferCandidateSnapshot.scrubbedPreedit,
          candidateActionSurface.renderedCandidateButtons == 2,
          candidateActionSurface.renderedLegacyActionButtons == 0,
          !candidateActionSurface.settingsButtonVisible,
          candidateActionSurface.settingsAccessibilityLabel == nil,
          !CandidateKeyboardRoutingRules.ownsLocally(
            keycode: 0x30,
            isExpanded: false
          ),
          !CandidateKeyboardRoutingRules.ownsLocally(
            keycode: 0x30,
            isExpanded: true
          ),
          !CandidateKeyboardRoutingRules.ownsLocally(
            keycode: 0x31,
            isExpanded: false
          ),
          CandidateKeyboardRoutingRules.ownsLocally(
            keycode: 0x31,
            isExpanded: true
          ),
          CandidateKeyboardRoutingRules.ownsLocally(
            keycode: 0x39,
            isExpanded: true
          ),
          BufferCandidateRoutingRules.shouldFollowBufferCaret(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: false,
            capturesExactFocus: true
          ),
          !BufferCandidateRoutingRules.shouldFollowBufferCaret(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: false,
            capturesExactFocus: false
          ),
          !BufferCandidateRoutingRules.shouldFollowBufferCaret(
            workbenchVisible: false,
            presentationProtected: false,
            secureInput: false,
            capturesExactFocus: true
          ),
          !BufferCandidateRoutingRules.shouldFollowBufferCaret(
            workbenchVisible: true,
            presentationProtected: true,
            secureInput: false,
            capturesExactFocus: true
          ),
          !BufferCandidateRoutingRules.shouldFollowBufferCaret(
            workbenchVisible: true,
            presentationProtected: false,
            secureInput: true,
            capturesExactFocus: true
          ),
          CandidatePanelLevelRules.workbenchLevel(
            bundleID: "com.example.editor",
            hostKind: .frontmostApplication
          ) == CandidatePanelLevelRules.workbenchStandard,
          CandidatePanelLevelRules.workbenchLevel(
            bundleID: FocusHostRules.iShotBundleID,
            hostKind: .nonactivatingSystemOverlay
          ) == CandidatePanelLevelRules.level(
            bundleID: FocusHostRules.iShotBundleID,
            hostKind: .nonactivatingSystemOverlay
          ) else {
        print("FAILED: inline composition and floating candidate ownership")
        return false
    }

    let primary = NSRect(x: 0, y: 0, width: 1440, height: 900)
    let secondary = NSRect(x: 1440, y: 0, width: 1280, height: 800)
    let leftSecondary = NSRect(x: -1280, y: 0, width: 1280, height: 800)
    let openingFrame = NSRect(x: 200,
                              y: 200,
                              width: 680,
                              height: BufferWindowGeometry.collapsedHeight)
    let middleCaret = NSRect(x: 720, y: 500, width: 0, height: 22)
    let belowCaret = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: middleCaret,
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    let runtimeHeights = [
        BufferWindowGeometry.collapsedHeight,
        BufferWindowGeometry.height(expanded: false),
        BufferWindowGeometry.translationCollapsedHeight,
        BufferWindowGeometry.height(
            expanded: false,
            mode: .derived(targetRows: 2)
        ),
        BufferWindowGeometry.maximumRuntimeHeight,
        BufferWindowGeometry.collapsedHeight,
    ]
    let belowResizes = runtimeHeights.reduce(into: [belowCaret.frame]) { frames, height in
        frames.append(BufferWindowGeometry.resizedOutward(
            frames.last!,
            height: height,
            openingSide: .belowTarget
        ))
    }
    let bottomCaret = NSRect(x: 720, y: 12, width: 0, height: 22)
    let aboveCaret = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: bottomCaret,
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    let aboveResizes = runtimeHeights.reduce(into: [aboveCaret.frame]) { frames, height in
        frames.append(BufferWindowGeometry.resizedOutward(
            frames.last!,
            height: height,
            openingSide: .aboveTarget
        ))
    }
    let belowAnchor = middleCaret
    let belowCandidateSize = NSSize(width: 420, height: 60)
    let belowCandidateOrigin = CandidatePanelGeometry.origin(
        anchor: belowAnchor,
        panelSize: belowCandidateSize,
        visibleFrame: primary,
        preferredSide: .below
    )
    let aboveAnchor = bottomCaret
    let aboveCandidateSize = NSSize(width: 420, height: 60)
    let aboveCandidateOrigin = CandidatePanelGeometry.origin(
        anchor: aboveAnchor,
        panelSize: aboveCandidateSize,
        visibleFrame: primary,
        preferredSide: .above
    )
    let strictUnavailableAboveCandidate = CandidatePanelGeometry.originIfAvailable(
        anchor: NSRect(x: 200, y: 850, width: 680, height: 40),
        panelSize: NSSize(width: 420, height: 60),
        visibleFrame: primary,
        preferredSide: .above,
        strictPreferredSide: true
    )
    let strictUnavailableBelowCandidate = CandidatePanelGeometry.originIfAvailable(
        anchor: NSRect(x: 200, y: 12, width: 680, height: 40),
        panelSize: NSSize(width: 420, height: 60),
        visibleFrame: primary,
        preferredSide: .below,
        strictPreferredSide: true
    )
    // Current 44pt height fits below, but the largest two-rail 78pt forecast
    // does not. Choose the roomy upper side before the result rail appears.
    let forecastAwareOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: NSRect(x: 720, y: 90, width: 0, height: 22),
        visibleFrames: [primary],
        fallback: primary
    )
    let leftCaret = NSRect(x: -640, y: 520, width: 0, height: 22)
    let leftScreenOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: leftCaret,
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    let rightEdgeOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: NSRect(x: 1438, y: 520, width: 0, height: 22),
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    let maximumHeightOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: NSRect(
            x: openingFrame.minX,
            y: openingFrame.minY,
            width: openingFrame.width,
            height: BufferWindowGeometry.height(
                expanded: false,
                mode: .derived(targetRows: 3)
            )
        ),
        targetRect: NSRect(x: 720, y: 700, width: 0, height: 22),
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    let noTargetOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: nil,
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    let invalidTargetOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: NSRect(x: CGFloat.nan, y: 300, width: 0, height: 22),
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    let offscreenTargetOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: NSRect(x: 4000, y: 300, width: 0, height: 22),
        visibleFrames: [primary, secondary, leftSecondary],
        fallback: primary
    )
    // Box-aligned openings: left edge follows the field, width adopts it, and
    // the top edge sits one gap under the field's bottom edge.
    let chatBox = NSRect(x: 300, y: 460, width: 760, height: 44)
    let chatCaret = NSRect(x: 420, y: 468, width: 0, height: 22)
    let boxAlignedOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: chatCaret,
        boxRect: chatBox,
        visibleFrames: [primary],
        fallback: primary
    )
    // A field narrower than the readable minimum clamps up and keeps its left
    // edge rather than shrinking the workbench.
    let searchBox = NSRect(x: 240, y: 600, width: 220, height: 28)
    let searchCaret = NSRect(x: 260, y: 604, width: 0, height: 20)
    let narrowBoxOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: searchCaret,
        boxRect: searchBox,
        visibleFrames: [primary],
        fallback: primary
    )
    // A document-sized text area still frames the workbench horizontally, but
    // must not push it to the bottom of the editor.
    let editorBox = NSRect(x: 120, y: 80, width: 900, height: 700)
    let editorCaret = NSRect(x: 300, y: 700, width: 0, height: 22)
    let tallBoxOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: editorCaret,
        boxRect: editorBox,
        visibleFrames: [primary],
        fallback: primary
    )
    // A field wider than the workbench maximum clamps down, still left-aligned.
    let wideBox = NSRect(x: 140, y: 300, width: 1200, height: 40)
    let wideCaret = NSRect(x: 200, y: 308, width: 0, height: 22)
    let wideBoxOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: wideCaret,
        boxRect: wideBox,
        visibleFrames: [primary],
        fallback: primary
    )
    // A stale box that no longer surrounds the caret is rejected outright.
    let staleBoxOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: chatCaret,
        boxRect: NSRect(x: 20, y: 40, width: 300, height: 30),
        visibleFrames: [primary],
        fallback: primary
    )
    let caretOnlyOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: chatCaret,
        visibleFrames: [primary],
        fallback: primary
    )
    // A box near the bottom has no room below, so it flips above while keeping
    // its left edge and width.
    let bottomBox = NSRect(x: 360, y: 30, width: 700, height: 40)
    let bottomBoxCaret = NSRect(x: 400, y: 38, width: 0, height: 22)
    let flippedBoxOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: openingFrame,
        targetRect: bottomBoxCaret,
        boxRect: bottomBox,
        visibleFrames: [primary],
        fallback: primary
    )
    guard boxAlignedOpening.side == .belowTarget,
          boxAlignedOpening.frame.minX == chatBox.minX,
          boxAlignedOpening.frame.width == chatBox.width,
          boxAlignedOpening.frame.maxY
            == chatBox.minY - BufferWindowGeometry.inputAnchorGap,
          narrowBoxOpening.frame.minX == searchBox.minX,
          narrowBoxOpening.frame.width
            == BufferWindowGeometry.standardMinimumWidth,
          narrowBoxOpening.frame.maxY
            == searchBox.minY - BufferWindowGeometry.inputAnchorGap,
          tallBoxOpening.frame.minX == editorBox.minX,
          tallBoxOpening.frame.width == editorBox.width,
          wideBoxOpening.frame.minX == wideBox.minX,
          wideBoxOpening.frame.width
            == BufferWindowGeometry.standardMaximumWidth,
          tallBoxOpening.frame.maxY
            == editorCaret.minY - BufferWindowGeometry.inputAnchorGap,
          staleBoxOpening == caretOnlyOpening,
          // Without a box the caret no longer fixes the left edge; the panel
          // centres on it at its current width.
          caretOnlyOpening.frame.midX == chatCaret.midX,
          flippedBoxOpening.side == .aboveTarget,
          flippedBoxOpening.frame.minX == bottomBox.minX,
          flippedBoxOpening.frame.width == bottomBox.width,
          flippedBoxOpening.frame.minY
            == bottomBox.maxY + BufferWindowGeometry.inputAnchorGap,
          BufferWindowGeometry.isPlausibleInputBox(chatBox,
                                                   caret: chatCaret,
                                                   visibleFrames: [primary]),
          !BufferWindowGeometry.isPlausibleInputBox(
            NSRect(x: 300, y: 460, width: 0, height: 44),
            caret: chatCaret,
            visibleFrames: [primary]
          ),
          !BufferWindowGeometry.isPlausibleInputBox(
            chatBox,
            caret: NSRect(x: 4000, y: 468, width: 0, height: 22),
            visibleFrames: [primary]
          ) else {
        print("FAILED: input-box aligned workbench opening geometry",
              boxAlignedOpening, narrowBoxOpening, tallBoxOpening,
              wideBoxOpening, staleBoxOpening, flippedBoxOpening)
        return false
    }

    let manualOrigin = NSPoint(x: 160, y: 240)
    let transientPersistence = BufferWindowGeometry.canonicalPersistedFrame(
        belowCaret.frame,
        persistedOrigin: manualOrigin,
        transientOpeningOrigin: true
    )
    let userPersistence = BufferWindowGeometry.canonicalPersistedFrame(
        belowCaret.frame,
        persistedOrigin: manualOrigin,
        transientOpeningOrigin: false
    )
    guard BufferWindowGeometry.isPlausibleInputAnchor(
            middleCaret,
            visibleFrames: [primary]
          ),
          !BufferWindowGeometry.isPlausibleInputAnchor(.zero,
                                                       visibleFrames: [primary]),
          !BufferWindowGeometry.isPlausibleInputAnchor(
            NSRect(x: 200, y: 200, width: 0, height: 300),
            visibleFrames: [primary]
          ),
          belowCaret.side == .belowTarget,
          belowCaret.frame.maxY
            == middleCaret.minY - BufferWindowGeometry.inputAnchorGap,
          belowResizes.allSatisfy({
              $0.maxY == middleCaret.minY - BufferWindowGeometry.inputAnchorGap
                  && !$0.intersects(middleCaret)
          }),
          belowCaret.frame.midX == middleCaret.midX,
          BufferWorkbenchMetrics.contentLeadingInset == 12,
          aboveCaret.side == .aboveTarget,
          aboveCaret.frame.minY
            == bottomCaret.maxY + BufferWindowGeometry.inputAnchorGap,
          aboveResizes.allSatisfy({
              $0.minY == bottomCaret.maxY + BufferWindowGeometry.inputAnchorGap
                  && !$0.intersects(bottomCaret)
          }),
          belowCandidateOrigin.y + belowCandidateSize.height
            == belowAnchor.minY - 6,
          aboveCandidateOrigin.y == aboveAnchor.maxY + 6,
          strictUnavailableAboveCandidate == nil,
          strictUnavailableBelowCandidate == nil,
          forecastAwareOpening.side == .aboveTarget,
          forecastAwareOpening.frame.minY == 90 + 22
            + BufferWindowGeometry.inputAnchorGap,
          !aboveCaret.frame.intersects(bottomCaret),
          leftScreenOpening.side == .belowTarget,
          leftSecondary.contains(leftScreenOpening.frame),
          rightEdgeOpening.frame.maxX <= primary.maxX
            - BufferWindowGeometry.screenSafetyMargin,
          maximumHeightOpening.side == .belowTarget,
          maximumHeightOpening.frame.height
            == BufferWindowGeometry.height(
                expanded: false,
                mode: .derived(targetRows: 3)
            ),
          noTargetOpening.side == .bottomFallback,
          noTargetOpening.frame.midX == primary.midX,
          noTargetOpening.frame.minY > primary.minY,
          noTargetOpening.frame.midY < primary.midY,
          invalidTargetOpening == noTargetOpening,
          offscreenTargetOpening == noTargetOpening,
          transientPersistence.origin == manualOrigin,
          transientPersistence.width == belowCaret.frame.width,
          transientPersistence.height == BufferWindowGeometry.collapsedHeight,
          userPersistence.origin == belowCaret.frame.origin else {
        print("FAILED: focus-aware workbench opening geometry",
              belowCaret, aboveCaret, leftScreenOpening, rightEdgeOpening,
              maximumHeightOpening, noTargetOpening,
              invalidTargetOpening, offscreenTargetOpening)
        return false
    }

    let offscreen = NSRect(x: 4000, y: -900, width: 680, height: 230)
    let restored = BufferWindowGeometry.clampedFrame(
        offscreen,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        visibleFrames: [primary, secondary],
        fallback: primary
    )
    guard primary.contains(restored),
          restored.width >= BufferWindowGeometry.standardMinimumWidth,
          restored.height == BufferWindowGeometry.expandedHeight else {
        print("FAILED: offscreen frame was not restored to fallback screen", restored)
        return false
    }

    let legacyWorkbench = NSRect(x: 120, y: 220, width: 680, height: 340)
    let migrated = BufferWindowGeometry.clampedFrame(
        legacyWorkbench,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        visibleFrames: [primary],
        fallback: primary
    )
    let candidateSize = NSSize(width: 420, height: 60)
    let rightEdgeAnchor = NSRect(x: 1430, y: 400, width: 4, height: 4)
    let clampedCandidate = CandidatePanelGeometry.origin(anchor: rightEdgeAnchor,
                                                         panelSize: candidateSize,
                                                         visibleFrame: primary)
    let oldCompact = NSRect(x: 180, y: 240, width: 680, height: 52)
    let migratedOldCompact = BufferWindowGeometry.clampedFrame(
        oldCompact,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        visibleFrames: [primary],
        fallback: primary
    )
    let legacyCollapsed = BufferWindowGeometry.clampedFrame(
        migratedOldCompact,
        expanded: false,
        visibleFrames: [primary],
        fallback: primary
    )
    let translationExpanded = BufferWindowGeometry.clampedFrame(
        migratedOldCompact,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        mode: .translation,
        visibleFrames: [primary],
        fallback: primary
    )
    let compactDerivedExpanded = BufferWindowGeometry.clampedFrame(
        translationExpanded,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        mode: .singleDerived,
        visibleFrames: [primary],
        fallback: primary
    )
    let streamCandidatesTwoExpanded = BufferWindowGeometry.clampedFrame(
        translationExpanded,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        mode: .derived(targetRows: 2),
        visibleFrames: [primary],
        fallback: primary
    )
    let streamCandidatesExpanded = BufferWindowGeometry.clampedFrame(
        streamCandidatesTwoExpanded,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        mode: .derived(targetRows: 3),
        visibleFrames: [primary],
        fallback: primary
    )
    let standardAfterTranslation = BufferWindowGeometry.clampedFrame(
        translationExpanded,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        mode: .standard,
        visibleFrames: [primary],
        fallback: primary
    )
    let floatingCandidateInvariant = BufferWindowGeometry.clampedFrame(
        migratedOldCompact,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        mode: .translation,
        visibleFrames: [primary],
        fallback: primary
    )
    let canonicalFrame = BufferWindowGeometry.canonicalPersistedFrame(
        floatingCandidateInvariant,
        persistedOrigin: nil,
        transientOpeningOrigin: false
    )
    let aligned = BufferWindowGeometry.pixelAligned(
        NSRect(x: 10.24, y: 20.26, width: 680.24, height: 44),
        scale: 2
    )
    guard migrated.height == BufferWindowGeometry.expandedHeight,
          migrated.maxY == legacyWorkbench.maxY,
          migratedOldCompact.minY == oldCompact.minY,
          migratedOldCompact.height == BufferWindowGeometry.expandedHeight,
          legacyCollapsed.height == BufferWindowGeometry.collapsedHeight,
          legacyCollapsed.minY == migratedOldCompact.minY,
          translationExpanded.height == BufferWindowGeometry.translationExpandedHeight,
          translationExpanded.minY == migratedOldCompact.minY,
          compactDerivedExpanded.height == BufferWindowGeometry.expandedHeight,
          compactDerivedExpanded.minY == translationExpanded.minY,
          streamCandidatesTwoExpanded.height
            == BufferWindowGeometry.translationExpandedHeight,
          streamCandidatesTwoExpanded.minY == translationExpanded.minY,
          streamCandidatesExpanded.height
            == BufferWindowGeometry.translationExpandedHeight,
          streamCandidatesExpanded.minY == streamCandidatesTwoExpanded.minY,
          standardAfterTranslation.height == BufferWindowGeometry.expandedHeight,
          standardAfterTranslation.minY == translationExpanded.minY,
          BufferWindowGeometry.height(expanded: false) == 42,
          BufferWindowGeometry.height(expanded: false, mode: .translation) == 74,
          BufferWindowGeometry.height(expanded: true) == 73,
          BufferWindowGeometry.height(expanded: true, mode: .translation) == 105,
          floatingCandidateInvariant.height == 105,
          floatingCandidateInvariant.minY == migratedOldCompact.minY,
          canonicalFrame.height == BufferWindowGeometry.collapsedHeight,
          clampedCandidate.x + candidateSize.width <= primary.maxX - 6,
          aligned.minX * 2 == (aligned.minX * 2).rounded(),
          aligned.minY * 2 == (aligned.minY * 2).rounded() else {
        print("FAILED: legacy workbench did not migrate to a compact bar", migrated)
        return false
    }

    let oversized = NSRect(x: 1500, y: 40, width: 3000, height: 2000)
    let fitted = BufferWindowGeometry.clampedFrame(
        oversized,
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        visibleFrames: [primary, secondary],
        fallback: primary
    )
    guard fitted.width <= secondary.width,
          fitted.height == BufferWindowGeometry.expandedHeight,
          secondary.contains(fitted) else {
        print("FAILED: oversized frame was not clamped to its screen", fitted)
        return false
    }

    let tiny = NSRect(x: -480, y: 0, width: 480, height: 160)
    let tinyFitted = BufferWindowGeometry.clampedFrame(
        NSRect(x: -900, y: -500, width: 680, height: 230),
        expanded: BufferWorkbenchLayout.toolbarInitiallyExpanded,
        visibleFrames: [tiny],
        fallback: tiny
    )
    let tinyOpening = BufferWindowGeometry.openingPlacement(
        currentFrame: NSRect(
            x: -400,
            y: 20,
            width: 680,
            height: BufferWindowGeometry.height(
                expanded: false,
                mode: .derived(targetRows: 3)
            )
        ),
        targetRect: NSRect(x: -240, y: 70, width: 0, height: 20),
        visibleFrames: [tiny],
        fallback: tiny
    )
    guard tiny.contains(tinyFitted),
          tinyFitted.width <= tiny.width,
          tinyFitted.height <= tiny.height,
          tiny.contains(tinyOpening.frame),
          tinyOpening.side != .bottomFallback else {
        print("FAILED: tiny visible frame clamp", tinyFitted, tinyOpening)
        return false
    }

    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.finishLaunching()
    let transitionOverride = ProcessInfo.processInfo.environment[
        "RIMEBUFFER_LAYOUT_TRANSITION_RENDER_DIR"
    ]
    let transitionRoot: URL
    let removesTransitionRoot: Bool
    if let transitionOverride, !transitionOverride.isEmpty {
        transitionRoot = URL(fileURLWithPath: transitionOverride,
                             isDirectory: true)
        removesTransitionRoot = false
    } else {
        transitionRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent(
                "rimebuffer-layout-transition-\(UUID().uuidString)",
                isDirectory: true
            )
        removesTransitionRoot = true
    }
    do {
        try FileManager.default.createDirectory(
            at: transitionRoot,
            withIntermediateDirectories: true
        )
    } catch {
        print("FAILED: cannot create workbench transition render directory")
        return false
    }
    defer {
        if removesTransitionRoot {
            try? FileManager.default.removeItem(at: transitionRoot)
        }
    }
    for width: CGFloat in [400, 520, 760] {
        let overlay = BufferWindowController.shared
            .exerciseRailActionOverlayForSmoke(panelWidth: width)
        guard overlay.passed else {
            print(
                "FAILED: live Buffer rail action overlay",
                "width=\(overlay.panelWidth)",
                "rail2=\(overlay.railFrameWithTwoActions)",
                "rail3=\(overlay.railFrameWithThreeActions)",
                "overlay=\(overlay.overlayFrame)",
                "actions=\(overlay.actionFrames)",
                "sibling=\(overlay.overlayIsNonArrangedSibling)",
                "contained=\(overlay.overlayContainedByRail)",
                "ordered=\(overlay.actionsOrderedOnOneRow)",
                "hits=\(overlay.actionHitTestingWorks)",
                "passThrough=\(overlay.fadeAreaPassesThrough)",
                "surface=\(overlay.interactiveSurfaceVisibleAtIdle)",
                "a11y=\(overlay.interactiveAccessibilityLabelsAreReadable)",
                "functionAndSelectorIcons=\(overlay.functionMenuAndSelectorHavePluginIcons)",
                "appIcon=\(overlay.targetApplicationIconIsReal)",
                "passive=\(overlay.targetApplicationIndicatorIsPassive)",
                "beforeClose=\(overlay.targetApplicationIconPrecedesClose)",
                "scrubbed=\(overlay.protectedStateScrubsApplicationIdentity)",
                "ambiguous=\(overlay.hasAmbiguousLayout)"
            )
            return false
        }
    }

    let transition = BufferWindowController.shared
        .exerciseLayoutTransitionForSmoke(
            standardBeforePath: transitionRoot
                .appendingPathComponent("01-standard-before.png").path,
            derivedPath: transitionRoot
                .appendingPathComponent("02-derived.png").path,
            standardAfterPath: transitionRoot
                .appendingPathComponent("03-standard-after.png").path
        )
    let toolbarTransition = BufferWindowController.shared
        .exerciseToolbarToggleForSmoke(
            collapsedPath: transitionRoot
                .appendingPathComponent("04-toolbar-collapsed.png").path,
            expandedPath: transitionRoot
                .appendingPathComponent("05-toolbar-expanded.png").path,
            collapsedAgainPath: transitionRoot
                .appendingPathComponent("06-toolbar-collapsed-again.png").path
        )
    let transitionEpsilon: CGFloat = 0.5
    guard transition.renderedAllFrames,
          abs(transition.standardBefore.height
              - transition.expectedStandardHeight) <= transitionEpsilon,
          abs(transition.derived.height
              - transition.expectedDerivedHeight) <= transitionEpsilon,
          abs(transition.standardAfter.height
              - transition.expectedStandardHeight) <= transitionEpsilon,
          abs(transition.repairedStandard.height
              - transition.expectedStandardHeight) <= transitionEpsilon,
          abs(transition.standardAfter.minY
              - transition.standardBefore.minY) <= transitionEpsilon,
          abs(transition.standardAfter.width
              - transition.standardBefore.width) <= transitionEpsilon,
          toolbarTransition.renderedAllFrames,
          !toolbarTransition.toolbarHiddenInitially,
          toolbarTransition.toolbarVisibleWhenExpanded,
          !toolbarTransition.toolbarHiddenAfterCollapse,
          abs(toolbarTransition.collapsed.height
              - BufferWindowGeometry.expandedHeight) <= transitionEpsilon,
          abs(toolbarTransition.expanded.height
              - BufferWindowGeometry.expandedHeight) <= transitionEpsilon,
          abs(toolbarTransition.collapsedAgain.height
              - BufferWindowGeometry.expandedHeight) <= transitionEpsilon,
          abs(toolbarTransition.collapsed.minY
              - toolbarTransition.expanded.minY) <= transitionEpsilon,
          abs(toolbarTransition.collapsedAgain.minY
              - toolbarTransition.collapsed.minY) <= transitionEpsilon else {
        print("FAILED: live workbench layout transition",
              "before=\(transition.standardBefore)",
              "derived=\(transition.derived)",
              "after=\(transition.standardAfter)",
              "repaired=\(transition.repairedStandard)",
              "toolbar=\(toolbarTransition.collapsed) -> "
                + "\(toolbarTransition.expanded) -> "
                + "\(toolbarTransition.collapsedAgain)")
        return false
    }

    guard BufferWindowController.shared.aiChannelControlsAreSelectableForSmoke(
        outputPath: transitionRoot
            .appendingPathComponent("07-ai-model-and-effort.png").path
    ) else {
        print("FAILED: AI channel model/effort toolbar controls")
        return false
    }

    guard BufferWindowController.shared.scholayControlsAndResultForSmoke(
        outputPath: transitionRoot
            .appendingPathComponent("08-scholay-two-boxes.png").path
    ) else {
        print("FAILED: Scholay toolbar and two copyable result boxes")
        return false
    }

    guard BufferWindowController.shared.imageResultMailboxButtonForSmoke(
        outputPath: transitionRoot
            .appendingPathComponent("09-image-mailbox-button.png").path
    ) else {
        print("FAILED: image result Mailbox button")
        return false
    }

    print("buffer window smoke: OK")
    return true
}

func runMarineBridgeSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) Marine bridge smoke test ==")
    let model = BufferModel.shared
    let oldEnabled = model.enabled
    let oldOnChange = model.onChange
    defer {
        model.discardForPrivacy()
        model.enabled = oldEnabled
        model.onChange = oldOnChange
    }

    model.onChange = nil
    model.discardForPrivacy()

    MarineBridge.shared.checkForFocusedIntent()
    let deadline = Date().addingTimeInterval(5)
    while Date() < deadline {
        if let block = model.blocks.first {
            let expected = ProcessInfo.processInfo.environment["MARINE_BRIDGE_SMOKE_EXPECTED"]
            if let expected, !expected.isEmpty, block.text != expected {
                print("FAILED: loaded unexpected draft", "got=\(block.text)", "expected=\(expected)")
                return false
            }
            guard model.active, model.loadingMessage == nil else {
                print("FAILED: draft loaded but buffer state is inconsistent",
                      "active=\(model.active)",
                      "loading=\(model.loadingMessage ?? "nil")")
                return false
            }
            print("loaded draft chars:", block.text.count)
            print("marine bridge smoke: OK")
            return true
        }
        RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
    }

    print("FAILED: no Marine draft loaded",
          "loading=\(model.loadingMessage ?? "nil")",
          "active=\(model.active)",
          "blocks=\(model.blocks.count)")
    return false
}

/// Pins the "unsupported interval" rule behind the candidate-window size controls:
/// a dependent metric (button height, candidate glyph, index label) can only be
/// set as high as its container currently allows, because anything taller is
/// clipped or silently absorbed by the layout. A wrong verdict here means the
/// settings sliders would offer sizes that cannot render faithfully.
func runCandidateMetricsSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) candidate metrics smoke test ==")
    var ok = true
    func check(_ cond: Bool, _ msg: String) {
        if !cond { print("FAILED: \(msg)"); ok = false }
    }
    for (message, result) in CandidateWindow.adaptiveWidthChecksForSmoke() {
        check(result, message)
    }
    check(CandidateLayout.fittedPanelWidth(rowWidths: [100, 200], preeditWidth: 300, maximumWidth: 460) == 300,
          "preedit can widen the panel within its ceiling")
    check(CandidateLayout.fittedPanelWidth(rowWidths: [200], preeditWidth: 0, maximumWidth: 180) == 180,
          "narrow displays bound candidate width")

    // The dependency chain must stay strip -> button -> candidate glyph -> label.
    check(CandidateWindowMetric.compactCandidateHeight.containerMetric?.metric == .compactStripHeight,
          "button height should be bounded by strip height")
    check(CandidateWindowMetric.candidateFontSize.containerMetric?.metric == .compactCandidateHeight,
          "candidate glyph should be bounded by button height")
    check(CandidateWindowMetric.labelFontSize.containerMetric?.metric == .candidateFontSize,
          "index label should be bounded by candidate glyph")
    for m in [CandidateWindowMetric.baseWidth, .compactStripHeight, .preeditHeight] {
        check(m.containerMetric == nil, "\(m.rawValue) should be a free container metric")
    }

    func vals(strip: Double, button: Double, glyph: Double, label: Double) -> [CandidateWindowMetric: Double] {
        [.baseWidth: 460, .preeditHeight: 20,
         .compactStripHeight: strip, .compactCandidateHeight: button,
         .candidateFontSize: glyph, .labelFontSize: label]
    }

    // Button ceiling tracks the strip (strip − 2), capped at its own max 44.
    check(CandidateWindowMetric.compactCandidateHeight
        .supportedRange(given: vals(strip: 32, button: 24, glyph: 16, label: 10)).upperBound == 30,
          "strip=32 should cap button height at 30")
    check(CandidateWindowMetric.compactCandidateHeight
        .supportedRange(given: vals(strip: 64, button: 24, glyph: 16, label: 10)).upperBound == 44,
          "strip=64 should allow the full 44 button")

    // Candidate glyph ceiling tracks the button with enough room for AppKit's
    // ascender/descender line box (24pt is roughly 30px tall on the system font).
    check(CandidateWindowMetric.candidateFontSize
        .supportedRange(given: vals(strip: 32, button: 22, glyph: 16, label: 10)).upperBound == 16,
          "button=22 should cap the candidate glyph at 16")
    check(CandidateWindowMetric.candidateFontSize
        .supportedRange(given: vals(strip: 64, button: 44, glyph: 16, label: 10)).upperBound == 24,
          "button=44 should allow the full 24 candidate glyph")

    let resolvedChain = CandidateWindowMetrics.resolvedValues(
        vals(strip: 32, button: 22, glyph: 24, label: 18)
    )
    check(resolvedChain[.compactCandidateHeight] == 22,
          "a supported button height should remain unchanged")
    check(resolvedChain[.candidateFontSize] == 16,
          "the full resolver should clamp candidate glyphs to their button")
    check(resolvedChain[.labelFontSize] == 16,
          "the full resolver should then clamp labels to the candidate glyph")
    check(CandidateLayout.candidateSeparatorRunWidth == 14,
          "preview and live candidate separators should occupy the same width")

    // Exercise the real AppKit/CoreText hierarchy at normal, selected, maximum
    // font and extra-tall button sizes. The title view occupies a symmetric
    // content rect and the painted ink must stay centred within the button.
    let rendererCases: [(
        button: CGFloat,
        glyph: CGFloat,
        label: CGFloat,
        highlighted: Bool
    )] = [
        (24, 17, 10, false),
        (24, 17, 10, true),
        (30, 24, 18, true),
        (44, 24, 18, false),
    ]
    for rendererCase in rendererCases {
        let snapshot = CandidateWindow.textRendererSnapshotForSmoke(
            buttonHeight: rendererCase.button,
            candidateFontSize: rendererCase.glyph,
            labelFontSize: rendererCase.label,
            highlighted: rendererCase.highlighted
        )
        let prefix = "renderer \(Int(rendererCase.glyph))pt/\(Int(rendererCase.button))px"
        check(snapshot.nativeButtonTitleIsEmpty && snapshot.nativeAttributedTitleIsEmpty,
              "\(prefix) should not use NSButtonCell for text")
        let inset = CandidateLayout.compactCandidateHorizontalPadding / 2
        check(abs(snapshot.titleViewFrame.minX - inset) < 0.01
                && abs(snapshot.titleViewFrame.maxX
                    - (snapshot.buttonBounds.maxX - inset)) < 0.01
                && abs(snapshot.titleViewFrame.minY - snapshot.buttonBounds.minY) < 0.01
                && abs(snapshot.titleViewFrame.height - snapshot.buttonBounds.height) < 0.01,
              "\(prefix) title view should fill a symmetric button content rect")
        check(snapshot.buttonBounds.height + 0.01 >= ceil(snapshot.requiredLineHeight),
              "\(prefix) button \(snapshot.buttonBounds.height) should contain line box \(snapshot.requiredLineHeight)")
        check(snapshot.baselineCenterDelta < 0.01,
              "\(prefix) label and candidate visual centres should align")
        check(snapshot.titleViewHitTestIsNil,
              "\(prefix) title view should leave clicks to the button")
        check(snapshot.candidateGlyphPixelCount > 0,
              "\(prefix) should paint candidate glyphs")
        check(snapshot.horizontalInkCenterDelta <= 1.25,
              "\(prefix) horizontal ink delta \(snapshot.horizontalInkCenterDelta) should be centred")
        check(snapshot.verticalInkCenterDelta <= 1.25,
              "\(prefix) vertical ink delta \(snapshot.verticalInkCenterDelta) should be centred")
    }

    // Reproduce the live strip's tightest titles at the user's current sizing.
    // Rasterizing the real CoreText hierarchy catches both a missing lone CJK
    // glyph and an off-centre title that the roomy 180px case could conceal.
    let tightSingleGlyphCases: [(
        label: String,
        text: String,
        highlighted: Bool
    )] = [
        ("6", "现", false),
        ("", "现", true),
    ]
    for rendererCase in tightSingleGlyphCases {
        let snapshot = CandidateWindow.textRendererSnapshotForSmoke(
            buttonHeight: 24,
            candidateFontSize: 17,
            labelFontSize: 10,
            candidateText: rendererCase.text,
            label: rendererCase.label,
            usesTightWidth: true,
            highlighted: rendererCase.highlighted
        )
        let prefix = rendererCase.label.isEmpty
            ? "tight single glyph"
            : "tight labeled single glyph"
        check(snapshot.candidateGlyphPixelCount > 0,
              "\(prefix) should render candidate ink at its natural width")
        check(snapshot.horizontalInkCenterDelta <= 1.25,
              "\(prefix) should remain horizontally centred")
        check(snapshot.verticalInkCenterDelta <= 1.25,
              "\(prefix) should remain vertically centred")
    }

    // Index-label ceiling tracks the candidate glyph.
    check(CandidateWindowMetric.labelFontSize
        .supportedRange(given: vals(strip: 34, button: 24, glyph: 12, label: 10)).upperBound == 12,
          "candidate font=12 should cap the index label at 12")
    check(CandidateWindowMetric.labelFontSize
        .supportedRange(given: vals(strip: 34, button: 24, glyph: 24, label: 10)).upperBound == 18,
          "candidate font=24 should allow the index label up to 18")

    // The layout absorbs an over-tall button — the very reason the control forbids it.
    let over = CandidateWindowMetrics(baseWidth: 460, compactStripHeight: 32,
                                      compactCandidateHeight: 44, preeditHeight: 20,
                                      candidateFontSize: 16, labelFontSize: 10)
    check(CandidateLayout.candidateButtonHeight(over) <= 30,
          "an over-tall button must be clamped by the strip during layout")

    if ok { print("candidate metrics smoke: OK") }
    return ok
}

/// Guards the Classic colorways and the Rasta theme against regressions to a
/// system-owned accent or foregrounds that disappear on small UI text.
func runThemeSmokeTest() -> Bool {
    print("== \(ProductIdentity.displayName) theme contrast smoke test ==")
    var ok = true
    func check(_ condition: Bool, _ message: String) {
        if !condition {
            print("FAILED: \(message)")
            ok = false
        }
    }

    check(RimeAppearanceMode.night.rawValue == "night",
          "墨竹 must preserve the persisted night raw value")
    check(RimeAppearanceMode.day.rawValue == "day",
          "翡翠 must preserve the persisted day raw value")
    check(RimeAppearanceMode.quiet.rawValue == "quiet",
          "静谧 must use a stable persisted quiet raw value")
    check(RimeAppearanceMode.rasta.rawValue == "rasta",
          "拉斯塔 must use a stable persisted rasta raw value")
    check(RimeAppearanceMode(rawValue: "night") == .night,
          "the legacy night preference must still load as 墨竹")
    check(RimeAppearanceMode(rawValue: "day") == .day,
          "the legacy day preference must still load as 翡翠")
    check(RimeAppearanceMode(rawValue: "quiet") == .quiet,
          "the quiet preference must load as 静谧")
    check(RimeAppearanceMode.night.title == "墨竹",
          "night's visible theme name should be 墨竹")
    check(RimeAppearanceMode.day.title == "翡翠",
          "day's visible theme name should be 翡翠")
    check(RimeAppearanceMode.quiet.title == "静谧",
          "quiet's visible theme name should be 静谧")
    check(RimeAppearanceMode.rasta.title == "拉斯塔",
          "rasta's visible theme name should be 拉斯塔")
    check(RimeAppearanceMode.allCases == [.night, .day, .quiet, .rasta, .liquidGlass],
          "theme order should preserve existing themes and append Liquid Glass")
    check(RimeAppearanceMode.night.family == .classic
            && RimeAppearanceMode.day.family == .classic
            && RimeAppearanceMode.quiet.family == .classic
            && RimeAppearanceMode.rasta.family == .rasta,
          "three legacy palettes must be Classic colorways while Rasta is independent")

    let day = RimeThemePalettes.day
    let night = RimeThemePalettes.night
    let quiet = RimeThemePalettes.quiet
    let rasta = RimeThemePalettes.rasta
    let productGreen = RimeThemePalettes.productGreen
    check(productGreen == 0x22C55E,
          "the product accent should remain the approved fixed green")
    check(day.accentBlue == productGreen && day.accentGreen == productGreen,
          "翡翠 should use the fixed product green for every accent alias")
    check(night.accentBlue == productGreen && night.accentGreen == productGreen,
          "墨竹 should use the fixed product green for every accent alias")
    check(day.accentBlue == night.accentBlue,
          "the two product-green themes must keep the same accent")
    check(quiet.accentBlue == 0xA3A3A3
            && quiet.accentGreen == quiet.accentBlue,
          "静谧 should use the approved neutral accent in both legacy slots")
    check(quiet.accentGreen != productGreen,
          "静谧 must not leak the product green into its normal theme accent")
    check(Set([rasta.brandRed, rasta.brandYellow, rasta.brandGreen]).count == 3,
          "拉斯塔 must expose distinct red, yellow, and green brand roles")
    check(rasta.accentBlue == rasta.brandGreen
            && rasta.accentSecondary == rasta.brandYellow
            && rasta.accentTertiary == rasta.brandRed,
          "拉斯塔 semantic accents must map to green, yellow, and red")
    check(quiet.accentForeground == 0x000000,
          "静谧 accent controls should choose their higher-contrast black foreground")
    check(KeyboardHeatmapColorRules.sRGBHex(
            red: 34.0 / 255.0,
            green: 197.0 / 255.0,
            blue: 94.0 / 255.0
          ) == productGreen,
          "heatmap sRGB conversion should preserve the product green")
    check(ProcessInfo.processInfo.environment["RIMEBUFFER_APPEARANCE_MODE"] == nil
            || RimeUI.appearance.rawValue
                == ProcessInfo.processInfo.environment["RIMEBUFFER_APPEARANCE_MODE"],
          "the preview-only theme override should select the requested palette")

    let textColors: [(String, UInt32)] = [
        ("primary", day.textPrimary),
        ("secondary", day.textSecondary),
        ("muted", day.textMuted),
    ]
    for (name, color) in textColors {
        let ratio = RimeColorContrast.ratio(
            foreground: color,
            background: day.candidateBackground
        )
        check(ratio >= 4.5, "day \(name) contrast \(ratio) should be >= 4.5")
    }

    let daySelectedTextRatio = RimeColorContrast.ratio(
        foreground: day.selectedCandidateText,
        background: day.selectedCandidateBackground
    )
    check(daySelectedTextRatio >= 4.5,
          "day selected text contrast \(daySelectedTextRatio) should be >= 4.5")
    check(day.selectedCandidateText
            == RimeColorContrast.preferredForeground(
                background: day.selectedCandidateBackground
            ),
          "day selected text should use the safest monochrome foreground")
    let daySelectionRatio = RimeColorContrast.ratio(
        foreground: day.selectedCandidateBackground,
        background: day.candidateBackground
    )
    check(daySelectionRatio >= 3,
          "day selected background contrast \(daySelectionRatio) should be >= 3")

    let mutedBufferRatio = RimeColorContrast.ratio(
        foreground: day.textMuted,
        background: day.bufferBackgroundSecondary
    )
    check(mutedBufferRatio >= 4.5,
          "day muted workbench contrast \(mutedBufferRatio) should be >= 4.5")

    let borderRatio = RimeColorContrast.ratio(
        foreground: day.borderStrong,
        background: day.candidateBackground
    )
    check(borderRatio >= 3,
          "day strong border contrast \(borderRatio) should be >= 3")

    let heatmapBackgrounds: [(String, UInt32)] = [
        ("day surface", day.surfaceSecondary),
        ("day accent", day.accentGreen),
        ("night surface", night.surfaceSecondary),
        ("night accent", night.accentGreen),
        ("quiet surface", quiet.surfaceSecondary),
        ("quiet accent", quiet.accentGreen),
    ]
    for (name, background) in heatmapBackgrounds {
        let foreground = KeyboardHeatmapColorRules.preferredForeground(
            background: background
        )
        check(foreground == RimeColorContrast.preferredForeground(
                background: background
              ),
              "\(name) heatmap foreground should use the contrast rule")
        let ratio = RimeColorContrast.ratio(
            foreground: foreground,
            background: background
        )
        check(ratio >= 4.5,
              "\(name) heatmap text contrast \(ratio) should be >= 4.5")
    }

    let dayFlyChordSuccess = FlyChordSettingsThemeRules.successTextHex(
        for: .day
    )
    check(dayFlyChordSuccess == day.selectedCandidateBackground,
          "翡翠 FlyChord success text should use the deep product green")
    let dayFlyChordRatio = RimeColorContrast.ratio(
        foreground: dayFlyChordSuccess,
        background: day.surfaceSecondary
    )
    check(dayFlyChordRatio >= 4.5,
          "翡翠 FlyChord success contrast \(dayFlyChordRatio) should be >= 4.5")

    let nightFlyChordSuccess = FlyChordSettingsThemeRules.successTextHex(
        for: .night
    )
    check(nightFlyChordSuccess == productGreen,
          "墨竹 FlyChord success text should use the product green")
    let nightFlyChordRatio = RimeColorContrast.ratio(
        foreground: nightFlyChordSuccess,
        background: night.surfaceSecondary
    )
    check(nightFlyChordRatio >= 4.5,
          "墨竹 FlyChord success contrast \(nightFlyChordRatio) should be >= 4.5")

    let quietFlyChordSuccess = FlyChordSettingsThemeRules.successTextHex(
        for: .quiet
    )
    check(quietFlyChordSuccess == quiet.accentGreen,
          "静谧 FlyChord success text should use its neutral accent")
    let quietFlyChordRatio = RimeColorContrast.ratio(
        foreground: quietFlyChordSuccess,
        background: quiet.surfaceSecondary
    )
    check(quietFlyChordRatio >= 4.5,
          "静谧 FlyChord success contrast \(quietFlyChordRatio) should be >= 4.5")

    let nightTextColors: [(String, UInt32)] = [
        ("primary", night.textPrimary),
        ("secondary", night.textSecondary),
        ("muted", night.textMuted),
    ]
    for (name, color) in nightTextColors {
        let ratio = RimeColorContrast.ratio(
            foreground: color,
            background: night.candidateBackground
        )
        check(ratio >= 4.5, "night \(name) contrast \(ratio) should be >= 4.5")
    }

    let nightSelectedTextRatio = RimeColorContrast.ratio(
        foreground: night.selectedCandidateText,
        background: night.selectedCandidateBackground
    )
    check(nightSelectedTextRatio >= 4.5,
          "night selected text contrast \(nightSelectedTextRatio) should be >= 4.5")
    check(night.selectedCandidateText
            == RimeColorContrast.preferredForeground(
                background: night.selectedCandidateBackground
            ),
          "night selected text should use the safest monochrome foreground")
    let nightSelectionRatio = RimeColorContrast.ratio(
        foreground: night.selectedCandidateBackground,
        background: night.candidateBackground
    )
    check(nightSelectionRatio >= 3,
          "night selected background contrast \(nightSelectionRatio) should be >= 3")

    let nightWorkbenchStatusRatio = RimeColorContrast.ratio(
        foreground: night.textSecondary,
        background: night.bufferBackground
    )
    check(nightWorkbenchStatusRatio >= 4.5,
          "night workbench status contrast \(nightWorkbenchStatusRatio) should be >= 4.5")

    let nightWorkbenchMutedRatio = RimeColorContrast.ratio(
        foreground: night.textMuted,
        background: night.bufferBackground
    )
    check(nightWorkbenchMutedRatio >= 4.5,
          "night muted workbench contrast \(nightWorkbenchMutedRatio) should be >= 4.5")

    let nightWorkbenchBorderRatio = RimeColorContrast.ratio(
        foreground: night.borderStrong,
        background: night.bufferBackground
    )
    check(nightWorkbenchBorderRatio >= 3,
          "night workbench border contrast \(nightWorkbenchBorderRatio) should be >= 3")

    let quietColors = [
        quiet.accentBlue,
        quiet.accentGreen,
        quiet.bufferBackground,
        quiet.bufferBackgroundSecondary,
        quiet.bufferBorder,
        quiet.surface,
        quiet.surfaceSecondary,
        quiet.surfaceTertiary,
        quiet.border,
        quiet.borderStrong,
        quiet.textPrimary,
        quiet.textSecondary,
        quiet.textMuted,
        quiet.selectedCandidateBackground,
        quiet.selectedCandidateText,
        quiet.candidateBackground,
    ]
    check(quietColors.allSatisfy { color in
        let red = (color >> 16) & 0xff
        let green = (color >> 8) & 0xff
        let blue = color & 0xff
        return red == green && green == blue
    }, "every 静谧 palette token should be chroma-free")

    for (name, color) in [
        ("primary", quiet.textPrimary),
        ("secondary", quiet.textSecondary),
        ("muted", quiet.textMuted),
    ] {
        let ratio = RimeColorContrast.ratio(
            foreground: color,
            background: quiet.candidateBackground
        )
        check(ratio >= 4.5,
              "quiet \(name) contrast \(ratio) should be >= 4.5")
    }
    let quietSelectedTextRatio = RimeColorContrast.ratio(
        foreground: quiet.selectedCandidateText,
        background: quiet.selectedCandidateBackground
    )
    check(quietSelectedTextRatio >= 4.5,
          "quiet selected text contrast \(quietSelectedTextRatio) should be >= 4.5")
    check(quiet.selectedCandidateText
            == RimeColorContrast.preferredForeground(
                background: quiet.selectedCandidateBackground
            ),
          "quiet selected text should use the safest monochrome foreground")
    let quietSelectionRatio = RimeColorContrast.ratio(
        foreground: quiet.selectedCandidateBackground,
        background: quiet.candidateBackground
    )
    check(quietSelectionRatio >= 3,
          "quiet selected background contrast \(quietSelectionRatio) should be >= 3")
    let quietAccentTextRatio = RimeColorContrast.ratio(
        foreground: quiet.accentText,
        background: quiet.surfaceSecondary
    )
    check(quietAccentTextRatio >= 4.5,
          "quiet small accent text contrast \(quietAccentTextRatio) should be >= 4.5")
    let quietBorderRatio = RimeColorContrast.ratio(
        foreground: quiet.borderStrong,
        background: quiet.bufferBackground
    )
    check(quietBorderRatio >= 3,
          "quiet workbench border contrast \(quietBorderRatio) should be >= 3")

    check(RimeAppearanceMode.day.appKitAppearanceName(increasedContrast: false) == .aqua,
          "day mode should force Aqua")
    check(RimeAppearanceMode.night.appKitAppearanceName(increasedContrast: false) == .darkAqua,
          "night mode should force Dark Aqua")
    check(RimeAppearanceMode.day.appKitAppearanceName(increasedContrast: true)
            == .accessibilityHighContrastAqua,
          "day mode should preserve increased contrast")
    check(RimeAppearanceMode.night.appKitAppearanceName(increasedContrast: true)
            == .accessibilityHighContrastDarkAqua,
          "night mode should preserve increased contrast")
    check(RimeAppearanceMode.quiet.appKitAppearanceName(increasedContrast: false)
            == .darkAqua,
          "quiet mode should force Dark Aqua")
    check(RimeAppearanceMode.quiet.appKitAppearanceName(increasedContrast: true)
            == .accessibilityHighContrastDarkAqua,
          "quiet mode should preserve increased contrast")
    check(RimeAppearanceMode.rasta.appKitAppearanceName(increasedContrast: false)
            == .darkAqua,
          "rasta mode should force Dark Aqua")
    check(RimeAppearanceMode.rasta.appKitAppearanceName(increasedContrast: true)
            == .accessibilityHighContrastDarkAqua,
          "rasta mode should preserve increased contrast")

    if ok { print("theme contrast smoke: OK") }
    return ok
}
