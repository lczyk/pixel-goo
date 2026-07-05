#version 330 core
out vec4 color;

uniform sampler2D density_src; // instantaneous density field (rebuilt each frame), R channel
uniform int window_radius;     // box half-width in texels

// density accumulates additively (unbounded); tone-map to the [0,1] the physics uses so the
// tensor isn't dominated by bright cores. matches tmap() in velocity.frag.
float tmap(float d) { return 1.0 - exp(-d); }

// Gradient structure tensor of the density field, box-averaged over a (2r+1)^2 window. Writes
// (Jxx, Jxy, Jyy) = avg(dx^2), avg(dx*dy), avg(dy^2). ONE non-separable pass: the buffer is tiny
// (~density res), so the fewest draw calls wins over the fewest fetches. Source is the
// INSTANTANEOUS density (not the slow field) -- the box-average already denoises, and the current
// lines to break live here, not in the smeared long-term field. The velocity shader eigen-solves
// this per particle: J's major eigenvector points ACROSS a density ridge, so it's the push
// direction to break lines; coherence (l1-l2)/(l1+l2) gates it.
void main() {
    ivec2 sz = textureSize(density_src, 0);
    vec2 texel = 1.0 / vec2(sz);
    vec2 uv = gl_FragCoord.xy * texel;

    vec3 J = vec3(0.0);
    float n = 0.0;
    for (int j = -window_radius; j <= window_radius; j++) {
        for (int i = -window_radius; i <= window_radius; i++) {
            vec2 p = uv + vec2(i, j) * texel;
            // central differences of the tone-mapped density (GL_REPEAT wrap handles edges).
            // the /2 factor is dropped -- a constant scale absorbed into --line-force.
            float dx = tmap(texture(density_src, p + vec2(texel.x, 0.0)).x)
                     - tmap(texture(density_src, p - vec2(texel.x, 0.0)).x);
            float dy = tmap(texture(density_src, p + vec2(0.0, texel.y)).x)
                     - tmap(texture(density_src, p - vec2(0.0, texel.y)).x);
            J += vec3(dx * dx, dx * dy, dy * dy);
            n += 1.0;
        }
    }
    color = vec4(J / n, 1.0);
}
