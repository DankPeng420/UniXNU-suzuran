// gputest: the first user-space program on the Z5 Compact's GPU (NativeAdreno430's user client).
// Started by launchd at boot (job com.suzuran.gputest). It waits for NativeAddon to have written this boot's files
// (and runs itself again if that replaced it), then:
//   1. GEM_NEW a command buffer and a target buffer, maps both,
//   2. writes CP_MEM_WRITE packets into the command buffer, SUBMITs it as an indirect buffer, WAIT_FENCEs,
//   3. checks the target, then times 1000 small submissions.
// Every result line goes to the kernel log through the user client's log call (the phone has no terminal).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
extern "C" {
#include "../iokit_min.h"
}
#include "../../drivers/NativeAdreno430/MsmUapi.h"

using namespace msm;

static io_connect_t gpu;

static void say(const char *format, ...)
{
    struct { char text[240]; } line;
    va_list ap;
    va_start(ap, format);
    vsnprintf(line.text, sizeof line.text, format, ap);
    va_end(ap);
    if (gpu) {
        size_t out = 0;
        IOConnectCallStructMethod(gpu, kLog, &line, sizeof line, NULL, &out);
    }
    fprintf(stderr, "gputest: %s\n", line.text);
}

static int call(uint32_t selector, void *data, size_t size)
{
    size_t out = size;
    return IOConnectCallStructMethod(gpu, selector, data, size, data, &out);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static io_service_t wait_service(const char *name, const char *property, int seconds)
{
    for (int i = 0; i < seconds * 10; i++) {
        io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(name));
        if (s) {
            if (!property) return s;
            CFStringRef key = CFStringCreateWithCString(NULL, property, kCFStringEncodingUTF8);
            CFTypeRef value = IORegistryEntryCreateCFProperty(s, key, NULL, 0);
            CFRelease(key);
            if (value) {
                CFRelease(value);
                return s;
            }
            IOObjectRelease(s);
        }
        usleep(100000);
    }
    return 0;
}

struct bo { uint32_t handle; uint64_t iova; volatile uint32_t *map; };

static int bo_new(struct bo *b, uint64_t size)
{
    struct msm::GemNew n = {size, 0x00020000 /* MSM_BO_WC */, 0};
    if (call(kGemNew, &n, sizeof n)) return -1;
    struct msm::GemInfo info = {n.handle, INFO_GET_IOVA, 0, 0, 0};
    if (call(kGemInfo, &info, sizeof info)) return -1;
    mach_vm_address_t address = 0;
    mach_vm_size_t mapped = 0;
    if (IOConnectMapMemory64(gpu, n.handle, mach_task_self(), &address, &mapped, kIOMapAnywhere)) return -1;
    b->handle = n.handle;
    b->iova = info.value;
    b->map = (volatile uint32_t *)(uintptr_t)address;
    return 0;
}

static uint32_t pkt3(uint32_t opcode, uint32_t count) { return 0xc0000000u | ((count - 1) << 16) | (opcode << 8); }

static int submit_wait(struct bo *cmd, uint32_t dwords, struct bo *target, uint32_t *fence_out)
{
    struct msm::SubmitBo bos[2] = {{1 /* READ */, cmd->handle, 0}, {2 /* WRITE */, target->handle, 0}};
    struct msm::SubmitCmd cmds[1] = {{SUBMIT_CMD_BUF, 0, 0, dwords * 4, 0, 0, 0}};
    struct msm::Submit s;
    memset(&s, 0, sizeof s);
    s.nrBos = 2;
    s.nrCmds = 1;
    s.bos = (uint64_t)(uintptr_t)bos;
    s.cmds = (uint64_t)(uintptr_t)cmds;
    int r = call(kGemSubmit, &s, sizeof s);
    if (r) return r;
    uint64_t deadline = now_ns() + 2000000000ull;
    struct msm::WaitFence w = {s.fence, 0, {(int64_t)(deadline / 1000000000ull), (int64_t)(deadline % 1000000000ull)}, 0};
    if (fence_out) *fence_out = s.fence;
    return call(kWaitFence, &w, sizeof w);
}

int main(int argc, char **argv)
{
    (void)argc;
    io_service_t addon = wait_service("NativeAddon", "AddonDone", 300);
    if (addon) IOObjectRelease(addon);
    if (!getenv("GPUTEST_AGAIN")) {   // this boot's addon may have replaced this program: run the one on disk
        setenv("GPUTEST_AGAIN", "1", 1);
        execv(argv[0], argv);
    }
    io_service_t service = wait_service("NativeAdreno430", "GPU", 120);
    if (!service) {
        fprintf(stderr, "gputest: no GPU service\n");
        return 1;
    }
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gpu);
    IOObjectRelease(service);
    if (kr) {
        fprintf(stderr, "gputest: IOServiceOpen 0x%x\n", kr);
        return 1;
    }
    say("build " __DATE__ " " __TIME__ ", pid %d", getpid());
    struct msm::Param p = {0, PARAM_GPU_ID, 0, 0, 0};
    call(kGetParam, &p, sizeof p);
    struct msm::Param c = {0, PARAM_CHIP_ID, 0, 0, 0};
    call(kGetParam, &c, sizeof c);
    struct msm::Param t1 = {0, PARAM_TIMESTAMP, 0, 0, 0};
    call(kGetParam, &t1, sizeof t1);
    say("gpu id %llu chip id 0x%08llx timestamp %llu", (unsigned long long)p.value, (unsigned long long)c.value,
        (unsigned long long)t1.value);

    struct bo cmd, target;
    if (bo_new(&cmd, 4096) || bo_new(&target, 4096)) {
        say("FAIL: buffers");
        return 1;
    }
    say("command buffer at 0x%08llx, target at 0x%08llx", (unsigned long long)cmd.iova, (unsigned long long)target.iova);
    uint32_t n = 0;
    cmd.map[n++] = pkt3(0x3d, 2);   // CP_MEM_WRITE
    cmd.map[n++] = (uint32_t)target.iova;
    cmd.map[n++] = 0xa430c0de;
    cmd.map[n++] = pkt3(0x3d, 2);
    cmd.map[n++] = (uint32_t)target.iova + 4;
    cmd.map[n++] = 0x5eed5eed;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t fence = 0;
    int r = submit_wait(&cmd, n, &target, &fence);
    int ok = !r && target.map[0] == 0xa430c0de && target.map[1] == 0x5eed5eed;
    say("submit from user space: fence %u, wait %d, target 0x%08x 0x%08x: %s", fence, r, target.map[0], target.map[1],
        ok ? "PASS" : "FAIL");
    if (!ok) return 1;

    const int rounds = 1000;
    uint64_t start = now_ns();
    for (int i = 0; i < rounds; i++) {
        cmd.map[2] = (uint32_t)i;
        __asm__ volatile("dsb sy" ::: "memory");
        if ((r = submit_wait(&cmd, 3, &target, &fence)) || target.map[0] != (uint32_t)i) {
            say("FAIL at round %d: wait %d, target %u", i, r, target.map[0]);
            return 1;
        }
    }
    uint64_t took = now_ns() - start;
    struct msm::Param t2 = {0, PARAM_TIMESTAMP, 0, 0, 0};
    call(kGetParam, &t2, sizeof t2);
    say("%d submit+wait round trips in %llu us (%llu us each), gpu clock %llu ticks: PASS", rounds,
        (unsigned long long)(took / 1000), (unsigned long long)(took / 1000 / rounds),
        (unsigned long long)(t2.value - t1.value));
    return 0;
}
