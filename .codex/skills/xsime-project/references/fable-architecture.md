# Fable Architecture Notes

## Purpose

Fable's version turned the prototype into a real macOS input method instead of an external text injector. The important change is not visual polish; it is the boundary: IMK owns text delivery, librime owns input logic, and the custom UI stays in the same process.

## Why The Previous Attempts Failed

- AX/paste injection is not a reliable way to put text into arbitrary web, Electron, terminal, and native fields.
- A separate candidate UI synchronized by `state.json` adds a failure dependency: if the UI process is late or dead, typing behavior and display diverge.
- Avoiding `setMarkedText` lets hostile clients echo raw keystrokes, producing bugs like `zuoye作业`.
- Without a marked-text session, many clients return a zero caret rect, so candidate windows drift to screen corners or fallback positions.
- A shared Rime session lets composition state bleed between focused fields.
- Sharing Squirrel's `~/Library/Rime` userdb while Squirrel is running can trip LevelDB locks and silently damage candidate availability.

## Non-Negotiable Contracts

1. **Single process:** keep `IMKInputController`, `CRimeBridge`, `CandidateWindow`, `BufferModel`, `BufferWindowController`, `StatusMenu`, and settings inside `RIMES.app`.
2. **Native delivery:** every commit, raw fallback, chord commit, and buffer flush enters the client through `Delivery.insert`.
3. **Marked text while composing:** use `CompositionSession.update` for active preedit and `CompositionSession.clear` only when composition is resolved.
4. **Per-client session:** each controller owns its Rime session; the librime process is global, but session state is not shared.
5. **Rime first for bindings:** feed relevant modifier combinations to Rime before falling through, except Command-key app shortcuts, which force-commit then return `false`.
6. **Chord is schema-gated:** release replay only runs for `my_combo`; sequential schemas must not see synthetic release events.
7. **Bridge ABI is fragile:** `RimeApi` field order is memory layout. Do not reorder or prune fields. Add new exported wrappers at the C API boundary.
8. **User data isolation:** keep `~/Library/RIMES` as the active user dir while Squirrel is installed and running as fallback.
9. **Fallback cannot drop printable text:** if Rime is unhealthy or no session exists, printable keys and Return must still insert.

## Key Files

- `Sources/RimeBuffer/RIMESController.swift`: lifecycle, key routing, commit drain, schema switching, UI update.
- `Sources/RimeBuffer/CompositionSession.swift`: marked-text protocol, inline vs placeholder.
- `Sources/RimeBuffer/CandidateWindow.swift`: nonactivating candidate panel, caret/cached/fallback positioning, mouse selection.
- `Sources/RimeBuffer/ChordController.swift`: delayed release replay for `my_combo`.
- `Sources/RimeBuffer/RimeKey.swift`: X11 keysym mapping and modifier masks.
- `Sources/RimeBuffer/RimeEngine.swift`: Swift wrapper over the bridge, per-session calls.
- `Sources/CRimeBridge/CRimeBridge.cpp`: librime dlopen, vtable, context/status wrappers.
- `build_install.sh`: seed `~/Library/RIMES` (copying pre-rename `~/Library/RimeBuffer` data first), build, sign, install, register.

## Validation Checklist

- `swift build -c debug`
- `.build/debug/RimeBuffer smoke` when bridge/Rime/schema/key code changed.
- `tail -f ~/rimebuffer.log` during manual typing tests.
- Manual tests for IME changes: Safari, Electron/Codex, terminal, WeChat or another hostile field.
- Verify candidate window tracks caret, raw letters do not leak, F4/Ctrl-grave switcher works, buffer mode still routes commits through the same drain path.
