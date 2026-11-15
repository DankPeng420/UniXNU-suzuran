// suzuran: eagl.c's lines into the kernel log, through NativeAdreno430's log call (selector 0x7f, 240 bytes a line),
// split at newlines and at 239 characters. The connection is opened once.
#include <stdint.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

static io_connect_t gpu_connection(void)
{
    static io_connect_t gpu;
    static int tried;
    if (!tried) {
        tried = 1;
        io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("NativeAdreno430"));
        if (service) {
            if (IOServiceOpen(service, mach_task_self(), 0, &gpu) != KERN_SUCCESS) gpu = 0;
            IOObjectRelease(service);
        }
    }
    return gpu;
}

// The panel's page flip (NativeAdreno430 0x7c: which buffer to draw onto; 0x7b: that one is drawn): 0, or -1.
int suzuran_panel(unsigned selector, uint32_t *id)
{
    io_connect_t gpu = gpu_connection();
    struct { uint32_t surface, handle; uint64_t size; uint32_t offset, rowBytes, width, height, format, planes; } r;
    if (!gpu) return -1;
    memset(&r, 0, sizeof r);
    r.surface = *id;
    size_t out = sizeof r;
    if (IOConnectCallStructMethod(gpu, selector, &r, sizeof r, &r, &out) != KERN_SUCCESS) return -1;
    *id = r.surface;
    return 0;
}

void suzuran_log(const char *text)
{
    io_connect_t gpu = gpu_connection();
    if (!gpu) return;
    while (*text) {
        struct { char text[240]; } line;
        size_t n = strcspn(text, "\n");
        if (n > sizeof line.text - 1) n = sizeof line.text - 1;
        memset(&line, 0, sizeof line);
        memcpy(line.text, text, n);
        if (n) {
            size_t out = 0;
            IOConnectCallStructMethod(gpu, 0x7f, &line, sizeof line, NULL, &out);
        }
        text += n;
        if (*text == '\n') text++;
    }
}
