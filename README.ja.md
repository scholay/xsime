# XSIME

[![中文](.github/readme/labels/zh.svg)](README.md) [![English](.github/readme/labels/en.svg)](README.en.md) [![日本語](.github/readme/labels/ja.svg)](README.ja.md) [![한국어](.github/readme/labels/ko.svg)](README.ko.md) [![Español](.github/readme/labels/es.svg)](README.es.md)

オープンソースの精神に敬意を表します。本プロジェクトの中国語入力ロジックは [RIME 入力メソッドエンジン](https://rime.im/) に基づいています。
複数のオペレーティングシステムに対応する入力メソッドです。個人の使い方を受け止める、三つの独自のスロットがあります。1. 確定前の明示的なバッファ（Buffer） 2. クリップボード履歴、スクリーンショット、個人の知識ベースを扱う記憶カプセル（Capsule） 3. 外部からの情報を受け取る会話ウィンドウ（Mailbox）。

全拼、双拼、声筆、五筆、英語の方案に対応し、速記で使う並打入力と、カスタム方案の読み込みにも対応します。はじめて使う人向けに、インストールパッケージは librime と辞書を**同梱**し、すぐ使えます。

5つのプラットフォームに **五筆 86** と **XSIME Wubi** を内蔵しています。五筆 86 は `z` に続けて全拼を入力して文字を検索でき、候補の括弧内に五筆コードを表示します。XSIME Wubi は切り替えなしで五筆と全拼を検索し、完全一致の五筆を優先します。両方案とも4打鍵で候補が一意なら自動確定し、重複コードと全拼検索では候補選択を維持します。[五筆ガイド](docs/wubi.md)を参照してください。

> 製品・プロジェクト・アーキテクチャ名を **XSIME** に統一します。簡体字名は **犀术IME**、繁体字名は **犀術IME** です（旧 RIMES / Lingxi IME）。公開済みアプリ、ストア情報、届出は順次更新します。[命名・互換性方針](docs/development/branding.md)。

## デモ動画

- [Bilibili · 全体紹介](https://www.bilibili.com/video/BV17XuH6SEDg/)
- [Douyin · 製品デモ](https://www.douyin.com/video/7671078195197742355)

## 何を解決するか

一つの入力メソッドで、翻訳、生成、推敲といった日常の作業を、今開いているアプリを離れずに済ませます。入力メソッドとプラグインが担います。

- **Buffer**（`⌘⇧B`）：確定前のテキスト作業台。中国語も英語も、まずバッファに入ります。リアルタイム翻訳や、選んだ AI コネクタによる生成と書き直しができます。確認したあと、現在の入力欄へ**明示的に送ります**。
- **Capsule**（`⌘⇧C`）：画面下端のバー。コピーした直後のテキスト、リンク、画像、ファイル、色は「最近」に出ます。残しておきたいものは、ノート、画像、PDF、スキル、パスワードに収めます。
- **Mailbox**（`⌘⇧M`）：AI の会話、メモ、確認待ちの外部プッシュは、このウィンドウに残ります。新しい会話を始め、設定済みのコネクタを選べます。

## 主な機能について

> インストール後、グラフィカルログインに入ると、一度きりのバックグラウンド処理が `open -g` で同じ XSIME プロセスを起動します。そのため Buffer、Clipboard History、Mailbox、Capsule のグローバルショートカットは、どの入力メソッドからでも使えます。
>
> リリースパッケージはシステム payload を置き換える前に、この Mac の通常アカウントをすべて監査します。postinstall が退役させられる現在の GUI ユーザーの開発版を除き、同じ ID の開発版アプリやジョブが見つかった場合、または home を安全に確認できない場合は、すぐに失敗します。postinstall が開発版を退役させ、もう一度監査したあとで、ロールバックできるトランザクションとしてシステムジョブを更新します。ログイン guard は、その後に現れた開発版の痕跡を防御的に止めるだけです。どちらのジョブも `KeepAlive` を設定せず、二つ目の UI/IME サービスも起動しません。
>
> Mailbox と Capsule は、キーボードフォーカスを普通に取る管理ウィンドウです。ほかの入力メソッドからショートカット（または Mailbox の通知）で開くとき、XSIME は先に自分を現在の入力メソッドへ切り替えてから開きます。閉じても元には戻さず、その後自分でほかの入力メソッドへ切り替えても、その選択は取り消されません。そのときは下記のとおり機能が制限されます。
>
> これらの機能は、ほかの入力メソッドの IMK クライアントに触れず、その未確定文字列を読んだり、確定したり、取り消したりしません。注入するキーは、Capsule を有効化したときの任意の `⌘V` 一回だけです（下記の Capsule バーを参照）。設定のショートカットも入力メソッドをまたいで使えます。Mailbox と Capsule のページは設定と状態だけを示し、実際の会話と内容はそれぞれのウィンドウに残します。

| 機能 | ショートカット | 内容 | 操作 | 保存 | 境界 |
|---|---|---|---|---|---|
| 入力方案 | — | 霧凇全拼、自然碼、小鶴、五筆 86、XSIME Wubi、英語 | — | — | — |
| バッファ | `⌘⇧B` | 確定前のテキスト | 先に XSIME へ切り替え、取り込んで分割して送る | — | ほかへ切り替えたあとはシステムクリップボードのみ。IMK は使わない |
| Capsule バー | `⌘⇧C` | 直前のコピー。ノート、画像、PDF、スキル、パスワード | クリックで選択、ダブルクリックまたは Return で貼り付け。`⌘S` で保存 | この Mac のみ | 貼り付けにはアクセシビリティが必要。未許可ならクリップボードへ入れるだけ |
| Mailbox | `⌘⇧M` | AI の会話、メモ、確認待ちのプッシュ | 新しい会話。最初の Return で生成を始める | モデルはその会話だけに結び付く | CLI はそれぞれの既定モデル |
| Capsule 管理 | 歯車または筆 | 五種類の項目。プレビューとコピー | 四組の並打のあとパスワードを表示。最長 15 秒 | iCloud は任意。パスワードと鍵はこの Mac に残す | 合言葉はローカルのダイジェストだけを保存 |
| 設定 | `⌘⇧S` | ショートカット、状態、同期、セキュリティ | 現在の入力メソッドが XSIME のときだけ開く | — | Mailbox / Capsule のウィンドウは埋め込まない |

リアルタイム翻訳、AI 生成、意識の流れ入力はバッファプラグインです。並打は組み込み拡張です。バージョンと ID は下の一覧を見てください。

| 名前 | 種類 | 説明 | 既定 |
|---|---|---|---|
| リアルタイム翻訳 | バッファプラグイン | Apple のオンデバイス翻訳。AI も使える | 有効。macOS 15+ |
| AI 生成 | バッファプラグイン | Codex、Claude Code、または OpenAI 互換 API。Plain / Markdown / JSON。結果は Buffer に残り、自分で確定する | 有効 |
| 意識の流れ入力 | バッファプラグイン | 拼音または並打を選んだ AI に渡し、互いに排他的な候補を最大 5 つ返す | 有効。選んでから送る |
| 並打 | 組み込み拡張 | 同時押しと左右分け打ち。キー配列はカスタムできる | 無効 |

## 同梱のバッファプラグイン

下の表は [`Catalog/buffer-plugins.json`](Catalog/buffer-plugins.json) から生成されています。プラグインを更新するときはバージョンも更新し、`python3 scripts/sync-buffer-plugin-catalog.py --check` を通してください。

| プラグイン | ID | バージョン | 既定の導入 | 既定の状態 |
|---|---|---:|---|---|
| ChatGPT | `builtin.codex-cli` | 1.1 | XSIME に同梱 | 有効 |
| Claude | `builtin.claude-code-cli` | 1.1 | XSIME に同梱 | 有効 |
| AI API | `builtin.openai-compatible` | 1.0 | XSIME に同梱 | 有効 |
| Reference | `builtin.scholay` | 0.1 | XSIME に同梱 | 有効 |
| Polisher | `builtin.polisher` | 0.1 | XSIME に同梱 | 有効 |
| LaTeX | `builtin.latex` | 0.1 | XSIME に同梱 | 有効 |
| リアルタイム翻訳 | `builtin.apple-translation` | 2.2 | XSIME に同梱 | 有効 |
| 意識の流れ入力 | `builtin.stream-input` | 1.4 | XSIME に同梱 | 有効 |
| 電子音楽 | `builtin.music` | 0.2.3 | XSIME に同梱 | 有効 |
| モールス符号 | `builtin.morse` | 0.1.0 | XSIME に同梱 | 有効 |

表のプラグインはすべて XSIME に同梱され、新規インストール後は既定で有効です。

## 組み込み拡張

| 拡張 | 安定 ID | バージョン | 既定の状態 |
|---|---|---:|---|
| 統計 | `builtin.statistics` | 2.0 | 有効 |
| タイピング速度 | `builtin.typing-speed` | 2.0 | 有効 |
| 並打 | `builtin.fly-chord-learning` | 2.0 | 無効 |

## インストール

[1.2.0 リリース](https://github.com/scholay/xsime/releases/tag/v1.2.0)から、macOS PKG、Android APK／AAB、Windows EXE／ZIP、Linux DEB（Ubuntu 24.04 / 26.04 amd64／Fcitx5）と再署名用の未署名 iOS IPA をダウンロードできます。macOS は署名・公証済み、Android は従来の長期署名鍵を使用し、Windows EXE は未署名です。iOS は [App Store](https://apps.apple.com/us/app/lingxi-ime/id6814620242)／[TestFlight 公開招待](https://testflight.apple.com/join/Kdj9RB4q)を利用してください。インストール可能なバージョンは Apple のページに表示されます。5つのプラットフォームで共通の Rime ZIP やコード付き UTF-8 TXT／TSV 辞書を読み込めます。[インポートガイド](docs/portable-scheme-import.md)と[1.2.0 の検証記録](docs/releases/1.2.0.md)を参照してください。ソースからのローカルビルドも可能です。

```bash
git clone --recurse-submodules https://github.com/scholay/xsime.git
cd xsime
```

| プラットフォーム | 進捗 | ビルド |
|---|---|---|
| macOS | 入力メソッドに加え、Buffer、Capsule、Mailbox | `./build_install.sh` |
| iOS | キーボードと本体アプリ（iOS 17+）。オフラインの拼音、自然碼、五筆、英語、および Buffer | Xcode で [`platforms/ios/RIMES.xcodeproj`](platforms/ios/README.md) を開く |
| Windows | ネイティブ TSF 入力メソッド、Buffer、並打、公式プラグイン設定。x64 / x86 対応 | [`platforms/windows/native/README.md`](platforms/windows/native/README.md) を参照 |
| Android | ネイティブ InputConnection キーボード、Buffer、6つの公式プラグイン、設定可能な AI サービス | [`platforms/android/README.md`](platforms/android/README.md) を参照 |
| Linux | Fcitx5 入力メソッド、Buffer、Capsule、Mailbox | [`platforms/linux/ime/README.md`](platforms/linux/ime/README.md) を参照 |

## ドキュメント

| ドキュメント | 内容 |
| --- | --- |
| [ドキュメント一覧](docs/README.md) | 現行ガイドと各プラットフォームの入口 |
| [システム構成](docs/architecture/system.md) | 入力、Buffer、共通処理とデータ境界 |
| [開発・保守](docs/development/maintenance.md) | 主線、プラグイン依存、CI と検証 |
| [リリース手順](docs/release/README.md) | ビルド、署名、更新と配布 |
| [変更履歴](CHANGELOG.md) | 公開済みバージョンの変更 |

## アプリの更新

1.1.6 から、5つのプラットフォームの設定に「更新を確認」があります。macOS は署名・公証済み PKG、Windows は完全な EXE、Android は従来の鍵で署名した APK をダウンロードし、システムのインストーラで更新します。Linux は Ubuntu 24.04 amd64 向けの DEB を取得し、システムのパッケージ管理に権限を与えてインストールします。Windows EXE は現在未署名です。

アプリを再度開き、実行中のバージョンを確認してから完了を表示します。設定、取り込んだ入力方案、学習記録は保持します。システムが認証、ログアウト、再起動を求めた場合は案内に従ってください。旧版に更新機能がない場合は、上記の配布ページから一度手動で更新してください。

iOS は実際に提供されているストア版を確認し、App Store でダウンロード・インストールします。テスト版は TestFlight が管理します。GitHub の公開だけでは iOS の公開を意味しません。

4つのインストーラは [`scholay/xsime`](https://github.com/scholay/xsime/releases/latest) の同じ `vX.Y.Z` Release に公開します。CI はすべての配布ファイルをアップロードして読み戻した後、最後に共通の `update-manifest.json` を公開します。プレビュー版は正式更新の対象外です。iOS は独立した `ios-vX.Y.Z` のストア向け手順を使います。

保守担当者は5つのプラットフォームの main CI が通過してから公開します。[docs/release/README.md](docs/release/README.md) と [CHANGELOG.md](CHANGELOG.md) を参照してください。

```bash
./scripts/release.sh stable --dry-run  # バージョン、5端の CI、公開条件を確認
./scripts/release.sh stable            # 正式 tag を作り、GitHub Actions で構築・公開
```

## リンク

- [<img src=".github/readme/friends/rime.png" width="20" height="20" alt="" align="absmiddle"> RIME 入力メソッドエンジン](https://rime.im/) — 本プロジェクトの中国語入力は RIME に基づいています。
- [<img src=".github/readme/friends/linux-do.png" width="20" height="20" alt="" align="absmiddle"> Linux.do](https://linux.do/) — 誠実で、友好的で、結束し、専門的なコミュニティ Linux.do と、その仲間たちに感謝します。
- [<img src=".github/readme/friends/irime.jpg" width="20" height="20" alt="" align="absmiddle"> iRime](https://github.com/jimmy54/iRime) — XSIME への指導と紹介に、iRime の作者へ感謝します。
- [<img src=".github/readme/friends/scholay.svg" width="20" height="20" alt="" align="absmiddle"> Scholay](https://www.scholay.com/t/M4ZZW8KRE5) — 最先端の研究とイノベーションに基づくアーキテクチャの指針を提供していただき、感謝します。
- [<img src=".github/readme/friends/enterscholar.svg" width="20" height="20" alt="" align="absmiddle"> EnterScholar コミュニティ](https://enterscholar.com/) — AI 時代の研究者向けコミュニティプラットフォームに感謝します。

## 貢献者

一覧は [CONTRIBUTORS.md](CONTRIBUTORS.md) にあります。

本プロジェクトの中心的なメンテナーは、プログラマー出身ではない、一般利用者向けのプロダクトマネージャーです。vibe coding の時代に感謝しています。

**AI プログラミング支援**：Claude、Cursor、Codex、Grok が設計、実装、レビューに加わりました。

## 既知の問題

- **Linux：デプロイが終わる前に Fcitx5 を終了すると、プロセスが数分残ることがある。** 初回またはバックグラウンドの librime デプロイは途中で取り消せません。辞書が大きいほど顕著です。[#43](https://github.com/scholay/xsime/issues/43) を参照。
- **Linux：Buffer のツールバーをドラッグした直後に別の入力欄をクリックすると、キャプチャが開いたままになることがある。** 今のところ X11 の Firefox だけで再現します。離してから約 10 ミリ秒以内に、同じウィンドウの別の入力欄を押した場合です。もう一度クリックするか Esc で戻ります。[#44](https://github.com/scholay/xsime/issues/44) を参照。

## ライセンスと第三者

XSIME 自身のコードは [Apache License 2.0](LICENSE) を採用しています。適用範囲と従来の MIT 許諾については [LICENSING.md](LICENSING.md) を参照してください。コアメンテナーは[学术海](https://pm.scholay.com)です。

中国語入力には [Rime 入力エンジン（librime）](https://github.com/rime/librime)を使用しています。[NOTICE](NOTICE) に帰属情報、[ATTRIBUTION.md](ATTRIBUTION.md) に任意の表示例を記載しています。表示の推奨は追加のライセンス条件ではありません。

第三者のコンポーネント、方案、辞書、Lua/OpenCC データはそれぞれのライセンスとクレジットを保ちます。[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)、[LICENSES](LICENSES)、`rime-data/licenses/` を参照してください。公式の無料プラグインは [rimes-plugins](https://github.com/scholay/rimes-plugins) で管理します。
