import Cocoa
import Darwin
import Security
import CryptoKit

// MARK: - GitHub Release models

private struct GitHubRelease: Codable {
    let tagName: String
    let body: String?
    let assets: [GitHubAsset]

    enum CodingKeys: String, CodingKey {
        case tagName = "tag_name"
        case body, assets
    }
}

private struct GitHubAsset: Codable {
    let name: String
    let browserDownloadUrl: String

    enum CodingKeys: String, CodingKey {
        case name
        case browserDownloadUrl = "browser_download_url"
    }
}

// Platform metadata is intentionally heterogeneous: Android minimumOS is an
// integer and Linux describes distributions. Decode only the macOS member.
struct MacUpdateManifest: Decodable {
    struct Asset: Decodable {
        let url: String
        let sha256: String
        let size: Int64
        let minimumOS: String
        let architecture: String
    }
    struct Platforms: Decodable { let macos: Asset }
    let schemaVersion: Int
    let version: String
    let platforms: Platforms
}

private enum UpdateNetworkRules {
    // Repository identity survives renames. Keep aliases explicit, never accept
    // arbitrary redirects or look-alike repositories under the same owner.
    static let repositoryID = "1327328463"
    static let releaseAPI = "https://api.github.com/repositories/\(repositoryID)/releases/latest"
    static func repositoryAliases(owner: String, repository: String) -> [String] {
        guard owner == "scholay", ["rimes", "xsime"].contains(repository) else { return [] }
        return ["rimes", "xsime"]
    }
    static func stableVersionComponents(_ version: String) -> [Int]? {
        let components = version.split(separator: ".", omittingEmptySubsequences: false)
        guard components.count == 3 else { return nil }

        var result: [Int] = []
        result.reserveCapacity(3)
        for component in components {
            guard !component.isEmpty,
                  component.utf8.allSatisfy({ (48...57).contains($0) }),
                  (component == "0" || component.first != "0"),
                  let value = Int(component) else {
                return nil
            }
            result.append(value)
        }
        return result
    }

    static func stableVersion(fromTag tag: String) -> String? {
        guard tag.first == "v" else { return nil }
        let version = String(tag.dropFirst())
        return stableVersionComponents(version) == nil ? nil : version
    }

    static func expectedPackageName(version: String) -> String? {
        guard stableVersionComponents(version) != nil else { return nil }
        return "RIMES-\(version).pkg"
    }

    static func isSafeHTTPSURL(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == "https",
              let host = url.host?.lowercased(),
              !host.isEmpty,
              url.user == nil,
              url.password == nil,
              url.fragment == nil,
              url.port == nil || url.port == 443 else {
            return false
        }
        return host == "api.github.com"
            || host == "github.com"
            || host.hasSuffix(".githubusercontent.com")
    }

    static func isExactReleaseAPIURL(_ url: URL,
                                     owner: String,
                                     repository: String) -> Bool {
        let aliases = repositoryAliases(owner: owner, repository: repository)
        guard !aliases.isEmpty, isSafeHTTPSURL(url) else { return false }
        let expected = aliases.map { "https://api.github.com/repos/\(owner)/\($0)/releases/latest" }
        return (expected + [releaseAPI]).contains(url.absoluteString)
    }

    static func isExactReleaseAssetURL(_ url: URL,
                                       owner: String,
                                       repository: String,
                                       version: String) -> Bool {
        guard let packageName = expectedPackageName(version: version) else {
            return false
        }
        let expected = repositoryAliases(owner: owner, repository: repository).map {
            "https://github.com/\(owner)/\($0)/releases/download/v\(version)/\(packageName)"
        }
        return isSafeHTTPSURL(url) && expected.contains(url.absoluteString)
    }

    static func isAllowedFinalPackageURL(_ url: URL) -> Bool {
        guard isSafeHTTPSURL(url), let host = url.host?.lowercased() else {
            return false
        }
        return host == "github.com" || host.hasSuffix(".githubusercontent.com")
    }
}

/// Pure validation for the two version-bearing manifests inside the flat
/// product archive. The release asset name alone is not a version binding: an
/// older package signed by the same team could otherwise be renamed to match a
/// newer GitHub tag and pass both macOS trust checks.
enum UpdatePackageMetadataRules {
    static let packageIdentifier = RimesIdentity.bundleIdentifier
    static let componentPackageName = "component.pkg"

    static func validationFailure(distributionData: Data,
                                  packageInfoData: Data,
                                  expectedVersion: String) -> String? {
        guard UpdateNetworkRules.stableVersionComponents(expectedVersion) != nil else {
            return "期望的安装包版本不是严格的 X.Y.Z 格式"
        }

        do {
            let options: XMLNode.Options = [.nodeLoadExternalEntitiesNever]
            let distribution = try XMLDocument(data: distributionData,
                                               options: options)
            guard distribution.rootElement()?.name == "installer-gui-script" else {
                return "Distribution 根节点无效"
            }

            // productbuild normalizes the embedded archive reference to
            // "#component.pkg" and may add a second id-only pkg-ref carrying
            // bundle-version metadata. Require one and only one versioned
            // archive reference, and reject any other package identifier.
            let productReferences = try distribution.nodes(
                forXPath: "/installer-gui-script/pkg-ref"
            ).compactMap { $0 as? XMLElement }
            let versionedReferences = productReferences.filter {
                $0.attribute(forName: "version") != nil
            }
            guard !productReferences.isEmpty,
                  productReferences.allSatisfy({ reference in
                      reference.attribute(forName: "id")?.stringValue == packageIdentifier
                          && {
                              let target = reference.stringValue?
                                  .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
                              return target.isEmpty || target == "#\(componentPackageName)"
                          }()
                  }),
                  versionedReferences.count == 1,
                  let productReference = versionedReferences.first,
                  productReference.attribute(forName: "id")?.stringValue == packageIdentifier,
                  productReference.attribute(forName: "version")?.stringValue == expectedVersion,
                  productReference.stringValue?
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                    == "#\(componentPackageName)" else {
                return "Distribution 的产品包 ID、版本或组件引用不匹配"
            }

            let choiceReferences = try distribution.nodes(
                forXPath: "/installer-gui-script/choice/pkg-ref"
            ).compactMap { $0 as? XMLElement }
            guard choiceReferences.count == 1,
                  choiceReferences.first?.attribute(forName: "id")?.stringValue
                    == packageIdentifier else {
                return "Distribution 的安装选择未唯一绑定到预期产品包"
            }

            let packageInfo = try XMLDocument(data: packageInfoData,
                                              options: options)
            guard let root = packageInfo.rootElement(),
                  root.name == "pkg-info",
                  root.attribute(forName: "identifier")?.stringValue == packageIdentifier,
                  root.attribute(forName: "version")?.stringValue == expectedVersion else {
                return "组件 PackageInfo 的包 ID 或版本不匹配"
            }
            let payloadBundles = try packageInfo.nodes(
                forXPath: "/pkg-info/bundle"
            ).compactMap { $0 as? XMLElement }
            guard payloadBundles.count == 1,
                  let payloadBundle = payloadBundles.first,
                  payloadBundle.attribute(forName: "id")?.stringValue
                    == packageIdentifier,
                  payloadBundle.attribute(forName: "path")?.stringValue
                    == "./\(RimesIdentity.productName).app",
                  payloadBundle.attribute(
                    forName: "CFBundleShortVersionString"
                  )?.stringValue == expectedVersion else {
                return "组件 payload app 的 bundle ID、路径或版本不匹配"
            }
        } catch {
            return "安装包元数据不是可验证的 XML：\(error.localizedDescription)"
        }
        return nil
    }
}

/// URLSession follows GitHub's release-asset redirects, but never permits a
/// redirect to downgrade transport or leave GitHub-controlled HTTPS hosts.
private final class UpdateHTTPSRedirectDelegate: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession,
                    task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        guard let url = request.url, UpdateNetworkRules.isSafeHTTPSURL(url) else {
            completionHandler(nil)
            return
        }
        completionHandler(request)
    }
}

// MARK: - Update state

enum UpdateStatus: Equatable {
    case idle
    case checking
    case available(version: String, downloadUrl: String, notes: String)
    case noUpdate
    case downloading
    case readyToInstall(version: String, localPackage: URL)
    case installing
    case error(String)

    static func == (lhs: UpdateStatus, rhs: UpdateStatus) -> Bool {
        switch (lhs, rhs) {
        case (.idle, .idle), (.checking, .checking), (.noUpdate, .noUpdate),
             (.downloading, .downloading), (.installing, .installing):
            return true
        case let (.available(v1, _, _), .available(v2, _, _)): return v1 == v2
        case let (.readyToInstall(v1, u1), .readyToInstall(v2, u2)): return v1 == v2 && u1 == u2
        case let (.error(m1), .error(m2)): return m1 == m2
        default: return false
        }
    }
}

/// Pulls signed RIMES installer packages from GitHub Releases.
///
/// Design mirrors Toolbit's UpdateManager (silent check → silent download →
/// notify → user-confirmed install), but the application never edits an Input
/// Methods directory itself. A Developer ID-signed and notarized `.pkg` is
/// assessed by macOS, then opened with the system Installer.
/// It deliberately avoids actor isolation to match the rest of this plain-AppKit
/// codebase. Network and verification callbacks hop back via `onMain`, and
/// `status` is only ever mutated there.
///
/// Not thread-safe by construction — call it from the main thread only.
final class UpdateManager {
    static let shared = UpdateManager()

    // Source of truth for the repo; matches .github/workflows and README.
    private let githubOwner = "scholay"
    private let githubRepo = "xsime"

    static let changed = Notification.Name("XSIMEApplicationUpdateChanged")
    private(set) var progress: Double = 0
    private(set) var completionMessage: String?
    private(set) var installationRequested = false
    private var downloadObservation: NSKeyValueObservation?
    private var expectedDigest: String?
    private var expectedSize: Int64?
    private var authenticatedManifest: Data?
    private(set) var status: UpdateStatus = .idle
    /// Invoked on the main thread whenever `status` changes, so the status-bar
    /// menu can rebuild and reflect an available update.
    var onChange: (() -> Void)?

    var autoCheckEnabled: Bool {
        didSet { UserDefaults.standard.set(autoCheckEnabled, forKey: Self.autoCheckKey) }
    }
    private(set) var lastCheckDate: Date?

    private static let autoCheckKey = "updateAutoCheckEnabled"
    private static let maximumPackageBytes: Int64 = 512 * 1024 * 1024
    private static let maximumMetadataBytes: Int64 = 1024 * 1024
    private static let maximumToolOutputBytes = 256 * 1024
    private static let trustToolTimeout: TimeInterval = 20
    private static let metadataToolTimeout: TimeInterval = 10
    private static let forcedTerminationGrace: TimeInterval = 1
    private let checkInterval: TimeInterval = 3600  // 1 hour
    private var timer: Timer?
    private let sessionDelegate = UpdateHTTPSRedirectDelegate()
    private lazy var session: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.urlCache = nil
        configuration.httpCookieStorage = nil
        configuration.httpShouldSetCookies = false
        return URLSession(configuration: configuration,
                          delegate: sessionDelegate,
                          delegateQueue: nil)
    }()

    let currentVersion = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.0.0"

    /// The version the user can install right now, if any.
    var pendingVersion: String? {
        switch status {
        case let .available(v, _, _): return v
        case let .readyToInstall(v, _): return v
        default: return nil
        }
    }

    var isUpdateReady: Bool {
        if case .readyToInstall = status { return true }
        return false
    }

    private var isBusy: Bool {
        switch status {
        case .checking, .downloading, .installing:
            return true
        default:
            return false
        }
    }

    private init() {
        if UserDefaults.standard.object(forKey: Self.autoCheckKey) == nil {
            autoCheckEnabled = true                 // opt-out, not opt-in
        } else {
            autoCheckEnabled = UserDefaults.standard.bool(forKey: Self.autoCheckKey)
        }
        if let target = UserDefaults.standard.string(forKey: "applicationUpdateTarget"),
           UpdateNetworkRules.stableVersionComponents(currentVersion) != nil,
           currentVersion.compare(target, options: .numeric) != .orderedAscending {
            completionMessage = "更新完成，当前版本 \(currentVersion)"
            UserDefaults.standard.removeObject(forKey: "applicationUpdateTarget")
        }
    }

    // MARK: - Periodic silent check

    /// Kick off an immediate check and schedule hourly ones. Runs on the main
    /// runloop (started from `main.swift` before `NSApplication.run()`).
    func startPeriodicUpdateCheck() {
        guard autoCheckEnabled else { return }
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: checkInterval, repeats: true) { [weak self] _ in
            self?.silentCheckAndDownload()
        }
        silentCheckAndDownload()
    }

    func stopPeriodicUpdateCheck() {
        timer?.invalidate()
        timer = nil
    }

    /// Check, and if there's a newer release, download it quietly, leaving
    /// `status` at `.readyToInstall` so the menu can offer a one-click install.
    private func silentCheckAndDownload() {
        guard autoCheckEnabled, !isBusy else { return }
        if isUpdateReady { onChange?(); return }     // already staged
        checkForUpdates { [weak self] in
            guard let self else { return }
            if case let .available(version, url, _) = self.status {
                self.downloadPackage(from: url, version: version, completion: { _ in })
            }
        }
    }

    // MARK: - Manual check (from the status menu)

    func checkNowManually() {
        if isUpdateReady {
            promptAndInstall()
            return
        }
        if isBusy {
            showAlert(title: "更新进行中", message: "\(ProductBrand.name) 正在检查、下载或打开安装器。")
            return
        }
        checkForUpdates { [weak self] in
            guard let self else { return }
            switch self.status {
            case let .available(version, url, _):
                self.downloadPackage(from: url, version: version) { succeeded in
                    if succeeded {
                        self.promptAndInstall()
                    } else if case let .error(message) = self.status {
                        self.showUpdateFailure(title: "无法下载安全更新", message: message)
                    }
                }
            case .noUpdate:
                self.showAlert(title: "已是最新版本", message: "当前版本 \(self.currentVersion) 已经是最新。")
            case let .error(msg):
                self.showUpdateFailure(title: "检查更新失败", message: msg)
            default:
                break
            }
        }
    }

    // MARK: - Networking (completion-based; results delivered on the main thread)

    func checkFromSettings() {
        guard !isBusy else { return }
        checkForUpdates(completion: {})
    }

    func downloadFromSettings() {
        guard case let .available(version, url, _) = status else { return }
        downloadPackage(from: url, version: version, completion: { _ in })
    }

    private func checkForUpdates(completion: @escaping () -> Void) {
        setStatus(.checking)
        var request = URLRequest(url: URL(string: "https://github.com/scholay/xsime/releases/latest/download/update-manifest.json")!)
        request.timeoutInterval = 30
        request.setValue("XSIME-updater/1", forHTTPHeaderField: "User-Agent")
        session.dataTask(with: request) { [weak self] data, response, error in
            self?.onMain {
                guard let self else { return }
                defer { completion() }
                if let error { self.setStatus(.error("检查更新失败：\(error.localizedDescription)")); return }
                do {
                    guard let http = response as? HTTPURLResponse, http.statusCode == 200,
                          let final = http.url, UpdateNetworkRules.isSafeHTTPSURL(final),
                          let data, data.count <= 1024 * 1024 else { throw URLError(.badServerResponse) }
                    let trustedPayload = try UpdateManifestAuthentication.verifiedPayload(data)
                    let manifest = try JSONDecoder().decode(MacUpdateManifest.self, from: trustedPayload)
                    let asset = manifest.platforms.macos
                    guard manifest.schemaVersion == 1,
                          UpdateNetworkRules.stableVersionComponents(manifest.version) != nil,
                          let url = URL(string: asset.url),
                          UpdateNetworkRules.isExactReleaseAssetURL(url, owner: self.githubOwner, repository: self.githubRepo, version: manifest.version),
                          asset.sha256.range(of: "^[a-f0-9]{64}$", options: .regularExpression) != nil,
                          asset.size > 0, asset.size <= Self.maximumPackageBytes else { throw URLError(.cannotParseResponse) }
                    self.lastCheckDate = Date()
                    guard UpdateNetworkRules.stableVersionComponents(self.currentVersion) != nil else {
                        self.setStatus(.error("当前是开发构建，请从官方 Releases 安装正式版。")); return
                    }
                    let previous = UserDefaults.standard.string(forKey: "applicationUpdateHighestVerifiedVersion")
                    guard previous == nil || UpdateNetworkRules.stableVersionComponents(previous!) != nil else {
                        throw UpdateManifestAuthentication.Failure(reason: "本机更新版本记录损坏，已停止自动更新。请从官方发布页手动安装并核对版本。")
                    }
                    guard !self.isNewerVersion(self.currentVersion, than: manifest.version),
                          previous.map({ !self.isNewerVersion($0, than: manifest.version) }) ?? true else {
                        throw UpdateManifestAuthentication.Failure(reason: "更新信息低于已安装或此前验证过的版本，已拒绝旧清单。")
                    }
                    UserDefaults.standard.set(manifest.version, forKey: "applicationUpdateHighestVerifiedVersion")
                    guard UpdatePlatformCompatibility.supports(minimumOS: asset.minimumOS, architecture: asset.architecture) else {
                        self.setStatus(.error("此更新要求 macOS \(asset.minimumOS) 与 \(asset.architecture) 架构，请查看官方发布说明。")); return
                    }
                    guard self.isNewerVersion(manifest.version, than: self.currentVersion) else { self.setStatus(.noUpdate); return }
                    self.expectedDigest = asset.sha256; self.expectedSize = asset.size
                    self.authenticatedManifest = data
                    self.installationRequested = false
                    self.setStatus(.available(version: manifest.version, downloadUrl: asset.url, notes: ""))
                } catch { self.setStatus(.error("无法读取完整发布的更新信息，请稍后重试：\(error.localizedDescription)")) }
            }
        }.resume()
    }

    private func downloadPackage(from urlString: String, version: String,
                                 completion: @escaping (Bool) -> Void) {
        do {
            guard let manifest = authenticatedManifest else { throw UpdateManifestAuthentication.Failure(reason: "请先取得签名更新清单。") }
            _ = try UpdateManifestAuthentication.verifiedPayload(manifest)
        } catch { setStatus(.error(error.localizedDescription)); completion(false); return }
        guard let expectedPackageName = UpdateNetworkRules.expectedPackageName(version: version),
              let url = URL(string: urlString),
              UpdateNetworkRules.isExactReleaseAssetURL(url,
                                                         owner: githubOwner,
                                                         repository: githubRepo,
                                                         version: version) else {
            setStatus(.error("无效的下载地址")); completion(false); return
        }
        setStatus(.downloading)
        var request = URLRequest(url: url)
        request.setValue("application/octet-stream", forHTTPHeaderField: "Accept")
        request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        request.setValue("RIMES/\(currentVersion)", forHTTPHeaderField: "User-Agent")
        request.timeoutInterval = 120

        // Refuse an asset whose declared size is absent, empty, or unreasonable
        // before asking URLSession to put its body on disk. The regular-file
        // size is checked again after download, so this is not the sole guard.
        var preflight = request
        preflight.httpMethod = "HEAD"
        preflight.timeoutInterval = 30
        session.dataTask(with: preflight) { [weak self] _, response, error in
            guard let self else { return }
            if let error {
                self.onMain {
                    self.setStatus(.error("下载预检查失败: \(error.localizedDescription)"))
                    completion(false)
                }
                return
            }
            guard let http = response as? HTTPURLResponse,
                  (200..<300).contains(http.statusCode),
                  let finalURL = http.url,
                  UpdateNetworkRules.isAllowedFinalPackageURL(finalURL),
                  http.suggestedFilename == expectedPackageName,
                  http.expectedContentLength > 0,
                  http.expectedContentLength <= Self.maximumPackageBytes else {
                self.onMain {
                    self.setStatus(.error("更新包必须是来自 GitHub 的 HTTPS/2xx 响应，文件名必须精确匹配，且大小必须在 512 MiB 以内。"))
                    completion(false)
                }
                return
            }
            self.performPackageDownload(request,
                                        expectedPackageName: expectedPackageName,
                                        version: version,
                                        completion: completion)
        }.resume()
    }

    private func performPackageDownload(_ request: URLRequest,
                                        expectedPackageName: String,
                                        version: String,
                                        completion: @escaping (Bool) -> Void) {
        let task = session.downloadTask(with: request) { [weak self] tempURL, response, error in
            // IMPORTANT: URLSession deletes `tempURL` as soon as this handler
            // returns, so move it synchronously here (on the background thread)
            // BEFORE hopping to main for the status update.
            guard let self else { return }
            if let error {
                self.onMain { self.setStatus(.error("下载失败: \(error.localizedDescription)")); completion(false) }
                return
            }
            guard let http = response as? HTTPURLResponse,
                  (200..<300).contains(http.statusCode),
                  let finalURL = http.url,
                  UpdateNetworkRules.isAllowedFinalPackageURL(finalURL),
                  http.suggestedFilename == expectedPackageName,
                  http.expectedContentLength <= Self.maximumPackageBytes,
                  let tempURL else {
                let statusCode = (response as? HTTPURLResponse)?.statusCode
                let detail = statusCode.map { " (HTTP \($0))" } ?? ""
                self.onMain {
                    self.setStatus(.error("下载响应未通过 HTTPS/GitHub/2xx/精确文件名检查\(detail)。"))
                    completion(false)
                }
                return
            }

            let fileManager = FileManager.default
            let stagingDirectory = fileManager.temporaryDirectory
                .appendingPathComponent("rimes-update-\(UUID().uuidString)", isDirectory: true)
            let destination = stagingDirectory.appendingPathComponent(expectedPackageName,
                                                                       isDirectory: false)
            do {
                try fileManager.createDirectory(at: stagingDirectory,
                                                withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])
                try fileManager.moveItem(at: tempURL, to: destination)
                try fileManager.setAttributes([.posixPermissions: 0o600],
                                              ofItemAtPath: destination.path)
            } catch {
                try? fileManager.removeItem(at: stagingDirectory)
                self.onMain { self.setStatus(.error("保存文件失败: \(error.localizedDescription)")); completion(false) }
                return
            }

            DispatchQueue.global(qos: .userInitiated).async {
                guard let expectedDigest = self.expectedDigest, let expectedSize = self.expectedSize,
                      let data = try? Data(contentsOf: destination, options: .mappedIfSafe), Int64(data.count) == expectedSize,
                      SHA256.hash(data: data).map({ String(format: "%02x", $0) }).joined() == expectedDigest else {
                    try? FileManager.default.removeItem(at: stagingDirectory)
                    self.onMain { self.setStatus(.error("下载包的大小或 SHA-256 不匹配，请重新下载。")); completion(false) }
                    return
                }
                if let failure = self.packageVerificationFailure(at: destination,
                                                                 version: version) {
                    try? FileManager.default.removeItem(at: stagingDirectory)
                    self.onMain {
                        IMELog.write("update: downloaded package rejected for v\(version)")
                        self.setStatus(.error(failure))
                        completion(false)
                    }
                    return
                }
                self.onMain {
                    IMELog.write("update: verified signed package v\(version) -> \(destination.path)")
                    self.setStatus(.readyToInstall(version: version,
                                                   localPackage: destination))
                    completion(true)
                }
            }
        }
        progress = 0
        downloadObservation = task.progress.observe(\.fractionCompleted, options: [.new]) { [weak self] value, _ in
            self?.onMain {
                self?.progress = value.fractionCompleted
                NotificationCenter.default.post(name: Self.changed, object: self)
            }
        }
        task.resume()
    }

    // MARK: - Install (delegated to macOS Installer)

    /// Confirm with the user, re-verify the staged package, then hand it to the
    /// system Installer. Only valid when an update is staged. Main thread only.
    func promptAndInstall() {
        guard case let .readyToInstall(version, package) = status else { return }
        let alert = NSAlert()
        alert.messageText = "更新到 \(ProductIdentity.displayName) v\(version)？"
        alert.informativeText = "\(ProductBrand.name) 将再次验证安装包，然后打开 macOS 系统安装器。只有系统安装器会在您授权后更新输入法。"
        alert.addButton(withTitle: "打开系统安装器")
        alert.addButton(withTitle: "稍后")
        alert.window.appearance = RimeUI.appKitAppearance
        guard StandaloneWindowFocusCoordinator.shared
            .runModalAlertIfRIMESActive(alert) == .alertFirstButtonReturn else {
            return
        }
        installStagedUpdate(version: version, package: package)
    }

    private func installStagedUpdate(version: String, package: URL) {
        setStatus(.installing)
        let manifest = authenticatedManifest
        let digest = expectedDigest
        let size = expectedSize
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            do {
                guard let manifest, let digest, let size else { throw UpdateManifestAuthentication.Failure(reason: "请重新检查并下载签名更新。") }
                _ = try UpdateManifestAuthentication.verifiedPayload(manifest)
                let bytes = try Data(contentsOf: package, options: .mappedIfSafe)
                guard Int64(bytes.count) == size,
                      SHA256.hash(data: bytes).map({ String(format: "%02x", $0) }).joined() == digest else {
                    throw UpdateManifestAuthentication.Failure(reason: "待安装文件已变化，请重新下载。")
                }
            } catch {
                self.onMain { self.setStatus(.error(error.localizedDescription)) }
                return
            }
            if let failure = self.packageVerificationFailure(at: package,
                                                             version: version) {
                try? FileManager.default.removeItem(at: package.deletingLastPathComponent())
                self.onMain {
                    self.setStatus(.error(failure))
                    self.showUpdateFailure(title: "安装包安全验证失败", message: failure)
                }
                return
            }

            self.onMain {
                UserDefaults.standard.set(version, forKey: "applicationUpdateTarget")
                guard NSWorkspace.shared.open(package) else {
                    let message = "macOS 无法打开已验证的安装包。请从官方 Releases 页重新下载最新签名版本。"
                    self.setStatus(.error(message))
                    self.showUpdateFailure(title: "无法打开系统安装器", message: message)
                    return
                }
                self.installationRequested = true
                IMELog.write("update: handed verified package v\(version) to macOS Installer")
                // NSWorkspace reports only that Installer was opened, not
                // whether its wizard eventually completed. Keep the verified
                // package retryable if the user cancels or closes Installer.
                self.setStatus(.readyToInstall(version: version,
                                               localPackage: package))
            }
        }
    }

    // MARK: - Helpers

    func openReleasesPage() {
        if let url = URL(string: "https://github.com/\(githubOwner)/\(githubRepo)/releases") {
            NSWorkspace.shared.open(url)
        }
    }

    private func setStatus(_ new: UpdateStatus) {
        guard new != status else { return }
        status = new
        onChange?()
        NotificationCenter.default.post(name: Self.changed, object: self)
    }

    private func onMain(_ block: @escaping () -> Void) {
        if Thread.isMainThread { block() } else { DispatchQueue.main.async(execute: block) }
    }

    private struct ToolResult {
        let launched: Bool
        let terminationStatus: Int32?
        let timedOut: Bool
        let output: String
    }

    /// Returns a user-facing failure, or nil only when both macOS trust checks
    /// accept the exact regular file we staged. Both tools are always attempted
    /// so a partial/misconfigured local policy can never become a bypass.
    private func packageVerificationFailure(at package: URL, version: String) -> String? {
        guard package.isFileURL,
              let expectedName = UpdateNetworkRules.expectedPackageName(version: version),
              package.lastPathComponent == expectedName else {
            return "安装包版本或文件名与 Release 不匹配，已拒绝打开。请从官方 Releases 页重新下载。"
        }

        do {
            let values = try package.resourceValues(forKeys: [
                .isRegularFileKey,
                .isSymbolicLinkKey,
                .fileSizeKey,
            ])
            guard values.isRegularFile == true,
                  values.isSymbolicLink != true,
                  let fileSize = values.fileSize,
                  fileSize > 0,
                  Int64(fileSize) <= Self.maximumPackageBytes else {
                return "下载的安装包不是 512 MiB 以内的有效普通文件，已拒绝打开。请从官方 Releases 页重新下载。"
            }
        } catch {
            return "无法检查下载的安装包：\(error.localizedDescription)。请从官方 Releases 页重新下载。"
        }

        let expectedTeamIdentifier = currentSigningTeamIdentifier()
        let signature = runTool(
            "/usr/sbin/pkgutil",
            ["--check-signature", package.path],
            timeout: Self.trustToolTimeout
        )
        let assessment = runTool(
            "/usr/sbin/spctl",
            ["--assess", "--type", "install", package.path],
            timeout: Self.trustToolTimeout
        )
        IMELog.write("update: trust check pkgutil=\(toolSummary(signature)) spctl=\(toolSummary(assessment))")

        guard !signature.timedOut, !assessment.timedOut else {
            return "macOS 安装包验证工具 pkgutil 或 spctl 超时。\(ProductBrand.name) 已按安全默认拒绝打开安装包；请从官方 Releases 页重新下载或获取帮助。"
        }
        guard signature.launched, assessment.launched,
              signature.terminationStatus != nil,
              assessment.terminationStatus != nil else {
            return "无法调用 macOS 系统验证工具 pkgutil 和 spctl，\(ProductBrand.name) 已按安全默认拒绝打开安装包。请前往官方 Releases 页获取帮助。"
        }
        guard signature.terminationStatus == 0, assessment.terminationStatus == 0 else {
            return "安装包未同时通过 macOS 签名检查和安全策略评估。它可能是本地开发包、旧版未签名 Release，或已损坏。\(ProductBrand.name) 不会降级或绕过检查；请从官方 Releases 页下载最新签名安装包。"
        }
        guard let expectedTeamIdentifier else {
            return "当前 \(ProductBrand.name) 不是可验证的正式签名构建（例如本地开发或 ad-hoc 构建），无法将更新包绑定到同一签名团队。\(ProductBrand.name) 已按安全默认拒绝打开；请从官方 Releases 页手动安装最新签名版本。"
        }
        guard packageInstallerTeamMatches(signature.output,
                                          expectedTeamIdentifier: expectedTeamIdentifier) else {
            return "安装包的 Developer ID Installer 签名团队与当前 \(ProductBrand.name) 不一致，已拒绝打开。\(ProductBrand.name) 不会降级或绕过同团队校验；请前往官方 Releases 页核实。"
        }
        if let metadataFailure = packageMetadataFailure(at: package,
                                                        expectedVersion: version) {
            IMELog.write("update: package metadata rejected: \(metadataFailure)")
            return "安装包内的产品/组件 ID 或版本与 Release v\(version) 不一致，已拒绝打开（\(metadataFailure)）。请从官方 Releases 页重新下载。"
        }
        return nil
    }

    /// Pin updates to the 10-character Apple signing team of this running app.
    /// Dynamic validity is checked before reading signing information so an
    /// ad-hoc, unsigned, or modified local build cannot provide a trust anchor.
    private func currentSigningTeamIdentifier() -> String? {
        var runningCode: SecCode?
        let selfStatus = SecCodeCopySelf(SecCSFlags(), &runningCode)
        guard selfStatus == errSecSuccess, let runningCode else {
            IMELog.write("update: SecCodeCopySelf failed status=\(selfStatus)")
            return nil
        }

        let validityStatus = SecCodeCheckValidity(runningCode, SecCSFlags(), nil)
        guard validityStatus == errSecSuccess else {
            IMELog.write("update: current code signature invalid status=\(validityStatus)")
            return nil
        }

        var staticCode: SecStaticCode?
        let staticStatus = SecCodeCopyStaticCode(runningCode, SecCSFlags(), &staticCode)
        guard staticStatus == errSecSuccess, let staticCode else {
            IMELog.write("update: SecCodeCopyStaticCode failed status=\(staticStatus)")
            return nil
        }

        var signingInformation: CFDictionary?
        let informationStatus = SecCodeCopySigningInformation(
            staticCode,
            SecCSFlags(rawValue: kSecCSSigningInformation),
            &signingInformation
        )
        guard informationStatus == errSecSuccess, let signingInformation else {
            IMELog.write("update: signing information unavailable status=\(informationStatus)")
            return nil
        }

        let dictionary = signingInformation as NSDictionary
        guard let teamIdentifier = dictionary[kSecCodeInfoTeamIdentifier] as? String,
              isValidTeamIdentifier(teamIdentifier) else {
            IMELog.write("update: current code has no valid 10-character team identifier")
            return nil
        }
        return teamIdentifier
    }

    private func isValidTeamIdentifier(_ value: String) -> Bool {
        let bytes = value.utf8
        return bytes.count == 10 && bytes.allSatisfy {
            (48...57).contains($0) || (65...90).contains($0)
        }
    }

    private func packageInstallerTeamMatches(_ pkgutilOutput: String,
                                             expectedTeamIdentifier: String) -> Bool {
        let expectedSuffix = "(\(expectedTeamIdentifier))"
        return pkgutilOutput.split(whereSeparator: \.isNewline).contains { rawLine in
            let line = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
            return line.contains("Developer ID Installer:") && line.hasSuffix(expectedSuffix)
        }
    }

    /// Extracts only the two small version-bearing manifests. `xar` never
    /// expands the component Payload, so validation cannot write into an
    /// install location or execute package scripts.
    private func packageMetadataFailure(at package: URL,
                                        expectedVersion: String) -> String? {
        let fileManager = FileManager.default
        let extractionRoot = fileManager.temporaryDirectory
            .appendingPathComponent("rimes-package-metadata-\(UUID().uuidString)",
                                    isDirectory: true)
        let outerDirectory = extractionRoot.appendingPathComponent("product",
                                                                    isDirectory: true)
        // A product archive represents component.pkg as a directory in its
        // XAR table of contents. Pre-create it and use --keep-existing so a
        // hostile archive cannot substitute a symlink as PackageInfo's parent.
        let componentDirectory = outerDirectory.appendingPathComponent(
            UpdatePackageMetadataRules.componentPackageName,
            isDirectory: true
        )
        do {
            try fileManager.createDirectory(at: extractionRoot,
                                            withIntermediateDirectories: false,
                                            attributes: [.posixPermissions: 0o700])
            try fileManager.createDirectory(at: outerDirectory,
                                            withIntermediateDirectories: false,
                                            attributes: [.posixPermissions: 0o700])
            try fileManager.createDirectory(at: componentDirectory,
                                            withIntermediateDirectories: false,
                                            attributes: [.posixPermissions: 0o700])
        } catch {
            try? fileManager.removeItem(at: extractionRoot)
            return "无法创建受限的元数据检查目录"
        }
        defer { try? fileManager.removeItem(at: extractionRoot) }

        let productExtraction = runTool(
            "/usr/bin/xar",
            [
                "-x", "-k", "-f", package.path,
                "-C", outerDirectory.path,
                "Distribution",
                "\(UpdatePackageMetadataRules.componentPackageName)/PackageInfo",
            ],
            timeout: Self.metadataToolTimeout
        )
        guard !productExtraction.timedOut else {
            return "只读展开产品元数据超时"
        }
        guard productExtraction.launched,
              productExtraction.terminationStatus == 0 else {
            return "无法只读展开产品元数据"
        }

        do {
            let productEntries = try fileManager.contentsOfDirectory(
                atPath: outerDirectory.path
            )
            guard Set(productEntries) == Set([
                "Distribution", UpdatePackageMetadataRules.componentPackageName,
            ]) else {
                return "产品包元数据成员不唯一"
            }
        } catch {
            return "无法枚举产品包元数据"
        }

        let distributionURL = outerDirectory.appendingPathComponent("Distribution",
                                                                     isDirectory: false)
        guard isBoundedRegularFile(distributionURL,
                                   maximumBytes: Self.maximumMetadataBytes),
              isRegularDirectory(componentDirectory) else {
            return "产品包元数据成员类型无效"
        }

        do {
            guard try fileManager.contentsOfDirectory(atPath: componentDirectory.path)
                == ["PackageInfo"] else {
                return "组件包元数据成员不唯一"
            }
        } catch {
            return "无法枚举组件包元数据"
        }

        let packageInfoURL = componentDirectory.appendingPathComponent("PackageInfo",
                                                                        isDirectory: false)
        guard isBoundedRegularFile(packageInfoURL,
                                   maximumBytes: Self.maximumMetadataBytes) else {
            return "组件 PackageInfo 不是受限大小的普通文件"
        }

        do {
            let distributionData = try Data(contentsOf: distributionURL,
                                            options: [.mappedIfSafe])
            let packageInfoData = try Data(contentsOf: packageInfoURL,
                                           options: [.mappedIfSafe])
            return UpdatePackageMetadataRules.validationFailure(
                distributionData: distributionData,
                packageInfoData: packageInfoData,
                expectedVersion: expectedVersion
            )
        } catch {
            return "无法读取安装包元数据：\(error.localizedDescription)"
        }
    }

    private func isBoundedRegularFile(_ url: URL, maximumBytes: Int64) -> Bool {
        do {
            let values = try url.resourceValues(forKeys: [
                .isRegularFileKey,
                .isSymbolicLinkKey,
                .fileSizeKey,
            ])
            guard values.isRegularFile == true,
                  values.isSymbolicLink != true,
                  let fileSize = values.fileSize else {
                return false
            }
            return fileSize > 0 && Int64(fileSize) <= maximumBytes
        } catch {
            return false
        }
    }

    private func isRegularDirectory(_ url: URL) -> Bool {
        do {
            let values = try url.resourceValues(forKeys: [
                .isDirectoryKey,
                .isSymbolicLinkKey,
            ])
            return values.isDirectory == true && values.isSymbolicLink != true
        } catch {
            return false
        }
    }

    /// Launches a child with output redirected to a bounded temporary file.
    /// Pipes are deliberately avoided: a child that fills a pipe before exit
    /// could deadlock the timeout path. A timed-out child is terminated and,
    /// if necessary, killed; this never targets RIMES, Installer, or an input
    /// method process.
    private func runTool(_ path: String,
                         _ arguments: [String],
                         timeout: TimeInterval) -> ToolResult {
        guard FileManager.default.isExecutableFile(atPath: path) else {
            return ToolResult(launched: false,
                              terminationStatus: nil,
                              timedOut: false,
                              output: "system tool is unavailable")
        }

        let fileManager = FileManager.default
        let outputURL = fileManager.temporaryDirectory
            .appendingPathComponent("rimes-tool-output-\(UUID().uuidString)",
                                    isDirectory: false)
        guard fileManager.createFile(
            atPath: outputURL.path,
            contents: nil,
            attributes: [.posixPermissions: 0o600]
        ) else {
            return ToolResult(launched: false,
                              terminationStatus: nil,
                              timedOut: false,
                              output: "cannot create tool output file")
        }
        defer { try? fileManager.removeItem(at: outputURL) }

        let outputHandle: FileHandle
        do {
            outputHandle = try FileHandle(forWritingTo: outputURL)
        } catch {
            return ToolResult(launched: false,
                              terminationStatus: nil,
                              timedOut: false,
                              output: error.localizedDescription)
        }
        var outputHandleIsOpen = true
        defer {
            if outputHandleIsOpen { try? outputHandle.close() }
        }

        let process = Process()
        let exitSignal = DispatchSemaphore(value: 0)
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = arguments
        process.standardOutput = outputHandle
        process.standardError = outputHandle
        process.terminationHandler = { _ in exitSignal.signal() }
        do {
            try process.run()

            let timedOut = exitSignal.wait(timeout: .now() + timeout) == .timedOut
            if timedOut {
                if process.isRunning { process.terminate() }
                if exitSignal.wait(timeout: .now() + Self.forcedTerminationGrace) == .timedOut,
                   process.isRunning {
                    _ = Darwin.kill(process.processIdentifier, SIGKILL)
                    _ = exitSignal.wait(timeout: .now() + Self.forcedTerminationGrace)
                }
            }

            try? outputHandle.synchronize()
            try? outputHandle.close()
            outputHandleIsOpen = false
            let output = readToolOutput(at: outputURL)
            let status = process.isRunning || timedOut ? nil : process.terminationStatus
            return ToolResult(launched: true,
                              terminationStatus: status,
                              timedOut: timedOut,
                              output: output)
        } catch {
            try? outputHandle.close()
            outputHandleIsOpen = false
            return ToolResult(launched: false,
                              terminationStatus: nil,
                              timedOut: false,
                              output: error.localizedDescription)
        }
    }

    private func readToolOutput(at url: URL) -> String {
        do {
            let handle = try FileHandle(forReadingFrom: url)
            defer { try? handle.close() }
            let data = try handle.read(upToCount: Self.maximumToolOutputBytes) ?? Data()
            let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
            let fullSize = (attributes[.size] as? NSNumber)?.intValue ?? data.count
            let suffix = fullSize > data.count ? "\n[output truncated]" : ""
            return (String(decoding: data, as: UTF8.self) + suffix)
                .trimmingCharacters(in: .whitespacesAndNewlines)
        } catch {
            return "unable to read tool output: \(error.localizedDescription)"
        }
    }

    private func toolSummary(_ result: ToolResult) -> String {
        let status: String
        if result.timedOut {
            status = "timeout"
        } else {
            status = result.terminationStatus.map(String.init) ?? "not-launched"
        }
        let oneLine = result.output
            .replacingOccurrences(of: "\n", with: " ")
            .replacingOccurrences(of: "\r", with: " ")
        return "\(status) \(oneLine.prefix(300))"
    }

    private func showAlert(title: String, message: String, warning: Bool = false) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = warning ? .warning : .informational
        alert.addButton(withTitle: "好的")
        alert.window.appearance = RimeUI.appKitAppearance
        _ = StandaloneWindowFocusCoordinator.shared
            .runModalAlertIfRIMESActive(alert)
    }

    private func showUpdateFailure(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: "打开官方 Releases")
        alert.addButton(withTitle: "关闭")
        alert.window.appearance = RimeUI.appKitAppearance
        if StandaloneWindowFocusCoordinator.shared
            .runModalAlertIfRIMESActive(alert) == .alertFirstButtonReturn {
            openReleasesPage()
        }
    }

    /// Numeric component-wise comparison for strict stable X.Y.Z versions.
    private func isNewerVersion(_ candidate: String, than current: String) -> Bool {
        guard let a = UpdateNetworkRules.stableVersionComponents(candidate),
              let b = UpdateNetworkRules.stableVersionComponents(current) else {
            return false
        }
        for i in 0..<3 {
            let x = a[i]
            let y = b[i]
            if x != y { return x > y }
        }
        return false
    }
}

/// Pure updater-policy smoke: no network, package launch, signature lookup, or
/// filesystem mutation. This specifically locks the anti-replay contract that
/// both product and component metadata must match the requested Release.
func runUpdatePackageMetadataSmokeTest() -> Bool {
    print("== RIMES update package metadata smoke test ==")
    guard runUpdateAuthenticationSmokeTest() else { return false }
    for (language, expected) in [("zh-Hans", "犀术IME"), ("zh-Hant", "犀術IME"),
                                  ("zh-TW", "犀術IME"), ("zh-HK", "犀術IME"),
                                  ("en", "XSIME"), ("ja", "XSIME")] {
        guard ProductBrand.name(language: language) == expected else {
            print("FAILED: localized product name"); return false
        }
    }
    let acceptedAPIs = [UpdateNetworkRules.releaseAPI,
        "https://api.github.com/repos/scholay/rimes/releases/latest",
        "https://api.github.com/repos/scholay/xsime/releases/latest"]
    let rejectedAPIs = [
        "https://api.github.com/repositories/1327328464/releases/latest",
        "https://api.github.com/repos/attacker/xsime/releases/latest",
        "https://api.github.com/repos/scholay/xsime-fork/releases/latest",
        "http://api.github.com/repos/scholay/xsime/releases/latest",
        "https://api.github.com/repos/scholay/xsime/releases/latest?override=true",
        "https://api.github.com/repos/scholay/xsime/releases/latest#fragment"]
    for raw in acceptedAPIs + rejectedAPIs {
        guard UpdateNetworkRules.isExactReleaseAPIURL(URL(string: raw)!, owner: "scholay", repository: "xsime") == acceptedAPIs.contains(raw) else {
            print("FAILED: update repository API allowlist"); return false
        }
    }
    for repo in ["rimes", "xsime", "xsime-fork"] {
        let url = URL(string: "https://github.com/scholay/\(repo)/releases/download/v1.2.3/RIMES-1.2.3.pkg")!
        guard UpdateNetworkRules.isExactReleaseAssetURL(url, owner: "scholay", repository: "xsime", version: "1.2.3") == (repo != "xsime-fork"),
              !UpdateNetworkRules.isExactReleaseAssetURL(url, owner: "attacker", repository: "xsime", version: "1.2.3"),
              !UpdateNetworkRules.isExactReleaseAssetURL(url, owner: "scholay", repository: "xsime", version: "1.2.4") else {
            print("FAILED: update repository asset allowlist"); return false
        }
    }
    let distribution = Data("""
    <?xml version="1.0"?>
    <installer-gui-script>
      <choice id="default">
        <pkg-ref id="\(RimesIdentity.bundleIdentifier)"/>
      </choice>
      <pkg-ref id="\(RimesIdentity.bundleIdentifier)" version="1.2.3">#component.pkg</pkg-ref>
      <pkg-ref id="\(RimesIdentity.bundleIdentifier)"><bundle-version/></pkg-ref>
    </installer-gui-script>
    """.utf8)
    let packageInfo = Data("""
    <?xml version="1.0"?>
    <pkg-info identifier="\(RimesIdentity.bundleIdentifier)" version="1.2.3">
      <bundle path="./\(RimesIdentity.productName).app" id="\(RimesIdentity.bundleIdentifier)"
              CFBundleShortVersionString="1.2.3" CFBundleVersion="42"/>
    </pkg-info>
    """.utf8)
    let wrongProductID = Data(
        String(decoding: distribution, as: UTF8.self)
            .replacingOccurrences(
                of: "id=\"\(RimesIdentity.bundleIdentifier)\" version=\"1.2.3\"",
                with: "id=\"example.attacker\" version=\"1.2.3\""
            ).utf8
    )
    let wrongProductVersion = Data(
        String(decoding: distribution, as: UTF8.self)
            .replacingOccurrences(
                of: "version=\"1.2.3\"",
                with: "version=\"1.2.4\""
            ).utf8
    )
    let wrongComponentID = Data(
        String(decoding: packageInfo, as: UTF8.self)
            .replacingOccurrences(
                of: RimesIdentity.bundleIdentifier,
                with: "example.attacker"
            ).utf8
    )
    let wrongComponentVersion = Data(
        String(decoding: packageInfo, as: UTF8.self)
            .replacingOccurrences(
                of: "version=\"1.2.3\"",
                with: "version=\"1.2.4\""
            ).utf8
    )
    let wrongPayloadVersion = Data(
        String(decoding: packageInfo, as: UTF8.self)
            .replacingOccurrences(
                of: "CFBundleShortVersionString=\"1.2.3\"",
                with: "CFBundleShortVersionString=\"9.9.9\""
            ).utf8
    )

    guard UpdatePackageMetadataRules.validationFailure(
        distributionData: distribution,
        packageInfoData: packageInfo,
        expectedVersion: "1.2.3"
    ) == nil,
    UpdatePackageMetadataRules.validationFailure(
        distributionData: distribution,
        packageInfoData: packageInfo,
        expectedVersion: "1.2.4"
    ) != nil,
    UpdatePackageMetadataRules.validationFailure(
        distributionData: wrongProductID,
        packageInfoData: packageInfo,
        expectedVersion: "1.2.3"
    ) != nil,
    UpdatePackageMetadataRules.validationFailure(
        distributionData: wrongProductVersion,
        packageInfoData: packageInfo,
        expectedVersion: "1.2.3"
    ) != nil,
    UpdatePackageMetadataRules.validationFailure(
        distributionData: distribution,
        packageInfoData: wrongComponentID,
        expectedVersion: "1.2.3"
    ) != nil,
    UpdatePackageMetadataRules.validationFailure(
        distributionData: distribution,
        packageInfoData: wrongComponentVersion,
        expectedVersion: "1.2.3"
    ) != nil,
    UpdatePackageMetadataRules.validationFailure(
        distributionData: distribution,
        packageInfoData: wrongPayloadVersion,
        expectedVersion: "1.2.3"
    ) != nil,
    UpdatePackageMetadataRules.validationFailure(
        distributionData: distribution,
        packageInfoData: packageInfo,
        expectedVersion: "01.2.3"
    ) != nil else {
        print("FAILED: update package metadata rules")
        return false
    }

    print("update package metadata smoke OK")
    return true
}
