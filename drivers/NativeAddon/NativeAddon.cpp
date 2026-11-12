// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeAddon: files delivered with the boot payload, written into the root file system by the kernel.
//
// The phone has no shell and no way to copy files into the iOS root from outside (its HFS+ is journaled and
// written by iOS itself). So the loader (v17) carries an archive in its payload, places it in the kernel window
// and announces it as /chosen/memory-map "NativeAddon" (physical address, size). Once the root file system is
// up -- well before launchd reads its job list -- this driver writes each file whose content differs from what is
// there (unlink, then create: a program running from the old file keeps it) with the archive's mode and owner. Its own
// property "AddonDone" (files written, files already current) tells a waiting program that it is finished.
//
// Archive (little endian): "SZADDON1", u32 count, u32 version (1); then per file:
//   u32 pathBytes (with the NUL), u32 mode, u32 uid, u32 gid, u64 size, path (padded to 8), data (padded to 8).
// Only files, in directories that exist. Boot-arg nativeaddon=0 skips it.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <sys/mount.h>

extern "C" vm_offset_t ml_static_ptovirt(vm_offset_t);
// bsd/vfs/vfs_syscalls.c (not KPI, but this driver is linked into the kernel)
extern "C" int unlink1(vfs_context_t ctx, vnode_t start_dvp, user_addr_t path, enum uio_seg segflg, int flags);

namespace {

struct Entry {
    uint32_t pathBytes, mode, uid, gid;
    uint64_t size;
};

constexpr uint64_t align8(uint64_t n) { return (n + 7) & ~7ull; }

// 0: same content, 1: differs or missing
int differs(const char *path, const uint8_t *data, uint64_t size, vfs_context_t ctx)
{
    vnode_t vp = nullptr;
    if (vnode_open(path, FREAD, 0, 0, &vp, ctx)) return 1;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    int result = 1;
    if (!vnode_getattr(vp, &va, ctx) && VATTR_IS_SUPPORTED(&va, va_data_size) && va.va_data_size == size) {
        constexpr uint32_t kChunk = 64 * 1024;
        uint8_t *buffer = static_cast<uint8_t *>(IOMalloc(kChunk));
        result = buffer ? 0 : 1;
        for (uint64_t off = 0; buffer && !result && off < size; off += kChunk) {
            const uint32_t n = static_cast<uint32_t>(size - off < kChunk ? size - off : kChunk);
            uio_t uio = uio_create(1, static_cast<off_t>(off), UIO_SYSSPACE, UIO_READ);
            if (!uio) { result = 1; break; }
            uio_addiov(uio, CAST_USER_ADDR_T(buffer), n);
            if (VNOP_READ(vp, uio, 0, ctx) || uio_resid(uio) != 0 || memcmp(buffer, data + off, n)) result = 1;
            uio_free(uio);
        }
        if (buffer) IOFree(buffer, kChunk);
    }
    vnode_close(vp, FREAD, ctx);
    return result;
}

// The old file is unlinked first (a program running from it keeps its pages) and the new one created in its place.
int writeFile(const char *path, const Entry &e, const uint8_t *data, vfs_context_t ctx)
{
    int error = unlink1(ctx, NULLVP, CAST_USER_ADDR_T(path), UIO_SYSSPACE, 0);
    if (error && error != ENOENT) return error;
    vnode_t vp = nullptr;
    error = vnode_open(path, FWRITE | O_CREAT | O_EXCL, static_cast<int>(e.mode & 07777), 0, &vp, ctx);
    if (error) return error;
    constexpr uint32_t kChunk = 1024 * 1024;
    for (uint64_t off = 0; !error && off < e.size; off += kChunk) {
        const uint32_t n = static_cast<uint32_t>(e.size - off < kChunk ? e.size - off : kChunk);
        uio_t uio = uio_create(1, static_cast<off_t>(off), UIO_SYSSPACE, UIO_WRITE);
        if (!uio) { error = ENOMEM; break; }
        uio_addiov(uio, CAST_USER_ADDR_T(data + off), n);
        error = VNOP_WRITE(vp, uio, 0, ctx);
        if (!error && uio_resid(uio) != 0) error = EIO;
        uio_free(uio);
    }
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_SET(&va, va_mode, static_cast<mode_t>(e.mode & 07777));
        VATTR_SET(&va, va_uid, e.uid);
        VATTR_SET(&va, va_gid, e.gid);
        error = vnode_setattr(vp, &va, ctx);
    }
    if (!error) error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    vnode_close(vp, FWRITE, ctx);
    return error;
}

bool rootReady(vfs_context_t ctx)
{
    vnode_t vp = nullptr;
    if (vnode_lookup("/System/Library/xpc/launchd.plist", 0, &vp, ctx)) return false;
    vnode_put(vp);
    return true;
}

}

class NativeAddon final : public IOService {
    OSDeclareDefaultStructors(NativeAddon)
    static void run(void *self, wait_result_t);
public:
    bool start(IOService *provider) override;
};
OSDefineMetaClassAndStructors(NativeAddon, IOService)

void NativeAddon::run(void *self, wait_result_t)
{
    NativeAddon *service = static_cast<NativeAddon *>(self);
    uint32_t written = 0, current = 0, failed = 0;
    uint64_t range[2] = {};
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    OSData *where = map ? OSDynamicCast(OSData, map->getProperty("NativeAddon")) : nullptr;
    if (where && where->getLength() == sizeof range) memcpy(range, where->getBytesNoCopy(), sizeof range);
    if (map) map->release();
    const uint8_t *archive = range[0] && range[1] >= 16 ? reinterpret_cast<const uint8_t *>(ml_static_ptovirt(range[0]))
                                                         : nullptr;
    if (!archive || memcmp(archive, "SZADDON1", 8)) {
        IOLog("NativeAddon: no archive in this boot (memory-map 0x%llx, %llu bytes)\n", range[0], range[1]);
    } else {
        vfs_context_t ctx = vfs_context_create(nullptr);
        int waited = 0;
        while (!rootReady(ctx) && waited < 600) {
            IOSleep(100);
            ++waited;
        }
        uint32_t count = 0;
        memcpy(&count, archive + 8, 4);
        IOLog("NativeAddon: archive of %u files (%llu bytes) at 0x%llx, root ready after %d ms\n", count, range[1],
              range[0], waited * 100);
        // Right away: launchd reads its job list (one of the files this may replace) ~30 s after the root is up
        // (measured, boot 22), and a file here is replaced by unlink + create, not atomically.
        uint64_t pos = 16;
        for (uint32_t i = 0; i < count; ++i) {
            Entry e;
            if (pos + sizeof e > range[1]) break;
            memcpy(&e, archive + pos, sizeof e);
            pos += sizeof e;
            const char *path = reinterpret_cast<const char *>(archive + pos);
            if (e.pathBytes < 2 || e.pathBytes > 1024 || pos + align8(e.pathBytes) + e.size > range[1] ||
                path[e.pathBytes - 1] || path[0] != '/') {
                IOLog("NativeAddon: entry %u is malformed; stopping\n", i);
                ++failed;
                break;
            }
            pos += align8(e.pathBytes);
            const uint8_t *data = archive + pos;
            pos += align8(e.size);
            if (!differs(path, data, e.size, ctx)) {
                ++current;
                continue;
            }
            const int error = writeFile(path, e, data, ctx);
            IOLog("NativeAddon: %s (%llu bytes, mode %o, %u:%u): %s %d\n", path, e.size, e.mode, e.uid, e.gid,
                  error ? "FAILED" : "written", error);
            if (error) ++failed; else ++written;
        }
        vfs_context_rele(ctx);
    }
    IOLog("NativeAddon: done: %u written, %u already current, %u failed\n", written, current, failed);
    OSNumber *n = OSNumber::withNumber((static_cast<uint64_t>(written) << 32) | current, 64);
    if (n) {
        service->setProperty("AddonDone", n);
        n->release();
    }
    service->setProperty("AddonFailed", failed, 32);
    service->registerService();
    service->release();
}

bool NativeAddon::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    uint32_t enabled = 1;
    PE_parse_boot_argn("nativeaddon", &enabled, sizeof enabled);
    if (!enabled) return true;
    retain();
    thread_t thread = nullptr;
    if (kernel_thread_start(run, this, &thread) != KERN_SUCCESS) {
        release();
        return true;
    }
    thread_deallocate(thread);
    return true;
}
