# Windows connectors and host shortcuts

The settings shell follows the macOS navigation structure: compact proportional
tabs, a top status/route area, and separate **AI 模型** / **本地网关** pages.

API Providers have independent identities and endpoint-bound Credential Manager
entries. Browsing a draft does not replace the current text model. “设为当前文本模型”
selects the route immediately. Text edits persist after a short debounce or on blur;
there is no Save footer. An empty key field preserves the stored key;
“清除密钥” explicitly deletes it immediately. Removing a Provider requires confirmation
and retires only its own credential in the same rollback-capable transaction.
Model discovery never follows redirects.

Codex uses `%LOCALAPPDATA%/RIMES/ai/codex-home` for both official CLI login and
generation. The connector never copies personal Codex credentials or reads its
MCP, hooks, rules, skills or projects. Login runs asynchronously, can be cancelled,
and is checked using the official CLI. Text generation continues to use the
existing restricted, ephemeral, tool-free process and validated JSONL output.
The AI page has ChatGPT, Claude and AI Provider selection cards. Selecting a supported
card immediately activates its connector. Claude and OpenRouter Native Decisions/Jev
routes are explicitly disabled until implemented. The ChatGPT panel shows availability,
authorization progress, cancellation and an explicit private login-link copy action;
path/model overrides live under Advanced. The Provider panel has documentation, model
discovery and route status, with unavailable actions disabled. Switching sidebar pages
remembers each subpage for the settings host lifetime. Small windows scroll the detail
content; disabled gateway configuration is dimmed and cannot be copied.

The local gateway binds only `127.0.0.1:47700`. `GET /v1/health` is inert;
`POST /v1/inbound` and `POST /mcp` require a dedicated bearer credential. Host and
Origin checks, bounded HTTP/JSON parsing, receive deadlines and exclusive binding
are enforced. The MCP tools are `buffer_push`, `buffer_stream_begin`,
`buffer_stream_append` and `buffer_stream_end`. Accepted content enters Mailbox
for review; no ingress endpoint reads private content, binds an input target or
delivers text. Streams are bounded and become reviewable when completed.
Disabling the gateway closes its listener and discards unfinished streams.
E2E IME brokers do not start a gateway.

Cursor, Codex, Claude Code and generic MCP configuration previews are redacted.
Only an explicit Copy action retrieves/generates the local gateway token.
Clipboard exclusion prevents copied credentials from entering Capsule collection
or Windows clipboard history. Client configuration files are never overwritten.

`Alt+Shift+B`, `Alt+Shift+C` and `Alt+Shift+M` toggle Buffer, Capsule and Mailbox.
A visible component closes even if it no longer owns input capture. Normal close
behavior, including saving Mailbox drafts and protecting Capsule edits, remains.

While Buffer owns the active TSF input lease, `Alt+Shift+Up/Down` cycles Input and
installed, enabled Buffer plugins in the same order as the mode menu. Extra
modifiers and peripheral leases do not trigger cycling. Repeats do not switch
again; the matching key release remains owned after modifiers are released.
Pending chord/preedit is settled into Buffer, never injected into the host.
Switching cancels an old producer without submitting a request or delivering text;
an outstanding delivery/ack blocks switching. API and ChatGPT plugin selection
uses the matching connector profile without overwriting the saved default route.

Validation targets: RimesRuntime, RimesConnections, RimesCodexConnector,
RimesLocalGateway, RimesSettingsUi, RimesBufferLayout, RimesBrokerClientContract.
RimesMetricsUiTests renders settings across all four themes; RimesPeripheralTests
`--ui <directory>` exercises the native component toggles. RimesConnectorProbe is
opt-in local setup/live testing and is never invoked by CTest. It emits only status
for a synthetic generation, never credentials or provider response text.


ChatGPT generation uses the app-server stdio protocol, with an application-scoped
Codex home, the same text-only permission profile and disabled tools as macOS.
The initialize and empty-MCP check precede any user prompt. Explicit model
selection is verified against thread/start; silent reroutes fail with the source
retained. `item/agentMessage/delta` updates provisional chips, public summary
notifications update a separate row, and successful turn/completed finalizes the
same chip identities. Raw reasoning is never rendered. SSE connectors provide
incremental answer chips and an activity indicator; public `reasoning_summary`
is separate from deliverable text. Unsupported CLI/failed/incomplete streams do
not silently fall back to non-streaming output.

`RimesCodexProbe <codex.exe> --streaming-handshake` is a prompt-free local protocol
check in an isolated home. It does not start a thread or invoke a model.
