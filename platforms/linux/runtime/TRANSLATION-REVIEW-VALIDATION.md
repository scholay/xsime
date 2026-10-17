# Translation cancellation and revocation review

Follow-up to [PR #187](https://github.com/scholay/xsime/pull/187), 2026-10-17.

Cancelled Python processes retain their process-group identity during cleanup.
Previously, four cancelled jobs could still fill the worker list and reject the
next sentence. A fixed four-thread pool now distinguishes live requests from
cleanup: the latest live request waits for a worker, while cancelled queued
requests are discarded. The number of workers and live queued requests stays
bounded. Shutdown cancels and joins the pool before dispatcher detachment;
the process-group cleanup and PID-reuse protection remain intact.

A rejected translation callback now publishes the state after authorization
synchronization. Clearing the host's translation units without publishing could
otherwise leave old plaintext visible in the renderer.

Checks on Ubuntu 24.04 arm64:

- `background-jobs`: hold four workers in cleanup, cancel and retry 100 times,
  reject a fifth genuinely live request, accept the latest replacement, avoid
  executing obsolete queued work, and refuse work after shutdown.
- `buffer-translation-host`: publish a ready prefix, start a pending tail,
  revoke authorization, then confirm both the host units and the emitted UI
  snapshot lose the old translation. Existing edit/retry/protection cases pass.
- `runtime-process`: existing cancellation, timeout, bounded output and
  process-group checks pass.
- Production Fcitx with the pinned offline engine: stable-prefix reuse, exact
  source retirement, Return delivery, frozen siblings, reopen and send-all pass.

These tests do not establish physical desktop or pixel acceptance for #156.

### Queued cancellation completion (2026-10-18)

Accepted work removed before execution now invokes its cancellation completion exactly once, outside the worker-pool mutex. The translation adapter reports an unsuccessful completion for that case as it does for cancelled active work. This also permits a completion to schedule work without deadlocking the pool. A deterministic fixture holds all four workers in cleanup, cancels 100 queued replacements and checks 100 completions with zero obsolete work executed; the latest request still runs. `background-jobs` and `buffer-translation-host` passed on the local Linux fixture. Review: PR #187 comment 4303129786.
