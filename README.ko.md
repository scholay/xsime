# XSIME

[![中文](.github/readme/labels/zh.svg)](README.md) [![English](.github/readme/labels/en.svg)](README.en.md) [![日本語](.github/readme/labels/ja.svg)](README.ja.md) [![한국어](.github/readme/labels/ko.svg)](README.ko.md) [![Español](.github/readme/labels/es.svg)](README.es.md)

위대한 오픈 소스 정신에 경의를 표합니다. 이 프로젝트의 중국어 입력 로직은 [RIME 입력기 엔진](https://rime.im/)을 기반으로 합니다.
여러 운영체제를 지원하는 입력기입니다. 사용자의 쓰임을 받는 세 개의 독창적인 슬롯이 있습니다. 1. 확정 전의 명시적 버퍼(Buffer) 2. 클립보드 기록, 화면 캡처, 개인 지식 베이스를 다루는 기억 캡슐(Capsule) 3. 바깥에서 들어오는 정보를 받는 대화 창(Mailbox).

완전 병음, 이중 병음, 성필(声笔), 오필, 영어 방안을 지원하고, 속기의 병격 입력과 사용자 방안 가져오기도 지원합니다. 처음 쓰는 사람을 위해 설치 패키지는 librime과 사전을 **함께 담아** 바로 쓸 수 있습니다.

다섯 플랫폼 모두 **오필 86**과 **XSIME Wubi**를 제공합니다. 오필 86에서는 `z` 다음에 완전 병음을 입력해 글자를 찾을 수 있으며, 후보 옆 괄호에 오필 코드를 표시합니다. XSIME Wubi는 모드 전환 없이 오필과 병음을 검색하고 정확히 일치하는 오필을 우선합니다. 두 방안 모두 네 글자 오필 코드의 후보가 하나뿐이면 자동 확정하며, 중복 코드와 병음 검색에서는 후보 선택을 유지합니다. [오필 안내](docs/wubi.md)를 참고하세요.

> 제품·프로젝트·아키텍처 이름은 **XSIME**, 중국어 간체는 **犀术IME**, 번체는 **犀術IME**입니다(이전 RIMES / Lingxi IME). 기존 앱, 스토어 정보와 등록 정보는 별도로 순차 변경합니다. [명명 및 호환성 정책](docs/development/branding.md)을 참고하세요.

## 시연 영상

- [Bilibili · 전체 소개](https://www.bilibili.com/video/BV17XuH6SEDg/)
- [Douyin · 제품 시연](https://www.douyin.com/video/7671078195197742355)

## 무엇을 해결하는가

입력기 하나로 번역, 생성, 다듬기 같은 일상 작업을, 지금 쓰는 앱을 떠나지 않고 처리합니다. 입력기와 플러그인이 맡습니다.

- **Buffer**(`⌘⇧B`): 확정 전의 텍스트 작업대. 중국어와 영어는 먼저 버퍼로 들어갑니다. 실시간 번역을 하거나, 고른 AI 연결기로 생성하고 고칠 수 있습니다. 확인한 뒤에야 현재 입력칸으로 **명시적으로 전달**합니다.
- **Capsule**(`⌘⇧C`): 화면 아래의 막대. 방금 복사한 텍스트, 링크, 이미지, 파일, 색은 「최근」에 나타납니다. 오래 남길 것은 노트, 이미지, PDF, 스킬, 비밀번호로 넣습니다.
- **Mailbox**(`⌘⇧M`): AI 대화, 메모, 검토를 기다리는 외부 알림은 이 창에 남습니다. 새 대화를 만들고 이미 설정한 연결기를 고를 수 있습니다.

## 주요 기능에 대해

> 설치를 마치고 그래픽 로그인 세션에 들어가면, 한 번의 백그라운드 작업이 `open -g`로 같은 XSIME 프로세스를 띄웁니다. 그래서 Buffer, Clipboard History, Mailbox, Capsule의 전역 단축키는 어떤 입력기에서나 쓸 수 있습니다.
>
> 배포 패키지는 시스템 payload를 바꾸기 전에 이 Mac의 일반 계정을 모두 점검합니다. postinstall이 물러나게 할 수 있는 현재 GUI 사용자의 개발판을 빼고, 같은 ID의 개발판 앱이나 작업이 있거나 home을 안전하게 확인할 수 없으면 바로 실패합니다. postinstall이 개발판을 물러나게 하고 다시 점검한 뒤에야, 되돌릴 수 있는 트랜잭션으로 시스템 작업을 갱신합니다. 로그인 guard는 나중에 나타난 개발판 흔적만 방어적으로 막습니다. 두 작업 모두 `KeepAlive`를 두지 않으며, 두 번째 UI/IME 서비스도 시작하지 않습니다.
>
> Mailbox와 Capsule은 키보드 포커스를 보통의 방식으로 받는 관리 창입니다. 다른 입력기에서 단축키나 Mailbox 알림으로 열면, XSIME가 먼저 자신을 현재 입력기로 바꾼 다음 엽니다. 닫을 때 되돌리지 않으며, 이후에 직접 다른 입력기로 바꿔도 그 선택은 취소되지 않습니다. 그때는 아래에 적힌 대로 기능이 제한됩니다.
>
> 이 기능들은 다른 입력기의 IMK 클라이언트에 접근하지 않고, 그 조합 중인 문자열을 읽거나 확정하거나 취소하지 않습니다. 넣는 키는 Capsule을 활성화할 때의 선택적 `⌘V` 한 번뿐입니다(아래 Capsule 막대 참고). 설정의 단축키도 입력기를 넘어 쓸 수 있습니다. Mailbox와 Capsule 페이지는 설정과 상태만 보여 주고, 실제 대화와 내용은 각자 창에 둡니다.

| 기능 | 단축키 | 내용 | 조작 | 저장 | 경계 |
|---|---|---|---|---|---|
| 입력 방안 | — | Rime Ice 완전 병음, 자연마, 소학, 오필 86, XSIME Wubi, 영어 | — | — | — |
| 버퍼 | `⌘⇧B` | 확정 전의 텍스트 | 먼저 XSIME로 전환한 뒤 잡아 나누어 전달 | — | 다른 입력기로 옮긴 뒤에는 시스템 클립보드만 쓰고 IMK는 쓰지 않음 |
| Capsule 막대 | `⌘⇧C` | 최근 복사. 노트, 이미지, PDF, 스킬, 비밀번호 | 클릭은 선택, 더블 클릭 또는 Return은 붙여넣기. `⌘S`로 보관 | 이 Mac에만 | 붙여넣기에는 손쉬운 사용 권한이 필요. 없으면 클립보드에만 넣음 |
| Mailbox | `⌘⇧M` | AI 대화, 메모, 검토 대기 알림 | 새 대화. 첫 Return에 생성 시작 | 모델은 그 대화에만 묶임 | CLI는 각자 기본 모델 |
| Capsule 관리 | 톱니 또는 붓 | 다섯 종류. 미리보기와 복사 | 네 번의 병격 뒤에 비밀번호를 봄. 최대 15초 | iCloud는 선택. 비밀번호와 키는 이 Mac에 남김 | 암호는 로컬 요약만 저장 |
| 설정 | `⌘⇧S` | 단축키, 상태, 동기화, 보안 | 현재 입력기가 XSIME일 때만 열림 | — | Mailbox / Capsule 창을 안에 넣지 않음 |

실시간 번역, AI 생성, 의식의 흐름 입력은 버퍼 플러그인입니다. 병격은 내장 확장입니다. 버전과 ID는 아래 목록을 보십시오.

| 이름 | 종류 | 설명 | 기본 |
|---|---|---|---|
| 실시간 번역 | 버퍼 플러그인 | Apple 기기 내 번역. AI도 가능 | 사용. macOS 15+ |
| AI 생성 | 버퍼 플러그인 | Codex, Claude Code 또는 OpenAI 호환 API. Plain / Markdown / JSON. 결과는 Buffer에 남고 직접 확정 | 사용 |
| 의식의 흐름 입력 | 버퍼 플러그인 | 병음 또는 병격을 고른 AI에 넘겨, 서로 배타적인 추측을 최대 5개 | 사용. 고른 뒤에 전달 |
| 병격 | 내장 확장 | 동시에 누르기와 좌우를 나누어 누르기. 키 배열도 직접 정할 수 있음 | 끄기 |

## 기본 버퍼 플러그인

아래 표는 [`Catalog/buffer-plugins.json`](Catalog/buffer-plugins.json)에서 생성됩니다. 플러그인을 갱신할 때는 버전도 바꾸고 `python3 scripts/sync-buffer-plugin-catalog.py --check`를 통과해야 합니다.

| 플러그인 | ID | 버전 | 기본 설치 | 기본 상태 |
|---|---|---:|---|---|
| ChatGPT | `builtin.codex-cli` | 1.1 | XSIME에 포함 | 사용 |
| Claude | `builtin.claude-code-cli` | 1.1 | XSIME에 포함 | 사용 |
| AI API | `builtin.openai-compatible` | 1.0 | XSIME에 포함 | 사용 |
| Reference | `builtin.scholay` | 0.1 | XSIME에 포함 | 사용 |
| Polisher | `builtin.polisher` | 0.1 | XSIME에 포함 | 사용 |
| LaTeX | `builtin.latex` | 0.1 | XSIME에 포함 | 사용 |
| 실시간 번역 | `builtin.apple-translation` | 2.2 | XSIME에 포함 | 사용 |
| 의식의 흐름 입력 | `builtin.stream-input` | 1.4 | XSIME에 포함 | 사용 |
| 전자 음악 | `builtin.music` | 0.2.3 | XSIME에 포함 | 사용 |
| 모스 부호 | `builtin.morse` | 0.1.0 | XSIME에 포함 | 사용 |

표의 플러그인은 모두 XSIME에 포함되며, 새로 설치하면 기본적으로 켜집니다.

## 내장 확장

| 확장 | 안정 ID | 버전 | 기본 상태 |
|---|---|---:|---|
| 통계 | `builtin.statistics` | 2.0 | 사용 |
| 타자 속도 | `builtin.typing-speed` | 2.0 | 사용 |
| 병격 | `builtin.fly-chord-learning` | 2.0 | 끄기 |

## 설치

[1.2.0 릴리스](https://github.com/scholay/xsime/releases/tag/v1.2.0)에서 macOS PKG, Android APK/AAB, Windows EXE/ZIP, Linux DEB(Ubuntu 24.04 / 26.04 amd64 / Fcitx5)와 재서명용 미서명 iOS IPA를 받을 수 있습니다. macOS는 서명 및 공증되었고 Android는 기존 장기 서명 키를 사용하며 Windows EXE는 서명되지 않았습니다. iOS는 [App Store](https://apps.apple.com/us/app/lingxi-ime/id6814620242) / [TestFlight 공개 초대](https://testflight.apple.com/join/Kdj9RB4q)를 이용하세요. 설치 가능한 버전은 Apple 페이지에 표시됩니다. 다섯 플랫폼 모두 동일한 Rime ZIP 또는 입력 코드가 포함된 UTF-8 TXT/TSV 사전을 가져올 수 있습니다. [가져오기 안내](docs/portable-scheme-import.md)와 [1.2.0 검증 기록](docs/releases/1.2.0.md)을 참고하세요. 소스를 받아 로컬에서 빌드할 수도 있습니다.

```bash
git clone --recurse-submodules https://github.com/scholay/xsime.git
cd xsime
```

| 플랫폼 | 진행 | 빌드 |
|---|---|---|
| macOS | 입력기, 그리고 Buffer, Capsule, Mailbox | `./build_install.sh` |
| iOS | 키보드와 본체 앱(iOS 17+). 오프라인 병음, 자연마, 오필, 영어, 그리고 Buffer | Xcode에서 [`platforms/ios/RIMES.xcodeproj`](platforms/ios/README.md)를 연다 |
| Windows | 네이티브 TSF 입력기, Buffer, 병격 및 공식 플러그인 설정; x64 / x86 | [`platforms/windows/native/README.md`](platforms/windows/native/README.md) 참고 |
| Android | 네이티브 InputConnection 키보드, Buffer, 공식 플러그인 6개 및 설정 가능한 AI 서비스 | [`platforms/android/README.md`](platforms/android/README.md) 참고 |
| Linux | Fcitx5 입력기, Buffer, Capsule, Mailbox | [`platforms/linux/ime/README.md`](platforms/linux/ime/README.md) 참고 |

## 문서

| 문서 | 내용 |
| --- | --- |
| [문서 목록](docs/README.md) | 현재 안내와 플랫폼별 진입점 |
| [시스템 구조](docs/architecture/system.md) | 입력, Buffer, 공유 코드와 데이터 경계 |
| [개발 및 유지보수](docs/development/maintenance.md) | 주 브랜치, 플러그인 의존성, CI 및 검증 |
| [릴리스 절차](docs/release/README.md) | 빌드, 서명, 업데이트 및 배포 |
| [변경 기록](CHANGELOG.md) | 공개 버전별 변경 사항 |

## 앱 업데이트

1.1.6부터 다섯 플랫폼의 설정에서 업데이트를 확인할 수 있습니다. macOS는 서명과 공증을 마친 PKG, Windows는 전체 EXE, Android는 기존 키로 서명한 APK를 다운로드하여 시스템 설치 프로그램으로 업데이트합니다. Linux는 Ubuntu 24.04 amd64용 DEB를 내려받아 시스템 패키지 관리자에 설치를 승인합니다. Windows EXE는 현재 서명되지 않았습니다.

앱을 다시 열고 실행 중인 버전을 확인한 뒤 완료를 표시합니다. 설정, 가져온 입력 방안과 학습 기록은 유지됩니다. 시스템에서 권한 승인, 로그아웃 또는 재시작을 요구하면 안내를 따르세요. 구버전에 업데이트 기능이 없다면 위 다운로드 링크로 한 번 수동 업데이트하세요.

iOS는 실제로 제공되는 스토어 버전을 확인한 뒤 App Store에서 다운로드하고 설치합니다. 테스트 버전은 TestFlight에서 업데이트합니다. GitHub에 새 버전이 올라왔다고 같은 iOS 버전이 출시된 것은 아닙니다.

네 플랫폼의 설치 파일은 [`scholay/xsime`](https://github.com/scholay/xsime/releases/latest)의 같은 `vX.Y.Z` Release에 게시됩니다. CI가 모든 파일을 업로드하고 다시 내려받아 확인한 뒤 마지막으로 공통 `update-manifest.json`을 게시합니다. 미리보기 버전은 정식 업데이트에 포함되지 않습니다. iOS는 별도의 `ios-vX.Y.Z` 스토어 배포 절차를 사용합니다.

관리자는 다섯 플랫폼의 main CI가 통과한 뒤 배포합니다. [docs/release/README.md](docs/release/README.md)와 [CHANGELOG.md](CHANGELOG.md)를 참고하세요.

```bash
./scripts/release.sh stable --dry-run  # 버전, 다섯 플랫폼 CI와 배포 조건 확인
./scripts/release.sh stable            # 정식 태그를 만들고 GitHub Actions에서 빌드·배포
```

## 링크

- [<img src=".github/readme/friends/rime.png" width="20" height="20" alt="" align="absmiddle"> RIME 입력기 엔진](https://rime.im/) — 이 프로젝트의 중국어 입력은 RIME을 기반으로 합니다.
- [<img src=".github/readme/friends/linux-do.png" width="20" height="20" alt="" align="absmiddle"> Linux.do](https://linux.do/) — 진정성 있고 친절하며 단결되고 전문적인 커뮤니티 Linux.do와 그 구성원들에게 감사합니다.
- [<img src=".github/readme/friends/irime.jpg" width="20" height="20" alt="" align="absmiddle"> iRime](https://github.com/jimmy54/iRime) — XSIME에 대한 안내와 소개에, iRime 작성자에게 감사합니다.
- [<img src=".github/readme/friends/scholay.svg" width="20" height="20" alt="" align="absmiddle"> Scholay](https://www.scholay.com/t/M4ZZW8KRE5) — 최첨단 연구와 혁신에 기반한 아키텍처 지침을 제공해 주셔서 감사합니다.
- [<img src=".github/readme/friends/enterscholar.svg" width="20" height="20" alt="" align="absmiddle"> EnterScholar 커뮤니티](https://enterscholar.com/) — AI 시대의 학자들을 위한 커뮤니티 플랫폼에 감사드립니다.

## 기여자

전체 명단은 [CONTRIBUTORS.md](CONTRIBUTORS.md)에 있습니다.

이 프로젝트의 핵심 유지관리자는 프로그래머 출신이 아닌, 일반 사용자를 위한 제품 매니저입니다. vibe coding 시대에 감사합니다.

**AI 프로그래밍 도우미**: Claude, Cursor, Codex, Grok이 설계, 구현, 검토에 참여했습니다.

## 알려진 문제

- **Linux: 배포가 끝나기 전에 Fcitx5를 끝내면 프로세스가 몇 분 동안 남을 수 있다.** 처음이거나 백그라운드의 librime 배포는 도중에 취소할 수 없고, 사전이 클수록 더 두드러집니다. [#43](https://github.com/scholay/xsime/issues/43)을 보십시오.
- **Linux: Buffer 도구 막대를 드래그한 직후 다른 입력칸을 누르면 캡처가 켜진 채로 남을 수 있다.** 지금은 X11의 Firefox에서만 재현됩니다. 손을 뗀 뒤 약 10밀리초 안에 같은 창의 다른 입력칸을 누를 때입니다. 한 번 더 누르거나 Esc로 돌아옵니다. [#44](https://github.com/scholay/xsime/issues/44)를 보십시오.

## 라이선스와 제3자

XSIME 자체 코드는 [Apache License 2.0](LICENSE)을 따릅니다. 적용 범위와 기존 MIT 허가는 [LICENSING.md](LICENSING.md)를 참고하십시오. 핵심 유지관리자는 [学术海](https://pm.scholay.com)입니다.

중국어 입력은 [Rime 입력 엔진(librime)](https://github.com/rime/librime)을 사용합니다. 출처 정보는 [NOTICE](NOTICE), 권장 표시 예시는 [ATTRIBUTION.md](ATTRIBUTION.md)에 있습니다. 표시 권고는 추가 라이선스 조건이 아닙니다.

제3자 구성 요소, 방안, 사전, Lua/OpenCC 데이터는 각자의 라이선스와 저작권 표시를 유지합니다. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [LICENSES](LICENSES), `rime-data/licenses/`를 참고하십시오. 공식 무료 플러그인은 [rimes-plugins](https://github.com/scholay/rimes-plugins)에서 관리합니다.
