import AppKit
import CryptoKit

/// Public, isolated fixtures only; never uses the shared store or real credentials.
@MainActor
enum CapsulePasswordLinuxFixtures {
    static func export(to output: URL) throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("xsime-password-fixture-\(UUID().uuidString)")
        let suite = "xsime.password.fixture.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { try? FileManager.default.removeItem(at: root); defaults.removePersistentDomain(forName: suite) }
        let store = CapsulePasswordStore(rootURL: root, now: { Date(timeIntervalSince1970: 1_700_000_000) })
        let samples: [(String, String, String?, CapsulePasswordCategory)] = [
            ("公开测试登录 👨‍👩‍👧‍👦", "## 测试账户\n- 用户名：fixture-user\n- 密码：fixture-secret-only\n- 网址：https://example.com\n", "演示登录", .login),
            ("公开测试密钥", "- Token：public-test-token\n- TOTP：GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n", nil, .key),
            ("公开测试其他", "  保留前后空白\n```\npublic fixture\n```\n", "测试说明", .other),
        ]
        var documents: [[String: Any]] = []
        for (title, body, summary, category) in samples {
            let saved = try store.put(CapsulePasswordWriteRequest(title: title, body: body, summaryText: summary, category: category))
            documents.append(["id": saved.id.uuidString.lowercased(), "title": title, "body": body, "summary": summary ?? "", "category": category.rawValue,
                "document": try String(contentsOf: saved.fileURL, encoding: .utf8)])
        }
        let key = try Data(contentsOf: store.masterKeyURL)
        let legacyID = UUID().uuidString.lowercased(), legacyTitle = "公开旧格式"
        let legacy: [String: Any] = ["url": "https://example.com/legacy", "app": "fixture", "username": "legacy-user", "password": "legacy-fixture-secret", "previousPasswords": ["prior-fixture", ""]]
        let cipher = try ChaChaPoly.seal(JSONSerialization.data(withJSONObject: legacy, options: [.sortedKeys]), using: SymmetricKey(data: key),
            authenticating: Data("capsule-password-v1\n\(legacyID)\n\(legacyTitle)".utf8)).combined.base64EncodedString()
        let document = "---\ncapsule: password\nversion: 1\nid: \"\(legacyID)\"\ntitle: \"\(legacyTitle)\"\nupdated_at: \"2023-11-14T22:13:20.000Z\"\n---\n\n```capsule-password\n\(cipher)\n```\n"
        let legacyFile = store.passwordDirectoryURL.appendingPathComponent("\(legacyID).md")
        try Data(document.utf8).write(to: legacyFile)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: legacyFile.path)
        documents.append(["id": legacyID, "title": legacyTitle, "body": try store.record(id: UUID(uuidString: legacyID)!).secret.body,
            "summary": "", "category": "other", "document": document])
        for validLegacy in [true, false] {
            let id = UUID().uuidString.lowercased(), title = validLegacy ? "双格式优先旧记录" : "无效旧字段读取正文"
            var mixed = legacy
            mixed["body"] = "- 密码：modern-public-fixture"
            if !validLegacy { mixed["previousPasswords"] = "invalid old type" }
            let cipher = try ChaChaPoly.seal(JSONSerialization.data(withJSONObject: mixed, options: [.sortedKeys]), using: SymmetricKey(data: key),
                authenticating: Data("capsule-password-v1\n\(id)\n\(title)".utf8)).combined.base64EncodedString()
            let document = "---\ncapsule: password\nversion: 2\nid: \"\(id)\"\ntitle: \"\(title)\"\nupdated_at: \"2023-11-14T22:13:20.000Z\"\n---\n\n```capsule-password\n\(cipher)\n```\n"
            let file = store.passwordDirectoryURL.appendingPathComponent("\(id).md")
            try Data(document.utf8).write(to: file)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
            documents.append(["id": id, "title": title, "body": try store.record(id: UUID(uuidString: id)!).secret.body,
                "summary": "", "category": "other", "document": document])
        }
        let passcode = CapsuleRevealPasscodeStore(defaults: defaults)
        let custom = ["QAZ", "WSX", "EDC", "RFV"]
        passcode.set(CapsuleRevealPasscode(chords: custom.map { CapsuleRevealChord($0)! })!)
        let credential = defaults.string(forKey: "capsule.passwordRevealPasscode.credential.v1")!
        var totp: [[String: Any]] = []
        let seed = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
        for (label, value) in [
            ("TOTP", seed), ("备注", seed), ("二次验证", seed.lowercased()),
            ("验证码", "otpauth://totp/Public?secret=\(seed)&digits=8"),
            ("", "otpauth://totp/Public?secret=\(seed)&digits=7&period=60"),
            ("", "otpauth://totp/Public?secret=\(seed)&algorithm=SHA256"),
            ("", "otpauth://totp/Public?secret=\(seed)&period=5"),
            ("TOTP", "invalid!"), ("TOTP", "ABC"),
            ("", "otpauth://totp/Public?secret=\(seed)&digits=broken&period=broken"),
        ] {
            let parameters = CapsuleTOTP.parameters(label: label, value: value)
            var item: [String: Any] = ["label": label, "value": value, "valid": parameters != nil]
            if let parameters {
                item["digits"] = parameters.digits; item["period"] = parameters.period
                item["samples"] = [59, 1_111_111_109, 1_111_111_111, 1_234_567_890, 2_000_000_000, 20_000_000_000].map { seconds in
                    let code = CapsuleTOTP.code(parameters, at: Date(timeIntervalSince1970: Double(seconds)))
                    return ["seconds": seconds, "code": code.code, "remaining": code.remaining] as [String: Any]
                }
            }
            totp.append(item)
        }
        let repository = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        var sources: [String: String] = [:]
        for name in ["Sources/RimeBuffer/CapsulePasswordStore.swift", "Sources/RimeBuffer/CapsuleRevealPasscode.swift", "Sources/RimeBuffer/CapsuleStructure.swift", "Sources/RimeBuffer/CapsulePasswordLinuxFixtures.swift"] {
            sources[name] = SHA256.hash(data: try Data(contentsOf: repository.appendingPathComponent(name))).map { String(format: "%02x", $0) }.joined()
        }
        let data = try JSONSerialization.data(withJSONObject: ["purpose": "public-parity-fixture", "version": 1, "masterKey": key.base64EncodedString(),
            "documents": documents, "customCode": custom, "credential": credential, "totp": totp, "sources": sources], options: [.sortedKeys, .prettyPrinted, .withoutEscapingSlashes])
        try data.write(to: output, options: .atomic)
    }
    static func verifyLinux(at root: URL) throws {
        let manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("linux-public-fixture.json"))) as! [String: Any]
        guard manifest["purpose"] as? String == "public-parity-fixture", let expected = manifest["bodies"] as? [String: String] else { throw CocoaError(.coderReadCorrupt) }
        let store = CapsulePasswordStore(rootURL: root)
        let records = try store.listSummaries()
        guard records.count == expected.count else { throw CocoaError(.coderReadCorrupt) }
        for summary in records {
            guard try store.record(id: summary.id).secret.body == expected[summary.title] else { throw CocoaError(.coderReadCorrupt) }
        }
        print("Linux encrypted Markdown verified by macOS: \(records.count) public fixtures")
    }
}
