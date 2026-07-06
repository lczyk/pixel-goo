#version 330 core

// Force-potential (r) + dispersion (g) field. Fully overwritten every frame (single-buffered,
// stateless -- see sim.c's sim_step): a fullscreen analytic pass that paints a wall-shaped
// potential bump near the domain's bounding-rect edges and any --exclusions rects, plus a
// mouse-shaped bump (also written to g, the dispersion channel). velocity.frag integrates the r
// channel (textureVDI) to get a unified repulsion force, and point-samples g to damp the trail
// pull continuously near the cursor -- replacing the old analytic edgeRepell()/mouseRepell().

out vec4 out_repel;

uniform vec2 window_shape; // sim space (sim_width, sim_height) -- same space as position_buffer
uniform vec2 buffer_shape; // this buffer's own resolution (density_width, density_height)
uniform vec2 mouse_position;
uniform vec2 mouse_velocity;

#define MAX_EXCLUSION_RECTS 16
uniform vec4 exclusion_rects[MAX_EXCLUSION_RECTS]; // sim-space (xmin, ymin, xmax, ymax) per rect
uniform int exclusion_rect_count;

// Linear falloff: 1 at d=0, 0 at d>=radius. HARD-edged -- constant gradient inside the radius
// (so a constant repel force, not a soft ramp) and a hard cutoff at the radius, unlike the old
// squared bump which tapered its gradient to zero at the radius (a soft boundary). Used for the
// bounding-rect edges and the mouse; exclusions use the SDF ramp below, which is the same shape.
float ramp(float d, float radius) {
    return max(0.0, 1.0 - d / radius);
}

// Signed distance from p to rect (xmin,ymin,xmax,ymax): >0 outside, 0 on the boundary,
// <0 inside (magnitude = depth). Standard box SDF.
float rect_signed_distance(vec2 p, vec4 rect) {
    vec2 c = 0.5 * (rect.xy + rect.zw); // centre
    vec2 h = 0.5 * (rect.zw - rect.xy); // half-extents
    vec2 q = abs(p - c) - h;
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0);
}

// Exclusion-rect potential as a function of signed distance. ZERO outside the rect, rising
// linearly with depth INSIDE it. Two properties, both requested:
//  - no exterior repel: a particle outside the rect feels nothing from it. Combined with the
//    own-position gate in velocity.frag (which skips the force wherever the sampled potential is
//    0), this kills the old "avoid at reach distance" halo AND the exterior margin -- particles
//    drift right up to, and through, the boundary instead of veering off early. Soft: no barrier
//    at the edge, a particle can enter freely (and further with more momentum).
//  - still ejects once inside: potential rises with depth, so its gradient points inward
//    everywhere inside -> the repel (anti-gradient) pushes OUTWARD, ejecting a particle at any
//    depth toward the nearest wall. Monotonic, so no flat-centre trap.
// Continuous at the boundary (0 at sd=0) -> no hard edge. (Absolute magnitude deep inside is
// irrelevant: the VDI is a disc integral of value*offset, so a constant offset cancels -- only
// the local gradient, 1/radius, drives the force.)
float exclusion_potential(float sd, float radius) {
    return max(0.0, -sd) / radius; // 0 outside; |sd|/radius inside, rising with depth
}

void main() {
    // Recover the sim-space position this texel represents: the inverse of velocity.frag's
    // textureNormalisedCoords (position -> uv), so an r/g value painted here lines up with the
    // uv that later samples it.
    vec2 uv = gl_FragCoord.xy / buffer_shape;
    vec2 position = vec2(uv.x * window_shape.x, (1.0 - uv.y) * window_shape.y);

    float potential = 0.0;

    // Bounding-rect edges -- all 4, UNIFORM thickness. A single scalar radius (5% of the SHORTER
    // side), not 0.05 * window_shape per-axis -- the per-axis version made the left/right bands
    // (5% of width) wider than the top/bottom bands (5% of height) on a non-square window.
    float edge_repell_radius = 0.05 * min(window_shape.x, window_shape.y);
    potential += ramp(position.x, edge_repell_radius);                  // left
    potential += ramp(window_shape.x - position.x, edge_repell_radius); // right
    potential += ramp(position.y, edge_repell_radius);                  // top
    potential += ramp(window_shape.y - position.y, edge_repell_radius); // bottom

    // Exclusion-rect potential (--exclusions): monotonic hill (see exclusion_potential), using the
    // same uniform edge radius. Combined among THEMSELVES by max (not sum): two overlapping rects produce
    // the same peak force in their overlap as either alone, not a doubled force. The maxed result
    // then adds into the total potential alongside the (summed) edge walls and the mouse bump.
    // Exclusion interiors also drive the dispersion stencil (g) below -- like the mouse bubble,
    // an exclusion suppresses the trail pull inside it, so particles there don't get re-dragged in.
    float rect_radius = edge_repell_radius; // exclusion walls use the same uniform radius
    float excl_potential = 0.0;
    float dispersion = 0.0; // g channel: hard 0/1 stencil (inside a dead zone or not)
    for (int i = 0; i < exclusion_rect_count && i < MAX_EXCLUSION_RECTS; i++) {
        float sd = rect_signed_distance(position, exclusion_rects[i]);
        excl_potential = max(excl_potential, exclusion_potential(sd, rect_radius));
        if (sd < 0.0) dispersion = 1.0; // inside this exclusion -> suppress trail
    }
    potential += excl_potential;

    // Mouse: hard-edged linear repel ramp into r (radius scaled with cursor speed -- matches
    // velocity.frag's own mouse_repell_radius exactly, so the repel bubble is consistent).
    const float mouse_repell_radius_min = 30.0;
    const float mouse_repell_radius_max = 75.0;
    const float mouse_repell_speed_ref = 20.0; // cursor speed at which the radius reaches max
    // ease-out: radius grows fast at low speed then plateaus (quadratic 1-(1-t)^2), so a gentle
    // move already opens a wide bubble. Mirrored in velocity.frag + sim.c's --mouse-debug disc.
    float mouse_speed = length(mouse_velocity);
    float mouse_t = clamp(mouse_speed / mouse_repell_speed_ref, 0.0, 1.0);
    float mouse_repell_radius = mix(mouse_repell_radius_min, mouse_repell_radius_max, 1.0 - (1.0 - mouse_t) * (1.0 - mouse_t));
    potential += ramp(length(mouse_position - position), mouse_repell_radius);
    // Dispersion stencil (g): HARD boundary, not the old soft bump -- 1 inside the mouse bubble,
    // 0 outside, a crisp on/off for the trail gate in velocity.frag. (The repel buffer is
    // GL_LINEAR at density resolution, so readback still bilinear-softens this over ~1 texel.)
    if (length(mouse_position - position) < mouse_repell_radius) dispersion = 1.0;

    out_repel = vec4(potential, dispersion, 0.0, 1.0);
}
