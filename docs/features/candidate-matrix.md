# Candidate layout and matrix interaction

## Desktop contract

- Appearance settings choose horizontal or vertical compact candidates. The setting persists locally and does not rewrite a Rime schema or user dictionary.
- Down opens a matrix when more than one engine page exists; the viewport contains at most three pages, one page per row. More rows are fetched as selection advances.
- Left/right move across the complete row. A row that exceeds available width reveals the selected column without squeezing every candidate into tiny cells.
- Up/down and Page Up/Down move between matrix rows. `-`/`=` have the same paging behavior. Up at the first row or Escape returns to the opening page, preserving composition. A second Escape follows the ordinary schema cancellation path.
- Space commits the highlighted candidate. 1–9 select only a visible candidate in the active row. Mouse selection retains the actual engine page and candidate index.
- Ordinary Return retains the platform's raw-input behavior. Modifier combinations retain their schema/host bindings.
- Focus loss, commit, new input, secure fields and context retirement invalidate the old candidate projection and click callbacks.

macOS uses its existing nonactivating AppKit panel. Windows uses its negotiated TSF panel; UI-less applications retain control of their own presentation. Linux compact candidates remain in Fcitx, with a native GTK matrix while expanded. The matrix integrates with the Fcitx event loop and does not own an engine or inject text. X11 uses a non-focusing popup; Wayland uses layer-shell when supported, or an available XWayland display. A session with neither backend retains Fcitx candidates and keyboard navigation; that session cannot display the custom matrix. Compositor acceptance must distinguish these cases.

## Mobile contract

- Android adds an expand/collapse control and scrollable candidate matrix, including candidates beyond the current engine page. A serial engine worker enumerates candidates directly and returns absolute indices without sending schema-dependent paging keys.
- iOS and Android reserve fixed control gutters outside the scrolling content. Every matrix row has the same available width.
- With the same candidate prefix and layout width, content-space candidate rectangles are unchanged by scrolling, relayout and appending more candidates. Scrolling changes only the viewport offset.
- A changed composition retires the old list. A width, orientation or font change may compute a fresh layout.
- Taps use the candidate's original index, independent of wrapping. iOS hold/drag selection continues to hit-test the current scroll position.

## Regression coverage

- macOS: `candidate-pagination-smoke`, `matrix-smoke`, `smoke`; vertical native layout, numeric mapping, count limits and matrix Escape routing.
- Windows: candidate layout/window/settings/navigation tests; overflow reachability, native hit rectangles, visible numeric labels, anchor restoration and short final pages.
- Linux: `RimesCandidateMatrix`, in-process Fcitx typing E2E; expansion, later-page commit, Escape preserving composition and live orientation changes. Native popup and compositor checks supplement the headless tests.
- Android: core candidate enumeration tests, `CandidateMatrixContract` in rendering instrumentation, actual engine enumeration and later-index commit, plus testhost `candidate-matrix` mode.
- iOS: `CandidateStripTests`, `KeyboardLayoutTests`, `KeyboardInteractionTests`; fixed frames while scrolling, append stability, every candidate reachable, hold/drag selection and host/Buffer commits.

Automated tests and source builds do not replace Windows application or Linux compositor acceptance.
