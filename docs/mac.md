# mac apple-silicon gpu sim perf notes

- **macos opengl is deprecated and emulated over metal.** it caps at gl 4.1 core. **no compute
  shaders** (those need 4.3). so all gpgpu is done the old way: fragment-shader passes writing into
  float textures, ping-ponged. that's a gather model (each output texel reads inputs at computed
  locations; output position is fixed). you cannot scatter (write to a data-dependent location)
  except by drawing point primitives with a data-dependent `gl_Position`.
- **apple gpus are TBDR** (tile-based deferred renderers, powervr lineage). this is the single most
  important fact. consequences:
  - **alpha blending and overdraw are cheap** -- blending happens on-tile in fast memory. don't
    bother "optimising" blend modes; on this hw turning blend off saved <0.4ms on passes that cost
    5-8ms.
  - **the cost lives in two places instead:** (1) the tiler/geometry stage binning primitives into
    tiles, and (2) fragment *shading* of overdraw. both are sensitive to things that don't matter on
    an immediate-mode gpu.
  - tons of tiny primitives stress the tiler. 1px points are tiny primitives.

## profiling

- **gl timer queries are unreliable on macos.** use `glFinish()` after each pass + a cpu monotonic
  clock. this serialises the gpu (kills pipelining), so the per-pass *ratios* are trustworthy but the
  *total* is inflated vs the real pipelined frame. get the real number from wall-clock frame time
  with the profiler off.
- **always discard the first frame** (shader compile + buffer alloc warmup), it's 5-20x a steady
  frame.
- **chaotic clustering = big run-to-run variance.** the sim's dynamics are chaotic, so the same build
  at the same particle count gives different per-pass times depending on how clustered the goo
  happens to be at the moment of measurement. when comparing two builds, watch a *co-varying* pass
  (e.g. density cost) to tell whether you're seeing your change or just a different cluster state.
  repeat runs; compare against a same-session baseline, never an absolute remembered number.
- **amortised passes lie in the per-pass ema.** if a pass only runs every n frames, a per-frame
  glFinish-delta timer re-folds a near-zero value on the skip frames, so the pass reads ~1/n of its
  real per-update cost. judge amortised work by total frame time, not its per-pass column.

## the load-bearing lesson: spatial coherence of the scatter

the dominant render cost here was drawing ~millions of 1px points (one per particle) into the
screen. the surprise: it's **not** bound by the obvious suspects.

ruled out, each with a probe:
- fragment alu (colormap maths) -- stripping it saved almost nothing.
- the vertex *fetch mechanism* -- reading particle pos/vel via vertex-texture-fetch vs via a vertex
  attribute (vbo) measured **identical**. this is counter to immediate-mode-gpu intuition; on this
  stack the fetch path is not the bottleneck. (we re-tested this twice from scratch -- still
  identical.)
- the texture->vbo readback -- `glReadPixels` from an fbo into a PBO stays on-gpu (unified memory)
  and runs at ~memory bandwidth; copying ~tens of MB/frame was <0.1ms. not a factor even at large N.
- blend / overdraw fill -- cheap on tbdr (see above).

what it actually was: **how spatially scattered the points are, and how incoherently consecutive
draws hit screen tiles.** same point count, same shader:
- points drawn in a spatially-coherent order (raster/grid, or sorted by screen tile): ~baseline X.
- the same points drawn in particle-id order (which is random vs screen position): ~5x X.

mechanism: the tiler appends each primitive to its tile's list in the parameter buffer. consecutive
points that land in far-apart tiles scatter those writes all over memory (cache thrash) and touch
every tile (more active tiles = more per-tile overhead). coherent order = local writes, fewer active
tiles.

corollary that confirms it from the user side: the sim is **slow when particles are spread
uniformly** and **speeds up as they clump**, because clumping concentrates the draws into fewer
tiles. clustering is nature doing a partial tile-sort for free.

two practical exploits:
- **draw in tile-coherent order** via an index buffer (`glDrawElements`). build the order with a cheap
  cpu counting-sort by screen tile, off the positions you read back to a PBO. you don't need to re-sort
  every frame -- particles drift slowly, so an order rebuilt every n frames stays good (amortise it).
  this took the full-res point render from ~scattered-baseline down to ~coherent-baseline (the ~5x).
- accept that small/uniform spreads are the worst case and the sim gets faster as it settles. don't
  benchmark only the first second.

## techniques that worked (general shapes)

- **subsample internal fields, not the final image.** the density/trail fields are smooth, heavily
  downsampled, and lerped -- scattering only every Nth particle into them (with the per-point alpha
  scaled up to compensate) cuts their overdraw with no visible change. the final particle render still
  uses every particle. this was the first big win.
- **pre-rasterisation culling.** if a pass discards fragments by some test (stochastic by density, a
  threshold, etc), do the cull in the *vertex* shader and push culled points offscreen (`gl_Position`
  outside clip space) so they're clipped before the tiler ever bins them. a frag `discard` fires only
  after the point rasterised. caveat: this only pays if it culls a meaningful *fraction*; if few points
  qualify, the per-vertex branch costs all of them and it's a wash. measure the fraction, don't assume.
  use offscreen-clip, not `gl_PointSize=0` (some drivers still raster a 1px point at size 0).
- **seed stochastic culls on particle id, not frame counter.** seeding on a per-frame value rerolls the
  random every frame -> visible flicker. seeding on identity gives each particle a fixed threshold, so it
  culls/uncull smoothly as its local density crosses the threshold. no flicker, same average density.
- **temporal amortisation of slow fields.** a field that's a slow-moving snapshot (e.g. a per-frame
  density rebuild) can be rebuilt every n frames and reused in between. 1-2 frame staleness is negligible
  to the physics that samples it. cut that pass's amortised cost by ~1/n. start at n=2-3; very high n
  starts to show visual stepping.
- **temporal-accumulation fields need interleaved deposit, NOT skip-and-freeze.** a field built by
  decay-then-add (a trail) that also feeds back into the sim (particles follow trails, trails follow
  particles) will *break* if you freeze it for n frames then jump -- the step discontinuity cascades
  through the feedback loop (butterfly; visible discontinuities). instead: keep decaying every frame
  (cheap fullscreen pass) but deposit only a rotating 1/n subset of particles each frame, with per-point
  intensity scaled by n. the field evolves smoothly every frame, the expensive scatter drops ~1/n, and
  there's no discontinuity. (density tolerated freezing; the trail did not -- the difference is the
  feedback loop.)

## dead ends (don't re-try these on this hw)

- **instanced quads instead of `GL_POINTS`.** 4x the vertices + varyings binned for the same 1px output;
  measured *slower*. points are the cheapest primitive for 1px sprites on the tiler.
- **geometry shaders.** metal has no gs hardware; emulated, slow. not a path here.
- **vtf vs vertex attributes** -- identical (above). don't spend time converting for speed.
- **moving a per-fragment texture fetch to the vertex shader** when the point is 1px -- same fetch count,
  no change.
- **blend-mode changes for speed** -- blend is cheap on tbdr.
- **killing transcendentals (sin/cos) in the hot physics pass** -- the pass was texture-bound, not
  alu-bound, so it was a small win; and worse, a "faster" reformulation of the golden-spiral disc
  sampling changed the *dither*, which is load-bearing for the dynamics -> the sim visibly drifted. the
  per-sample spiral shear can't be made trig-free without changing the physics. reverted.
- **culling slow particles before raster** (sound in general) -- neutral here, because in a *churning*
  goo the clustered particles aren't actually slow (density repulsion + dither keep them moving), so too
  few sit below the velocity floor for the cull to pay, and the branch costs all of them.

## physics-safety lessons (this is a chaotic sim)

- **the dynamics amplify tiny changes.** anything that touches the velocity/sampling maths is a physics
  change even if it looks numerically equivalent -- judge it by eye over ~30s of evolution, not by one
  frame or a per-pass number. a change that looks identical for 5 seconds can diverge completely by 30.
- **fields that feed back are fragile to temporal hacks; one-way fields aren't.** a pure snapshot
  (density read by velocity) tolerates staleness/amortisation. an accumulation in a feedback loop (trail)
  needs the smooth interleave, not freezing.
- when amortising or subsampling a field, **compensate the magnitude** (alpha/intensity/decay) so the
  steady-state the physics sees is unchanged, not just the per-frame deposit.

## multi-monitor / retina windowing (gl drawable, not perf)

a window on -- or dragged to -- a monitor of a different backing scale (retina 2x vs non-retina
1x) rendered black or into a sub-quadrant. two distinct causes, both confirmed by apple's docs:

- **the GL drawable does not auto-refit.** a raw `NSOpenGLContext` keeps its old surface when the
  window moves to a screen of a different `contentsScale` unless you call `[context update]`. apple:
  update "updates the attached drawable objects ... ensures the renderer is properly updated for any
  virtual screen changes. if you don't update the rendering context, you may see rendering artifacts."
  fix here: a small `RGFW_window_updateContext_OpenGL` ([ctx update]) called before the upscale, plus
  re-querying the device-px framebuffer to re-fit the viewport.
- **the residual 1-frame flash is inherent to a polling render loop.** `getSizeInPixels`/the backing
  scale lag cocoa's actual display switch by one frame, so the transition frame is presented at the
  old size before any API reports the new one. the *correct* zero-lag fix is event-driven, not polled:
  handle `viewDidChangeBackingProperties` / `NSApplicationDidChangeScreenParametersNotification`, or
  drive rendering from a `CVDisplayLink` / layer-backed `NSOpenGLLayer` (which also fixes live-resize
  flicker -- the same root cause). that's an architecture change; we mitigated instead by setting the
  window background opaque black so the gap blinks black (invisible vs the content) not white.
- nb: `wantsBestResolutionOpenGLSurface` defaults to NO (1px-per-point regardless of backing scale) --
  relevant if you ever want the gl surface itself at native retina res.

sources:
- [Working with Rendering Contexts (apple, the `update` semantics)](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_contexts/opengl_contexts.html)
- [Optimizing OpenGL for High Resolution (apple, backing scale / `viewDidChangeBackingProperties`)](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/EnablingOpenGLforHighResolution/EnablingOpenGLforHighResolution.html)
- [Advanced NSView setup with OpenGL/Metal on macOS (layer-backed view fix)](https://metashapes.com/blog/advanced-nsview-setup-opengl-metal-macos/)
- [OpenGL flickering on window resize (macrumors thread)](https://forums.macrumors.com/threads/opengl-flickering-on-window-resizing.1537562/)

## the floor: per-particle cost scales with N

after the structural wins, the remaining cost is just "do per-particle work for N particles" across the
physics pass + the few scatter passes. that scales ~linearly with particle count. 10x the particles is
~10x the work; you can shave the per-particle *constant* but you can't make 10x-particles match 1x. pick
a particle count for the frame budget you want, and use the levers above to push the constant down so you
can afford more particles at that budget.

## links / leads

found during research; verify before trusting, and note several are immediate-mode-gpu or
nvidia-specific and **don't fully transfer** to apple tbdr.

- apple developer docs: "optimize metal performance for apple silicon" / the tbdr + tile-memory
  material. the canonical explanation of why blend/overdraw is cheap and geometry/tiling is where you
  pay. (transfers directly -- gl is emulated on this same metal/tbdr.)
- apple developer forums thread ~#69217 -- a particle sim hitting the same tiler-binding wall, ~2x faster
  via voxel/screen-bucket draw ordering with no primitive-count change. closest real analog to the
  tile-sort win here.
- powervr / imagination tbdr architecture docs -- parameter buffer, tiling, hidden-surface removal.
  apple gpus are this lineage; good background for *why* primitive count and draw order matter.
- markus schutz et al, point-cloud rendering with compute -- the big "draw order for locality" results
  live here, BUT they're nvidia/compute and depth-test-overdraw driven; the headline 4x numbers do not
  transfer (we have no depth test and overdraw is cheap). useful for the *idea*, not the magnitudes.
- geeks3d point-sprite vs instancing benchmarks -- corroborates point sprites beating instancing for
  many-particle draws.
