# local patches

local changes to `lib/RGFW.h` (not a header-only drop-in). the `.patch` is a unified
diff against pristine upstream, so it survives re-vendoring.

## rgfw-fullscreen-keyable.patch

one change to `RGFW_window_setFullscreen` (macos), tagged `pixel-goo local patch` inline.

upstream makes the window borderless + status-level (`setBorder FALSE` / `setLevel:25`
/ `orderFront`) right before `toggleFullScreen:`. a borderless status-level window
can't become the key window on cocoa, so `keyDown` never arrives and escape-to-quit is
dead in fullscreen. the native `toggleFullScreen:` already gives a proper keyable
fullscreen Space (title bar + menu bar auto-hidden), so the prelude is dropped.

generated against upstream `ColleagueRiley/RGFW` `main` (v2.0.0-dev), which matched the
vendored copy byte-for-byte apart from this change. arguably an upstream bug worth a PR.

### re-applying after re-vendoring RGFW.h

```
# from repo root, with a fresh pristine lib/RGFW.h in place
patch -p1 < lib/patches/rgfw-fullscreen-keyable.patch
```

if upstream moved and it won't apply cleanly, grep `pixel-goo local patch` in this repo's
git history for the exact hunk, reapply by hand, and regenerate:

```
diff -u -L a/lib/RGFW.h -L b/lib/RGFW.h <pristine-RGFW.h> lib/RGFW.h \
  > lib/patches/rgfw-fullscreen-keyable.patch
```
