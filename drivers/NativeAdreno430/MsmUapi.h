// The parts of Linux's msm DRM interface (include/uapi/drm/msm_drm.h, MIT) that freedreno uses, as the
// user client's calls: selector = the ioctl's number (DRM_COMMAND_BASE + DRM_MSM_x; 0x09 = DRM's GEM_CLOSE),
// structure in and out as-is. Same contract as HakiOS's hakios-drm shim (MIT), which Mesa is built against.
#pragma once
#include <stdint.h>

namespace msm {

constexpr uint32_t kCommandBase = 0x40;
enum : uint32_t {
    kGetParam = kCommandBase + 0x00, kSetParam = kCommandBase + 0x01, kGemNew = kCommandBase + 0x02,
    kGemInfo = kCommandBase + 0x03, kGemCpuPrep = kCommandBase + 0x04, kGemCpuFini = kCommandBase + 0x05,
    kGemSubmit = kCommandBase + 0x06, kWaitFence = kCommandBase + 0x07, kGemMadvise = kCommandBase + 0x08,
    kSubmitQueueNew = kCommandBase + 0x0a, kSubmitQueueClose = kCommandBase + 0x0b,
    kSubmitQueueQuery = kCommandBase + 0x0c,
    kGemClose = 0x09,          // DRM_IOCTL_GEM_CLOSE
    kSurface = 0x7e,           // ours (HakiOS's HAKIOS_GPU_SURFACE): an IOSurface as a buffer
    kLog = 0x7f,               // ours: a line for the kernel log (the test tools have no terminal)
};

struct Timespec { int64_t sec, nsec; };
struct Param { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct GemNew { uint64_t size; uint32_t flags, handle; };
struct GemInfo { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct GemCpuPrep { uint32_t handle, op; Timespec timeout; };
struct SubmitCmd { uint32_t type, submitIdx, submitOffset, size, pad, nrRelocs; uint64_t relocs; };
struct SubmitBo { uint32_t flags, handle; uint64_t presumed; };
struct Submit {
    uint32_t flags, fence, nrBos, nrCmds;
    uint64_t bos, cmds;
    int32_t fenceFd;
    uint32_t queueId;
    uint64_t inSyncobjs, outSyncobjs;
    uint32_t nrInSyncobjs, nrOutSyncobjs, syncobjStride, pad;
};
struct WaitFence { uint32_t fence, flags; Timespec timeout; uint32_t queueId; };
struct GemMadvise { uint32_t handle, madv, retained; };
struct SubmitQueue { uint32_t flags, prio, id; };
struct SubmitQueueQuery { uint64_t data; uint32_t id, param, len, pad; };
struct GemClose { uint32_t handle, pad; };
struct Log { char text[240]; };
// hakios-drm's struct hakios_drm_surface
struct Surface {
    uint32_t surface, handle;
    uint64_t size;
    uint32_t offset, rowBytes;
    uint32_t width, height, pixelFormat, planes;
};

enum : uint32_t {
    PARAM_GPU_ID = 1, PARAM_GMEM_SIZE = 2, PARAM_CHIP_ID = 3, PARAM_MAX_FREQ = 4, PARAM_TIMESTAMP = 5,
    PARAM_GMEM_BASE = 6, PARAM_PRIORITIES = 7, PARAM_PP_PGTABLE = 8, PARAM_FAULTS = 9, PARAM_SUSPENDS = 10,
};
enum : uint32_t {
    INFO_GET_OFFSET = 0, INFO_GET_IOVA = 1, INFO_SET_NAME = 2, INFO_GET_NAME = 3, INFO_SET_IOVA = 4,
    INFO_GET_FLAGS = 5,
};
enum : uint32_t { PREP_NOSYNC = 4 };
enum : uint32_t { SUBMIT_CMD_BUF = 1, SUBMIT_CMD_IB_TARGET_BUF = 2, SUBMIT_CMD_CTX_RESTORE_BUF = 3 };
enum : uint32_t { SUBMIT_FENCE_FD_OUT = 0x20000000, SUBMIT_FENCE_SN_IN = 0x02000000 };

}
