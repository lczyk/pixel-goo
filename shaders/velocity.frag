#version 330 core

#define PI 3.141592653589793
#define GOLD 1.618033988749895
#define MOUSE_REPELL
#define EDGE_REPELL

// Access fragmet coordinates in integer steps
// TODO: explain more
layout(pixel_center_integer) in vec4 gl_FragCoord;
out vec4 out_velocity;

uniform sampler2D density_buffer;
uniform sampler2D trail_buffer;

#ifdef MOUSE_REPELL
uniform vec2 mouse_position;
uniform vec2 mouse_velocity; // per-frame cursor delta (sim units); not wired into the force yet
#endif

uniform vec2 window_shape;
in float VertexID;

uniform int epoch_counter;
uniform float drag_coefficient;
uniform float dither_coefficient;
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

// Vector Disk Integral over a texture using golden spiral disc sampling
vec2 textureVDI(sampler2D textureSampler, vec2 position, float radius, float near_clip, int N) {

    // Value of the integral
    vec2 integral = vec2(0,0);

    // A pile of static values pulled out of the loop
    float invN2 = 1/(N * N); // inverse of N^2
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
        float sample = tmap(texture(textureSampler, textureNormalisedCoords(position + sample_xy)).x); // tmap: density is additive/unbounded

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
    float wedge_direction = acos(dot(velocity,vec2(1,0))/length(velocity));
    float invN2 = 1/(N * N); // inverse of N^2
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

// Mouse interaction
#ifdef MOUSE_REPELL
vec2 mouseRepell(vec2 mouse_vector, float mouse_repell_radius, float mouse_repell_coefficient) {
    vec2 repell = vec2(0,0);
    float mouse_vector_length = length(mouse_vector);
    if (mouse_vector_length < mouse_repell_radius) {
        vec2 mouse_vector_normal;
        if (mouse_vector_length > 0) {
            mouse_vector_normal = mouse_vector / mouse_vector_length;
        } else {
            mouse_vector_normal = random_vec2(vec2(0,2) + VertexID + epoch_counter);
            mouse_vector_normal /= length(mouse_vector_normal);
        }
        repell = (1-(mouse_vector_length/mouse_repell_radius)) * (1-(mouse_vector_length/mouse_repell_radius)) * mouse_vector_normal;
    }
    return mouse_repell_coefficient * repell;
}
#endif

// Mouse interaction
#ifdef EDGE_REPELL
vec2 edgeRepell(vec2 position, vec2 window_shape, vec2 edge_repell_radius, float edge_repell_coefficient) {
    vec2 repell = vec2(0,0);
    float edge_vector_length;
    // Left edge repell
    edge_vector_length = position.x;
    if (edge_vector_length < edge_repell_radius.x) {
        repell += vec2(-1,0) * (1-(edge_vector_length/edge_repell_radius.x)) * (1-(edge_vector_length/edge_repell_radius.x));
    }
    // Right edge repell
    edge_vector_length = window_shape.x-position.x;
    if (edge_vector_length < edge_repell_radius.x) {
        repell += vec2(1,0) * (1-(edge_vector_length/edge_repell_radius.x)) * (1-(edge_vector_length/edge_repell_radius.x));
    }
    // Top edge repell
    edge_vector_length = position.y;
    if (edge_vector_length < edge_repell_radius.y) {
        repell += vec2(0,-1) * (1-(edge_vector_length/edge_repell_radius.y)) * (1-(edge_vector_length/edge_repell_radius.y));
    }
    // Bottom edge repell
    // edge_vector_length = window_shape.y-position.y;
    // if (edge_vector_length < edge_repell_radius.y) {
    //     repell += vec2(0,1) * (1-(edge_vector_length/edge_repell_radius.y)) * (1-(edge_vector_length/edge_repell_radius.y));
    // }
    return edge_repell_coefficient * repell;
}
#endif

void main() {
    ivec2 buffer_position = ivec2(gl_FragCoord.xy);
    vec2 position = vec2(texelFetch(position_buffer, buffer_position, 0)); // current position
    vec2 velocity = vec2(texelFetch(velocity_buffer, buffer_position, 0)); // previous velocity
    float density = tmap(texture(density_buffer, textureNormalisedCoords(position)).x); // tmap: density is additive/unbounded

    vec2 new_velocity = velocity;

    // Mouse repell
#ifdef MOUSE_REPELL
    const float mouse_repell_radius_min = 30;   // radius at rest (mouse always repels this much)
    const float mouse_repell_radius_max = 75;   // radius at/above full cursor speed
    const float mouse_repell_speed_scale = 3.0; // radius (sim units) per unit of cursor speed
    const float mouse_repell_coefficient = 0.1;
    const float mouse_drag_fraction = 0.1;      // drag gain at the reference cursor speed
    const float mouse_drag_speed_ref = 30.0;    // cursor speed (sim u/frame) at which gain == fraction
    // Scale the interaction radius with cursor speed, floored at the min: faster motion
    // grows the radius up to the cap.
    float mouse_speed = length(mouse_velocity);
    float mouse_repell_radius = clamp(mouse_speed * mouse_repell_speed_scale, mouse_repell_radius_min, mouse_repell_radius_max);
    vec2 mouse_vector = mouse_position - position;
    float mouse_dist = length(mouse_vector);
    new_velocity -= mouseRepell(mouse_vector, mouse_repell_radius, mouse_repell_coefficient);
    bool inmouseradius = mouse_dist < mouse_repell_radius;
    // Drag: sweep in-radius particles along the cursor's motion (squared spatial falloff,
    // like the repel). Gain also scales with cursor speed, so the imparted velocity grows
    // ~speed^2 -- faster flicks fling particles much harder; a still mouse imparts nothing.
    float mouse_drag_falloff = max(0.0, 1.0 - mouse_dist / mouse_repell_radius);
    float mouse_drag_gain = mouse_drag_fraction * mouse_speed / mouse_drag_speed_ref;
    new_velocity += mouse_drag_gain * mouse_drag_falloff * mouse_drag_falloff * mouse_velocity;
    
    // Screen wrap of mouse repell (basically add additional 8 mouse positions)
    // new_velocity -= mouseRepell(mouse_vector + vec2(+window_shape.x,0), mouse_repell_radius, mouse_repell_coefficient);
    // new_velocity -= mouseRepell(mouse_vector + vec2(-window_shape.x,0), mouse_repell_radius, mouse_repell_coefficient);
    // new_velocity -= mouseRepell(mouse_vector + vec2(0,+window_shape.y), mouse_repell_radius, mouse_repell_coefficient);
    // new_velocity -= mouseRepell(mouse_vector + vec2(0,-window_shape.y), mouse_repell_radius, mouse_repell_coefficient);
    // new_velocity -= mouseRepell(mouse_vector + vec2(+window_shape.x,+window_shape.y), mouse_repell_radius, mouse_repell_coefficient);
    // new_velocity -= mouseRepell(mouse_vector + vec2(-window_shape.x,+window_shape.y), mouse_repell_radius, mouse_repell_coefficient);
    // new_velocity -= mouseRepell(mouse_vector + vec2(+window_shape.x,-window_shape.y), mouse_repell_radius, mouse_repell_coefficient);
    // new_velocity -= mouseRepell(mouse_vector + vec2(-window_shape.x,-window_shape.y), mouse_repell_radius, mouse_repell_coefficient);
#endif

    // Integrate density over a disk in a radius
#ifdef MOUSE_REPELL
    // if (!inmouseradius) {
#endif /* MOUSE_REPELL */
        vec2 density_integral = textureVDI(density_buffer, position, 20, 2, 20);
        // new_velocity -= 0.01 * density_integral;
        new_velocity -= 0.04 * density_integral;
        // new_velocity -= (1-density) * 0.02 * density_integral;
        // new_velocity -= (1-(1-density)*(1-density)) * 0.02 * density_integral;
#ifdef MOUSE_REPELL
    // }
#endif /* MOUSE_REPELL */

    // Trial integral
#ifdef MOUSE_REPELL
    if (!inmouseradius) {
#endif /* MOUSE_REPELL */
        vec2 trail_integral = textureVWI(trail_buffer, position, velocity, PI*0.6, 30, 10, 20);
        // new_velocity += 0.07 * trail_integral;
        // new_velocity += clamp(1-density,0.2,1.0) * 0.05 * trail_integral;
        new_velocity += clamp((1-(density * density * density)), 0.8, 1) * 0.07 * trail_integral;
#ifdef MOUSE_REPELL
    }
#endif /* MOUSE_REPELL */

#ifdef EDGE_REPELL
    vec2 edge_repell_radius = 0.05 * window_shape;
    const float edge_repell_coefficient = 0.15;
    new_velocity -= edgeRepell(position, window_shape, edge_repell_radius, edge_repell_coefficient);
#endif /* EDGE_REPELL */

    // Dither
    // new_velocity += dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);
    // new_velocity += (1-density) * dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);
    
    // new_velocity += clamp(1-density,0.1,1.0) * clamp(1-density,0.1,1.0) * 2 * dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);
    new_velocity += density * dither_coefficient * random_vec2(vec2(0,0) + VertexID + epoch_counter);
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