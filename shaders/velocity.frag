#version 330 core

#define PI 3.141592653589793
#define GOLD 1.618033988749895
#define MOUSE_REPELL

// Access fragmet coordinates in integer steps
// TODO: explain more
layout(pixel_center_integer) in vec4 gl_FragCoord;
out vec4 out_velocity;

uniform sampler2D density_buffer;
uniform sampler2D trail_buffer;
uniform sampler2D repel_buffer; // r = force potential (walls + exclusion rects + mouse), g = dispersion

#ifdef MOUSE_REPELL
uniform vec2 mouse_position;
uniform vec2 mouse_velocity; // per-frame cursor delta (sim units); not wired into the force yet
#endif

uniform vec2 window_shape;
uniform vec2 buffer_shape; // repel buffer resolution (repel_width, repel_height) -- for the local-gradient step
in float VertexID;

uniform int epoch_counter;
uniform float drag_coefficient;
uniform float dither_coefficient;
uniform float dither_density_gain; // --dither-gain: gain on the density-scaled dither
uniform float dither_ortho;        // --dither-ortho: density^2-scaled jitter perpendicular to velocity
uniform int legacy_wedge;          // --legacy-wedge: 1 = pre-fix trail-integral heading (acos)
uniform float density_force;  // density-gradient repel strength (was 0.04)
uniform float trail_force;    // trail-gradient attract strength (was 0.07)
uniform float repel_coefficient; // --repel: unified repel-buffer force strength (walls + exclusions + mouse)
uniform float reach;          // density/interaction VDI sampling radius, sim px (was 20)
uniform float anti_stick;     // --anti-stick: tiny cardinal drift freeing motionless particles in repel zones
uniform float trail_reach;    // trail VWI sampling radius, sim px (was 30)
uniform sampler2D position_buffer;
uniform sampler2D velocity_buffer;

// Based on:
// https://thebookofshaders.com/10/
// http://patriciogonzalezvivo.com
vec2 random_vec2 (vec2 seed) { // random_vec2 from -1 to +1
    float a = dot(seed.xy,vec2(0.890,0.870));
    float b = dot(seed.xy,vec2(-0.670,0.570));
    return 2*fract(sin(vec2(a,b))*43758.5453123)-1;
}

float random_float (float seed) { // random_vec2 from -1 to +1
    return 2*fract(sin(seed * 0.890)*43758.5453123)-1;
}

float modFloat(float x, float y) {
    return x - y * floor(x/y);
}

// The density buffer is now accumulated ADDITIVELY (unbounded). Tone-map it back to the
// [0,1] range the physics expects: 1-exp(-d) reproduces the old saturating field
// (1-(1-a)^n) to high precision, so the dynamics are ~unchanged. Apply on every density
// read (the scalar below + each disc-integral sample), but NOT to the trail field.
float tmap(float d) { return 1.0 - exp(-d); }

// Textures are smaples from the *bottom* left corner, and in normalised coordinates
vec2 textureNormalisedCoords(vec2 coordinate) {
    coordinate = mod(coordinate, window_shape);
    return vec2(
        +coordinate.x/window_shape.x,
        -(coordinate.y/window_shape.y)
        );
}

// Vector Disk Integral over a texture using golden spiral disc sampling.
// apply_tmap: true for the additive/unbounded density field (needs the saturating tone-map
// to stay in a sane range); false for fields that are already bounded per-source (e.g. the
// interaction potential), where tmap would just needlessly compress overlapping sources.
vec2 textureVDI(sampler2D textureSampler, vec2 position, float radius, float near_clip, int N, bool apply_tmap) {

    // Value of the integral
    vec2 integral = vec2(0,0);

    // A pile of static values pulled out of the loop
    float invN2 = 1.0/float(N * N); // inverse of N^2 (float div -- int 1/(N*N) was 0)
    float phase_multiplier = (1/radius) * PI * random_float(0 + VertexID + epoch_counter); // Static random value (-pi,pi) divided by the radius
    float reduced_radius = radius - near_clip;

    // Sample N points on a disk
    for (int i = 0; i < N; i ++) {

        // Goldern-ratio disc sampling
        float r = (i+0.5) * (i+0.5) * invN2 * reduced_radius + near_clip;;
        float theta = 2*PI*GOLD * (i+0.5);

        // Dither phase
        // This should be rerolled for each i, but its faster to just roll it once per frame
        // float phase = sqrt(r*iradius) * phase_multiplier;
        float phase = r * phase_multiplier;
        // float phase = 0; // or it could alwasy be zero i guess
        theta += phase;

        // Relative coordinate of the sample
        vec2 sample_xy = r * vec2(cos(theta), sin(theta));
        float sample = texture(textureSampler, textureNormalisedCoords(position + sample_xy)).x;
        if (apply_tmap) { sample = tmap(sample); } // tmap: density is additive/unbounded

        // Final check to make sure no nonsence is added to the integral
        if (isnan(sample) || isinf(sample)) { sample = 0; }
        integral += sample * sample_xy;
    }
    // Divide by the number of points to get the final value of the integral
    return integral/N;
}

// Vector Wedge Integral over a texture using golden spiral disc sampling
vec2 textureVWI(sampler2D textureSampler, vec2 position, vec2 velocity, float wedge_angle, float radius, float near_clip, int N) {
    vec2 integral = vec2(0,0);
    // heading of the velocity. acos(vx/|v|) only spans [0,PI] -- it drops the sign of vy, so
    // downward-moving particles sense the trail in a mirrored (upward) cone. that's the pre-fix
    // behaviour, kept behind --legacy-wedge; atan(y,x) gives the correct full [-PI,PI] heading.
    float wedge_direction = (legacy_wedge != 0)
        ? acos(dot(velocity, vec2(1,0)) / length(velocity))
        : atan(velocity.y, velocity.x);
    float invN2 = 1.0/float(N * N); // inverse of N^2 (float div -- int 1/(N*N) was 0)
    float phase_multiplier = (1/radius) * PI * random_float(1 + VertexID + epoch_counter); // Static random value (-pi,pi)

    for (int i = 0; i < N; i ++) {
        float r = (i+0.5) * (i+0.5) * invN2 * (radius-near_clip) + near_clip;
        float theta = wedge_angle * GOLD * (i+0.5);
        theta += r * phase_multiplier;
        theta = modFloat(theta, wedge_angle) - wedge_angle * 0.5; // limit theta to +/- half of the wedge_angle
        vec2 sample_xy = r * vec2(cos(theta + wedge_direction), sin(theta + wedge_direction));
        float sample = texture(textureSampler, textureNormalisedCoords(position + sample_xy)).x;
        if (isnan(sample) || isinf(sample)) { sample = 0; }
        integral += sample * sample_xy;
    }
    return integral/N;
}

void main() {
    ivec2 buffer_position = ivec2(gl_FragCoord.xy);
    vec2 position = vec2(texelFetch(position_buffer, buffer_position, 0)); // current position
    vec2 velocity = vec2(texelFetch(velocity_buffer, buffer_position, 0)); // previous velocity
    float density = tmap(texture(density_buffer, textureNormalisedCoords(position)).x); // tmap: density is additive/unbounded

    vec2 new_velocity = velocity;

    // Mouse drag: the repel itself now comes from the unified repel-buffer force below
    // (repel_buffer.r, splatted in repel.frag); this is the one mouse mechanism
    // that stays analytic -- it sweeps particles along the cursor's MOTION (not a repulsion
    // potential), so it still reads mouse_position/mouse_velocity directly.
#ifdef MOUSE_REPELL
    const float mouse_repell_radius_min = 30;   // radius at rest (mouse always repels this much)
    const float mouse_repell_radius_max = 75;   // radius at/above the reference cursor speed
    const float mouse_repell_speed_ref = 20.0;  // cursor speed (sim u/frame) at which radius == max
    const float mouse_drag_fraction = 0.1;      // drag gain at the reference cursor speed
    const float mouse_drag_speed_ref = 30.0;    // cursor speed (sim u/frame) at which gain == fraction
    // Interaction radius eases out with cursor speed: it grows FAST at low speed then plateaus at
    // max (quadratic ease-out 1-(1-t)^2), so even a gentle move already opens a wide bubble.
    // Matches repel.frag's own mouse radius + sim.c's --mouse-debug disc exactly.
    float mouse_speed = length(mouse_velocity);
    float mouse_t = clamp(mouse_speed / mouse_repell_speed_ref, 0.0, 1.0);
    float mouse_repell_radius = mix(mouse_repell_radius_min, mouse_repell_radius_max, 1.0 - (1.0 - mouse_t) * (1.0 - mouse_t));
    vec2 mouse_vector = mouse_position - position;
    float mouse_dist = length(mouse_vector);
    // Drag: sweep in-radius particles along the cursor's motion (squared spatial falloff,
    // like the repel). Gain also scales with cursor speed, so the imparted velocity grows
    // ~speed^2 -- faster flicks fling particles much harder; a still mouse imparts nothing.
    float mouse_drag_falloff = max(0.0, 1.0 - mouse_dist / mouse_repell_radius);
    float mouse_drag_gain = mouse_drag_fraction * mouse_speed / mouse_drag_speed_ref;
    new_velocity += mouse_drag_gain * mouse_drag_falloff * mouse_drag_falloff * mouse_velocity;
#endif

    // Integrate density over a disk in a radius
    // (the dispersion gate below was retired -- see the trail integral -- this one was already
    // dead/commented before this rewrite and stays that way: the density integral is never
    // gated by dispersion.)
#ifdef MOUSE_REPELL
    // if (!inmouseradius) {
#endif /* MOUSE_REPELL */
        vec2 density_integral = textureVDI(density_buffer, position, reach, 2, 20, true);
        // new_velocity -= 0.01 * density_integral;
        new_velocity -= density_force * density_integral;
        // new_velocity -= (1-density) * 0.02 * density_integral;
        // new_velocity -= (1-(1-density)*(1-density)) * 0.02 * density_integral;
#ifdef MOUSE_REPELL
    // }
#endif /* MOUSE_REPELL */

    // Trail integral. Dispersion (repel_buffer.g, mouse-sourced -- see repel.frag)
    // continuously damps the trail pull instead of the old hard mouse-radius gate: 0 right at
    // the mouse (fully suppressed), ramping linearly to 1 (unaffected) at its repel radius.
    float dispersion = texture(repel_buffer, textureNormalisedCoords(position)).g;
    vec2 trail_integral = textureVWI(trail_buffer, position, velocity, PI*0.6, trail_reach, 10, 20);
    new_velocity += (1.0 - dispersion) * clamp((1-(density * density * density)), 0.8, 1) * trail_force * trail_integral;

    // Unified repel-buffer force: repels off the pre-splatted potential field r (bounding-
    // rect edges, --exclusions rects, and the mouse -- see repel.frag). repel_coefficient
    // (--repel) is the overall strength (walls + exclusions + mouse); pre-unification this was the
    // edge-only --edge-repel strength.
    //
    // HARD-edged: two things keep the boundary crisp on both sides.
    //  - the force is the LOCAL gradient of r (a 4-tap central difference over one repel-buffer
    //    texel), NOT a reach-wide disc integral (VDI). The VDI averaged over `reach` (~20
    //    sim px), so it picked up a source as soon as its disc TOUCHED one -- a fat soft halo. The
    //    local gradient only sees r within a texel, so the force turns on within a texel of the
    //    boundary. (The repel buffer is at density resolution, so ~1 texel is the hardness
    //    floor.) The potential shapes are hard too: linear ramps / SDF, constant gradient inside.
    //  - the own-position gate: if r == 0 right here, this particle is outside every primitive --
    //    shortcut to no force, so the OUTSIDE stays hard (no gradient bleed from a neighbouring
    //    in-primitive texel pulling on a particle that isn't in one).
    float own_potential = texture(repel_buffer, textureNormalisedCoords(position)).r;
    if (own_potential > 0.0) {
        vec2 texel = window_shape / buffer_shape; // one repel-buffer texel, in sim px
        float rL = texture(repel_buffer, textureNormalisedCoords(position - vec2(texel.x, 0.0))).r;
        float rR = texture(repel_buffer, textureNormalisedCoords(position + vec2(texel.x, 0.0))).r;
        float rD = texture(repel_buffer, textureNormalisedCoords(position - vec2(0.0, texel.y))).r;
        float rU = texture(repel_buffer, textureNormalisedCoords(position + vec2(0.0, texel.y))).r;
        // Divide by the 2-texel span -> true grad(r) in sim units, so the force is independent of
        // buffer/render resolution (a codebase invariant: --render-scale must not change dynamics).
        vec2 grad = vec2(rR - rL, rU - rD) / (2.0 * texel);
        new_velocity -= repel_coefficient * grad;
    }

    // Anti-stick nudge: a TINY constant cardinal drift (per particle, fixed by index) applied ONLY
    // to particles that are essentially motionless inside a dispersion zone -- i.e. "completely
    // stuck". A particle can pin on a spot where the repel gradient cancels (a rect's medial axis,
    // or a flat patch of the coarse repel buffer) and sit there forever. This only breaks the
    // symmetry so it drifts off that spot, after which the repel gradient re-engages and ejects it.
    // It is NOT the ejection mechanism: where the gradient is live the particle is already moving
    // (the repel above pushes it toward the nearest outside), so `stuck` -> 0 and this fades out --
    // the repel, not the drift, decides the direction. That's the fix for particles previously just
    // sliding in their fixed cardinal direction instead of being ejected toward the boundary.
    if (dispersion > 0.0) {
        float stuck = 1.0 - clamp(length(velocity) / 0.05, 0.0, 1.0); // 1 if motionless, ->0 once moving
        if (stuck > 0.0) {
            int dir = int(mod(VertexID, 4.0));
            vec2 nudge = dir == 0 ? vec2(0.0, 1.0)   // up
                       : dir == 1 ? vec2(0.0, -1.0)  // down
                       : dir == 2 ? vec2(-1.0, 0.0)  // left
                       :            vec2(1.0, 0.0);  // right
            new_velocity += dispersion * stuck * anti_stick * nudge; // --anti-stick; tiny, only unsticks the motionless
        }
    }

    // Dither
    // new_velocity += dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);
    // new_velocity += (1-density) * dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);
    
    // new_velocity += clamp(1-density,0.1,1.0) * clamp(1-density,0.1,1.0) * 2 * dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);
    // dither_density_gain (--dither-gain): how strongly dither ramps with local density
    // (>1 = more jitter in dense regions, keeps packed veins churning instead of freezing).
    new_velocity += dither_density_gain * density * dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);

    // Orthogonal dither: density^2-scaled jitter perpendicular to the current velocity.
    if (dither_ortho > 0) {
        float vmag = length(new_velocity);
        if (vmag > 0) {
            vec2 perp = vec2(-new_velocity.y, new_velocity.x) / vmag; // unit normal
            // smoothstep gate: ~0 until density nears the knee, then ramps in fast.
            // NOTE: knee 0.75..1.0 hardcoded; widen/shift if it triggers too early/late.
            // deterministic per-particle side: left or right, fixed by VertexID (no epoch term).
            float side = sign(random_float(VertexID));
            new_velocity += dither_ortho * density * density * side * perp;
        }
    }
    // new_velocity += density * density * density * 2 * 2 * dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);

    // Drift
    // new_velocity += 0.1 * vec2(1.0, 1.0);
    // new_velocity += 0.02 * (1-density) * vec2(0.0, -1.0);
    // vec2 rotating_gravity = vec2(sin(epoch_counter*2*PI/300), cos(epoch_counter*2*PI/300));
    // new_velocity += (1- clamp(velocity,0.0,1.0)*density) * 0.1 * rotating_gravity;
    // if (density > 0.95) {
    //     new_velocity += 0.09 * abs(random_vec2(vec2(3,2) + VertexID + epoch_counter)) * vec2(0,-1);
    // }

    // Resolve drag after all other acceleration to make sure that very high drag coefficient works
    float old_velocity_magnitude = length(velocity);
    float velocity_magnitude = length(new_velocity);
    vec2 velocity_normal;
    if (velocity_magnitude == 0) { // Return random normal if magnitude is zero
        velocity_normal = random_vec2(vec2(1,0) + VertexID + epoch_counter);
        velocity_normal /= length(velocity_normal);
    } else {
        velocity_normal = new_velocity / velocity_magnitude;
    }

    // float drag_magnitude = drag_coefficient * velocity_magnitude * velocity_magnitude;
    float drag_magnitude = drag_coefficient * old_velocity_magnitude * old_velocity_magnitude;

    // float drag_magnitude = density * drag_coefficient * velocity_magnitude * velocity_magnitude;
    // float drag_magnitude = (1-density) * drag_coefficient * velocity_magnitude * velocity_magnitude;
    drag_magnitude = min(velocity_magnitude, drag_magnitude); // Cap drag as to not push particles the other way
    new_velocity -= drag_magnitude * velocity_normal;

    // if (any(isnan(new_velocity)) || any(isinf(new_velocity))) {
    //     new_velocity = vec2(0,0);
    // }

    // If the velocity got messed up somehow, recover it to a small random value
    if ( isinf(new_velocity.x) || isnan(new_velocity.x ) ) { new_velocity.x = random_float(-1 + VertexID + epoch_counter); }
    if ( isinf(new_velocity.y) || isnan(new_velocity.y ) ) { new_velocity.y = random_float(-2 + VertexID + epoch_counter); }

    out_velocity = vec4(new_velocity, 0.0, 1.0);
}