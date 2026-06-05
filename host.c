/* host.c — minimal EGL + GLES2 fullscreen fragment-shader runner for the
 * MIPS Creator CI20 (PowerVR SGX540, FLIP WSEGL / fbdev, no X needed).
 *
 * Renders the fragment shader into a smaller offscreen FBO, then upscales it to
 * the full 1080p framebuffer with a cheap textured blit. This keeps the heavy
 * per-pixel shader work at the lower internal resolution => much higher fps.
 *
 * Uniforms provided to the scene shader (Shadertoy/glslViewer style):
 *     uniform float u_time;        seconds since start
 *     uniform vec2  u_resolution;  internal (render) resolution in pixels
 *
 * Build on the board:  gcc -O2 host.c -I./include -lEGL -lGLESv2 -lm -lrt -o glrun
 * Run:   ./glrun <shader> [seconds] [scale]
 *        seconds: 0 or omitted => run until Ctrl-C
 *        scale:   integer divisor for internal res (default 3 => 1920/3 = 640x360)
 *                 scale 1 = native res (slowest), higher = faster/softer
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open shader '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static GLuint compile_shader(GLenum type, const char *src, const char *label) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "ERROR: %s shader compile failed:\n%s\n", label, log);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "ERROR: program link failed:\n%s\n", log);
        return 0;
    }
    return p;
}

static const char *SCENE_VS =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *BLIT_VS =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *BLIT_FS =
    "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

int main(int argc, char **argv) {
    const char *frag_path = (argc > 1) ? argv[1] : "shaders/plasma.frag";
    int run_seconds = (argc > 2) ? atoi(argv[2]) : 0;
    int scale       = (argc > 3) ? atoi(argv[3]) : 3;
    if (scale < 1) scale = 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char *frag_src = read_file(frag_path);
    if (!frag_src) return 1;
    fprintf(stderr, "loaded fragment shader: %s (scale 1/%d)\n", frag_path, scale);

    /* ---- EGL bring-up ---- */
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "ERROR: eglGetDisplay => NO_DISPLAY\n"); return 1; }

    EGLint emaj = 0, emin = 0;
    if (!eglInitialize(dpy, &emaj, &emin)) {
        fprintf(stderr, "ERROR: eglInitialize failed (0x%x) — is X still running?\n", eglGetError());
        return 1;
    }
    fprintf(stderr, "EGL %d.%d  vendor='%s'\n", emaj, emin, eglQueryString(dpy, EGL_VENDOR));

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint n_cfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &config, 1, &n_cfg) || n_cfg < 1) {
        fprintf(stderr, "ERROR: eglChooseConfig failed (0x%x)\n", eglGetError());
        return 1;
    }

    EGLSurface surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)0, NULL);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "ERROR: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return 1;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "ERROR: eglCreateContext failed (0x%x)\n", eglGetError()); return 1; }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "ERROR: eglMakeCurrent failed (0x%x)\n", eglGetError()); return 1; }

    EGLint W = 0, H = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &W);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &H);
    if (W <= 0 || H <= 0) { W = 1920; H = 1080; }
    int rw = W / scale, rh = H / scale;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    fprintf(stderr, "screen %dx%d  render %dx%d  GL_RENDERER='%s'\n",
            W, H, rw, rh, (const char *)glGetString(GL_RENDERER));

    /* ---- programs ---- */
    GLuint scene_vs = compile_shader(GL_VERTEX_SHADER, SCENE_VS, "scene-vertex");
    GLuint scene_fs = compile_shader(GL_FRAGMENT_SHADER, frag_src, "fragment");
    GLuint blit_vs  = compile_shader(GL_VERTEX_SHADER, BLIT_VS, "blit-vertex");
    GLuint blit_fs  = compile_shader(GL_FRAGMENT_SHADER, BLIT_FS, "blit-fragment");
    if (!scene_vs || !scene_fs || !blit_vs || !blit_fs) return 1;

    GLuint scene_prog = link_program(scene_vs, scene_fs);
    GLuint blit_prog  = link_program(blit_vs, blit_fs);
    if (!scene_prog || !blit_prog) return 1;

    GLint u_time = glGetUniformLocation(scene_prog, "u_time");
    GLint u_res  = glGetUniformLocation(scene_prog, "u_resolution");
    GLint u_tex  = glGetUniformLocation(blit_prog, "u_tex");

    /* ---- offscreen render target (texture + FBO) at internal resolution ---- */
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbs != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "ERROR: FBO incomplete (0x%x)\n", fbs);
        return 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* fullscreen quad (two triangles) in clip space, shared by both passes */
    static const GLfloat quad[] = {
        -1.0f, -1.0f,   1.0f, -1.0f,  -1.0f,  1.0f,
        -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
    };

    fprintf(stderr, "rendering%s ... (Ctrl-C to stop)\n", run_seconds > 0 ? " (timed)" : "");

    double t0 = now_sec();
    unsigned long frames = 0;
    while (running) {
        double t = now_sec() - t0;
        if (run_seconds > 0 && t >= run_seconds) break;

        /* pass 1: scene -> small FBO */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, rw, rh);
        glUseProgram(scene_prog);
        if (u_time >= 0) glUniform1f(u_time, (float)t);
        if (u_res  >= 0) glUniform2f(u_res, (float)rw, (float)rh);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        /* pass 2: upscale FBO texture -> screen */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        glUseProgram(blit_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        if (u_tex >= 0) glUniform1i(u_tex, 0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        eglSwapBuffers(dpy, surf);
        frames++;
    }
    double elapsed = now_sec() - t0;
    fprintf(stderr, "\n%lu frames in %.1fs (%.1f fps)\n",
            frames, elapsed, elapsed > 0 ? frames / elapsed : 0.0);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    free(frag_src);
    return 0;
}
