#ifdef GL_ES
precision mediump float;
#endif

uniform float u_time;
uniform vec2  u_resolution;
uniform float u_energy;   // 0..1  held-note intensity
uniform float u_pitch;    // -1..1 pitch bend
uniform float u_mod;      // 0..1  mod wheel (CC1)
uniform float u_note;     // 0..1  last note / 127

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec2 p = uv - 0.5;
    p.x *= u_resolution.x / u_resolution.y;

    // pitch bend warps space; mod wheel adds swirl
    p *= 1.0 + u_pitch * 0.5;
    float ang = u_mod * 6.2831 * length(p);
    float ca = cos(ang), sa = sin(ang);
    p = mat2(ca, -sa, sa, ca) * p;

    float t = u_time * (0.5 + u_energy * 2.0);   // notes speed it up

    float v = sin(p.x * 10.0 + t)
            + sin(p.y * 10.0 + t * 1.3)
            + sin((p.x + p.y) * 10.0 + t * 0.7)
            + sin(length(p) * 24.0 - t * 2.0);
    v *= 0.25;

    // last note shifts hue; energy pumps brightness
    vec3 base = 0.5 + 0.5 * cos(6.2831 * (v + u_note + vec3(0.0, 0.33, 0.67)));
    vec3 col = base * (0.35 + 0.65 * u_energy);

    // bright core flash on energy
    col += u_energy * 0.4 * smoothstep(0.5, 0.0, length(p));

    gl_FragColor = vec4(col, 1.0);
}
