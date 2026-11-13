// libsuzuran.inject.dylib: our code inside a stock iOS program. The kernel starts backboardd (the window server) with
// DYLD_INSERT_LIBRARIES naming this library (boot-args dyldinsert=backboardd dyldallow=1). It says in the kernel log
// that it was loaded; with boot-arg suzurangl=1 it then loads libsuzuran.gl.dylib (HakiOS's eagl.c on our Mesa),
// which puts itself behind EAGLContext, so CoreAnimation's render server draws with OpenGL ES on the Adreno 430
// instead of its software renderer. Without suzurangl=1 nothing else happens.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include "../iokit_min.h"

#define LOG_SELECTOR 0x7f   // NativeAdreno430's user client: one line for the kernel log

static void say(const char *format, ...)
{
    struct { char text[240]; } line;
    va_list ap;
    va_start(ap, format);
    vsnprintf(line.text, sizeof line.text, format, ap);
    va_end(ap);
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("NativeAdreno430"));
    if (!service) return;
    io_connect_t gpu = 0;
    if (IOServiceOpen(service, mach_task_self(), 0, &gpu) == KERN_SUCCESS) {
        size_t out = 0;
        IOConnectCallStructMethod(gpu, LOG_SELECTOR, &line, sizeof line, NULL, &out);
        IOServiceClose(gpu);
    }
    IOObjectRelease(service);
}

// boot-arg NAME=VALUE (the first one), or -1
static int boot_arg(const char *name)
{
    char args[1024] = {0};
    size_t size = sizeof args - 1;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) return -1;
    const size_t length = strlen(name);
    for (char *p = args; (p = strstr(p, name)) != NULL; p += length) {
        if ((p == args || p[-1] == ' ') && p[length] == '=') return atoi(p + length + 1);
    }
    return -1;
}

#define GL_LIBRARY "/usr/lib/libsuzuran.gl.dylib"

__attribute__((constructor)) static void loaded(void)
{
    const int gl = boot_arg("suzurangl");
    say("suzuran-inject: loaded into %s[%d], suzurangl=%d", getprogname(), getpid(), gl);
    if (gl != 1 || strcmp(getprogname(), "backboardd") != 0) return;
    if (access(GL_LIBRARY, R_OK) != 0) {
        say("suzuran-inject: no %s", GL_LIBRARY);
        return;
    }
    setenv("FD_MESA_DEBUG", "sysmem", 0);   // GMEM (OCMEM) is not powered: draw straight into system memory
    void *library = dlopen(GL_LIBRARY, RTLD_NOW);
    say("suzuran-inject: %s %s%s", GL_LIBRARY, library ? "loaded" : "failed: ", library ? "" : dlerror());
}
