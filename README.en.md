# XSIME

Official website: [xsime.com](https://xsime.com).

[![中文](.github/readme/labels/zh.svg)](README.md) [![English](.github/readme/labels/en.svg)](README.en.md) [![日本語](.github/readme/labels/ja.svg)](README.ja.md) [![한국어](.github/readme/labels/ko.svg)](README.ko.md) [![Español](.github/readme/labels/es.svg)](README.es.md)

With respect for open source, the Chinese input logic of this project is based on the [RIME input method engine](https://rime.im/).
XSIME is an input method for more than one operating system. Three original slot-like surfaces carry what you want to do: 1. an explicit buffer before text is committed 2. Capsule, for clipboard history, screenshots, and a personal knowledge base 3. Mailbox, a conversation window for information that arrives from outside.

Schemes include full Pinyin, double Pinyin, Shengbi, Wubi, and English, plus the chorded input used in stenography and custom imports. For new users, the package **bundles** librime and the dictionaries and is ready to use.

All five platforms include **Wubi 86** and **XSIME Wubi**. Wubi 86 supports `z` followed by Pinyin, such as `zni`, with Wubi codes shown in parentheses. XSIME Wubi accepts Wubi codes and Pinyin without switching modes, prioritizing exact Wubi matches. Both schemes automatically commit a unique four-key Wubi candidate; ambiguous codes and Pinyin lookups retain candidate selection. See the [Wubi guide](docs/wubi.md).

> **XSIME** is the unified product, project and architecture name: **犀术IME** in Simplified Chinese and **犀術IME** in Traditional Chinese (formerly RIMES / Lingxi IME). Branding is in transition; installed apps, store listings and filings are updated separately. See [Branding and compatibility](docs/development/branding.md).

## Demo videos

- [Bilibili — full walkthrough](https://www.bilibili.com/video/BV17XuH6SEDg/)
- [Douyin — product demo](https://www.douyin.com/video/7671078195197742355)

## What problem it solves

One input method covers everyday translation, generation, and polishing, without leaving the app you are in. The input method and its plug-ins do the work:

- **Buffer** (`⌘⇧B`): a text workbench before commit. Chinese or English enters the buffer first, where you can translate it live or generate and rewrite it with the selected AI connector. After you confirm, it is **explicitly delivered** into the current field.
- **Capsule** (`⌘⇧C`): a bar at the bottom of the screen. Text, links, images, files, and colors you just copied appear in Recent. What you want to keep goes into notes, images, PDFs, skills, or passwords.
- **Mailbox** (`⌘⇧M`): AI conversations, notes, and external items waiting for review stay in this window. You can start a conversation and choose a connector you have already configured.

## Notes on the main capabilities

> After installation and a graphical login, a one-shot background job uses `open -g` to start the same XSIME process, so the global shortcuts for Buffer, Clipboard History, Mailbox, and Capsule work across input methods.
>
> Before a release package replaces the system payload, it audits every ordinary local account. It fails immediately if it finds a same-ID development app or job other than the current GUI user's development install, which postinstall can retire, or if a home cannot be checked safely. Postinstall retires that development install, audits again, and only then updates the system job as a transaction that can be rolled back. The login guard only stops later if development traces appear. Neither job sets `KeepAlive`, and neither starts a second UI/IME service.
>
> Mailbox and Capsule are ordinary windows that take keyboard focus. Opening either from another input method, by shortcut or by a Mailbox notification, switches to XSIME first so the feature is complete. Closing it does not switch back, and a later switch you make yourself is left as you set it. The feature then falls back as described below.
>
> These features do not touch another input method's IMK client, and they do not read, commit, or cancel its composition. The only injected key is the optional single `⌘V` when Capsule activates (see the Capsule bar below). The Settings shortcut also works across input methods. Its Mailbox and Capsule pages show configuration and status only. Conversations and records stay in their own windows.

| Capability | Shortcut | Contents | Action | Storage | Limit |
|---|---|---|---|---|---|
| Input schemes | — | Rime Ice full Pinyin, Natural Code, Xiaohe, Wubi 86, XSIME Wubi, English | — | — | — |
| Buffer | `⌘⇧B` | Text before it is committed | Switch to XSIME, then capture and deliver in chunks | — | After you switch away, only the system clipboard is used, not IMK |
| Capsule bar | `⌘⇧C` | Recent copies; notes, images, PDFs, skills, passwords | Click to select, double-click or Return to paste. `⌘S` saves | This Mac only | Paste needs Accessibility; otherwise it only reaches the clipboard |
| Mailbox | `⌘⇧M` | AI conversations, notes, items waiting for review | New conversation; the first Return starts generation | The model is bound to that conversation | CLIs use their default model |
| Capsule manager | Gear or brush | Five kinds of items, with preview and copy | Four chords reveal a password, for at most 15 seconds | Optional iCloud; passwords and keys stay on this Mac | The passphrase is stored only as a local digest |
| Settings | `⌘⇧S` | Shortcuts, status, sync, and security | Opens only while XSIME is the current input method | — | Does not embed the Mailbox or Capsule windows |

Live translation, AI generation, and stream input are buffer plug-ins. Chording is a built-in extension. Versions and IDs are in the lists below.

| Name | Kind | Notes | Default |
|---|---|---|---|
| Live translation | Buffer plug-in | Apple on-device translation, or AI | Enabled, macOS 15+ |
| AI generation | Buffer plug-in | Codex, Claude Code, or an OpenAI-compatible API. Plain / Markdown / JSON stays in Buffer until you commit it | Enabled |
| Stream input | Buffer plug-in | Pinyin or chords go to the selected AI, which returns at most 5 mutually exclusive guesses | Enabled; delivered only after you choose one |
| Chording | Built-in extension | Combined chords and left-then-right split strokes, plus custom keymaps | Disabled |

<!-- BEGIN PRESET BUFFER PLUGINS -->
## Official macOS plug-ins

This table is generated from [`Catalog/buffer-plugins.json`](Catalog/buffer-plugins.json). Every plug-in update must also update its catalog version and pass `python3 scripts/sync-buffer-plugin-catalog.py --check`.

| Plug-in | ID | Version | Default installation | Default state |
|---|---|---:|---|---|
| Translation | `builtin.apple-translation` | 2.3.0 | Bundled with XSIME | Enabled |
| Capture | `builtin.capsule.capture` | 1.1.0 | Bundled with XSIME | Enabled |
| Notes | `builtin.capsule.notes` | 1.1.0 | Bundled with XSIME | Enabled |
| Passwords | `builtin.capsule.passwords` | 1.1.0 | Bundled with XSIME | Enabled |
| Resources | `builtin.capsule.resources` | 1.1.0 | Bundled with XSIME | Enabled |
| Temporary | `builtin.capsule.temporary` | 1.1.0 | Bundled with XSIME | Enabled |
| Claude | `builtin.claude-code-cli` | 1.2.0 | On demand in Settings | Disabled |
| ChatGPT | `builtin.codex-cli` | 1.2.0 | On demand in Settings | Disabled |
| Chord input | `builtin.fly-chord-learning` | 2.1.0 | Bundled with XSIME | Disabled |
| LaTeX | `builtin.latex` | 1.1.0 | On demand in Settings | Disabled |
| Chat | `builtin.mailbox.chat` | 1.1.0 | Bundled with XSIME | Enabled |
| Inbox | `builtin.mailbox.inbox` | 1.1.0 | Bundled with XSIME | Enabled |
| Terminal | `builtin.mailbox.terminal` | 1.1.0 | Bundled with XSIME | Enabled |
| Morse Code | `builtin.morse` | 0.1.0 | On demand in Settings | Disabled |
| Electronic Music | `builtin.music` | 0.2.3 | On demand in Settings | Disabled |
| AI API | `builtin.openai-compatible` | 1.1.0 | On demand in Settings | Disabled |
| Polisher | `builtin.polisher` | 1.1.0 | On demand in Settings | Disabled |
| Reference | `builtin.scholay` | 0.1.0 | On demand in Settings | Disabled |
| Statistics | `builtin.statistics` | 2.1.0 | Bundled with XSIME | Enabled |
| Stream of Consciousness Input | `builtin.stream-input` | 1.4.0 | Bundled with XSIME | Enabled |
| Typing practice | `builtin.typing-speed` | 2.1.0 | Bundled with XSIME | Enabled |

Bundled plug-ins use the defaults above. Optional plug-ins require installation and explicit enablement. Upgrades preserve existing plugin state.
<!-- END PRESET BUFFER PLUGINS -->

## Built-in extensions

| Extension | Stable ID | Version | Default state |
|---|---|---:|---|
| Statistics | `builtin.statistics` | 2.0 | Enabled |
| Typing Speed | `builtin.typing-speed` | 2.0 | Enabled |
| Chording | `builtin.fly-chord-learning` | 2.0 | Disabled |

## Install

The [1.2.0 release](https://github.com/scholay/xsime/releases/tag/v1.2.0) provides macOS PKG, Android APK/AAB, Windows EXE/ZIP, Linux DEB packages, and an unsigned iOS community IPA. All five platforms support importing the same standard Rime ZIP or encoded UTF-8 TXT/TSV dictionary; see the [import guide](docs/portable-scheme-import.md).

| Platform | Installation |
|---|---|
| macOS | [1.2.0 PKG](https://github.com/scholay/xsime/releases/download/v1.2.0/RIMES-1.2.0.pkg), Apple Silicon and Intel, signed and notarized |
| Android | [1.2.0 APK](https://github.com/scholay/xsime/releases/download/v1.2.0/RIMES-Android-1.2.0.apk), version code 23, signed with the existing long-term key |
| Windows | [1.2.0 EXE](https://github.com/scholay/xsime/releases/download/v1.2.0/RIMES-Windows-1.2.0-Setup.exe), x64 Windows with x64/x86 TSF; unsigned EXE |
| Linux | [1.2.0 DEB](https://github.com/scholay/xsime/releases/download/v1.2.0/fcitx5-rimes_1.2.0_amd64.deb), Ubuntu 24.04 / 26.04 amd64 / Fcitx5 |
| iOS | [1.2.0 community IPA and signing notes](https://github.com/scholay/xsime/releases/tag/v1.2.0); [App Store](https://apps.apple.com/us/app/lingxi-ime/id6814620242) / [public TestFlight invitation](https://testflight.apple.com/join/Kdj9RB4q). Apple’s pages show the currently available builds. |

See the [1.2.0 release record](docs/releases/1.2.0.md) for build, import, and installation validation.

A user has reported host-app crashes on macOS 27 when switching to XSIME in the App Store sandboxed versions of WeChat and QQ after installation. If affected, switch to another input method and follow the [per-app input-source cache backup and recovery guide](docs/macos/input-source-cache-recovery.md#english). The installer does not clear other apps' caches; restarting the app or logging out is not a guaranteed fix. Investigation remains open in [#90](https://github.com/scholay/xsime/issues/90).

To build locally, clone the source and its pinned official plugins:

```bash
git clone --recurse-submodules https://github.com/scholay/xsime.git
cd xsime
```

| Platform | Status | Build |
|---|---|---|
| macOS | Input method, plus Buffer, Capsule, and Mailbox | `./build_install.sh` |
| iOS | Keyboard and main app (iOS 17+): offline Pinyin, Natural Code, Wubi, English, and Buffer | Open [`platforms/ios/RIMES.xcodeproj`](platforms/ios/README.md) in Xcode |
| Windows | Native TSF input method, Buffer, chording and official plugin settings; x64 / x86 | See [`platforms/windows/native/README.md`](platforms/windows/native/README.md) |
| Android | Native InputConnection keyboard, Buffer, six official plugins and configurable AI services | See [`platforms/android/README.md`](platforms/android/README.md) |
| Linux | Fcitx5 input method, Buffer, Capsule, and Mailbox | See [`platforms/linux/ime/README.md`](platforms/linux/ime/README.md) |

## Documentation

| Documentation | Contents |
| --- | --- |
| [Documentation index](docs/README.md) | Current guides and platform entry points |
| [System architecture](docs/architecture/system.md) | Native input, Buffer, shared code and data boundaries |
| [Development workflow](docs/development/maintenance.md) | Mainline, plugin dependencies, remote CI and acceptance |
| [Release workflow](docs/release/README.md) | Builds, signing, updates and distribution |
| [Changelog](CHANGELOG.md) | Published version changes |

## Application updates

All five platforms offer **Check for updates** in Settings. macOS downloads a signed, notarized PKG, Windows the full EXE, and Android an APK signed with the existing key. Each opens the system installer. On Ubuntu 24.04 / 26.04 amd64, Linux downloads a DEB and asks you to authorize the system package manager. The Windows EXE remains unsigned.

Completion is shown after reopening the app and verifying the running version. Settings, imported schemes and learned words are preserved. Follow any system request for authorization, sign-out or restart. If your older version lacks this flow, upgrade once using the downloads above.

iOS checks the available store version and opens App Store to download and install. TestFlight manages test builds. A GitHub release does not mean the matching iOS version is available; Apple’s pages show the current status.

The four installers share one `vX.Y.Z` release on [`scholay/xsime`](https://github.com/scholay/xsime/releases/latest). CI uploads and reads back every package before publishing the shared `update-manifest.json`. Preview builds stay outside this stable channel. iOS uses a separate `ios-vX.Y.Z` store workflow.

Maintainers release after all five mainline CI workflows pass. See [docs/release/README.md](docs/release/README.md) and [CHANGELOG.md](CHANGELOG.md):

```bash
./scripts/release.sh stable --dry-run  # check versions, five-platform CI and release prerequisites
./scripts/release.sh stable            # create the stable tag for GitHub Actions to build and publish
```

## Links

- [<img src=".github/readme/friends/rime.png" width="20" height="20" alt="" align="absmiddle"> RIME input method engine](https://rime.im/) — Chinese input in this project is based on RIME.
- [<img src=".github/readme/friends/linux-do.png" width="20" height="20" alt="" align="absmiddle"> Linux.do](https://linux.do/) — Thanks to Linux.do, a sincere, friendly, united, and professional community, and to its members.
- [<img src=".github/readme/friends/irime.jpg" width="20" height="20" alt="" align="absmiddle"> iRime](https://github.com/jimmy54/iRime) — Thanks to the iRime author for guidance and for helping introduce RIMES.
- [<img src=".github/readme/friends/scholay.svg" width="20" height="20" alt="" align="absmiddle"> Scholay](https://www.scholay.com/t/M4ZZW8KRE5) — Thanks for providing architectural guidance grounded in cutting-edge research and innovation.
- [<img src=".github/readme/friends/enterscholar.svg" width="20" height="20" alt="" align="absmiddle"> EnterScholar Community](https://enterscholar.com/) — Thanks to this community platform for scholars in the age of AI.

## Contributors

The full list is in [CONTRIBUTORS.md](CONTRIBUTORS.md).

The core maintainer is a consumer-product manager, not a programmer by training, and is grateful for the era of vibe coding.

**AI programming assistants**: Claude, Cursor, Codex, and Grok took part in design, implementation, and review.

## Known issues

- **Linux: quitting Fcitx5 before a deploy finishes can leave the process running for minutes.** A first-run or background librime deploy cannot be cancelled, and a larger dictionary makes the wait longer. See [#43](https://github.com/scholay/xsime/issues/43).
- **Linux: clicking another field immediately after dragging the Buffer toolbar can leave capture on.** Reproduced only in Firefox on X11, when the click lands in the same window within about 10 ms of release. Click again or press Esc to recover. See [#44](https://github.com/scholay/xsime/issues/44).

## License and third parties

XSIME's own code is licensed under [Apache License 2.0](LICENSE), subject to the scope and historical MIT grants described in [LICENSING.md](LICENSING.md). The core maintainer is [学术海](https://pm.scholay.com).

Chinese input uses the [Rime Input Method Engine (librime)](https://github.com/rime/librime). See [NOTICE](NOTICE) for attribution and [ATTRIBUTION.md](ATTRIBUTION.md) for optional display examples explaining the relationship between Rime, XSIME and downstream maintainers. These display suggestions add no license conditions.

Third-party components, schemes, dictionaries and Lua/OpenCC data retain their own licenses and credits; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [LICENSES](LICENSES) and `rime-data/licenses/`. Official free plugins are maintained in [rimes-plugins](https://github.com/scholay/rimes-plugins).
