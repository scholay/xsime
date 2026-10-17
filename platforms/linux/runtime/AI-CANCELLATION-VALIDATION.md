# AI cancellation and revocation follow-up

The AI API worker now shares the bounded job pool used by local translation.
Cancelled requests can finish libcurl/callback cleanup without rejecting the
next live request. Shutdown cancels active work, discards queued work and joins
all workers before dispatcher detachment. Revoked derived callbacks publish the
cleared state immediately, including when no other host event occurs.

Ubuntu 24.04 arm64 validation, 2026-10-17:

- `ai-http`: four real streaming HTTP requests held inside their callbacks,
  cancellation followed by a queued replacement, successful replacement and
  joined/idempotent shutdown. Existing fragmented streaming, early DONE, timeout,
  authorization, malformed protocol, size and redirect checks also pass.
- `buffer-derived-host`: emit a stream preview, revoke the plugin, then verify
  the rejected callback itself publishes an empty result snapshot. Existing
  alternatives, source editing, configuration, focus and shutdown tests pass.
- Production Fcitx: explicit HTTP request, streaming preview, final-only native
  delivery, source consumption and cancellation on close pass.

This is transport and host evidence; external providers and real desktop/pixel
acceptance remain separate parts of #158.

Queued requests cancelled before their worker starts now emit one terminal `Cancelled` result. The HTTP fixture keeps four streams in teardown, cancels an accepted fifth queued request, checks its cancellation result, then verifies a fresh request still completes. `ai-http` passed with the updated worker pool on 2026-10-18.
