// suzuran: eagl.c's lines into the kernel log, through NativeAdreno430's log call (selector 0x7f, 240 bytes a line),
// split at newlines and at 239 characters. The connection is opened once.
#include <string.h>
#include <IOKit/IOKitLib.h>

void suzuran_log(const char *text)
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
