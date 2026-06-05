#ifdef GL_ES
precision mediump float;
#endif

uniform float u_time;
uniform vec2  u_resolution;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    float t = u_time;

    float v = sin(uv.x * 10.0 + t)
            + sin(uv.y * 10.0 + t * 1.3)
            + sin((uv.x + uv.y) * 10.0 + t * 0.7)
            + sin(length(uv - 0.5) * 20.0 - t * 2.0);
    v *= 0.25;

    vec3 col = 0.5 + 0.5 * cos(6.28318 * (v + vec3(0.0, 0.33, 0.67)));
    gl_FragColor = vec4(col, 1.0);
}
