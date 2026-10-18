import Foundation
import Network

/// Loopback-only HTTP server carrying MCP (streamable HTTP) + plain HTTP push,
/// feeding InboundBus. The hand-written HTTP/1.1 parsing was validated by the
/// M2 spike (real Claude Code connected, SSE streamed, keep-alive held).
///
/// Security: binds 127.0.0.1 only; inbox and browser-context mutations require
/// scoped bearer tokens. Public health/proof/interactive-claim routes are inert
/// or fixed-extension-origin constrained. Parsing has strict line/size limits.
/// All InboundBus/BufferModel calls hop to the main thread.
final class LocalGateway {
    static let shared = LocalGateway()

    /// Public parity data only; does not start the server or read user tokens.
    static func linuxContract() -> [String: Any] {
        let origins: [String?] = [nil, "http://localhost", "http://127.0.0.1:47700",
            "https://[::1]:1000", "null", "https://example.com", "http://localhost/",
            "http://localhost@evil.test", "http://127.0.0.1:", "http://localhost?x=1",
            "http://%6cocalhost", "https://localhost:65536", " HTTP://LOCALHOST:1234 "]
        return ["schemaVersion": 1, "purpose": "public-gateway-contract",
            "defaultPort": Int(defaultPort), "maxHeaderBytes": maxHeaderBytes,
            "maxBodyBytes": maxBodyBytes, "maxPending": InboundBus.maxPending,
            "maxTextCount": InboundBus.maxTextCount,
            "maxPendingTextCount": InboundBus.maxPendingTextCount,
            "origins": origins.map { ["origin": $0 as Any? ?? NSNull(),
                                       "allowed": isAllowedOrigin($0)] },
            "mcp": Connection.linuxContract()]
    }

    static let defaultPort: UInt16 = 47700
    private static let maxHeaderBytes = 32 * 1024
    private static let maxBodyBytes = 256 * 1024

    /// CLI/agent clients normally omit Origin. When it is present, accept only
    /// a syntactically complete HTTP(S) loopback origin (an optional port is OK).
    /// Kept pure and internal so the CLI smoke harness can pin the edge cases.
    static func isAllowedOrigin(_ rawOrigin: String?) -> Bool {
        guard let rawOrigin else { return true }
        let origin = rawOrigin.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !origin.isEmpty,
              let components = URLComponents(string: origin),
              components.url != nil,
              let scheme = components.scheme?.lowercased(),
              scheme == "http" || scheme == "https",
              components.user == nil,
              components.password == nil,
              components.query == nil,
              components.fragment == nil,
              components.path.isEmpty,
              let parsedHost = components.host?.lowercased()
        else { return false }

        // Foundation has returned both bracketed and unbracketed IPv6 hosts
        // across OS releases; brackets are URL syntax, not part of the host.
        let host = parsedHost.hasPrefix("[") && parsedHost.hasSuffix("]")
            ? String(parsedHost.dropFirst().dropLast())
            : parsedHost
        let serializedHost: String
        switch host {
        case "localhost", "127.0.0.1": serializedHost = host
        case "::1": serializedHost = "[::1]"
        default: return false
        }
        let portSuffix: String
        if let port = components.port {
            guard (0...65_535).contains(port) else { return false }
            portSuffix = ":\(port)"
        } else {
            portSuffix = ""
        }
        // Comparing the reconstructed origin also rejects percent-encoded hosts,
        // empty/invalid ports, paths, and other parser-normalized spellings.
        return origin.lowercased() == "\(scheme)://\(serializedHost)\(portSuffix)"
    }

    private let queue = DispatchQueue(label: "etinput.gateway")
    private let lifecycleLock = NSLock()
    private var lifecycleEpoch: UInt64 = 0
    private var wantsRunning = false
    private var runningSnapshot = false

    // Queue-confined. NWListener/NWConnection callbacks are also delivered on
    // `queue`, so no other thread may read or mutate these collections.
    private var listener: NWListener?
    private var listenerEpoch: UInt64?
    private var connections: [ObjectIdentifier: Connection] = [:]
    private let marineChromeAvailable: () -> Bool

    init(marineChromeAvailable: @escaping () -> Bool = {
        MarineChromeGatewayAvailability.shared.isAvailable
    }) {
        self.marineChromeAvailable = marineChromeAvailable
    }

    var running: Bool {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        return runningSnapshot
    }

    var enabled: Bool {
        get { UserDefaults.standard.object(forKey: "gatewayEnabled") as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: "gatewayEnabled"); newValue ? start() : stop() }
    }
    var port: UInt16 {
        let v = UserDefaults.standard.integer(forKey: "gatewayPort")
        return v > 0 ? UInt16(v) : Self.defaultPort
    }

    func startIfEnabled() { if enabled { start() } }

    func start() {
        guard let epoch = requestStart() else { return }
        let requestedPort = port
        queue.async { [weak self] in
            self?.startOnQueue(epoch: epoch, port: requestedPort)
        }
    }

    func stop() {
        let epoch = requestStop()

        // Invalidate browser authority before waiting for queue-confined socket
        // teardown. A stale receive or main-thread mutation will now fail its
        // epoch guard even if its NWConnection callback was already enqueued.
        cancelPairingAndClearBrowserContext()

        queue.async { [weak self] in
            self?.stopOnQueue(epoch: epoch)
        }
    }

    private func startOnQueue(epoch: UInt64, port: UInt16) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard isLifecycleCurrent(epoch) else { return }

        // A newer start may overtake an older stop while their callers enqueue
        // work from different threads. Replacing all queue-owned resources here
        // makes the newest epoch authoritative regardless of enqueue order.
        tearDownQueueResources()
        MarineChromePairingBroker.shared.cancelAll()

        let params = NWParameters.tcp
        // Bind loopback only — never reachable off-box.
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!)
        guard let l = try? NWListener(using: params) else {
            IMELog.write("gateway: cannot bind 127.0.0.1:\(port)")
            failLifecycleOnQueue(epoch: epoch)
            return
        }
        l.newConnectionHandler = { [weak self, weak l] nw in
            guard let self, let l,
                  self.listener === l,
                  self.isLifecycleCurrent(epoch) else {
                nw.cancel()
                return
            }
            let c = Connection(
                nw,
                queue: self.queue,
                isGatewayCurrent: { [weak self] in
                    self?.isLifecycleCurrent(epoch) == true
                },
                marineChromeAvailable: self.marineChromeAvailable,
                onClose: { [weak self] in self?.dropOnQueue($0) }
            )
            self.connections[ObjectIdentifier(c)] = c   // retain (spike caught this bug)
            c.start()
        }
        l.stateUpdateHandler = { [weak self, weak l] state in
            guard let self, let l else { return }
            self.handleListenerState(
                state,
                listener: l,
                epoch: epoch,
                port: port
            )
        }
        listener = l
        listenerEpoch = epoch
        l.start(queue: queue)
    }

    private func stopOnQueue(epoch: UInt64) {
        dispatchPrecondition(condition: .onQueue(queue))
        // If a subsequent start already became authoritative, this stale stop
        // must not tear down its listener or cancel its fresh pairing prompt.
        guard isLifecycleStopped(epoch) else { return }
        tearDownQueueResources()
        MarineChromePairingBroker.shared.cancelAll()
    }

    private func tearDownQueueResources() {
        dispatchPrecondition(condition: .onQueue(queue))
        listener?.cancel()
        listener = nil
        listenerEpoch = nil
        connections.values.forEach { $0.cancel() }
        connections.removeAll()
    }

    private func handleListenerState(_ state: NWListener.State,
                                     listener expectedListener: NWListener,
                                     epoch: UInt64,
                                     port: UInt16) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard listener === expectedListener else { return }
        switch state {
        case .ready:
            guard markReady(epoch: epoch) else {
                expectedListener.cancel()
                return
            }
            IMELog.write("gateway ready on 127.0.0.1:\(port)")
        case .failed(let error):
            IMELog.write("gateway listener failed: \(error)")
            failLifecycleOnQueue(epoch: epoch)
        case .cancelled:
            // Explicit teardown clears `listener` first, so reaching this branch
            // means the current listener was cancelled independently.
            failLifecycleOnQueue(epoch: epoch)
        default:
            break
        }
    }

    private func failLifecycleOnQueue(epoch: UInt64) {
        dispatchPrecondition(condition: .onQueue(queue))
        guard let stoppedEpoch = invalidateLifecycleIfCurrent(epoch) else {
            if listenerEpoch == epoch {
                tearDownQueueResources()
            }
            return
        }
        cancelPairingAndClearBrowserContext()
        stopOnQueue(epoch: stoppedEpoch)
    }

    private func requestStart() -> UInt64? {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        guard !wantsRunning else { return nil }
        lifecycleEpoch &+= 1
        wantsRunning = true
        runningSnapshot = false
        return lifecycleEpoch
    }

    private func requestStop() -> UInt64 {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        lifecycleEpoch &+= 1
        wantsRunning = false
        runningSnapshot = false
        return lifecycleEpoch
    }

    private func invalidateLifecycleIfCurrent(_ epoch: UInt64) -> UInt64? {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        guard wantsRunning, lifecycleEpoch == epoch else { return nil }
        lifecycleEpoch &+= 1
        wantsRunning = false
        runningSnapshot = false
        return lifecycleEpoch
    }

    private func markReady(epoch: UInt64) -> Bool {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        guard wantsRunning, lifecycleEpoch == epoch else { return false }
        runningSnapshot = true
        return true
    }

    private func isLifecycleCurrent(_ epoch: UInt64) -> Bool {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        return wantsRunning && lifecycleEpoch == epoch
    }

    private func isLifecycleStopped(_ epoch: UInt64) -> Bool {
        lifecycleLock.lock()
        defer { lifecycleLock.unlock() }
        return !wantsRunning && lifecycleEpoch == epoch
    }

    private func cancelPairingAndClearBrowserContext() {
        MarineChromePairingBroker.shared.cancelAll()
        if Thread.isMainThread {
            MarineChromeContextStore.shared.clear()
        } else {
            DispatchQueue.main.sync {
                MarineChromeContextStore.shared.clear()
            }
        }
    }

    private func dropOnQueue(_ connection: Connection) {
        dispatchPrecondition(condition: .onQueue(queue))
        connections[ObjectIdentifier(connection)] = nil
    }

    // MARK: - one connection

    private final class Connection {
        private let conn: NWConnection
        private let gatewayQueue: DispatchQueue
        private let isGatewayCurrent: () -> Bool
        private let marineChromeAvailable: () -> Bool
        private let onClose: (Connection) -> Void
        private var buffer = Data()
        /// While an inbound mutation is running on the main thread, keep later
        /// pipelined requests buffered. HTTP/1.1 responses must be emitted in
        /// request order even though InboundBus is main-thread confined.
        private var awaitingAsyncResponse = false
        // Stateless MCP has no server-side session. Keep only a connection-local,
        // unverified client name as a best-effort source label.
        private var mcpClientName = "MCP"

        init(_ connection: NWConnection,
             queue: DispatchQueue,
             isGatewayCurrent: @escaping () -> Bool,
             marineChromeAvailable: @escaping () -> Bool,
             onClose: @escaping (Connection) -> Void) {
            conn = connection
            gatewayQueue = queue
            self.isGatewayCurrent = isGatewayCurrent
            self.marineChromeAvailable = marineChromeAvailable
            self.onClose = onClose
        }

        func start() {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            guard isGatewayCurrent() else {
                conn.cancel()
                return
            }
            conn.stateUpdateHandler = { [weak self] st in
                if case .failed = st { self?.close() }
                if case .cancelled = st { self.map { $0.onClose($0) } }
            }
            conn.start(queue: gatewayQueue)
            receive()
        }

        func cancel() {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            conn.cancel()
        }

        private func close() {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            conn.cancel()
        }

        private func receive() {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            guard isGatewayCurrent() else {
                close()
                return
            }
            conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isDone, err in
                guard let self else { return }
                guard self.isGatewayCurrent() else {
                    self.close()
                    return
                }
                if let data, !data.isEmpty {
                    self.buffer.append(data)
                    self.drain()
                }
                if isDone || err != nil { self.close(); return }
                // An inbound mutation is waiting for its durable result on the
                // main thread. Stop issuing reads so a pipelining client is
                // back-pressured by TCP instead of growing this Data buffer
                // without bound. jsonAfterMain resumes receive after sending
                // the matching response and draining already-buffered bytes.
                if self.awaitingAsyncResponse { return }
                self.receive()
            }
        }

        private func drain() {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            while true {
                guard !awaitingAsyncResponse else { return }
                guard isGatewayCurrent() else {
                    close()
                    return
                }
                let req: Req
                let consumed: Int
                switch parse(buffer) {
                case .incomplete:
                    return
                case .invalid:
                    close()
                    return
                case .complete(let parsedReq, let parsedBytes):
                    req = parsedReq
                    consumed = parsedBytes
                }
                buffer.removeSubrange(0..<consumed)
                handle(req)
                guard !awaitingAsyncResponse else { return }
                if (req.headers["connection"] ?? "").lowercased() == "close" { return }
            }
        }

        // MARK: HTTP parse

        private struct Req { var method = "", path = ""; var headers: [String: String] = [:]; var body = Data() }
        private enum ParseResult {
            case incomplete
            case invalid
            case complete(Req, Int)
        }

        private func parse(_ data: Data) -> ParseResult {
            let terminator = Data("\r\n\r\n".utf8)
            guard let end = data.range(of: terminator) else {
                // Leave room for a partial terminator beginning exactly at the
                // header limit; anything beyond that can never become valid.
                return data.count > LocalGateway.maxHeaderBytes + terminator.count - 1
                    ? .invalid : .incomplete
            }
            guard end.lowerBound <= LocalGateway.maxHeaderBytes,
                  let headerStr = String(data: data.subdata(in: 0..<end.lowerBound), encoding: .utf8)
            else { return .invalid }
            var lines = headerStr.components(separatedBy: "\r\n")
            guard !lines.isEmpty else { return .invalid }
            let start = lines.removeFirst().split(separator: " ")
            guard start.count == 3 else { return .invalid }
            var req = Req(); req.method = String(start[0]); req.path = String(start[1])
            for line in lines {
                guard let c = line.firstIndex(of: ":") else { continue }
                req.headers[line[..<c].trimmingCharacters(in: .whitespaces).lowercased()] =
                    line[line.index(after: c)...].trimmingCharacters(in: .whitespaces)
            }
            let bodyStart = end.upperBound
            let len: Int
            if let rawLength = req.headers["content-length"] {
                guard !rawLength.isEmpty,
                      rawLength.utf8.allSatisfy({ $0 >= 48 && $0 <= 57 }),
                      let parsedLength = Int(rawLength),
                      parsedLength <= LocalGateway.maxBodyBytes
                else { return .invalid }
                len = parsedLength
            } else {
                len = 0
            }
            guard data.count - bodyStart >= len else { return .incomplete }
            req.body = data.subdata(in: bodyStart..<(bodyStart + len))
            return .complete(req, bodyStart + len)
        }

        // MARK: responses

        private func send(_ status: String, headers: [String: String] = [:], body: Data = Data()) {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            guard isGatewayCurrent() else { return }
            var h = headers
            h["Content-Length"] = "\(body.count)"; h["Connection"] = "keep-alive"
            var head = "HTTP/1.1 \(status)\r\n"
            for (k, v) in h { head += "\(k): \(v)\r\n" }
            head += "\r\n"
            var out = Data(head.utf8); out.append(body)
            conn.send(content: out, completion: .contentProcessed { _ in })
        }

        private func onGatewayIfCurrent(
            _ operation: @escaping (Connection) -> Void
        ) {
            gatewayQueue.async { [weak self] in
                guard let self, self.isGatewayCurrent() else { return }
                operation(self)
            }
        }

        private func json(_ obj: Any, status: String = "200 OK") {
            let data = (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8)
            send(status, headers: ["Content-Type": "application/json"], body: data)
        }

        /// InboundBus and MailboxStore are main-thread confined. Defer the
        /// response until that mutation returns, then resume parsing buffered
        /// requests only after the matching response has been enqueued.
        private func jsonAfterMain(
            _ operation: @escaping () -> (object: Any, status: String)
        ) {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            precondition(!awaitingAsyncResponse)
            awaitingAsyncResponse = true
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                guard self.isGatewayCurrent() else {
                    self.gatewayQueue.async { [weak self] in self?.close() }
                    return
                }
                let response = operation()
                self.gatewayQueue.async { [weak self] in
                    guard let self else { return }
                    guard self.isGatewayCurrent() else {
                        self.close()
                        return
                    }
                    self.json(response.object, status: response.status)
                    self.awaitingAsyncResponse = false
                    self.drain()
                    if !self.awaitingAsyncResponse {
                        self.receive()
                    }
                }
            }
        }

        private func marineChromeHeaders(origin: String) -> [String: String] {
            [
                "Access-Control-Allow-Origin": origin,
                "Access-Control-Allow-Headers": "Authorization, Content-Type",
                "Access-Control-Allow-Methods": "GET, PUT, DELETE, OPTIONS",
                "Access-Control-Max-Age": "600",
                "Cache-Control": "no-store",
                "Vary": "Origin",
            ]
        }

        private func marineChromeJSON(_ obj: Any,
                                      origin: String,
                                      status: String = "200 OK") {
            let data = (try? JSONSerialization.data(withJSONObject: obj))
                ?? Data("{}".utf8)
            var headers = marineChromeHeaders(origin: origin)
            headers["Content-Type"] = "application/json"
            send(status, headers: headers, body: data)
        }

        // MARK: routing

        private func handle(_ req: Req) {
            dispatchPrecondition(condition: .onQueue(gatewayQueue))
            guard isGatewayCurrent() else { return }
            let path = req.path.split(separator: "?").first.map(String.init) ?? req.path
            if req.method == "GET", path == "/v1/health" { json(["ok": true]); return }
            if path.hasPrefix("/v1/marine-chrome/") {
                handleMarineChrome(req, path: path)
                return
            }
            // Spec MUST: reject cross-origin browsers (DNS-rebinding defence). Real
            // MCP agents are not browsers and omit Origin, so they pass through.
            guard LocalGateway.isAllowedOrigin(req.headers["origin"]) else {
                json(["error": "forbidden origin"], status: "403 Forbidden"); return
            }
            guard let auth = req.headers["authorization"], auth.hasPrefix("Bearer "),
                  GatewayToken.matches(String(auth.dropFirst(7))) else {
                json(["error": "unauthorized"], status: "401 Unauthorized"); return
            }
            switch (req.method, path) {
            case ("POST", "/v1/inbound"): handleInbound(req)
            case ("POST", "/mcp"): handleMCP(req)
            // Legal stateless Streamable HTTP: POST is the only implemented MCP
            // transport method. There is no SSE stream or session to terminate.
            case ("GET", "/mcp"), ("DELETE", "/mcp"):
                send("405 Method Not Allowed", headers: ["Allow": "POST"])
            default: json(["error": "not found"], status: "404 Not Found")
            }
        }

        private func handleMarineChrome(_ req: Req, path: String) {
            // MV3 service-worker GET requests made under an exact loopback host
            // permission can omit Origin. Keep this probe public and inert; it
            // never authenticates or pins an extension identity.
            if req.method == "GET", path == "/v1/marine-chrome/health" {
                let data = (try? JSONSerialization.data(withJSONObject: [
                    "ok": true,
                    "protocolVersion": MarineChromeProtocol.version,
                    "pairingMode": "interactive-claim-v1",
                    "extensionId": MarineChromeExtensionIdentity.identifier,
                ])) ?? Data("{}".utf8)
                send("200 OK", headers: [
                    "Cache-Control": "no-store",
                    "Content-Type": "application/json",
                ], body: data)
                return
            }

            guard MarineChromeGatewayRoutePolicy.allowedMethods(for: path)
                    != nil else {
                json(["error": "not found"], status: "404 Not Found")
                return
            }
            guard MarineChromeGatewayRoutePolicy.permitsPluginAvailability(
                path: path,
                isAvailable: marineChromeAvailable()
            ) else {
                json(
                    ["error": "marine-chrome is not available"],
                    status: "503 Service Unavailable"
                )
                return
            }

            guard let origin = MarineChromeExtensionOrigin.normalized(
                req.headers["origin"]
            ) else {
                json(["error": "forbidden extension origin"],
                     status: "403 Forbidden")
                return
            }
            if req.method == "OPTIONS" {
                let requestedMethod = req.headers[
                    "access-control-request-method"
                ]?.uppercased()
                let requestedHeaders = Set(
                    (req.headers["access-control-request-headers"] ?? "")
                        .split(separator: ",")
                        .map {
                            $0.trimmingCharacters(in: .whitespacesAndNewlines)
                                .lowercased()
                        }
                )
                guard MarineChromeGatewayRoutePolicy.allowsPreflight(
                    path: path,
                    requestedMethod: requestedMethod,
                    requestedHeaders: requestedHeaders
                ) else {
                    marineChromeJSON(
                        ["error": "invalid CORS preflight"],
                        origin: origin,
                        status: "403 Forbidden"
                    )
                    return
                }
                guard MarineChromePairingOrigin.permitsPreflight(
                    origin,
                    path: path
                ), marineChromeAvailable() else {
                    marineChromeJSON(
                        ["error": "extension origin is not paired"],
                        origin: origin,
                        status: "403 Forbidden"
                    )
                    return
                }
                send("204 No Content",
                     headers: marineChromeHeaders(origin: origin))
                return
            }
            guard MarineChromeGatewayRoutePolicy.allows(method: req.method,
                                                         path: path) else {
                marineChromeJSON(["error": "method not allowed"],
                                 origin: origin,
                                 status: "405 Method Not Allowed")
                return
            }
            if MarineChromeGatewayRoutePolicy.requiresJSON(method: req.method) {
                let contentType = req.headers["content-type"]?
                    .split(separator: ";", maxSplits: 1)
                    .first?
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                    .lowercased()
                guard contentType == "application/json" else {
                    marineChromeJSON(
                        ["error": "content type must be application/json"],
                        origin: origin,
                        status: "415 Unsupported Media Type"
                    )
                    return
                }
            }
            if path == "/v1/marine-chrome/prove" {
                guard marineChromeAvailable(),
                      MarineChromeExtensionIdentity.matches(origin: origin) else {
                    marineChromeJSON(
                        ["error": "unexpected extension identity"],
                        origin: origin,
                        status: "403 Forbidden"
                    )
                    return
                }
                let nonce: String
                do {
                    nonce = try MarineChromeServerProof.decodeRequest(req.body)
                } catch {
                    marineChromeJSON(
                        ["error": "invalid server proof request"],
                        origin: origin,
                        status: "400 Bad Request"
                    )
                    return
                }
                guard let proof = try? MarineChromeGatewayAvailability.shared
                    .valueIfAvailable({
                        try MarineChromeServerProof.make(nonce: nonce)
                    }) else {
                    marineChromeJSON(
                        ["error": "server proof unavailable"],
                        origin: origin,
                        status: "500 Internal Server Error"
                    )
                    return
                }
                marineChromeJSON([
                    "protocolVersion": MarineChromeProtocol.version,
                    "proof": proof,
                ], origin: origin)
                return
            }
            if path == "/v1/marine-chrome/pair" {
                guard let object = try? JSONSerialization.jsonObject(
                    with: req.body
                ) as? [String: Any],
                      object["protocolVersion"] as? Int
                        == MarineChromeProtocol.version else {
                    marineChromeJSON(["error": "invalid pairing request"],
                                     origin: origin,
                                     status: "400 Bad Request")
                    return
                }
            }
            if path == "/v1/marine-chrome/pair/request" {
                guard marineChromeAvailable(),
                      MarineChromeExtensionIdentity.matches(origin: origin) else {
                    marineChromeJSON(
                        ["error": "unexpected extension identity"],
                        origin: origin,
                        status: "403 Forbidden"
                    )
                    return
                }
                let claim: MarineChromePairingClaim
                do {
                    claim = try MarineChromePairingClaim.decode(
                        req.body,
                        origin: origin
                    )
                } catch {
                    marineChromeJSON(
                        ["error": "invalid interactive pairing claim"],
                        origin: origin,
                        status: "400 Bad Request"
                    )
                    return
                }
                guard marineChromeAvailable() else {
                    marineChromeJSON(
                        ["error": "marine-chrome is not available"],
                        origin: origin,
                        status: "503 Service Unavailable"
                    )
                    return
                }
                switch MarineChromePairingBroker.shared.submit(claim) {
                case .pending(let expiresAt):
                    marineChromeJSON([
                        "state": "pending",
                        "protocolVersion": MarineChromeProtocol.version,
                        "expiresAt": expiresAt,
                    ], origin: origin, status: "202 Accepted")
                case .issued(let token, let expiresAt):
                    marineChromeJSON([
                        "state": "paired",
                        "paired": true,
                        "protocolVersion": MarineChromeProtocol.version,
                        "token": token,
                        "expiresAt": expiresAt,
                    ], origin: origin)
                case .denied:
                    marineChromeJSON([
                        "state": "denied",
                        "error": "pairing request denied",
                    ], origin: origin, status: "403 Forbidden")
                case .expired:
                    marineChromeJSON([
                        "state": "expired",
                        "error": "pairing request expired",
                    ], origin: origin, status: "410 Gone")
                case .busy(let retryAfter):
                    marineChromeJSON([
                        "state": "busy",
                        "error": "another pairing request is active",
                        "retryAfter": retryAfter,
                    ], origin: origin,
                       status: "429 Too Many Requests")
                case .failed:
                    marineChromeJSON([
                        "state": "failed",
                        "error": "pairing credential could not be issued",
                    ], origin: origin,
                       status: "500 Internal Server Error")
                }
                return
            }
            guard marineChromeAvailable() else {
                marineChromeJSON(
                    ["error": "marine-chrome is not available"],
                    origin: origin,
                    status: "503 Service Unavailable"
                )
                return
            }
            guard let authorization = req.headers["authorization"],
                  authorization.hasPrefix("Bearer ") else {
                marineChromeJSON(["error": "unauthorized extension"],
                                 origin: origin,
                                 status: "401 Unauthorized")
                return
            }
            let bearerToken = String(authorization.dropFirst(7))
            let authenticated = MarineChromeGatewayAvailability.shared
                .valueIfAvailable {
                    MarineChromePairingOrigin.authenticateAndPair(
                        origin: origin,
                        bearerToken: bearerToken
                    )
                } ?? false
            guard authenticated else {
                marineChromeJSON(["error": "unauthorized extension"],
                                 origin: origin,
                                 status: "401 Unauthorized")
                return
            }

            switch (req.method, path) {
            case ("PUT", "/v1/marine-chrome/pair"):
                marineChromeJSON([
                    "paired": true,
                    "protocolVersion": MarineChromeProtocol.version,
                ], origin: origin)
            case ("PUT", "/v1/marine-chrome/context"):
                let context: MarineChromeContext
                do {
                    context = try MarineChromeProtocol.decodeContext(req.body)
                } catch {
                    marineChromeProtocolError(error, origin: origin)
                    return
                }
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.isGatewayCurrent() else { return }
                    let authenticated = MarineChromeGatewayToken.matches(
                        bearerToken
                    )
                    let enabled = MarineChromeWorkspace.shared
                        .acceptsBrowserContext
                    let accepted = authenticated && enabled
                        && MarineChromeContextStore.shared.accept(context)
                    guard authenticated else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "extension token was rotated"],
                                origin: origin,
                                status: "401 Unauthorized"
                            )
                        }
                        return
                    }
                    guard enabled else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "marine-chrome is not active"],
                                origin: origin,
                                status: "503 Service Unavailable"
                            )
                        }
                        return
                    }
                    guard accepted else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "stale browser context"],
                                origin: origin,
                                status: "409 Conflict"
                            )
                        }
                        return
                    }
                    self.onGatewayIfCurrent { connection in
                        connection.marineChromeJSON([
                            "accepted": true,
                            "contextId": context.contextId,
                        ], origin: origin)
                    }
                }
            case ("PUT", "/v1/marine-chrome/heartbeat"):
                let heartbeat: MarineChromeHeartbeat
                do {
                    heartbeat = try MarineChromeProtocol.decodeHeartbeat(req.body)
                } catch {
                    marineChromeProtocolError(error, origin: origin)
                    return
                }
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.isGatewayCurrent() else { return }
                    let authenticated = MarineChromeGatewayToken.matches(
                        bearerToken
                    )
                    let enabled = MarineChromeWorkspace.shared
                        .acceptsBrowserContext
                    let accepted = authenticated && enabled
                        && MarineChromeContextStore.shared.heartbeat(heartbeat)
                    guard authenticated else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "extension token was rotated"],
                                origin: origin,
                                status: "401 Unauthorized"
                            )
                        }
                        return
                    }
                    guard enabled else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "marine-chrome is not active"],
                                origin: origin,
                                status: "503 Service Unavailable"
                            )
                        }
                        return
                    }
                    guard accepted else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "browser context is no longer current"],
                                origin: origin,
                                status: "409 Conflict"
                            )
                        }
                        return
                    }
                    self.onGatewayIfCurrent { connection in
                        connection.marineChromeJSON(
                            ["accepted": true],
                            origin: origin
                        )
                    }
                }
            case ("DELETE", "/v1/marine-chrome/context"):
                let revocation: MarineChromeRevocation
                do {
                    revocation = try MarineChromeProtocol.decodeRevocation(req.body)
                } catch {
                    marineChromeProtocolError(error, origin: origin)
                    return
                }
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.isGatewayCurrent() else { return }
                    let authenticated = MarineChromeGatewayToken.matches(
                        bearerToken
                    )
                    let revoked = authenticated
                        && MarineChromeContextStore.shared.revoke(revocation)
                    guard authenticated else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "extension token was rotated"],
                                origin: origin,
                                status: "401 Unauthorized"
                            )
                        }
                        return
                    }
                    guard revoked else {
                        self.onGatewayIfCurrent { connection in
                            connection.marineChromeJSON(
                                ["error": "stale browser revocation"],
                                origin: origin,
                                status: "409 Conflict"
                            )
                        }
                        return
                    }
                    self.onGatewayIfCurrent { connection in
                        connection.marineChromeJSON(
                            ["revoked": true],
                            origin: origin
                        )
                    }
                }
            default:
                marineChromeJSON(["error": "not found"],
                                 origin: origin,
                                 status: "404 Not Found")
            }
        }

        private func marineChromeProtocolError(_ error: Error,
                                               origin: String) {
            let status: String
            if error as? MarineChromeContextError == .oversized {
                status = "413 Content Too Large"
            } else {
                status = "400 Bad Request"
            }
            marineChromeJSON(["error": "invalid browser context"],
                             origin: origin,
                             status: status)
        }

        private func handleInbound(_ req: Req) {
            guard let obj = try? JSONSerialization.jsonObject(with: req.body) as? [String: Any],
                  let text = obj["text"] as? String else {
                json(["error": "bad request"], status: "400 Bad Request"); return
            }
            let format: AITextContentFormat
            if let raw = obj["format"] {
                guard let value = raw as? String,
                      let parsed = AITextContentFormat(rawValue: value) else {
                    json(["error": "format must be plain, markdown, or json"],
                         status: "400 Bad Request")
                    return
                }
                format = parsed
            } else {
                format = .plain
            }
            let source = obj["source"] as? String ?? "http"
            let title = obj["title"] as? String
            jsonAfterMain {
                let result = InboundBus.shared.submitDetailed(
                    origin: .http(source: source),
                    text: text,
                    title: title,
                    format: format
                )
                switch result {
                case .pending:
                    return (["accepted": true, "disposition": "mailbox"], "200 OK")
                case .staged:
                    return (["accepted": true, "disposition": "buffer"], "200 OK")
                case .rejected(let reason):
                    return (["accepted": false, "error": Self.rejectionMessage(reason)],
                            Self.rejectionHTTPStatus(reason))
                }
            }
        }

        // MCP Streamable HTTP, protocol rev 2025-06-18 (negotiates down to older
        // clients). Single JSON response per POST — no server-initiated stream.
        private func handleMCP(_ req: Req) {
            guard let msg = try? JSONSerialization.jsonObject(with: req.body) as? [String: Any] else {
                json(["jsonrpc": "2.0", "error": ["code": -32700, "message": "parse error"], "id": NSNull()],
                     status: "400 Bad Request"); return
            }
            let method = msg["method"] as? String ?? ""
            let id = msg["id"] ?? NSNull()
            // A message with no id is a notification/response → 202 Accepted, no body.
            let isNotification = msg["id"] == nil

            // Spec MUST: after initialization the client sends MCP-Protocol-Version;
            // an unsupported value is a 400. A missing header stays lenient (assume
            // the negotiated version) so simpler clients keep working.
            if method != "initialize",
               let pv = req.headers["mcp-protocol-version"],
               !Self.supportedProtocolVersions.contains(pv) {
                json(["jsonrpc": "2.0", "id": id,
                      "error": ["code": -32600, "message": "unsupported MCP-Protocol-Version: \(pv)"]],
                     status: "400 Bad Request")
                return
            }

            switch method {
            case "initialize":
                let params = msg["params"] as? [String: Any]
                if let ci = params?["clientInfo"] as? [String: Any],
                   let name = ci["name"] as? String { mcpClientName = name }
                // Version negotiation: echo the client's version if we speak it,
                // otherwise offer our latest and let the client decide.
                let requested = params?["protocolVersion"] as? String ?? ""
                let chosen = Self.supportedProtocolVersions.contains(requested)
                    ? requested : Self.latestProtocolVersion
                json(["jsonrpc": "2.0", "id": id, "result": [
                    "protocolVersion": chosen,
                    "capabilities": ["tools": ["listChanged": false]],
                    "serverInfo": [
                        "name": ProductIdentity.displayName,
                        "title": "\(ProductIdentity.displayName) 缓冲区",
                        "version": "1",
                    ],
                    "instructions": "把文字送进用户输入法的缓冲区收件箱，等用户确认后由用户上屏。"
                        + "只进不出：无法读取缓冲区，也不会自动上屏。",
                ]])
            case "notifications/initialized":
                send("202 Accepted")
            case "ping":
                json(["jsonrpc": "2.0", "id": id, "result": [String: Any]()])
            case "tools/list":
                json(["jsonrpc": "2.0", "id": id, "result": ["tools": Self.toolList]])
            case "tools/call":
                handleToolCall(msg, id: id)
            default:
                if isNotification { send("202 Accepted") }
                else { json(["jsonrpc": "2.0", "id": id, "error": ["code": -32601, "message": "method not found"]]) }
            }
        }

        private func handleToolCall(_ msg: [String: Any], id: Any) {
            let params = msg["params"] as? [String: Any]
            let name = params?["name"] as? String ?? ""
            let args = params?["arguments"] as? [String: Any] ?? [:]
            let client = mcpClientName
            func ok(_ text: String) {
                json(["jsonrpc": "2.0", "id": id, "result": ["content": [["type": "text", "text": text]]]])
            }
            func error(_ text: String) {
                json(["jsonrpc": "2.0", "id": id, "result": [
                    "content": [["type": "text", "text": text]],
                    "isError": true,
                ]])
            }
            switch name {
            case "buffer_push":
                let text = args["text"] as? String ?? ""
                let title = args["title"] as? String ?? args["kind"] as? String
                let format: AITextContentFormat
                if let raw = args["format"] {
                    guard let value = raw as? String,
                          let parsed = AITextContentFormat(rawValue: value) else {
                        error("format must be plain, markdown, or json")
                        return
                    }
                    format = parsed
                } else {
                    format = .plain
                }
                jsonAfterMain {
                    let result = InboundBus.shared.submitDetailed(
                        origin: .mcp(client: client),
                        text: text,
                        title: title,
                        format: format
                    )
                    let responseText: String
                    let isError: Bool
                    switch result {
                    case .pending:
                        responseText = "accepted \(text.count) chars into Mailbox for review"
                        isError = false
                    case .staged:
                        responseText = "staged \(text.count) chars into Buffer"
                        isError = false
                    case .rejected(let reason):
                        responseText = Self.rejectionMessage(reason)
                        isError = true
                    }
                    return (["jsonrpc": "2.0", "id": id, "result": [
                        "content": [["type": "text", "text": responseText]],
                        "isError": isError,
                    ]], "200 OK")
                }
            case "buffer_stream_begin":
                let sid = "st-" + String(UUID().uuidString.prefix(8))
                let title = args["title"] as? String
                let format: AITextContentFormat
                if let raw = args["format"] {
                    guard let value = raw as? String,
                          let parsed = AITextContentFormat(rawValue: value) else {
                        error("format must be plain, markdown, or json")
                        return
                    }
                    format = parsed
                } else {
                    format = .plain
                }
                jsonAfterMain {
                    let opened = InboundBus.shared.beginStream(
                        origin: .mcp(client: client),
                        streamID: sid,
                        title: title,
                        format: format
                    ) != nil
                    if opened {
                        return (["jsonrpc": "2.0", "id": id, "result": [
                            "content": [["type": "text", "text": "stream \(sid) open"]],
                            "structuredContent": ["stream_id": sid],
                            "isError": false,
                        ]], "200 OK")
                    }
                    return (["jsonrpc": "2.0", "id": id, "result": [
                        "content": [["type": "text", "text": "stream could not be opened"]],
                        "isError": true,
                    ]], "200 OK")
                }
            case "buffer_stream_append":
                guard let sid = args["stream_id"] as? String,
                      let delta = args["delta"] as? String else {
                    error("stream_id and delta are required")
                    return
                }
                jsonAfterMain {
                    let result = InboundBus.shared.appendStream(streamID: sid, delta: delta)
                    let text: String
                    let isError: Bool
                    switch result {
                    case .appended:
                        text = "appended"
                        isError = false
                    case .notFound:
                        text = "stream was not found"
                        isError = true
                    case .tooLarge:
                        text = "stream text is too large"
                        isError = true
                    }
                    return (["jsonrpc": "2.0", "id": id, "result": [
                        "content": [["type": "text", "text": text]],
                        "isError": isError,
                    ]], "200 OK")
                }
            case "buffer_stream_end":
                guard let sid = args["stream_id"] as? String else {
                    error("stream_id is required")
                    return
                }
                jsonAfterMain {
                    let ended = InboundBus.shared.endStream(streamID: sid)
                    let text = ended
                        ? "stream committed to Mailbox for review"
                        : "stream could not be committed to Mailbox"
                    return (["jsonrpc": "2.0", "id": id, "result": [
                        "content": [["type": "text", "text": text]],
                        "isError": !ended,
                    ]], "200 OK")
                }
            default:
                json(["jsonrpc": "2.0", "id": id, "error": ["code": -32602, "message": "unknown tool"]])
            }
        }

        private static func rejectionMessage(
            _ reason: InboundBus.SubmissionRejection
        ) -> String {
            switch reason {
            case .empty: return "text is empty"
            case .blocked: return "source is blocked"
            case .full: return "Mailbox review queue is full"
            case .tooLarge: return "text is too large"
            case .persistenceUnavailable: return "Mailbox persistence is unavailable"
            }
        }

        private static func rejectionHTTPStatus(
            _ reason: InboundBus.SubmissionRejection
        ) -> String {
            switch reason {
            case .empty: return "400 Bad Request"
            case .blocked: return "403 Forbidden"
            case .full: return "429 Too Many Requests"
            case .tooLarge: return "413 Content Too Large"
            case .persistenceUnavailable: return "503 Service Unavailable"
            }
        }

        // Protocol revisions we can speak, newest first. `initialize` negotiates
        // one of these; anything else on MCP-Protocol-Version is a 400.
        private static let supportedProtocolVersions = ["2025-06-18", "2025-03-26", "2024-11-05"]
        private static let latestProtocolVersion = "2025-06-18"
        static func linuxContract() -> [String: Any] {
            ["versions": supportedProtocolVersions, "latest": latestProtocolVersion,
             "tools": toolList]
        }


        // Give-only tools: an agent can push, never read the buffer or deliver.
        // `readOnlyHint: false` (they mutate the inbox) + no destructive/open-world
        // effects — the annotations any 2025-06-18 client can surface to the user.
        private static let toolList: [[String: Any]] = [
            ["name": "buffer_push",
             "title": "送入缓冲区",
             "description": "把一段文字送进 \(ProductIdentity.displayName) 的缓冲区收件箱，等用户确认后上屏。不会自动上屏。",
             "inputSchema": ["type": "object",
                             "properties": ["text": ["type": "string", "description": "要送入的文字"],
                                            "title": ["type": "string", "description": "可选来源标题"],
                                            "format": ["type": "string", "enum": ["plain", "markdown", "json"], "description": "可选正文格式"]],
                             "required": ["text"]],
             "annotations": ["title": "送入缓冲区", "readOnlyHint": false, "destructiveHint": false, "openWorldHint": false]],
            ["name": "buffer_stream_begin",
             "title": "开始流式条目",
             "description": "开一个流式条目，返回 stream_id。",
             "inputSchema": ["type": "object", "properties": [
                "title": ["type": "string"],
                "format": ["type": "string", "enum": ["plain", "markdown", "json"]],
             ]],
             "annotations": ["title": "开始流式条目", "readOnlyHint": false, "destructiveHint": false, "openWorldHint": false]],
            ["name": "buffer_stream_append",
             "title": "追加流式内容",
             "description": "向流式条目追加文字（原位更新，不产生新条目）。",
             "inputSchema": ["type": "object",
                             "properties": ["stream_id": ["type": "string"], "delta": ["type": "string"]],
                             "required": ["stream_id", "delta"]],
             "annotations": ["title": "追加流式内容", "readOnlyHint": false, "destructiveHint": false, "openWorldHint": false]],
            ["name": "buffer_stream_end",
             "title": "结束流式条目",
             "description": "结束流式条目，使其可被接受。",
             "inputSchema": ["type": "object", "properties": ["stream_id": ["type": "string"]],
                             "required": ["stream_id"]],
             "annotations": ["title": "结束流式条目", "readOnlyHint": false, "destructiveHint": false, "openWorldHint": false]],
        ]
    }
}
