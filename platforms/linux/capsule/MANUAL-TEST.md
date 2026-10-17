# Linux Capsule — real-desktop checklist

Automated CI covers the C++ store/model, the in-process Fcitx5
`testfrontend` Capsule suite, and a headless DBus/Xvfb path. It does
**not** replace a real session. Run this on Debian 13 Xfce (X11) and a
wlroots Wayland session (labwc or sway).

Record distro, session type, compositor, and host for every row.

## Machines

| Distro | Session | Compositor | Host | Result |
|---|---|---|---|---|
| Debian 13 | X11 | Xfce | gedit | |
| Debian 13 | X11 | Xfce | kate | |
| Debian 13 | X11 | Xfce | Firefox | |
| Debian 13 | X11 | Xfce | xfce4-terminal / GNOME Terminal | |
| Debian 13 | Wayland | labwc | gedit | |
| Debian 13 | Wayland | labwc | kate | |
| Debian 13 | Wayland | labwc | Firefox | |
| Debian 13 | Wayland | labwc | foot / kitty / GNOME Terminal | |
| Debian 13 | Wayland | sway | gedit | |
| Ubuntu 24.04 | X11 or Wayland | any | one GTK + one terminal | |

Also try one machine with stock `fcitx5-rime` still installed: RIMES must
stay a separate IM, and `~/.local/share/rimes/capsule` must stay distinct
from `~/.local/share/fcitx5/rime`.

## Per-host cases

For each host above, switch to **RIMES** (雾凇全拼) first. The hotkey
does nothing under another IM (including stock `fcitx5-rime`). The
**临时** tab is empty — it is not ported.

1. **Direct still works.** With Capsule hidden, type `nihao` + Space. The
   field contains `你好` and no leftover `nihao`.
2. **Open Capsule.** Press `Ctrl+Shift+V` (or `Super+Shift+V`). The rail
   appears at the bottom. The host keeps keyboard focus — the next click
   must not type into the Capsule window itself.
3. **Seed card.** The 笔记 tab shows `RIMES 默认词条` (or your own notes).
   Count is `1 ITEM` on a fresh profile.
4. **Insert, do not leak.** Press Return, `Ctrl+1`, or **double-click**
   the card (two clicks on the same card within ~400 ms, including
   `xdotool click --repeat 2` and QMP 20–300 ms gaps). A single click
   only moves the highlight. The field gains `RIMES` and no extra
   newline. The rail **closes** after a successful insert. A terminal
   must not execute the line.
5. **Search.** Open Capsule, type `zzzz`. The rail goes empty. Backspace
   back to a matching query; the seed card returns. Those letters must
   **not** appear in the host.
6. **Escape / hotkey close.** Open Capsule, press Escape **in that same
   field** (or the hotkey, or Close). The rail hides. The host did not
   receive Escape as a control character (a terminal must not abort).
7. **Focus change.** Open Capsule, click another field or window —
   including **another text field on the same Firefox / Chromium page**.
   Status must leave armed even if you wait a few seconds. Press Return
   in the new field: Capsule must **not** insert `RIMES` there. In the
   new field press Escape: that app must receive Escape; Capsule may stay
   visible. One `Ctrl+Shift+V` on the new field **re-arms**; a second
   press closes. Do not insert until it is re-armed.
8. **Password.** Click a password field in a GTK host that keeps the IM
   enabled. Capsule must refuse to arm / hide. Typed secrets must not
   appear in the rail. **Firefox-esr disables the IM on
   `<input type=password>`**, so the password flag never reaches the
   addon: still confirm Escape / Return / preedit do not inject ZWSP or
   swallow Escape in that field.
9. **Copy.** Open Capsule, press `Ctrl+C`. Paste into the host with the
   host's own paste (`Ctrl+Shift+V` in a terminal, `Ctrl+V` elsewhere).
   You get `RIMES`. Capsule itself must not synthesize Ctrl+V.
10. **Buffer priority.** Open Buffer (`Ctrl+Shift+B`), stage `你好`, then
    open Capsule. Return must send the Buffer chip, not a Capsule note.
    Pause Buffer, then Return inserts the selected note.
11. **X11 placement.** On Xfce the rail sits at the **bottom**, about
    940×208, keep-above a maximized gedit, and does not steal focus. The
    9-row candidate list for `shi` must still be readable.
12. **Wayland placement.** On labwc/sway the rail is overlay / always
    visible, at least 940 px wide (stretched with side margins on a
    1280-wide output is fine), cards are readable, and it does not steal
    focus. It sits at the bottom — expected.
13. **UI respawn.** While the rail is visible, `kill -9` the
    `rimes-capsule` process and press no keys. After a few seconds
    `pgrep -x rimes-capsule` must show **exactly one** process. There
    must be one rail, not a stack. Insert must keep working; do not
    restart fcitx5. `pgrep -x rimes-buffer` must be unchanged.
14. **Stale target.** Quit the armed app while the rail is open. The
    session disarms. Insert via ctl / Return must not write into a
    different window.
15. **Empty tabs.** 临时 / 捕获 / 图库 / 影集 / PDF / 技能 / 密码 show
    its module-specific empty state. Dedicated backends are tracked in #163–#167. Switching tabs with Tab must not type a Tab
    into the host.
16. **Clipboard is edge-triggered.** Open Capsule, `Ctrl+C` so the
    clipboard is the selected note. In another app copy a sentinel
    (`USERCLIP`). Type or backspace in the Capsule search box, then
    close and reopen the rail. The clipboard must still be `USERCLIP`.
    `kill -9` `rimes-capsule` and wait for the single respawn: clipboard
    must still be `USERCLIP` (a replacement UI must not replay the last
    copy). Focus a Firefox password field and paste: you get `USERCLIP`,
    never the stale note. `Ctrl+C` again on the same card must copy the
    note again. **labwc/sway:** from another app set a foreign value
    (`wl-copy FOREIGN`) without moving the pointer onto the bar, then
    press `Ctrl+C` from the keyboard only. The clipboard must become the
    selected note (via `wl-copy`). If `wl-clipboard` is not installed,
    the rail hint must say copy may need a click on the bar first — do
    not fail silently — and a click on the bar then `Ctrl+C` may use the
    GTK fallback.

## Session notes

- Xfce X11: confirm `gtk_window` keep-above above a maximized gedit.
- labwc/sway: if the rail is missing, check `libgtk-layer-shell0` is
  installed and `journalctl --user -u fcitx5` / `~/.local/share/rimes`
  logs. Keyboard-only `Ctrl+C` needs `wl-clipboard` (`wl-copy` on
  `PATH`); without it the rail hint must warn instead of failing silent.
- Firefox and terminals sometimes need `GTK_IM_MODULE=fcitx` in the
  session, not only the shell that launched the test.
- Capsule data lives in `~/.local/share/rimes/capsule`. Do not confuse it
  with `~/Library/RIMES/capsule` on a Mac share.

## What this checklist does not cover

Mailbox, iCloud, clipboard-history capture, password chords, Capture,
Image/PDF/Skill file paste, IBus, and a custom candidate window.

## 五模块管理页补充验收

- 在插件页逐个停用五模块，确认导航移除、在途操作失效、目录内容仍在；重新启用后恢复。
- 从底栏进入管理页，编辑标题、摘要、类型和内容；Ctrl+S 保存，Esc 返回底栏且不保留输入目标。
- 修改尚未保存时分别切换条目、模块、关闭窗口，确认取消后草稿不丢失；搜索不覆盖草稿。
- 用外部编辑器修改同一 UUID 后保存旧草稿，应提示冲突并保留双方内容。
- 锁屏/保护态隐藏编辑内容，恢复时保留进程内草稿，不自动保存或投递。
- 测试 940×660、620×430、放大窗口及不同缩放；同一内容与 macOS 对照四主题。检查字体、图标、侧栏、编辑表单与焦点/选中态，不以窗口尺寸相同代替像素验收。
