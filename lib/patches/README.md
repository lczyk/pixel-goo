# local patches

local changes to `lib/RGFW.h` (not a header-only drop-in). each `.patch` is a unified
diff against pristine upstream, so they survive re-vendoring. all hunks are tagged
`pixel-goo local patch` inline (except the plain `RGFW_windowNoFocusOnCreate` flag).

generated against upstream `ColleagueRiley/RGFW` `main` (v2.0.0-dev), which matched the
vendored copy byte-for-byte apart from these changes.

the patches are independent (disjoint regions) but the apply order below is the tested
one. all five together reproduce the vendored `lib/RGFW.h` exactly (verified: fresh
upstream + all patches `diff`s clean against the working copy).

## the patches

- **rgfw-no-focus-on-create.patch** -- adds the `RGFW_windowNoFocusOnCreate` window flag.
  on macos it orders the window front without `activateIgnoringOtherApps` / `makeKeyWindow`,
  so creating it doesn't steal keyboard focus (benchmarks, same-space cover).

- **rgfw-fullscreen-keyable.patch** -- `RGFW_window_setFullscreen` (macos). upstream makes
  the window borderless + status level (`setBorder FALSE` / `setLevel:25` / `orderFront`)
  right before `toggleFullScreen:`. a borderless status-level window can't become the key
  window on cocoa, so `keyDown` never arrives and escape-to-quit is dead in fullscreen. the
  native `toggleFullScreen:` already gives a proper keyable fullscreen Space, so the prelude
  is dropped. arguably an upstream bug worth a PR.

- **rgfw-samespace-cover.patch** -- adds `RGFW_window_coverDisplay` (macos). covers the whole
  display in the *current* Space (above the menu bar, sized to the screen frame) instead of
  native `toggleFullScreen:`, which would open a new Space and yank focus. used by
  `--no-keyfocus-steal` fullscreen.

- **rgfw-borderless-keyable.patch** -- lets the same-space borderless cover gain key focus on
  a single click. three bits: override `canBecomeKeyWindow` -> YES (a borderless NSWindow
  defaults to NO, so it could never be clicked into focus); `acceptsFirstMouse:` -> YES (the
  activating click on an inactive app is otherwise swallowed, needing a second click); and
  `makeFirstResponder(view)` on `windowDidBecomeKey` (route `keyDown` to the view at once, so
  esc works on the first click). together: one click grabs focus + enables esc-to-quit.

- **rgfw-nonkey-mousemoved.patch** -- adds `NSTrackingMouseMoved` to the view's tracking area
  so a non-key window gets continuous `mouseMoved` (not just enter/exit). without it the
  cursor position stales on the same-space cover until it's clicked into focus.

## re-applying after re-vendoring RGFW.h

```
# from repo root, with a fresh pristine lib/RGFW.h in place
for p in rgfw-no-focus-on-create rgfw-samespace-cover rgfw-borderless-keyable \
         rgfw-nonkey-mousemoved rgfw-fullscreen-keyable; do
  patch -p1 < lib/patches/$p.patch
done
```

if upstream moved and one won't apply cleanly, grep `pixel-goo local patch` in this repo's
git history for the exact hunk, reapply by hand, and regenerate that one:

```
diff -u -L a/lib/RGFW.h -L b/lib/RGFW.h <pristine-RGFW.h> lib/RGFW.h \
  > lib/patches/<name>.patch
```
