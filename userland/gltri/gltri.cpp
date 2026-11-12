// gltri: the first OpenGL ES drawing on the Z5 Compact's GPU. Mesa's freedreno (Adreno 430) behind OSMesa, through
// HakiOS's hakios-drm into NativeAdreno430. Started by gputest once its own tests pass. It makes an OpenGL ES 2
// context, clears a 128x128 buffer to blue, draws a red triangle, and checks pixels inside and outside the triangle;
// then times 100 frames. Every line goes to the kernel log through the GPU user client's log call.
// FD_MESA_DEBUG=sysmem: freedreno draws straight into memory; the A430's tile memory (GMEM) is OCMEM, not powered.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GL/glext.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
extern "C" {
#include "../iokit_min.h"
}

static io_connect_t logger;

static void say(const char *format, ...)
{
    struct { char text[240]; } line;
    va_list ap;
    va_start(ap, format);
    vsnprintf(line.text, sizeof line.text, format, ap);
    va_end(ap);
    if (logger) {
        size_t out = 0;
        IOConnectCallStructMethod(logger, 0x7f, &line, sizeof line, NULL, &out);
    }
    fprintf(stderr, "gltri: %s\n", line.text);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static GLuint shader(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[200] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        say("shader did not compile: %s", log);
    }
    return s;
}

static const char *pixel_name(uint32_t p)
{
    switch (p) {
    case 0xff0000ffu: return "red";
    case 0xffff0000u: return "blue";
    case 0: return "zero";
    default: return "other";
    }
}

int main(void)
{
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("NativeAdreno430"));
    if (service) {
        IOServiceOpen(service, mach_task_self(), 0, &logger);
        IOObjectRelease(service);
    }
    setenv("FD_MESA_DEBUG", "sysmem", 0);
    const int attribs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_STENCIL_BITS, 0,
                           OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 2,
                           OSMESA_CONTEXT_MINOR_VERSION, 0, 0};
    uint64_t t0 = now_ns();
    OSMesaContext ctx = OSMesaCreateContextAttribs(attribs, NULL);
    if (!ctx) {
        say("FAIL: no OSMesa context");
        return 1;
    }
    enum { W = 128, H = 128 };
    static uint32_t buffer[W * H];
    if (!OSMesaMakeCurrent(ctx, buffer, GL_UNSIGNED_BYTE, W, H)) {
        say("FAIL: OSMesaMakeCurrent");
        return 1;
    }
    say("context in %llu ms: %s / %s / %s", (unsigned long long)((now_ns() - t0) / 1000000),
        (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
        (const char *)glGetString(GL_VERSION));

    GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER,
        "#version 100\nattribute vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n"));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER,
        "#version 100\nprecision mediump float;\nvoid main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        say("FAIL: program did not link");
        return 1;
    }
    static const float triangle[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof triangle, triangle, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glUseProgram(program);
    glViewport(0, 0, W, H);

    // A frame is right when the triangle's middle is red and the corners have the clear colour. Each check is made
    // right after glFinish and, when it fails, again a while later: a frame that turns right by itself means the
    // driver let glFinish return before the GPU was done (a sync bug); one that stays wrong was never drawn.
    auto frame_ok = [&](uint32_t clear) {
        return buffer[(H / 2) * W + W / 2] == 0xff0000ffu && buffer[(H / 4) * W + W / 2] == 0xff0000ffu &&
            buffer[1 * W + 1] == clear && buffer[1 * W + W - 2] == clear && buffer[(H - 2) * W + 1] == clear &&
            buffer[(H - 2) * W + W - 2] == clear;
    };
    auto picture = [&]() {
        for (int y = H - 8; y >= 0; y -= 16) {   // a coarse picture of the buffer, top row first
            char row[W / 8 + 1];
            for (int x = 0; x < W / 8; x++) {
                const uint32_t p = buffer[y * W + x * 8];
                row[x] = p == 0xff0000ffu ? 'R' : p == 0xffff0000u ? 'b' : p == 0xffffff00u ? 'c' : p == 0 ? '.' : '?';
            }
            row[W / 8] = 0;
            say("  %s", row);
        }
    };

    t0 = now_ns();
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    const uint64_t first = now_ns() - t0;
    const GLenum error = glGetError();
    // inside: the triangle's middle; outside: the corners, which it does not reach
    const uint32_t centre = buffer[(H / 2) * W + W / 2], c0 = buffer[1 * W + 1], c1 = buffer[1 * W + W - 2],
        c2 = buffer[(H - 2) * W + 1], c3 = buffer[(H - 2) * W + W - 2];
    int red = 0, blue = 0;
    for (int i = 0; i < W * H; i++) {
        red += buffer[i] == 0xff0000ffu;
        blue += buffer[i] == 0xffff0000u;
    }
    const bool pass = !error && frame_ok(0xffff0000u) && red > 4000 && red + blue == W * H;
    say("first frame %llu us, gl error 0x%x: centre %08x (%s), corners %08x %08x %08x %08x, %d red + %d blue of %d: %s",
        (unsigned long long)(first / 1000), error, centre, pixel_name(centre), c0, c1, c2, c3, red, blue, W * H,
        pass ? "PASS, the Adreno 430 draws" : "FAIL");
    if (!pass) {
        picture();
        usleep(200000);
        int later = 0;
        for (int i = 0; i < W * H; i++) later += buffer[i] == 0xff0000ffu;
        say("first frame 200 ms later (no GL calls in between): centre %08x, %d red: %s", buffer[(H / 2) * W + W / 2],
            later, frame_ok(0xffff0000u) ? "now right: glFinish returned before the GPU was done (sync bug)"
                                          : "still wrong: the triangle was not drawn");
        if (!frame_ok(0xffff0000u)) picture();
    }
    int wrong = 0, late = 0;
    t0 = now_ns();
    for (int frame = 0; frame < 100; frame++) {
        const uint32_t clear = frame & 1 ? 0xffffff00u : 0xffff0000u;
        glClearColor(0.0f, frame & 1 ? 1.0f : 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        if (!frame_ok(clear)) {
            ++wrong;
            usleep(50000);
            if (frame_ok(clear)) ++late;
            if (wrong <= 3) {
                say("frame %d wrong after glFinish (centre %08x, corner %08x)%s", frame, buffer[(H / 2) * W + W / 2],
                    buffer[1 * W + 1], frame_ok(clear) ? ", right 50 ms later" : ", still wrong 50 ms later");
            }
        }
    }
    const uint64_t took = now_ns() - t0;
    say("100 frames (clear + triangle + finish, %dx%d) in %llu ms: %llu fps; %d wrong after glFinish, %d of them right "
        "later: %s", W, H, (unsigned long long)(took / 1000000), (unsigned long long)(100ull * 1000000000ull / (took ? took : 1)),
        wrong, late, !pass || wrong ? "FAIL" : "PASS");
    OSMesaDestroyContext(ctx);
    return !pass || wrong ? 1 : 0;
}
