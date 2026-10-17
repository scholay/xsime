import AppKit
import CryptoKit
import Foundation
import RimesCore

private final class PluginDistributionSmokeDownloader: ActionPluginManifestDownloading {
    private let result: Result<Data, Error>
    private let holdCompletion: Bool
    private let lock = NSLock()
    private var storedURLs: [URL] = []
    private var heldCompletion: ((Result<Data, Error>) -> Void)?

    var requestedURLs: [URL] {
        lock.lock()
        defer { lock.unlock() }
        return storedURLs
    }

    init(result: Result<Data, Error>, holdCompletion: Bool = false) {
        self.result = result
        self.holdCompletion = holdCompletion
    }

    func downloadManifest(
        from url: URL,
        completion: @escaping (Result<Data, Error>) -> Void
    ) {
        lock.lock()
        storedURLs.append(url)
        if holdCompletion {
            heldCompletion = completion
            lock.unlock()
            return
        }
        lock.unlock()
        completion(result)
    }

    func completeHeldDownload() {
        let completion: ((Result<Data, Error>) -> Void)?
        lock.lock()
        completion = heldCompletion
        heldCompletion = nil
        lock.unlock()
        completion?(result)
    }
}

private final class PluginDistributionSmokeInternalPlugin: InternalPlugin {
    let descriptor: PluginDescriptor
    private(set) var startCount = 0
    private(set) var stopCount = 0

    init(entry: PresetBufferPluginCatalogEntry) {
        descriptor = PluginDescriptor(
            key: PluginKey(domain: .builtIn, rawID: entry.id),
            wireID: nil,
            name: entry.nameZH,
            symbolName: "puzzlepiece.extension",
            version: entry.version,
            summary: entry.summaryZH,
            source: .builtIn,
            capabilities: [.bufferAction],
            settings: nil,
            canUninstall: true
        )
    }

    func start() { startCount += 1 }
    func stop() { stopCount += 1 }
    func makeSettingsViewController(subpageID: String) -> NSViewController? { nil }
}

private struct PluginDistributionSmokeFile: Equatable {
    let relativePath: String
    let kind: String
    let contents: Data?
}

private struct PluginDistributionSmokeDirectorySnapshot: Equatable {
    let rootExists: Bool
    let files: [PluginDistributionSmokeFile]
}

private enum PluginDistributionSmokeHarnessError: Error {
    case completionTimedOut
}

/// Executable coverage for first-run preset policy, one-time legacy migration,
/// and hash-pinned optional package installation. Every scenario owns an
/// isolated defaults suite and filesystem root, and downloads are local fakes.
func runPluginDistributionSmokeTest() -> Bool {
    guard checkDevelopmentHostCompatibility() else { return false }
    let fileManager = FileManager.default
    let expectedDefaultIDs = Set([BuiltInPluginID.appleTranslation, BuiltInPluginID.streamInput,
        BuiltInPluginID.statistics, BuiltInPluginID.typingSpeed, BuiltInPluginID.flyChordLearning])
        .union(CapsuleModuleID.allCases.map(BuiltInPluginID.capsule))
        .union(MailboxModuleID.allCases.map { $0.pluginKey.rawID })
    let expectedEnabledIDs = expectedDefaultIDs.subtracting([BuiltInPluginID.flyChordLearning])
    let legacyCapsuleBufferPluginID = "builtin.capsule"
    let expectedOptionalIDs: Set<String> = [BuiltInPluginID.codexCLI, BuiltInPluginID.claudeCodeCLI,
        BuiltInPluginID.openAICompatible, BuiltInPluginID.scholay, BuiltInPluginID.polisher,
        BuiltInPluginID.latex, BuiltInPluginID.music, BuiltInPluginID.morse]
    let retiredProductIDs: Set<String> = [
        BuiltInPluginID.myPrompt,
        BuiltInPluginID.remarkable,
        BuiltInPluginID.marineChrome,
        // The single generic AI Generation plug-in split into three task
        // plug-ins, one per backend.
        BuiltInPluginID.aiText,
    ]
    let expectedVersions = [
        BuiltInPluginID.codexCLI: "1.1.1", BuiltInPluginID.claudeCodeCLI: "1.1.0",
        BuiltInPluginID.openAICompatible: "1.1.0", BuiltInPluginID.scholay: "0.1.0",
        BuiltInPluginID.polisher: "1.1.0", BuiltInPluginID.latex: "1.1.0",
        BuiltInPluginID.appleTranslation: "2.3.0", BuiltInPluginID.streamInput: "1.4.0",
        BuiltInPluginID.music: "0.2.3", BuiltInPluginID.morse: "0.1.0",
        BuiltInPluginID.statistics: "2.0.0", BuiltInPluginID.typingSpeed: "2.0.0",
        BuiltInPluginID.flyChordLearning: "2.0.0",
    ].merging(Dictionary(uniqueKeysWithValues: CapsuleModuleID.allCases.map { (BuiltInPluginID.capsule($0), "1.1.0") }), uniquingKeysWith: { first, _ in first })
     .merging(Dictionary(uniqueKeysWithValues: MailboxModuleID.allCases.map { ($0.pluginKey.rawID, "1.1.0") }), uniquingKeysWith: { first, _ in first })


    func fail(_ message: String) -> Bool {
        print("FAILED: plugin distribution \(message)")
        return false
    }

    func digest(_ data: Data) -> String {
        SHA256.hash(data: data)
            .map { String(format: "%02x", $0) }
            .joined()
    }

    func encodedManifest(id: String,
                         version: String,
                         summaryZH: String = "本地测试包",
                         summaryEN: String = "Local smoke package") throws -> Data {
        let manifest = PresetBufferPluginPackageManifest(
            schemaVersion: 1,
            id: id,
            version: version,
            nameZH: "Smoke Optional",
            nameEN: "Smoke Optional",
            summaryZH: summaryZH,
            summaryEN: summaryEN
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(manifest)
    }

    func catalogEntry(id: String,
                      version: String,
                      sha256: String,
                      summaryZH: String = "本地测试包",
                      summaryEN: String = "Local smoke package")
        -> PresetBufferPluginCatalogEntry {
        PresetBufferPluginCatalogEntry(
            id: id,
            nameZH: "Smoke Optional",
            nameEN: "Smoke Optional",
            version: version,
            summaryZH: summaryZH,
            summaryEN: summaryEN,
            defaultInstalled: false,
            defaultEnabled: false,
            downloadAssetName: "preset-plugin-\(id)-\(version).json",
            sha256: sha256
        )
    }

    func installSynchronously(
        _ id: String,
        using store: PresetBufferPluginInstallationStore
    ) throws -> Result<PresetBufferPluginCatalogEntry, Error> {
        let semaphore = DispatchSemaphore(value: 0)
        var completedResult: Result<PresetBufferPluginCatalogEntry, Error>?
        store.install(id: id) { result in
            completedResult = result
            semaphore.signal()
        }
        let deadline = Date().addingTimeInterval(3)
        while semaphore.wait(timeout: .now()) != .success {
            guard Date() < deadline else { throw PluginDistributionSmokeHarnessError.completionTimedOut }
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        guard let completedResult else { throw PluginDistributionSmokeHarnessError.completionTimedOut }
        return completedResult
    }

    func directorySnapshot(
        _ root: URL
    ) throws -> PluginDistributionSmokeDirectorySnapshot {
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: root.path, isDirectory: &isDirectory) else {
            return PluginDistributionSmokeDirectorySnapshot(
                rootExists: false,
                files: []
            )
        }
        guard isDirectory.boolValue else {
            return PluginDistributionSmokeDirectorySnapshot(
                rootExists: true,
                files: [PluginDistributionSmokeFile(
                    relativePath: ".",
                    kind: "file",
                    contents: try Data(contentsOf: root)
                )]
            )
        }

        let keys: [URLResourceKey] = [
            .isDirectoryKey,
            .isRegularFileKey,
            .isSymbolicLinkKey,
        ]
        guard let enumerator = fileManager.enumerator(
            at: root,
            includingPropertiesForKeys: keys,
            options: []
        ) else {
            return PluginDistributionSmokeDirectorySnapshot(
                rootExists: true,
                files: []
            )
        }
        var files: [PluginDistributionSmokeFile] = []
        for case let url as URL in enumerator {
            let values = try url.resourceValues(forKeys: Set(keys))
            let relativePath = String(url.path.dropFirst(root.path.count + 1))
            let kind: String
            let contents: Data?
            if values.isSymbolicLink == true {
                kind = "symlink"
                contents = Data(
                    try fileManager.destinationOfSymbolicLink(atPath: url.path).utf8
                )
            } else if values.isDirectory == true {
                kind = "directory"
                contents = nil
            } else if values.isRegularFile == true {
                kind = "file"
                contents = try Data(contentsOf: url)
            } else {
                kind = "other"
                contents = nil
            }
            files.append(PluginDistributionSmokeFile(
                relativePath: relativePath,
                kind: kind,
                contents: contents
            ))
        }
        return PluginDistributionSmokeDirectorySnapshot(
            rootExists: true,
            files: files.sorted { $0.relativePath < $1.relativePath }
        )
    }

    func defaultsSnapshot(_ defaults: UserDefaults,
                          suiteName: String) throws -> Data {
        let domain = defaults.persistentDomain(forName: suiteName) ?? [:]
        return try JSONSerialization.data(withJSONObject: domain,
                                          options: [.sortedKeys])
    }

    func makeRegistry(
        defaults: UserDefaults,
        sandbox: URL,
        store: PresetBufferPluginInstallationStore,
        fixtures: [PluginDistributionSmokeInternalPlugin]
    ) -> PluginRegistry {
        PluginRegistry(
            internalPlugins: fixtures,
            defaults: defaults,
            externalManager: ActionPluginManager(
                rootURL: sandbox.appendingPathComponent("external", isDirectory: true),
                stateURL: sandbox.appendingPathComponent("external-state.json")
            ),
            bufferPluginSelection: BufferPluginSelectionStore(defaults: defaults),
            presetInstallationStore: store
        )
    }

    let catalogIDs = PresetBufferPluginCatalog.entries.map(\.id)
    guard Set(catalogIDs).count == catalogIDs.count else {
        return fail("catalog contains duplicate IDs")
    }
    guard Set(catalogIDs) == expectedDefaultIDs.union(expectedOptionalIDs),
          Dictionary(uniqueKeysWithValues: PresetBufferPluginCatalog.entries.map {
              ($0.id, $0.version)
          }) == expectedVersions,
          Set(PresetBufferPluginCatalog.entries.filter(\.defaultInstalled).map(\.id))
            == expectedDefaultIDs,
          Set(PresetBufferPluginCatalog.entries.filter(\.defaultEnabled).map(\.id))
            == expectedEnabledIDs,
          Set(PresetBufferPluginCatalog.entries.filter { !$0.defaultInstalled }.map(\.id))
            == expectedOptionalIDs else {
        return fail("fresh catalog must match bundled and optional preset policy")
    }
    let registeredIDs = Set(BuiltInPlugins.makeAll().map {
        $0.descriptor.key.rawID
    })
    guard retiredProductIDs.isDisjoint(with: Set(catalogIDs)),
          retiredProductIDs.isDisjoint(with: registeredIDs),
          !catalogIDs.contains(legacyCapsuleBufferPluginID),
          !registeredIDs.contains(legacyCapsuleBufferPluginID),
          expectedDefaultIDs.isSubset(of: registeredIDs) else {
        return fail("non-Buffer products remain catalogued or registered")
    }

    do {
        // Fresh profile: only the exact bundled defaults are installed and
        // running. Disabling one must survive a full registry/store rebuild.
        let sandbox = fileManager.temporaryDirectory.appendingPathComponent(
            "rimebuffer-plugin-distribution-fresh-\(UUID().uuidString)",
            isDirectory: true
        )
        let suiteName = "RimeBuffer.PluginDistributionFresh.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            return fail("fresh defaults suite")
        }
        defaults.removePersistentDomain(forName: suiteName)
        defer {
            defaults.removePersistentDomain(forName: suiteName)
            try? fileManager.removeItem(at: sandbox)
        }

        let store = PresetBufferPluginInstallationStore(
            defaults: defaults,
            rootURL: sandbox.appendingPathComponent("preset", isDirectory: true)
        )
        let fixtures = PresetBufferPluginCatalog.entries.map(
            PluginDistributionSmokeInternalPlugin.init(entry:)
        )
        do {
            let registry = makeRegistry(
                defaults: defaults,
                sandbox: sandbox,
                store: store,
                fixtures: fixtures
            )
            let snapshot = registry.allPlugins()
            let optionalFixturesDidNotStart = fixtures
                .filter { expectedOptionalIDs.contains($0.descriptor.key.rawID) }
                .allSatisfy { $0.startCount == 0 }
            guard Set(snapshot.filter(\.isInstalled).map { $0.id.rawID })
                    == expectedDefaultIDs,
                  Set(snapshot.filter(\.isEnabled).map { $0.id.rawID })
                    == expectedEnabledIDs,
                  Set(fixtures.filter { $0.startCount == 1 }
                    .map { $0.descriptor.key.rawID }) == expectedEnabledIDs,
                  optionalFixturesDidNotStart else {
                return fail("fresh registry install/enable policy")
            }
            try registry.setEnabled(
                false,
                for: PluginKey(domain: .builtIn, rawID: BuiltInPluginID.appleTranslation)
            )
            guard !registry.isEnabled(
                PluginKey(domain: .builtIn, rawID: BuiltInPluginID.appleTranslation)
            ) else {
                return fail("explicit default-plugin disable")
            }
        }

        guard let restartedDefaults = UserDefaults(suiteName: suiteName) else {
            return fail("fresh restart defaults suite")
        }
        let restartedStore = PresetBufferPluginInstallationStore(
            defaults: restartedDefaults,
            rootURL: sandbox.appendingPathComponent("preset", isDirectory: true)
        )
        let restartedFixtures = PresetBufferPluginCatalog.entries.map(
            PluginDistributionSmokeInternalPlugin.init(entry:)
        )
        let restartedRegistry = makeRegistry(
            defaults: restartedDefaults,
            sandbox: sandbox,
            store: restartedStore,
            fixtures: restartedFixtures
        )
        let restartedEnabled = Set(
            restartedRegistry.allPlugins().filter(\.isEnabled).map { $0.id.rawID }
        )
        guard restartedEnabled == expectedEnabledIDs.subtracting([BuiltInPluginID.appleTranslation]),
              restartedFixtures.first(where: {
                $0.descriptor.key.rawID == BuiltInPluginID.appleTranslation
              })?.startCount == 0 else {
            return fail("restart reopened a user-disabled default plugin")
        }
    } catch {
        return fail("fresh/restart scenario: \(error)")
    }

    do {
        // A verified optional package installs as present-but-disabled and the
        // receipt remains valid after rebuilding the store with no downloader use.
        let sandbox = fileManager.temporaryDirectory.appendingPathComponent(
            "rimebuffer-plugin-distribution-install-\(UUID().uuidString)",
            isDirectory: true
        )
        let suiteName = "RimeBuffer.PluginDistributionInstall.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            return fail("install defaults suite")
        }
        defaults.removePersistentDomain(forName: suiteName)
        defer {
            defaults.removePersistentDomain(forName: suiteName)
            try? fileManager.removeItem(at: sandbox)
        }

        let id = "builtin.optional-smoke"
        let data = try encodedManifest(id: id, version: "1.0")
        let entry = catalogEntry(id: id, version: "1.0", sha256: digest(data))
        let downloader = PluginDistributionSmokeDownloader(result: .success(data))
        let root = sandbox.appendingPathComponent("preset", isDirectory: true)
        let store = PresetBufferPluginInstallationStore(
            defaults: defaults,
            rootURL: root,
            downloader: downloader,
            completionQueue: DispatchQueue(label: "plugin-distribution-smoke.success"),
            hostVersion: "0.4.2",
            catalogEntries: [entry]
        )
        store.bootstrap(hadLegacyEnablement: false, legacyDisabledIDs: [])
        guard !store.isInstalled(id: id), !store.isOptionalEnabled(id: id) else {
            return fail("optional fixture started installed or enabled")
        }
        let result = try installSynchronously(id, using: store)
        guard (try? result.get()) == entry,
              store.isInstalled(id: id),
              !store.isOptionalEnabled(id: id),
              downloader.requestedURLs.map(\.absoluteString) == [
                "https://github.com/scholay/rimes-plugins/releases/download/v1.1.0/"
                    + entry.downloadAssetName!
              ],
              try Data(contentsOf: root
                .appendingPathComponent(id, isDirectory: true)
                .appendingPathComponent("manifest.json")) == data else {
            return fail("verified optional install should remain disabled")
        }

        guard let restartedDefaults = UserDefaults(suiteName: suiteName) else {
            return fail("install restart defaults suite")
        }
        let restartDownloader = PluginDistributionSmokeDownloader(
            result: .failure(URLError(.notConnectedToInternet))
        )
        let restartedStore = PresetBufferPluginInstallationStore(
            defaults: restartedDefaults,
            rootURL: root,
            downloader: restartDownloader,
            completionQueue: DispatchQueue(label: "plugin-distribution-smoke.restart"),
            catalogEntries: [entry]
        )
        restartedStore.bootstrap(hadLegacyEnablement: true, legacyDisabledIDs: [])
        guard restartedStore.isInstalled(id: id),
              !restartedStore.isOptionalEnabled(id: id),
              restartDownloader.requestedURLs.isEmpty else {
            return fail("installed-but-disabled receipt did not survive restart")
        }

        let restartedFixture = PluginDistributionSmokeInternalPlugin(entry: entry)
        do {
            let registry = makeRegistry(
                defaults: restartedDefaults,
                sandbox: sandbox,
                store: restartedStore,
                fixtures: [restartedFixture]
            )
            let key = PluginKey(domain: .builtIn, rawID: id)
            guard registry.allPlugins() == [RegisteredPlugin(
                descriptor: restartedFixture.descriptor,
                isEnabled: false,
                isInstalled: true
            )],
                  restartedFixture.startCount == 0,
                  BufferPluginMenuCatalog.entries(from: registry.allPlugins())
                    .compactMap(\.key).isEmpty else {
                return fail("downloaded optional registry state")
            }
            try registry.setEnabled(true, for: key)
            guard registry.isEnabled(key),
                  restartedFixture.startCount == 1,
                  BufferPluginMenuCatalog.entries(from: registry.allPlugins())
                    .compactMap(\.key) == [key] else {
                return fail("downloaded optional activation/menu exposure")
            }
        }

        guard let enabledRestartDefaults = UserDefaults(suiteName: suiteName) else {
            return fail("enabled install restart defaults suite")
        }
        let enabledRestartStore = PresetBufferPluginInstallationStore(
            defaults: enabledRestartDefaults,
            rootURL: root,
            downloader: restartDownloader,
            completionQueue: DispatchQueue(label: "plugin-distribution-smoke.enabled-restart"),
            catalogEntries: [entry]
        )
        let enabledRestartFixture = PluginDistributionSmokeInternalPlugin(entry: entry)
        let enabledRestartRegistry = makeRegistry(
            defaults: enabledRestartDefaults,
            sandbox: sandbox,
            store: enabledRestartStore,
            fixtures: [enabledRestartFixture]
        )
        guard enabledRestartRegistry.allPlugins().first?.isInstalled == true,
              enabledRestartRegistry.allPlugins().first?.isEnabled == true,
              enabledRestartFixture.startCount == 1,
              restartDownloader.requestedURLs.isEmpty else {
            return fail("enabled optional state did not survive restart")
        }
    } catch {
        return fail("verified install scenario: \(error)")
    }

    do {
        // A grant is bound to the exact receipt SHA, not merely its ID/version.
        // Simulate process death after replacement receipt publication but
        // before the success notification can force the registry's legacy
        // disabled-ID set closed. Restart must remain off, and a subsequent
        // enable must start even though that legacy ID set was already clear.
        let sandbox = fileManager.temporaryDirectory.appendingPathComponent(
            "rimebuffer-plugin-distribution-grant-crash-\(UUID().uuidString)",
            isDirectory: true
        )
        let suiteName = "RimeBuffer.PluginDistributionGrantCrash.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            return fail("grant crash defaults suite")
        }
        defaults.removePersistentDomain(forName: suiteName)
        defer {
            defaults.removePersistentDomain(forName: suiteName)
            try? fileManager.removeItem(at: sandbox)
        }

        let id = "builtin.grant-smoke"
        let version = "1.0"
        let oldData = try encodedManifest(
            id: id,
            version: version,
            summaryZH: "旧收据",
            summaryEN: "Old receipt"
        )
        let oldEntry = catalogEntry(
            id: id,
            version: version,
            sha256: digest(oldData),
            summaryZH: "旧收据",
            summaryEN: "Old receipt"
        )
        let root = sandbox.appendingPathComponent("preset", isDirectory: true)
        let oldStore = PresetBufferPluginInstallationStore(
            defaults: defaults,
            rootURL: root,
            downloader: PluginDistributionSmokeDownloader(result: .success(oldData)),
            completionQueue: DispatchQueue(label: "plugin-distribution-smoke.old-grant"),
            hostVersion: "0.4.2",
            catalogEntries: [oldEntry]
        )
        oldStore.bootstrap(hadLegacyEnablement: false, legacyDisabledIDs: [])
        guard (try? installSynchronously(id, using: oldStore).get()) == oldEntry else {
            return fail("old receipt install for grant crash")
        }
        do {
            let oldFixture = PluginDistributionSmokeInternalPlugin(entry: oldEntry)
            let oldRegistry = makeRegistry(
                defaults: defaults,
                sandbox: sandbox,
                store: oldStore,
                fixtures: [oldFixture]
            )
            let key = PluginKey(domain: .builtIn, rawID: id)
            try oldRegistry.setEnabled(true, for: key)
            guard oldRegistry.isEnabled(key),
                  oldStore.isOptionalEnabled(id: id),
                  oldFixture.startCount == 1 else {
                return fail("old SHA grant was not enabled")
            }
        }

        let newData = try encodedManifest(
            id: id,
            version: version,
            summaryZH: "同版本新收据",
            summaryEN: "Same-version replacement receipt"
        )
        let newEntry = catalogEntry(
            id: id,
            version: version,
            sha256: digest(newData),
            summaryZH: "同版本新收据",
            summaryEN: "Same-version replacement receipt"
        )
        guard oldEntry.version == newEntry.version,
              oldEntry.sha256 != newEntry.sha256 else {
            return fail("SHA grant fixture did not retain the same version")
        }

        let suspendedCompletion = DispatchQueue(
            label: "plugin-distribution-smoke.suspended-notification"
        )
        suspendedCompletion.suspend()
        var completionQueueIsSuspended = true
        defer {
            if completionQueueIsSuspended { suspendedCompletion.resume() }
        }
        let replacementStore = PresetBufferPluginInstallationStore(
            defaults: defaults,
            rootURL: root,
            downloader: PluginDistributionSmokeDownloader(result: .success(newData)),
            completionQueue: suspendedCompletion,
            hostVersion: "0.4.2",
            catalogEntries: [newEntry]
        )
        replacementStore.bootstrap(hadLegacyEnablement: true,
                                   legacyDisabledIDs: [])
        let replacementCompleted = DispatchSemaphore(value: 0)
        replacementStore.install(id: id) { _ in replacementCompleted.signal() }

        let manifestURL = root
            .appendingPathComponent(id, isDirectory: true)
            .appendingPathComponent("manifest.json")
        let receiptDeadline = Date().addingTimeInterval(3)
        while (try? Data(contentsOf: manifestURL)) != newData,
              Date() < receiptDeadline {
            Thread.sleep(forTimeInterval: 0.01)
        }
        guard (try? Data(contentsOf: manifestURL)) == newData else {
            return fail("replacement receipt was not published before crash")
        }

        let restartedStore = PresetBufferPluginInstallationStore(
            defaults: UserDefaults(suiteName: suiteName)!,
            rootURL: root,
            hostVersion: "0.4.2",
            catalogEntries: [newEntry]
        )
        let restartedFixture = PluginDistributionSmokeInternalPlugin(entry: newEntry)
        let restartedRegistry = makeRegistry(
            defaults: UserDefaults(suiteName: suiteName)!,
            sandbox: sandbox,
            store: restartedStore,
            fixtures: [restartedFixture]
        )
        let key = PluginKey(domain: .builtIn, rawID: id)
        guard restartedRegistry.allPlugins().first?.isInstalled == true,
              !restartedRegistry.isEnabled(key),
              !restartedStore.isOptionalEnabled(id: id),
              restartedFixture.startCount == 0 else {
            return fail("same-version replacement inherited the old SHA grant")
        }

        try restartedRegistry.setEnabled(true, for: key)
        guard restartedRegistry.isEnabled(key),
              restartedStore.isOptionalEnabled(id: id),
              restartedFixture.startCount == 1 else {
            return fail("effective optional state transition did not start plugin")
        }

        suspendedCompletion.resume()
        completionQueueIsSuspended = false
        guard replacementCompleted.wait(timeout: .now() + 3) == .success else {
            return fail("suspended replacement completion did not drain")
        }
    } catch {
        return fail("SHA grant/crash scenario: \(error)")
    }

    do {
        // Invalidation before the final transaction rejects the download and
        // leaves no receipt behind.
        let sandbox = fileManager.temporaryDirectory.appendingPathComponent(
            "rimebuffer-plugin-distribution-stale-\(UUID().uuidString)",
            isDirectory: true
        )
        let suiteName = "RimeBuffer.PluginDistributionStale.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            return fail("stale defaults suite")
        }
        defaults.removePersistentDomain(forName: suiteName)
        defer {
            defaults.removePersistentDomain(forName: suiteName)
            try? fileManager.removeItem(at: sandbox)
        }
        let id = "builtin.stale-smoke"
        let data = try encodedManifest(id: id, version: "1.0")
        let entry = catalogEntry(id: id, version: "1.0", sha256: digest(data))
        let downloader = PluginDistributionSmokeDownloader(
            result: .success(data),
            holdCompletion: true
        )
        let root = sandbox.appendingPathComponent("preset", isDirectory: true)
        let store = PresetBufferPluginInstallationStore(
            defaults: defaults,
            rootURL: root,
            downloader: downloader,
            completionQueue: DispatchQueue(label: "plugin-distribution-smoke.stale"),
            hostVersion: "0.4.2",
            catalogEntries: [entry]
        )
        store.bootstrap(hadLegacyEnablement: false, legacyDisabledIDs: [])
        let completion = DispatchSemaphore(value: 0)
        var result: Result<PresetBufferPluginCatalogEntry, Error>?
        store.install(id: id) {
            result = $0
            completion.signal()
        }
        store.invalidatePendingInstalls()
        downloader.completeHeldDownload()
        guard completion.wait(timeout: .now() + 3) == .success else {
            return fail("pre-commit generation completion")
        }
        guard case let .failure(error)? = result,
              error as? PresetBufferPluginInstallationError == .staleDownload,
              !store.isInstalled(id: id),
              !fileManager.fileExists(atPath: root
                .appendingPathComponent(id, isDirectory: true).path) else {
            return fail("pre-commit generation invalidation")
        }
    } catch {
        return fail("stale generation scenario: \(error)")
    }

    do {
        // Once the final generation check begins, invalidation must block until
        // the same lock has published the receipt. It can never fit between the
        // check and rename.
        let sandbox = fileManager.temporaryDirectory.appendingPathComponent(
            "rimebuffer-plugin-distribution-linearized-\(UUID().uuidString)",
            isDirectory: true
        )
        let suiteName = "RimeBuffer.PluginDistributionLinearized.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            return fail("linearization defaults suite")
        }
        defaults.removePersistentDomain(forName: suiteName)
        let commitEntered = DispatchSemaphore(value: 0)
        let releaseCommit = DispatchSemaphore(value: 0)
        var commitIsHeld = true
        defer {
            if commitIsHeld { releaseCommit.signal() }
            defaults.removePersistentDomain(forName: suiteName)
            try? fileManager.removeItem(at: sandbox)
        }
        let id = "builtin.linearized-smoke"
        let data = try encodedManifest(id: id, version: "1.0")
        let entry = catalogEntry(id: id, version: "1.0", sha256: digest(data))
        let store = PresetBufferPluginInstallationStore(
            defaults: defaults,
            rootURL: sandbox.appendingPathComponent("preset", isDirectory: true),
            downloader: PluginDistributionSmokeDownloader(result: .success(data)),
            completionQueue: DispatchQueue(label: "plugin-distribution-smoke.linearized"),
            hostVersion: "0.4.2",
            willCommitInstallation: {
                commitEntered.signal()
                _ = releaseCommit.wait(timeout: .now() + 3)
            },
            catalogEntries: [entry]
        )
        store.bootstrap(hadLegacyEnablement: false, legacyDisabledIDs: [])
        let installCompleted = DispatchSemaphore(value: 0)
        var installResult: Result<PresetBufferPluginCatalogEntry, Error>?
        store.install(id: id) {
            installResult = $0
            installCompleted.signal()
        }
        guard commitEntered.wait(timeout: .now() + 3) == .success else {
            return fail("linearized commit seam was not reached")
        }
        let invalidationCompleted = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            store.invalidatePendingInstalls()
            invalidationCompleted.signal()
        }
        guard invalidationCompleted.wait(timeout: .now() + 0.1) == .timedOut else {
            return fail("generation invalidation entered the commit transaction")
        }
        releaseCommit.signal()
        commitIsHeld = false
        guard installCompleted.wait(timeout: .now() + 3) == .success,
              (try? installResult?.get()) == entry,
              invalidationCompleted.wait(timeout: .now() + 3) == .success,
              store.isInstalled(id: id) else {
            return fail("linearized generation/receipt commit")
        }
    } catch {
        return fail("linearized generation scenario: \(error)")
    }

    func rejectedInstallPreservesState(
        label: String,
        payload: Data,
        entry: PresetBufferPluginCatalogEntry,
        expectedError: PresetBufferPluginInstallationError
    ) -> Bool {
        let sandbox = fileManager.temporaryDirectory.appendingPathComponent(
            "rimebuffer-plugin-distribution-reject-\(UUID().uuidString)",
            isDirectory: true
        )
        let suiteName = "RimeBuffer.PluginDistributionReject.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            return fail("\(label) defaults suite")
        }
        defaults.removePersistentDomain(forName: suiteName)
        defer {
            defaults.removePersistentDomain(forName: suiteName)
            try? fileManager.removeItem(at: sandbox)
        }

        do {
            let root = sandbox.appendingPathComponent("preset", isDirectory: true)
            let downloader = PluginDistributionSmokeDownloader(result: .success(payload))
            let store = PresetBufferPluginInstallationStore(
                defaults: defaults,
                rootURL: root,
                downloader: downloader,
                completionQueue: DispatchQueue(label: "plugin-distribution-smoke.reject"),
                hostVersion: "0.4.2",
                catalogEntries: [entry]
            )
            store.bootstrap(hadLegacyEnablement: false, legacyDisabledIDs: [])
            let defaultsBefore = try defaultsSnapshot(defaults, suiteName: suiteName)
            let filesBefore = try directorySnapshot(root)
            let result = try installSynchronously(entry.id, using: store)
            guard case let .failure(error) = result,
                  error as? PresetBufferPluginInstallationError == expectedError,
                  !store.isInstalled(id: entry.id),
                  !store.isOptionalEnabled(id: entry.id),
                  try defaultsSnapshot(defaults, suiteName: suiteName) == defaultsBefore,
                  try directorySnapshot(root) == filesBefore,
                  downloader.requestedURLs.count == 1 else {
                return fail("\(label) changed state or returned the wrong error")
            }
            let restartedStore = PresetBufferPluginInstallationStore(
                defaults: UserDefaults(suiteName: suiteName)!,
                rootURL: root,
                catalogEntries: [entry]
            )
            guard !restartedStore.isInstalled(id: entry.id),
                  !restartedStore.isOptionalEnabled(id: entry.id) else {
                return fail("\(label) appeared installed after restart")
            }
            return true
        } catch {
            return fail("\(label) scenario: \(error)")
        }
    }

    do {
        let expectedID = "builtin.reject-smoke"
        let validData = try encodedManifest(id: expectedID, version: "1.0")
        guard rejectedInstallPreservesState(
            label: "hash mismatch",
            payload: validData,
            entry: catalogEntry(
                id: expectedID,
                version: "1.0",
                sha256: String(repeating: "0", count: 64)
            ),
            expectedError: .invalidDigest
        ) else { return false }

        let wrongIDData = try encodedManifest(id: "builtin.wrong-id", version: "1.0")
        guard rejectedInstallPreservesState(
            label: "manifest ID mismatch",
            payload: wrongIDData,
            entry: catalogEntry(
                id: expectedID,
                version: "1.0",
                sha256: digest(wrongIDData)
            ),
            expectedError: .invalidManifest
        ) else { return false }

        let wrongVersionData = try encodedManifest(id: expectedID, version: "9.9")
        guard rejectedInstallPreservesState(
            label: "manifest version mismatch",
            payload: wrongVersionData,
            entry: catalogEntry(
                id: expectedID,
                version: "1.0",
                sha256: digest(wrongVersionData)
            ),
            expectedError: .invalidManifest
        ) else { return false }
    } catch {
        return fail("invalid package fixtures: \(error)")
    }

    do {
        let sandbox = fileManager.temporaryDirectory.appendingPathComponent("RIMES-Bundled-Lifecycle-\(UUID())", isDirectory: true)
        let suite = "RIMES.BundledLifecycle.\(UUID())"
        guard let defaults = UserDefaults(suiteName: suite) else { return false }
        defer { defaults.removePersistentDomain(forName: suite); try? fileManager.removeItem(at: sandbox) }
        let entry = PresetBufferPluginCatalog.entry(id: BuiltInPluginID.appleTranslation)!
        let store = PresetBufferPluginInstallationStore(defaults: defaults, rootURL: sandbox.appendingPathComponent("packages"),
            completionQueue: DispatchQueue(label: "RIMES.BundledLifecycle"), hostVersion: "1.1.0", catalogEntries: [entry])
        let fixture = PluginDistributionSmokeInternalPlugin(entry: entry)
        let registry = makeRegistry(defaults: defaults, sandbox: sandbox, store: store, fixtures: [fixture])
        defaults.set("preserved", forKey: "translation.user-setting")
        try registry.uninstallInternalPlugin(fixture.descriptor.key)
        guard !store.isInstalled(id: entry.id), !registry.isEnabled(fixture.descriptor.key), fixture.stopCount == 1 else {
            return fail("bundled uninstall did not revoke runtime")
        }
        _ = try installSynchronously(entry.id, using: store).get()
        guard store.isInstalled(id: entry.id), !registry.isEnabled(fixture.descriptor.key),
              defaults.string(forKey: "translation.user-setting") == "preserved" else {
            return fail("bundled reinstall re-enabled itself or removed preferences")
        }
        try registry.setEnabled(true, for: fixture.descriptor.key)
        guard registry.isEnabled(fixture.descriptor.key), fixture.startCount == 2 else {
            return fail("bundled explicit re-enable failed")
        }
    } catch { return fail("bundled lifecycle: \(error)") }

    print("PASSED: plugin distribution smoke test")
    return true
}

/// Exercise the Info.plist path used by an installed app, not the command-line
/// executable's catalog-version fallback. No user preferences or stores change.
private func checkDevelopmentHostCompatibility() -> Bool {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(
        "rimes-host-version-smoke-\(UUID().uuidString)", isDirectory: true
    )
    defer { try? FileManager.default.removeItem(at: root) }
    do {
        let id = BuiltInPluginID.capsule(.temporary)
        guard let data = PresetBufferPluginInstallationStore.bundledPackageData(id: id) else {
            throw PluginPackageError.invalid
        }
        let cases: [(String, String, PluginPackageError?)] = [
            ("1.1.0", "1.1.0", nil),
            ("1.1.0-dev.1a832865da0f", "1.1.0", nil),
            ("1.1.0-dev.1a832865da0f.dirty", "1.1.0", nil),
            ("1.0.0-dev.1a832865da0f", "1.0.0", .hostTooOld),
            ("1.1.0-dev.invalid", "1.1.0-dev.invalid", .invalid),
            ("1.1.0-preview.1", "1.1.0-preview.1", .invalid),
        ]
        for (index, item) in cases.enumerated() {
            let (displayVersion, expectedVersion, expectedError) = item
            let app = root.appendingPathComponent("Probe\(index).app")
            let contents = app.appendingPathComponent("Contents")
            try FileManager.default.createDirectory(at: contents, withIntermediateDirectories: true)
            let plist = try PropertyListSerialization.data(fromPropertyList: [
                "CFBundleIdentifier": "com.scholay.rimes.host-version-probe.\(index)",
                "CFBundlePackageType": "APPL",
                "CFBundleShortVersionString": displayVersion,
            ], format: .xml, options: 0)
            try plist.write(to: contents.appendingPathComponent("Info.plist"))
            guard let bundle = Bundle(url: app),
                  PresetBufferPluginInstallationStore.currentHostVersion(bundle: bundle) == expectedVersion,
                  bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String == displayVersion else {
                throw PluginPackageError.invalid
            }
            var validationError: PluginPackageError?
            do {
                _ = try OfficialPluginPackage.validated(data, expectedID: id,
                    expectedVersion: "1.1.0", platform: "macos",
                    hostVersion: PresetBufferPluginInstallationStore.currentHostVersion(bundle: bundle))
            } catch let error as PluginPackageError { validationError = error }
            guard validationError == expectedError else {
                print("FAILED: installed host compatibility for \(displayVersion)")
                return false
            }
        }
        return true
    } catch {
        print("FAILED: installed host compatibility fixture: \(error)")
        return false
    }
}
