# Windows capture stack and hover parity · 2026-10-24

Reference: macOS `CaptureCoordinator.showOverlay`, `CaptureResultPanel`,
`CaptureOverlayLayout`, and `CaptureCardControls`. Mailbox and independent
unavailable capabilities remain outside this change.

## Behavior

- Replace the single three-row results panel and pagination with separate
  208 × 148 point thumbnail windows, 18 point work-area margins and 8 point
  gaps. Newest is nearest the bottom corner. Overflow keeps its records and
  asset leases; dismissal reveals older cards without overlapping or deleting
  capture history. The stack stays on its original display until emptied.
- Keep the complete image visible with proportional fitting. Hover exposes
  Fluent copy/save controls centrally, close at top-left, annotation at
  bottom-left, and an explicitly disabled pin action at top-right. Copy failure
  retains the card; success removes only that overlay. Export retains it.
  Editing refreshes the same window/stack position and the current rendered
  asset. Double-click opens annotation; dragging offers the current image as a
  copy-only shell file object, without modifying the clipboard or moving the
  managed asset. Protection cancels an active drag.
- Render thumbnail, hover shading and glyphs in one alpha-composited frame.
  Native buttons retain accessibility and hit testing but do not paint separate
  background surfaces. Fractional round-corner coverage avoids binary HWND
  region clipping. Theme changes invalidate every live card.
- Capsule's main card rail invalidates only the copy-glyph rectangle on hover;
  its preview, text and card body are outside that damage region.

## Capture responsiveness

The old selection path allocated a full-screen bitmap and drew the desktop
twice through high-quality GDI+ resampling on every mouse move. The new path
keeps reusable original, dimmed and composition DIBs for the selection lifetime,
copies exact physical pixels, and damages only changed selection/hint bounds.
Cropping, PNG encoding and thumbnail scaling run on a worker after the overlay
has closed and the previous foreground has been restored. Store publication
stays serialized on the UI thread. Shielding/disable discards pending completion;
no delayed callback can publish or reopen a protected screenshot. The finished
thumbnail is reused directly, avoiding PNG decode immediately after encode.

## Validation

`RimesCapture` runs the dedicated `RimesPeripheralTests --capture-ui` fixture in
CTest, serially, on both architectures. It checks dimensions, passive hover,
clipboard failure/success using an isolated writer, capture cancellation,
physical crop resolution, annotation undo/redo and original retention, stable
window identity, seven-card overflow/reflow, and protection during preparation.
The 4K fixture checks exact selected pixels, dimmed outside pixels, stable GDI
resource usage, asynchronous release and protected completion, and reports
render/release timings without a hardware-dependent timing assertion.

The full peripheral UI fixture checks four themes and verifies that Capsule
hover changes no pixels outside the copy action. Rendered stack fixtures are
visually inspected as well as checked for bounds/overlap. Early local 4K runs
measured roughly 10–16 ms per complete frame and 6–8 ms for selection release
after cropping was moved off-thread; these are synthetic local measurements,
not an end-to-end desktop latency promise.

`--capture-preview` provides three isolated synthetic cards for manual testing;
its copy callback does not touch the real clipboard, and it exits after three
minutes. Computer Use did not expose these nonactivating tool windows as
targetable windows on this host. That attempt is not counted as a successful
physical mouse acceptance. External application drag/drop and mixed-monitor
hardware remain separate acceptance coverage.

Final build/CTest/CI and installation evidence is recorded under the ignored
`platforms/windows/native/out/capture-parity-20261024/` and
`platforms/windows/native/out/capture-stack-install-20261024/` directories.
