/* viz.c — MIDI-reactive GLES2 fragment-shader visualizer for the CI20.
 *
 * Combines:
 *   - EGL + GLES2 fullscreen renderer (PowerVR FLIP WSEGL, fbdev, no X)
 *   - FBO render-scale for speed (render small, upscale to 1080p)
 *   - a background pthread reading USB-MIDI via usbfs (any MIDI-class device)
 *
 * Uniforms exposed to the scene fragment shader:
 *   uniform float u_time;        seconds since start
 *   uniform vec2  u_resolution;  internal render resolution
 *   uniform float u_energy;      0..1, sum of held note velocities (decays)
 *   uniform float u_pitch;       -1..1, pitch bend
 *   uniform float u_mod;         0..1, mod wheel (CC1)
 *   uniform float u_note;        0..1, most recent note number / 127
 *
 * Build: gcc -O2 -std=gnu99 viz.c -I./include -lEGL -lGLESv2 -lm -lrt -lpthread -o viz
 * Run:   ./viz <shader> [seconds] [scale]   (defaults: shaders/reactive.frag 0 3)
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

/* ----------------------------- shared MIDI state ----------------------------- */
typedef struct {
    pthread_mutex_t lock;
    float vel[128];   /* current velocity 0..1 of each note (0 = off) */
    float cc[128];    /* CC values 0..1 */
    float pitch;      /* -1..1 */
    int   last_note;  /* most recent note-on number */
    int   running;
} midi_state;

static midi_state M;

/* ----------------------------- usbfs MIDI reader ----------------------------- */
#define SYSUSB "/sys/bus/usb/devices"

static int read_sysfs(const char *dir, const char *file, const char *fmt) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    int v = -1; if (fscanf(f, fmt, &v) != 1) v = -1; fclose(f); return v;
}

static int find_in_endpoint(const char *iface_dir) {
    DIR *d = opendir(iface_dir); if (!d) return -1;
    struct dirent *e; int ep = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "ep_", 3) == 0) {
            int addr = (int)strtol(e->d_name + 3, NULL, 16);
            if (addr & 0x80) { ep = addr; break; }
        }
    }
    closedir(d); return ep;
}

/* find first USB MIDI-streaming interface; fills path/iface/ep. returns 0 ok */
static int find_midi(char *path, size_t pn, int *iface, int *ep) {
    DIR *d = opendir(SYSUSB); if (!d) return -1;
    struct dirent *e; int found = -1;
    while ((e = readdir(d)) && found != 0) {
        if (!strchr(e->d_name, ':')) continue;
        char idir[512]; snprintf(idir, sizeof idir, "%s/%s", SYSUSB, e->d_name);
        if (read_sysfs(idir, "bInterfaceClass", "%x") != 0x01) continue;
        if (read_sysfs(idir, "bInterfaceSubClass", "%x") != 0x03) continue;
        int ifn = read_sysfs(idir, "bInterfaceNumber", "%x");
        int epi = find_in_endpoint(idir);
        if (ifn < 0 || epi < 0) continue;
        char parent[256];
        snprintf(parent, sizeof parent, "%.*s",
                 (int)(strchr(e->d_name, ':') - e->d_name), e->d_name);
        char pdir[512]; snprintf(pdir, sizeof pdir, "%s/%s", SYSUSB, parent);
        int bus = read_sysfs(pdir, "busnum", "%d");
        int dev = read_sysfs(pdir, "devnum", "%d");
        if (bus <= 0 || dev <= 0) continue;
        snprintf(path, pn, "/dev/bus/usb/%03d/%03d", bus, dev);
        *iface = ifn; *ep = epi; found = 0;
    }
    closedir(d); return found;
}

static void *midi_thread(void *arg) {
    (void)arg;
    char path[64]; int iface = 0, ep = 0x81;
    if (find_midi(path, sizeof path, &iface, &ep) != 0) {
        fprintf(stderr, "MIDI: no USB MIDI device found (visuals run without input)\n");
        return NULL;
    }
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "MIDI: open %s: %s\n", path, strerror(errno)); return NULL; }
    unsigned int ifc = iface;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
        fprintf(stderr, "MIDI: claim iface %u: %s\n", ifc, strerror(errno)); close(fd); return NULL;
    }
    fprintf(stderr, "MIDI: reading %s iface %d ep 0x%02x\n", path, iface, ep);

    unsigned char buf[64];
    while (M.running) {
        struct usbdevfs_bulktransfer bt;
        bt.ep = ep; bt.len = sizeof buf; bt.timeout = 200; bt.data = buf;
        int r = ioctl(fd, USBDEVFS_BULK, &bt);
        if (r < 0) { if (errno == ETIMEDOUT) continue; break; }
        pthread_mutex_lock(&M.lock);
        for (int i = 0; i + 4 <= r; i += 4) {
            unsigned char cin = buf[i] & 0x0f;
            unsigned char b = buf[i + 2], c = buf[i + 3];
            switch (cin) {
                case 0x9: /* note on */
                    if (c > 0) { M.vel[b & 127] = c / 127.0f; M.last_note = b & 127; }
                    else       { M.vel[b & 127] = 0.0f; }
                    break;
                case 0x8: M.vel[b & 127] = 0.0f; break;          /* note off */
                case 0xB: M.cc[b & 127] = c / 127.0f; break;     /* CC */
                case 0xE: M.pitch = ((b | (c << 7)) - 8192) / 8192.0f; break; /* pitch bend */
                default: break; /* ignore realtime clock (0xF) etc. */
            }
        }
        pthread_mutex_unlock(&M.lock);
    }
    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifc);
    close(fd);
    return NULL;
}

/* ----------------------------- GL helpers ----------------------------- */
static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static double now_sec(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1); if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); return b;
}

static GLuint compile(GLenum type, const char *src, const char *label) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "%s compile failed:\n%s\n", label, log); return 0; }
    return s;
}
static GLuint link2(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "link failed:\n%s\n", log); return 0; }
    return p;
}

static const char *SCENE_VS =
    "attribute vec2 a_pos;\nvoid main(){ gl_Position=vec4(a_pos,0.0,1.0); }\n";
static const char *BLIT_VS =
    "attribute vec2 a_pos;\nvarying vec2 v_uv;\n"
    "void main(){ v_uv=a_pos*0.5+0.5; gl_Position=vec4(a_pos,0.0,1.0); }\n";
static const char *BLIT_FS =
    "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
    "varying vec2 v_uv;\nuniform sampler2D u_tex;\n"
    "void main(){ gl_FragColor=texture2D(u_tex,v_uv); }\n";

int main(int argc, char **argv) {
    const char *frag_path = (argc > 1) ? argv[1] : "shaders/reactive.frag";
    int run_seconds = (argc > 2) ? atoi(argv[2]) : 0;
    int scale       = (argc > 3) ? atoi(argv[3]) : 3;
    if (scale < 1) scale = 1;

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    char *frag_src = read_file(frag_path);
    if (!frag_src) return 1;

    /* start MIDI thread */
    pthread_mutex_init(&M.lock, NULL);
    M.running = 1; M.last_note = 60;
    pthread_t mt;
    pthread_create(&mt, NULL, midi_thread, NULL);

    /* EGL */
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint emaj, emin;
    if (!eglInitialize(dpy, &emaj, &emin)) {
        fprintf(stderr, "eglInitialize failed (0x%x) — is X running?\n", eglGetError()); return 1; }
    const EGLint cfa[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                           EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8, EGL_NONE };
    EGLConfig cfg; EGLint nc;
    if (!eglChooseConfig(dpy, cfa, &cfg, 1, &nc) || nc < 1) { fprintf(stderr,"chooseConfig fail\n"); return 1; }
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
    if (surf == EGL_NO_SURFACE) { fprintf(stderr,"windowSurface fail 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    eglMakeCurrent(dpy, surf, surf, ctx);

    EGLint W=0,H=0; eglQuerySurface(dpy,surf,EGL_WIDTH,&W); eglQuerySurface(dpy,surf,EGL_HEIGHT,&H);
    if (W<=0||H<=0){W=1920;H=1080;}
    int rw=W/scale, rh=H/scale; if(rw<1)rw=1; if(rh<1)rh=1;
    fprintf(stderr, "screen %dx%d render %dx%d  %s\n", W,H,rw,rh,(const char*)glGetString(GL_RENDERER));

    GLuint sp = link2(compile(GL_VERTEX_SHADER,SCENE_VS,"scene-vs"),
                      compile(GL_FRAGMENT_SHADER,frag_src,"fragment"));
    GLuint bp = link2(compile(GL_VERTEX_SHADER,BLIT_VS,"blit-vs"),
                      compile(GL_FRAGMENT_SHADER,BLIT_FS,"blit-fs"));
    if (!sp || !bp) return 1;

    GLint u_time=glGetUniformLocation(sp,"u_time"), u_res=glGetUniformLocation(sp,"u_resolution");
    GLint u_energy=glGetUniformLocation(sp,"u_energy"), u_pitch=glGetUniformLocation(sp,"u_pitch");
    GLint u_mod=glGetUniformLocation(sp,"u_mod"), u_note=glGetUniformLocation(sp,"u_note");
    GLint u_tex=glGetUniformLocation(bp,"u_tex");

    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,rw,rh,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    GLuint fbo; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"FBO incomplete\n");return 1;}
    glBindFramebuffer(GL_FRAMEBUFFER,0);

    static const GLfloat quad[]={-1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1};

    fprintf(stderr, "rendering ... (Ctrl-C to stop)\n");
    double t0=now_sec(); unsigned long frames=0;
    float energy_s=0.0f; /* smoothed energy */
    while (running) {
        double t = now_sec()-t0;
        if (run_seconds>0 && t>=run_seconds) break;

        /* snapshot MIDI state */
        float energy=0.0f, pitch, mod, note;
        pthread_mutex_lock(&M.lock);
        for (int i=0;i<128;i++) energy += M.vel[i];
        pitch=M.pitch; mod=M.cc[1]; note=M.last_note/127.0f;
        pthread_mutex_unlock(&M.lock);
        if (energy>1.0f) energy=1.0f;
        energy_s += (energy-energy_s)*0.2f; /* smooth */

        /* pass 1: scene -> FBO */
        glBindFramebuffer(GL_FRAMEBUFFER,fbo); glViewport(0,0,rw,rh);
        glUseProgram(sp);
        if(u_time>=0)   glUniform1f(u_time,(float)t);
        if(u_res>=0)    glUniform2f(u_res,(float)rw,(float)rh);
        if(u_energy>=0) glUniform1f(u_energy,energy_s);
        if(u_pitch>=0)  glUniform1f(u_pitch,pitch);
        if(u_mod>=0)    glUniform1f(u_mod,mod);
        if(u_note>=0)   glUniform1f(u_note,note);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad);
        glDrawArrays(GL_TRIANGLES,0,6);

        /* pass 2: upscale -> screen */
        glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,W,H);
        glUseProgram(bp); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex);
        if(u_tex>=0) glUniform1i(u_tex,0);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad);
        glDrawArrays(GL_TRIANGLES,0,6);

        eglSwapBuffers(dpy,surf); frames++;
    }
    double el=now_sec()-t0;
    fprintf(stderr,"\n%lu frames in %.1fs (%.1f fps)\n",frames,el,el>0?frames/el:0.0);

    M.running=0; pthread_join(mt,NULL);
    eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(dpy,ctx); eglDestroySurface(dpy,surf); eglTerminate(dpy);
    free(frag_src);
    return 0;
}
