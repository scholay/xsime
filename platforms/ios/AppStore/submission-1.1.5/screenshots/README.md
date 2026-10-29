# iOS 1.1.5 screenshot intake

Received 2026-10-15 from the maintainer after the 1.1.5 (59) device-install
handoff. The maintainer accepted all six images as-is, without reshooting or
changing the homepage slogan, then authorized continuing the release workflow.

Uploaded to the App Store Connect 1.1.5 draft in the proposed order below for
the English primary locale. Simplified and Traditional Chinese inherit the same
six screenshots from the primary locale. The initial Simplified Chinese duplicate
set left three uploads unfinished; its draft references were removed so it uses
the fully processed primary set instead. This resolved review validation without
altering the six images or their order. The two old English iPhone screenshot
groups were removed from this draft only. All removed references retained their
original assets in Apple's Asset Library.
Screenshot upload alone is **not** App Review submission or release approval;
see the version preparation record for the separate build/submission state.

## Original files and proposed order

All six files are original 1179 × 2556 portrait PNGs with no alpha channel.
They were copied byte-for-byte from the supplied attachments, without cropping,
resizing, retouching or changing the displayed app content. The UI is Simplified
Chinese; these are not separately localized English or Traditional Chinese images.

| Order / saved file | Original attachment | Feature |
| --- | --- | --- |
| `zh-Hans/01-typing.png` | #1, `codex-clipboard-c1692962-2f27-4cf6-a900-73f6b96f552c.png` | Ordinary typing and candidates |
| `zh-Hans/02-buffer.png` | #4, `codex-clipboard-af8d5cfe-9f9e-4836-8956-0a4876c48366.png` | Buffer composition |
| `zh-Hans/03-translation.png` | #6, `codex-clipboard-723ced09-371c-419a-9df0-a0d7649809dc.png` | Translation source and result |
| `zh-Hans/04-nine-key.png` | #3, `codex-clipboard-f294a206-1a23-4794-a740-dc9131380181.png` | Nine-key layout and Buffer |
| `zh-Hans/05-chords.png` | #2, `codex-clipboard-a14c4623-3d5b-442a-a4ad-80310a9891a6.png` | Highlighted chord keys |
| `zh-Hans/06-home.png` | #5, `codex-clipboard-8ff07f14-295c-45c6-b861-85613aeb0d3d.png` | New product name, mascot and setup menu |

Apple's [screenshot specifications](https://developer.apple.com/help/app-store-connect/reference/app-information/screenshot-specifications)
list 1179 × 2556 as an accepted iPhone portrait size and require no transparency.
File-format compliance does not guarantee review approval.

## Visual review

- The new 犀术IME branding is visible. No API keys or private conversations
  were visible in the supplied screenshots.
- Attachment #2 (`05-chords.png`) and #6 (`03-translation.png`) were captured
  while scrolled: the floating back button partly covers the page title.
  A fresh capture with the title unobscured was suggested for store presentation;
  the maintainer chose to retain the supplied images. This was a visual-quality
  suggestion, not an asserted App Review rejection.
- Attachment #5 (`06-home.png`) still shows the old two-line slogan. The same
  strings remain in `platforms/ios/App/RimesApp.swift`; the canonical Simplified
  slogan in `docs/development/branding.md` is “心有灵犀，落笔见术”. Fixing this requires an app
  change and a new homepage capture, not editing the screenshot to depict an
  interface the installed app does not show. The maintainer chose to retain the
  current app slogan for this release. No app code was changed here.
- The screenshots demonstrate visible UI states, not complete functional
  regression testing, Full Access OFF behavior or AI-provider consent flows.

## Integrity

SHA-256 of each archived file was compared with its source attachment and matched.

```text
823af50a8baf93d462612ad18d22bbc531d4ca143fc1861a6d698b756f07041e  zh-Hans/01-typing.png
dd4cfa9b5ab8e1d9f3a96412b205c7e666d813ee5ab478ce46467474979cb00b  zh-Hans/02-buffer.png
5c7ef2be61f03953bb13c2b2d717f375dc93b14f27a0e0d489116f9b8edfc603  zh-Hans/03-translation.png
96cf6da8983dc72c78e80c8a435f0c859b2e77fe2e664d5da6ddcaa3fc75913d  zh-Hans/04-nine-key.png
bc800952287426014092afeb8fe71c54ada55651f5c3fced9f028e23298d4ff0  zh-Hans/05-chords.png
697eb101e770d683f9f391476a68e17548c16caa2c5e4c6ee2a690c026b56fd8  zh-Hans/06-home.png
```
