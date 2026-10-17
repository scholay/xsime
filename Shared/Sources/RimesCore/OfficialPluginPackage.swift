import Foundation

/// A downloaded official package supplies bounded data to a known interpreter.
/// It never selects an executable, network endpoint, credential, or input target.
public struct OfficialPluginPackage: Codable, Equatable {
    public struct Platform: Codable, Equatable {
        public let legacyID: String
        public let distribution: String
    }

    public struct Contribution: Codable, Equatable {
        public let type: String
        public let instructions: [String: String]?
        public let options: [String: String]?
    }

    public static let maximumBytes = 256 * 1024
    public let schemaVersion: Int
    public let sdkVersion: Int
    public let id: String
    public let version: String
    public let minimumHostVersion: String
    public let kind: String
    public let runtime: String
    public let nameZH: String
    public let nameEN: String
    public let summaryZH: String
    public let summaryEN: String
    public let platforms: [String: Platform]
    public let capabilities: [String]
    public let contribution: Contribution
    public let license: String
    public let licenseText: String
    public let notice: String

    /// The caller verifies the catalog's SHA-256 before invoking this parser.
    /// Expected identity comes from that trusted catalog, never from the file.
    public static func validated(_ data: Data, expectedID: String,
                                 expectedVersion: String, platform: String,
                                 hostVersion: String) throws -> Self {
        guard data.count <= maximumBytes else { throw PluginPackageError.invalid }
        let package: Self
        do { package = try JSONDecoder().decode(Self.self, from: data) }
        catch { throw PluginPackageError.invalid }
        guard package.id == expectedID, package.version == expectedVersion,
              package.id.range(of: #"^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$"#,
                               options: .regularExpression) != nil,
              PluginPackageVersion(package.version) != nil,
              let minimum = PluginPackageVersion(package.minimumHostVersion),
              let current = PluginPackageVersion(hostVersion) else {
            throw PluginPackageError.invalid
        }
        guard package.schemaVersion == 2, package.sdkVersion == 1,
              package.runtime == "host-interpreted" else {
            throw PluginPackageError.unsupported
        }
        let supported = Set(["macos", "ios", "android", "windows", "linux"])
        guard !package.platforms.isEmpty,
              Set(package.platforms.keys).isSubset(of: supported),
              package.platforms[platform] != nil else {
            throw PluginPackageError.unsupported
        }
        guard minimum <= current else { throw PluginPackageError.hostTooOld }
        for adapter in package.platforms.values {
            guard validText(adapter.legacyID, maximum: 128),
                  ["bundled", "download"].contains(adapter.distribution) else {
                throw PluginPackageError.invalid
            }
        }
        guard [package.nameZH, package.nameEN, package.summaryZH,
               package.summaryEN, package.notice].allSatisfy({ validText($0, maximum: 4096) }),
              package.license == "Apache-2.0",
              validText(package.licenseText, maximum: 32768) else {
            throw PluginPackageError.invalid
        }
        try package.validateContribution(data)
        return package
    }

    public func instruction(mode: String = "default") throws -> String {
        guard let instruction = contribution.instructions?[mode] else {
            throw PluginPackageError.unsupported
        }
        return instruction
    }

    private func validateContribution(_ data: Data) throws {
        let expectedKind: String
        let expectedCapabilities: [String]
        let optionKeys: Set<String>
        var hasInstructions = false
        switch contribution.type {
        case "ai.prompt.v1":
            expectedKind = "buffer"; expectedCapabilities = ["buffer.read", "ai.generate"]
            optionKeys = []; hasInstructions = true
        case "ai.channel.v1":
            expectedKind = "buffer"; expectedCapabilities = ["buffer.read", "ai.generate"]
            optionKeys = ["channel"]
        case "translation.v1":
            expectedKind = "buffer"; expectedCapabilities = ["buffer.read", "translation.generate"]
            optionKeys = ["source", "target"]; hasInstructions = true
        case "stream.pinyin.v1":
            expectedKind = "buffer"; expectedCapabilities = ["buffer.read", "ai.generate"]
            optionKeys = ["maxCandidates"]
        case "reference.search.v1":
            expectedKind = "buffer"; expectedCapabilities = ["buffer.read", "ai.generate", "reference.search"]
            optionKeys = ["citationStyle"]
        case "music.keyboard.v1":
            expectedKind = "buffer"; expectedCapabilities = ["audio.play"]
            optionKeys = ["layout", "tracks"]
        case "morse.v1":
            expectedKind = "buffer"; expectedCapabilities = ["buffer.write", "audio.play"]
            optionKeys = ["alphabet", "key"]
        case "host.module.v1":
            guard ["capsule", "mailbox"].contains(kind) else { throw PluginPackageError.unsupported }
            expectedKind = kind; expectedCapabilities = [kind + ".navigate"]
            optionKeys = ["host", "module", "filters", "actions"]
        case "metrics.v1":
            expectedKind = "extension"; expectedCapabilities = ["metrics.aggregate"]
            optionKeys = ["events", "storage"]
        case "typing.practice.v1":
            expectedKind = "extension"; expectedCapabilities = ["metrics.aggregate"]
            optionKeys = ["source", "storage"]
        case "input.chord.v1":
            expectedKind = "extension"; expectedCapabilities = ["input.chord"]
            optionKeys = ["schema", "keymap"]
        default: throw PluginPackageError.unsupported
        }
        guard kind == expectedKind, capabilities == expectedCapabilities,
              let root = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let object = root["contribution"] as? [String: Any] else { throw PluginPackageError.unsupported }
        let expectedKeys = Set(["type"])
            .union(hasInstructions ? ["instructions"] : [])
            .union(optionKeys.isEmpty ? [] : ["options"])
        guard Set(object.keys) == expectedKeys,
              Set(contribution.options?.keys.map { $0 } ?? []) == optionKeys,
              (contribution.options ?? [:]).values.allSatisfy({ Self.validText($0, maximum: 256) }) else {
            throw PluginPackageError.invalid
        }
        if hasInstructions {
            guard let instructions = contribution.instructions, instructions["default"] != nil,
                  Set(instructions.keys).isSubset(of: ["default", "image"]),
                  instructions.values.allSatisfy({ Self.validText($0, maximum: 32768) }) else {
                throw PluginPackageError.invalid
            }
        }
        if contribution.type == "host.module.v1" {
            let options = contribution.options ?? [:]
            let modules: Set<String> = kind == "capsule"
                ? ["temporary", "capture", "notes", "resources", "passwords"] : ["terminal", "chat", "inbox"]
            let filters: Set<String> = ["all", "text", "image", "video", "bullet", "richText", "reference", "document", "project", "skills", "login", "key", "other", "unread", "attention", "archived"]
            guard options["host"] == kind, modules.contains(options["module"] ?? ""),
                  Set((options["actions"] ?? "").components(separatedBy: ",")).isSubset(of: ["open", "search"]),
                  Set((options["filters"] ?? "").components(separatedBy: ",")).isSubset(of: filters) else {
                throw PluginPackageError.unsupported
            }
        }
    }

    private static func validText(_ value: String, maximum: Int) -> Bool {
        !value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && !value.contains("\0") && value.utf8.count <= maximum
    }
}

public enum PluginPackageError: Error, Equatable {
    case invalid, unsupported, hostTooOld
}

public struct PluginPackageVersion: Comparable {
    private let components: [Int]
    public init?(_ value: String) {
        guard value.range(of: #"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"#,
                          options: .regularExpression) != nil else { return nil }
        let parsed = value.split(separator: ".").compactMap { Int($0) }
        guard parsed.count == 3 else { return nil }
        components = parsed
    }
    public static func < (lhs: Self, rhs: Self) -> Bool {
        lhs.components.lexicographicallyPrecedes(rhs.components)
    }
}
