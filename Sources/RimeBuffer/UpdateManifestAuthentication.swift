import Foundation
import CoreFoundation
import CryptoKit
import Darwin

enum UpdateManifestAuthentication {
    struct Failure: LocalizedError {
        let reason: String
        var errorDescription: String? { reason }
    }
    static let maximumValidity: Int64 = 90 * 24 * 60 * 60

    private static func integer(_ value: Any?) -> Int64? {
        guard let value = value as? NSNumber,
              CFGetTypeID(value) != CFBooleanGetTypeID(),
              !["f", "d"].contains(String(cString: value.objCType)),
              value.doubleValue.isFinite, value.doubleValue >= 0,
              value.doubleValue <= 253402300799,
              Double(value.int64Value) == value.doubleValue else { return nil }
        return value.int64Value
    }

    private static func base64(_ value: Any?) -> Data? {
        guard let text = value as? String, let data = Data(base64Encoded: text),
              data.base64EncodedString() == text else { return nil }
        return data
    }

    private static func publicKey(_ hex: String) -> Data? {
        guard hex.range(of: "^04[0-9a-f]{128}$", options: .regularExpression) != nil else { return nil }
        var data = Data()
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
            data.append(byte); index = next
        }
        return data
    }

    /// Verify exact signed bytes, then bind all legacy fields to the same JSON.
    /// Keys/time are injectable only for isolated smoke tests, never from settings.
    static func verifiedPayload(_ bytes: Data, now: Int64 = Int64(Date().timeIntervalSince1970),
                                keys: [String: String] = UpdateTrust.keys) throws -> Data {
        guard now >= 0, now <= 253402300499, bytes.count <= 1024 * 1024,
              var outer = try JSONSerialization.jsonObject(with: bytes) as? [String: Any],
              let auth = outer.removeValue(forKey: "authentication") as? [String: Any],
              integer(auth["format"]) == 1, auth["algorithm"] as? String == "ecdsa-p256-sha256",
              let payload = base64(auth["payload"]), payload.count <= 1024 * 1024,
              let decoded = try JSONSerialization.jsonObject(with: payload) as? [String: Any],
              NSDictionary(dictionary: outer).isEqual(to: decoded) else {
            throw Failure(reason: "更新信息缺少可信签名，或签名内容与清单不一致。请稍后重试或查看官方发布页。")
        }
        guard let issued = integer(decoded["issuedAt"]), let expires = integer(decoded["expiresAt"]),
              issued <= now + 300, expires > now, expires > issued,
              expires - issued <= maximumValidity else {
            throw Failure(reason: "更新签名已过期或尚未生效，请确认系统时间并稍后重试。")
        }
        guard let signatures = auth["signatures"] as? [[String: Any]],
              (1...8).contains(signatures.count) else {
            throw Failure(reason: "更新签名记录无效。")
        }
        var seen = Set<String>(), valid = false
        for record in signatures {
            guard Set(record.keys) == Set(["keyId", "signature"]),
                  let keyID = record["keyId"] as? String,
                  keyID.range(of: "^[a-zA-Z0-9._-]{1,80}$", options: .regularExpression) != nil,
                  seen.insert(keyID).inserted,
                  let signatureBytes = base64(record["signature"]), signatureBytes.count == 64 else {
                throw Failure(reason: "更新签名记录重复或格式无效。")
            }
            guard let encodedKey = keys[keyID], let bytes = publicKey(encodedKey),
                  let key = try? P256.Signing.PublicKey(x963Representation: bytes),
                  let signature = try? P256.Signing.ECDSASignature(rawRepresentation: signatureBytes) else { continue }
            if key.isValidSignature(signature, for: payload) { valid = true }
        }
        guard valid else { throw Failure(reason: "更新签名无法确认发布者身份，已拒绝下载或安装。") }
        return payload
    }
}

enum UpdatePlatformCompatibility {
    static func systemComponents(_ text: String) -> [Int]? {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard (1...3).contains(parts.count) else { return nil }
        var values: [Int] = []
        for part in parts {
            guard !part.isEmpty, part.count <= 9, part.utf8.allSatisfy({ (48...57).contains($0) }),
                  (part == "0" || part.first != "0"), let value = Int(part) else { return nil }
            values.append(value)
        }
        return values + Array(repeating: 0, count: 3 - values.count)
    }

    static var hardwareArchitecture: String {
        #if arch(arm64)
        return "arm64"
        #else
        // An Intel process under Rosetta may still install a native arm64 update.
        var arm: Int32 = 0
        var length = MemoryLayout<Int32>.size
        return sysctlbyname("hw.optional.arm64", &arm, &length, nil, 0) == 0 && arm == 1 ? "arm64" : "x86_64"
        #endif
    }

    static func supports(minimumOS: String, architecture: String,
                         system: OperatingSystemVersion = ProcessInfo.processInfo.operatingSystemVersion,
                         hardware: String = hardwareArchitecture) -> Bool {
        guard let minimum = systemComponents(minimumOS),
              ["universal", "arm64", "x86_64"].contains(architecture),
              architecture == "universal" || architecture == hardware else { return false }
        return ![system.majorVersion, system.minorVersion, system.patchVersion].lexicographicallyPrecedes(minimum)
    }
}

func runUpdateAuthenticationSmokeTest() -> Bool {
    do {
        let path = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            .appendingPathComponent("Shared/AppUpdate/fixtures/signed_manifest.json")
        let fixture = try JSONSerialization.jsonObject(with: Data(contentsOf: path)) as! [String: Any]
        let keys = fixture["trustedKeys"] as! [String: String]
        let now = (fixture["now"] as! NSNumber).int64Value
        let document = fixture["document"] as! [String: Any]
        func encoded(_ document: [String: Any]) throws -> Data { try JSONSerialization.data(withJSONObject: document) }
        func rejects(_ document: [String: Any], at time: Int64? = nil) -> Bool {
            do { _ = try UpdateManifestAuthentication.verifiedPayload(encoded(document), now: time ?? now, keys: keys); return false }
            catch { return true }
        }
        let payload = try UpdateManifestAuthentication.verifiedPayload(encoded(document), now: now, keys: keys)
        let manifest = try JSONDecoder().decode(MacUpdateManifest.self, from: payload)
        guard (try JSONSerialization.jsonObject(with: payload) as? [String: Any])?["version"] as? String == "9.8.7",
              manifest.platforms.macos.minimumOS == "13.0",
              manifest.platforms.macos.architecture == "universal",
              Set(keys.keys).isDisjoint(with: Set(UpdateTrust.keys.keys)) else { return false }
        var changed = document; changed.removeValue(forKey: "authentication")
        guard rejects(changed) else { return false }
        changed = document; changed["version"] = "99.0.0"
        guard rejects(changed), rejects(document, at: 1700086400), rejects(document, at: 1699999699) else { return false }
        changed = document
        var auth = changed["authentication"] as! [String: Any]
        var records = auth["signatures"] as! [[String: Any]]
        records.append(records[0]); auth["signatures"] = records; changed["authentication"] = auth
        guard rejects(changed) else { return false }
        changed = document; auth = changed["authentication"] as! [String: Any]
        records = auth["signatures"] as! [[String: Any]]
        records[0]["signature"] = Data(repeating: 0, count: 64).base64EncodedString()
        auth["signatures"] = records; changed["authentication"] = auth
        guard rejects(changed) else { return false }
        let mac13 = OperatingSystemVersion(majorVersion: 13, minorVersion: 6, patchVersion: 0)
        guard UpdatePlatformCompatibility.supports(minimumOS: "13.0", architecture: "universal", system: mac13, hardware: "x86_64"),
              !UpdatePlatformCompatibility.supports(minimumOS: "14.0", architecture: "universal", system: mac13, hardware: "x86_64"),
              !UpdatePlatformCompatibility.supports(minimumOS: "13.0", architecture: "arm64", system: mac13, hardware: "x86_64"),
              !UpdatePlatformCompatibility.supports(minimumOS: "13.beta", architecture: "universal", system: mac13) else { return false }
        print("Signed update metadata and system compatibility smoke OK")
        return true
    } catch {
        print("FAILED: signed update metadata smoke: \(error)")
        return false
    }
}
