# AI API validation — 2026-10-17

The Linux API adapter uses the current macOS semantic-block system prompt,
imported at build time from `AITextOpenAIRequestBuilder`. Source text remains the
user message. Provider URL/model/credential are frozen for each explicit request;
network work runs outside the Fcitx event loop. The portable official-plugin
codec owns bounded SSE/UTF-8 parsing and semantic result validation. Libcurl,
credentials, cancellation and native input targets remain host responsibilities.

## Verified in Ubuntu 24.04 arm64 on the macOS development host

- 32 CTests pass, including a real loopback HTTP server exercising fragmented
  UTF-8 SSE, complete logical-block previews, early `[DONE]` while HTTP stays open,
  cancellation, timeout, 401 mapping, 429, redirect refusal, malformed/tool
  responses, missing completion and size limits. Invalid configuration is rejected
  before transport. Server error bodies and credentials never become UI messages.
- The production addon runs in Fcitx `testfrontend` with an isolated provider,
  test credential and explicit optional-plugin installation. It checks no automatic
  AI request on typing, non-deliverable streaming previews, semantic output blocks,
  native partial/final insertion, source consumption only after final delivery,
  and close cancellation without late restoration.
- Existing host identity tests cover source changes, focus, configuration, plugin
  revocation, protection and shutdown. Translation still uses its offline engine
  and separate `translation.generate` capability.
- AI API remains an optional official package, matching macOS. A fresh Linux
  profile does not silently enable a network-capable plugin. Its 1.1.0 package must
  accompany the corresponding official-plugin release before public download.

These tests use a loopback protocol peer, not a paid external AI provider.
External-channel acceptance, physical X11/Wayland desktops and macOS pixel
comparison remain open in #158/#179. Generated source and successful builds are
not evidence that an installed desktop package or public Release was updated.

The request/stream format is based on the native implementation and the
[Chat Completions reference](https://developers.openai.com/api/reference/resources/chat).
Transport callbacks follow libcurl's documented
[write callback](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html) and
[cancellation callback](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html).

Run locally after the standard Linux build and shared Rime data preparation:

```sh
ctest --test-dir platforms/linux/ime/build --output-on-failure
RIMES_SHARED_DIR=/path/to/staged/data python3 platforms/linux/runtime/tests/ai_http_fixture.py \
  --fcitx platforms/linux/ime/build
```
