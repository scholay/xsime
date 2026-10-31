// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Windows.h>
#include <cwchar>

namespace rimes::windows::core {
// A capability owned by our local settings editor, never inferred from the
// title, application name or the focused window of another process.
inline constexpr wchar_t kTypingPracticeEditorProperty[] = L"Rimes.TypingPractice.Editor.v1";
inline constexpr wchar_t kTypingPracticeCompositionProperty[] = L"Rimes.TypingPractice.Composing.v1";
inline constexpr wchar_t kTypingPracticeCancelCompositionProperty[] = L"Rimes.TypingPractice.CancelComposition.v1";
inline constexpr wchar_t kTypingPracticeServiceWindowProperty[] = L"Rimes.TypingPractice.ServiceWindow.v1";
inline constexpr UINT kTypingPracticeCancelMessage = WM_APP + 92;
inline constexpr wchar_t kTypingPracticeLayoutPendingProperty[] = L"Rimes.TypingPractice.LayoutPending.v1";
inline constexpr UINT kTypingPracticeLayoutMessage = WM_APP + 93;
inline constexpr UINT kTypingPracticeCaretMessage = WM_APP + 94;
inline bool HasFloatingPracticeComposition(HWND editor) noexcept {
  return editor && GetPropW(editor, kTypingPracticeCompositionProperty) == reinterpret_cast<HANDLE>(editor);
}
inline bool HasCancelledPracticeComposition(HWND editor) noexcept {
  return editor && GetPropW(editor, kTypingPracticeCancelCompositionProperty) == reinterpret_cast<HANDLE>(editor);
}
inline bool CancelFloatingPracticeComposition(HWND editor) noexcept {
  DWORD editor_process = 0;
  if (!editor || GetWindowThreadProcessId(editor, &editor_process) != GetCurrentThreadId() ||
      editor_process != GetCurrentProcessId() || !HasFloatingPracticeComposition(editor) ||
      !SetPropW(editor, kTypingPracticeCancelCompositionProperty, reinterpret_cast<HANDLE>(editor))) return false;
  const auto service = reinterpret_cast<HWND>(GetPropW(editor, kTypingPracticeServiceWindowProperty));
  DWORD process = 0;
  if (service && GetWindowThreadProcessId(service, &process) == GetCurrentThreadId() &&
      process == GetCurrentProcessId())
    PostMessageW(service, kTypingPracticeCancelMessage, reinterpret_cast<WPARAM>(editor), 0);
  return true;
}
inline void NotifyTypingPracticeLayoutChanged(HWND editor) noexcept {
  DWORD editor_process = 0;
  if (!editor || GetWindowThreadProcessId(editor, &editor_process) != GetCurrentThreadId() ||
      editor_process != GetCurrentProcessId() || !HasFloatingPracticeComposition(editor) ||
      HasCancelledPracticeComposition(editor) ||
      GetPropW(editor, kTypingPracticeLayoutPendingProperty) == reinterpret_cast<HANDLE>(editor)) return;
  const auto service = reinterpret_cast<HWND>(GetPropW(editor, kTypingPracticeServiceWindowProperty));
  DWORD process = 0;
  if (service && GetWindowThreadProcessId(service, &process) == GetCurrentThreadId() &&
      process == GetCurrentProcessId() &&
      SetPropW(editor, kTypingPracticeLayoutPendingProperty, reinterpret_cast<HANDLE>(editor))) {
    if (!PostMessageW(service, kTypingPracticeLayoutMessage, reinterpret_cast<WPARAM>(editor), 0))
      RemovePropW(editor, kTypingPracticeLayoutPendingProperty);
  }
}
inline bool IsTypingPracticeEditor(HWND editor) noexcept {
  if (!editor || !IsWindowVisible(editor) || !IsWindowEnabled(editor) ||
      GetFocus() != editor ||
      GetPropW(editor, kTypingPracticeEditorProperty) != reinterpret_cast<HANDLE>(editor)) return false;
  DWORD process = 0;
  const auto thread = GetWindowThreadProcessId(editor, &process);
  if (process != GetCurrentProcessId() || thread != GetCurrentThreadId()) return false;
  wchar_t owner_class[64]{};
  return GetClassNameW(GetParent(editor), owner_class, 64) > 0 &&
         std::wcscmp(owner_class, L"Rimes.ExtensionPages") == 0;
}
inline bool QueryTypingPracticeCaret(HWND editor, RECT* caret) noexcept {
  if (!caret) return false;
  *caret = {};
  if (!IsTypingPracticeEditor(editor)) return false;
  RECT queried{};
  // Only the trusted same-thread editor may receive this local pointer. Its
  // cell geometry excludes RichEdit paragraph spacing included by GetTextExt.
  if (!SendMessageW(editor, kTypingPracticeCaretMessage, 0,
                    reinterpret_cast<LPARAM>(&queried)) ||
      !IsTypingPracticeEditor(editor) || queried.right <= queried.left ||
      queried.bottom <= queried.top) return false;
  *caret = queried;
  return true;
}
inline HWND TypingPracticeEditorForView(HWND view) noexcept {
  const auto editor = GetFocus();
  return IsTypingPracticeEditor(editor) && view &&
      (view == editor || IsChild(view, editor)) ? editor : nullptr;
}
}  // namespace rimes::windows::core
