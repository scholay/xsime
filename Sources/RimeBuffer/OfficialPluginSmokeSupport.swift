import Foundation

/// The panel smoke uses production singletons but owns its enablement and
/// selection preferences. Normal app launches always use the existing domain.
enum OfficialPluginSmokePreferences {
    private static let suite = "RIMES.MusicPluginSmoke.\(UUID().uuidString)"
    static let shared: UserDefaults = ["buffer-music-panel-smoke", "mailbox-linux-fixtures", "mailbox-workspace-smoke"].contains(where: CommandLine.arguments.contains)
        ? UserDefaults(suiteName: suite)! : .standard
    static func clear() { shared.removePersistentDomain(forName: suite) }
}

private final class BundledMusicSmokeDownloader: ActionPluginManifestDownloading {
    func downloadManifest(from url: URL, completion: @escaping (Result<Data, Error>) -> Void) {
        guard let data = PresetBufferPluginInstallationStore.bundledPackageData(id: BuiltInPluginID.music) else {
            completion(.failure(PresetBufferPluginInstallationError.invalidManifest)); return
        }
        completion(.success(data))
    }
}

/// Install the exact hash-pinned optional package through the real installer;
/// only its network transport is replaced with the bundled test fixture.
func withInstalledMusicPluginSmoke(_ body: () -> Bool) -> Bool {
    defer { OfficialPluginSmokePreferences.clear() }
    let registry = PluginRegistry.shared
    let installer = PresetBufferPluginInstallationStore(
        defaults: OfficialPluginSmokePreferences.shared,
        downloader: BundledMusicSmokeDownloader()
    )
    var installed: Result<PresetBufferPluginCatalogEntry, Error>?
    installer.install(id: BuiltInPluginID.music) { installed = $0 }
    let deadline = Date().addingTimeInterval(5)
    while installed == nil && Date() < deadline {
        RunLoop.current.run(until: Date().addingTimeInterval(0.01))
    }
    do {
        guard let installed else { print("FAILED: music fixture install timed out"); return false }
        _ = try installed.get()
        try registry.setEnabled(true, for: BufferMusicInternalPlugin.key)
        return body()
    } catch {
        print("FAILED: music fixture install or enable: \(error)"); return false
    }
}
