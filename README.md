# Draw

A minimal screenshot annotation tool for X11.

On launch it captures the entire screen, presents it in a fullscreen window,
and lets you draw over it in red with a circular brush.

## Build

```
make
```

Requires `libx11`, a C11 compiler (both standard on Arch), and a running
compositor (picom, KWin, Mutter, etc.) for the anti-aliased cursor overlay.

## Usage

```
./draw
```

| Input               | Action                         |
|---------------------|--------------------------------|
| Left mouse button   | Draw (hold)                    |
| 1–9                 | Switch colour                  |
| Ctrl+1..9           | Save drawing slot              |
| Ctrl+Shift+1..9     | Load drawing slot              |
| Right mouse button  | Hold to erase                  |
| Scroll up           | Increase brush                 |
| Scroll down         | Decrease brush                 |
| Side button (up)    | Undo                           |
| Side button (back)  | Redo                           |
| ESC                 | Quit                           |
| Ctrl+C              | Quit                           |

## How it works

1. `XGetImage` captures the root window (full virtual screen).
2. An override-redirect fullscreen window is created and grabs keyboard + pointer.
3. The screenshot is stored in a server-side `Pixmap` that acts as a back-buffer.
4. Mouse-down stamps / interpolates filled red circles onto the pixmap.
5. Only the dirty rectangle is blitted to the window each frame.
6. The cursor indicator is a **separate 32-bit ARGB window** rendered with
   per-pixel anti-aliasing (float distance-from-centre, 1 px ramp,
   premultiplied alpha).  The compositor blends it over the desktop.
   It follows the pointer via `XMoveWindow`; nothing is ever drawn then
   erased on the main canvas, so there is zero flicker.
7. Scroll wheel resizes both the brush and the cursor overlay on the fly.

## Notes

- A compositor (picom, KWin, Mutter …) is required for the transparent,
  anti-aliased cursor overlay.  Without one the cursor may appear as a
  solid rectangle.
- Saved drawing slots live under `$XDG_DATA_HOME/draw/slots/`, falling back to
  `~/.local/share/draw/slots/` when `XDG_DATA_HOME` is unset.
- This is an X11 tool. On Wayland-only sessions, `XGetImage` on the root window
  may return a blank image (an XWayland limitation). Run under an X11 session
  or Xwayland-compatible compositor for full functionality.
