# local patches

local changes to vendored libraries that aren't header-only drop-ins. each `.patch`
is a unified diff against the pristine upstream file, so it survives re-vendoring.

## rgfw-borderless-fullscreen.patch

four local changes to `lib/RGFW.h` (all tagged `pixel-goo local patch` inline) that
make borderless fullscreen behave on macos:

- `canBecomeKeyWindow` / `canBecomeMainWindow` forced to `YES`, so the borderless
  status-level fullscreen window can take key focus -- without it no `keyDown`
  events arrive and escape-to-quit never fires.
- drop the redundant `toggleFullScreen:` call in `RGFW_window_setFullscreen`; it
  dropped the window into a native fullscreen Space, which re-showed the menu bar
  and added the Space transition. the manual borderless + status-level cover is
  enough.
- hide the menu bar + dock via `setPresentationOptions` on enter (a status-level
  window does not cover the menu bar on modern macos, esp. with the notch), and
  restore them on exit.

generated against upstream `ColleagueRiley/RGFW` `main` (v2.0.0-dev), which matched
the vendored copy byte-for-byte apart from these changes.

### re-applying after re-vendoring RGFW.h

```
# from repo root, with a fresh pristine lib/RGFW.h in place
patch -p1 < lib/patches/rgfw-borderless-fullscreen.patch
```

if upstream has moved and the patch no longer applies cleanly, reapply the four
changes by hand -- grep `pixel-goo local patch` in this repo's git history for the
exact blocks -- and regenerate the patch:

```
diff -u -L a/lib/RGFW.h -L b/lib/RGFW.h <pristine-RGFW.h> lib/RGFW.h \
  > lib/patches/rgfw-borderless-fullscreen.patch
```
