#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <X11/cursorfont.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glx.h>


static const char *VERT_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n" // screenshot pixel coords
    "layout(location=1) in vec2 aTex;\n"
    "out vec2 vTex;\n"
    "out vec2 vScreen;\n"     // window pixel coords
    "uniform vec2  camPos;\n" // viewport TL in screenshot space
    "uniform float camScale;\n"
    "uniform vec2  winSize;\n"
    "void main() {\n"
    "    vScreen     = (aPos - camPos) * camScale;\n"
    "    vec2 ndc    = vScreen / winSize * 2.0 - 1.0;\n"
    "    ndc.y       = -ndc.y;\n" // flip: screen Y↓, GL Y↑
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    vTex = aTex;\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 330 core\n"
    "in  vec2 vTex;\n"
    "in  vec2 vScreen;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2  cursor;\n"
    "uniform float shadow;\n" // 0 = no effect, 0.75 = strong vignette
    "uniform float radius;\n" // spotlight radius in screen pixels
    "void main() {\n"
    "    vec4  col   = texture(uTex, vTex);\n"
    "    float dist  = length(vScreen - cursor);\n"
    "    /* 2px anti-aliased edge — crisp but not jagged */\n"
    "    float lit   = smoothstep(radius + 2.0, radius - 2.0, dist);\n"
    "    /* darken outside the circle */\n"
    "    col.rgb    *= mix(1.0 - shadow, 1.0, lit);\n"
    "    /* solid white disc at low opacity — the halo */\n"
    "    col.rgb     = mix(col.rgb, vec3(1.0), lit * 0.18);\n"
    "    FragColor   = col;\n"
    "}\n";

typedef struct
{
    float x, y;
} Vec2;

typedef struct
{
    Vec2 pos;    // top-left of visible area in screenshot pixel space
    float scale; // zoom — always ≥ 1.0
} Camera;

typedef struct
{
    Vec2 curr, prev;
    int dragging;
} Mouse;

typedef struct
{
    int on;       // is the spotlight active?
    float shadow; // animated darkness 0 → 0.75
    float radius; // in screen pixels
} Spotlight;

// Utils
static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Keep the camera viewport within screenshot bounds.
static void clamp_camera(Camera *c, int sw, int sh)
{
    if (c->scale <= 1.0f)
    {
        c->scale = 1.0f;
        c->pos.x = c->pos.y = 0.0f;
        return;
    }
    float vis_w = sw / c->scale;
    float vis_h = sh / c->scale;
    c->pos.x = clampf(c->pos.x, 0.0f, sw - vis_w);
    c->pos.y = clampf(c->pos.y, 0.0f, sh - vis_h);
}

// Zoom at a pivot point (screen pixel coords mx, my).
static void zoom_at(Camera *c, float new_scale, float mx, float my, int sw, int sh)
{
    float old = c->scale;
    /* world point under cursor must stay fixed:
       new_cam_pos = cursor/old_scale + old_cam_pos - cursor/new_scale  */
    c->pos.x += mx / old - mx / new_scale;
    c->pos.y += my / old - my / new_scale;
    c->scale = new_scale;
    clamp_camera(c, sw, sh);
}

static GLuint compile_shader(const char *src, GLenum type)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(s, 512, NULL, log);
        fprintf(stderr, "Shader error (%s):\n%s\n",
                type == GL_VERTEX_SHADER ? "vert" : "frag", log);
    }
    return s;
}

static GLuint link_program(const char *vert, const char *frag)
{
    GLuint vs = compile_shader(vert, GL_VERTEX_SHADER);
    GLuint fs = compile_shader(frag, GL_FRAGMENT_SHADER);
    GLuint prg = glCreateProgram();
    glAttachShader(prg, vs);
    glAttachShader(prg, fs);
    glLinkProgram(prg);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prg, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(prg, 512, NULL, log);
        fprintf(stderr, "Program link error:\n%s\n", log);
    }
    return prg;
}

int main(void)
{
    //  Open display
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
    {
        fputs("Cannot open X display\n", stderr);
        return 1;
    }

    Window root = DefaultRootWindow(dpy);

    // Screen refresh rate for animation timing
    XRRScreenConfiguration *scr_cfg = XRRGetScreenInfo(dpy, root);
    short rate = XRRConfigCurrentRate(scr_cfg);
    if (rate <= 0)
        rate = 60;
    float dt = 1.0f / (float)rate;

    // Screenshot
    XWindowAttributes root_wa;
    XGetWindowAttributes(dpy, root, &root_wa);
    int sw = root_wa.width, sh = root_wa.height;

    XImage *img = XGetImage(dpy, root, 0, 0, sw, sh, AllPlanes, ZPixmap);
    if (!img)
    {
        fputs("XGetImage failed\n", stderr);
        return 1;
    }

    // GLX setup
    int glx_attrs[] = {GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None};
    XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), glx_attrs);
    if (!vi)
    {
        fputs("No suitable GLX visual\n", stderr);
        return 1;
    }

    // Overlay window (fullscreen, on top)
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.event_mask = ButtonPressMask | ButtonReleaseMask |
                     KeyPressMask | PointerMotionMask |
                     ExposureMask;
    swa.override_redirect = True; /* no WM decorations / tiling    */
    swa.save_under = True;

    Window win = XCreateWindow(
        dpy, root, 0, 0, sw, sh, 0,
        vi->depth, InputOutput, vi->visual,
        CWColormap | CWEventMask | CWOverrideRedirect | CWSaveUnder, &swa);

    // force normal arrow cursor 
    Cursor cursor = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, win, cursor);

    XMapWindow(dpy, win);
    XStoreName(dpy, win, "spotlight");
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);

    // OpenGL context
    GLXContext glc = glXCreateContext(dpy, vi, NULL, GL_TRUE);
    glXMakeCurrent(dpy, win, glc);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK)
    {
        fputs("GLEW init failed\n", stderr);
        return 1;
    }

    // Shader program
    GLuint prog = link_program(VERT_SRC, FRAG_SRC);
    glUseProgram(prog);

    /* Cache uniform locations (avoids repeated string lookup per frame) */
    GLint u_camPos = glGetUniformLocation(prog, "camPos");
    GLint u_camScale = glGetUniformLocation(prog, "camScale");
    GLint u_winSize = glGetUniformLocation(prog, "winSize");
    GLint u_cursor = glGetUniformLocation(prog, "cursor");
    GLint u_shadow = glGetUniformLocation(prog, "shadow");
    GLint u_radius = glGetUniformLocation(prog, "radius");
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);

    // Quad geometry
    float fw = (float)sw, fh = (float)sh;
    float verts[] = {
        /* x     y      s    t  */
        fw, 0, 1.0f, 0.0f,  /* top-right    */
        fw, fh, 1.0f, 1.0f, /* bottom-right */
        0, fh, 0.0f, 1.0f,  /* bottom-left  */
        0, 0, 0.0f, 0.0f    /* top-left     */
    };
    unsigned int idx[] = {0, 1, 3, 1, 2, 3};

    GLuint vao, vbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    int stride = 4 * sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Screenshot texture
    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, sw, sh, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, img->data);
    glGenerateMipmap(GL_TEXTURE_2D);

    /* sharper zooming */
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_MIN_FILTER,
        GL_LINEAR_MIPMAP_LINEAR);

    /* keeps image clearer when enlarged */
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_MAG_FILTER,
        GL_LINEAR);

    /* anisotropic filtering */
    if (GLEW_EXT_texture_filter_anisotropic)
    {
        GLfloat maxAniso = 0.0f;
        glGetFloatv(
            GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,
            &maxAniso);

        glTexParameterf(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAX_ANISOTROPY_EXT,
            maxAniso);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    XDestroyImage(img); /* pixel data is now on the GPU */

    /* ── Initial state ──────────────────────────────────────────────── */
    Camera cam = {.pos = {0, 0}, .scale = 1.0f};
    Mouse mse = {0};
    Spotlight spot = {.on = 1, .shadow = 0.0f, .radius = 160.0f};

    /* Grab cursor position so the spotlight is centred from the first frame */
    {
        Window dummy_r, dummy_c;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(dpy, root, &dummy_r, &dummy_c, &rx, &ry, &wx, &wy, &mask);
        mse.curr.x = (float)rx;
        mse.curr.y = (float)ry;
        mse.prev = mse.curr;
    }

    const float ZOOM_STEP = 0.12f;   /* 12 % per scroll click      */
    const float RADIUS_STEP = 25.0f; /* pixels per Ctrl-scroll     */
    const float SHADOW_MAX = 0.75f;  /* max vignette darkness      */
    const float SHADOW_SPEED = 5.0f; /* fade speed (units/second)  */

    // Main loop
    int quit = 0;
    while (!quit)
    {

        // Keep focus so key events arrive even over other windows
        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);

        // Events
        while (XPending(dpy))
        {
            XEvent xev;
            XNextEvent(dpy, &xev);

            switch (xev.type)
            {

            case MotionNotify:
                mse.prev = mse.curr;
                mse.curr.x = (float)xev.xmotion.x;
                mse.curr.y = (float)xev.xmotion.y;
                break;

            case ButtonPress:
            {
                float mx = (float)xev.xbutton.x;
                float my = (float)xev.xbutton.y;
                int ctrl = (xev.xbutton.state & ControlMask) != 0;

                switch (xev.xbutton.button)
                {
                case Button1:
                    mse.dragging = 1;
                    mse.prev = mse.curr;
                    break;

                case Button4: // scroll up
                    if (ctrl)
                    {
                        spot.radius += RADIUS_STEP;
                    }
                    else
                    {
                        zoom_at(&cam,
                                cam.scale * (1.0f + ZOOM_STEP),
                                mx, my, sw, sh);
                    }
                    break;

                case Button5: // scroll down
                    if (ctrl)
                    {
                        spot.radius = fmaxf(30.0f, spot.radius - RADIUS_STEP);
                    }
                    else
                    {
                        float ns = fmaxf(1.0f, cam.scale / (1.0f + ZOOM_STEP));
                        zoom_at(&cam, ns, mx, my, sw, sh);
                    }
                    break;
                }
                break;
            }

            case ButtonRelease:
                if (xev.xbutton.button == Button1)
                    mse.dragging = 0;
                break;

            case KeyPress:
            {
                KeySym key = XLookupKeysym(&xev.xkey, 0);
                switch (key)
                {
                case XK_Escape:
                case XK_q:
                    quit = 1;
                    break;
                case XK_f:
                    spot.on = !spot.on;
                    break;
                case XK_0:
                    cam.pos.x = cam.pos.y = 0.0f;
                    cam.scale = 1.0f;
                    break;
                }
                break;
            }
            }
        }

        // Animate spotlight shadow
        {
            float target = spot.on ? SHADOW_MAX : 0.0f;
            float step = SHADOW_SPEED * dt;
            if (spot.shadow < target)
                spot.shadow = fminf(spot.shadow + step, target);
            else
                spot.shadow = fmaxf(spot.shadow - step, target);
        }

        // Render
        glViewport(0, 0, sw, sh);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(prog);
        glUniform2f(u_camPos, cam.pos.x, cam.pos.y);
        glUniform1f(u_camScale, cam.scale);
        glUniform2f(u_winSize, (float)sw, (float)sh);
        glUniform2f(u_cursor, mse.curr.x, mse.curr.y);
        glUniform1f(u_shadow, spot.shadow);
        glUniform1f(u_radius, spot.radius);

        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);

        glXSwapBuffers(dpy, win);
        glFinish();
    }

    // Cleanup
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, glc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    XFreeCursor(dpy, cursor);
    return 0;
}