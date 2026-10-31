// SPDX-License-Identifier: MIT
//
// Ported from ios-sweet (https://github.com/dipa-studio/ios-sweet), xnu/patches 0046, 0056, 0061,
// 0068 and 0079 (SweetSurface / SweetFramebuffer), for the Lumia 950 XL route of this device
// repository: built as a force-loaded driver archive instead of built-in xnu files, the panel being
// the firmware's GOP framebuffer.
//
// Copyright (c) 2026 dipa-studio
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

/*
 * Built-in IOMobileFramebuffer for the Lumia 950: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *      Boot-arg lumiafb_stayon=1 ignores off requests.
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI unless it is muted: on this phone it stops drawing from the first
 * presented frame on (boot-arg lumiafb_console=1 keeps it; panics draw).
 */
#include "NativeSurface.h"

extern "C" int native_console_muted;     /* osfmk/console/video_console.c (zzr overlay) */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

enum {
	kNativeFBGetLayerDefaultSurface  = 3,
	kNativeFBSwapBegin               = 4,
	kNativeFBSwapEnd                 = 5,
	kNativeFBSwapWait                = 6,
	kNativeFBGetID                   = 7,
	kNativeFBGetDisplaySize          = 8,
	kNativeFBRequestPowerChange      = 12,
	kNativeFBIsMainDisplay           = 18,
	kNativeFBGetDotPitch             = 28,
	kNativeFBNotifications           = 72,
};

#define NATIVE_FB_SWAP_REC_SIZE          0x46c
#define NATIVE_FB_LAYERS                 4
#define NATIVE_FB_NOTIFY_TYPES           8
#define NATIVE_FB_NOTIFY_VSYNC           5
#define NATIVE_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel at 1125 pixels across */
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as a real iPhone screen: by default the iPhone X's 1125x2436 (375x812
 * points @3x), or the canvas-width/canvas-height of the provider (the ADT's /arm-io/disp0,
 * which IOMobileFramebuffer.framework reads too).
 * The Lumia's firmware framebuffer is 1080x1920 (scaled up to the 1440x2560
 * panel by the display hardware), so by default the canvas is scaled to fit
 * it whole, centred, with bars at the sides (886x1920 at x 97).
 * Boot-arg lumiafb_fit=0 copies it 1:1 and centred instead (top and bottom
 * cut off); lumiafb_native=1 presents the real framebuffer size.
 */
#define NATIVE_FB_LOGICAL_W              1125
#define NATIVE_FB_LOGICAL_H              2436
#define NATIVE_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	bool          fFit;                     /* scale the canvas into fFitX/Y/W/H */
	uint32_t      fFitX, fFitY, fFitW, fFitH;
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	NativeSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[NATIVE_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
	uint32_t      fVsyncUS;       /* vsync period (boot-arg lumiafb_vsync, Hz) */
	bool          fPowerOff;      /* RequestPowerChange(0) */
	bool          fOffRequested;  /* backboardd asked for off while lumiafb_stayon kept the panel on */
	uint32_t      fFadeStep;      /* fade-out steps done while off */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
	void presentFitted(NativeSurface *s, uintptr_t src, int32_t sx, int32_t sy, int32_t dx, int32_t dy,
	    int32_t w, int32_t h);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

static AppleCLCD *gNativeCLCD;

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[NATIVE_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[NATIVE_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("NativeDisplay: no boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("lumiafb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	/* The 1440x2560 panel is wider than the iPhone X canvas: centre the canvas in it instead. */
	if (!native) {
		fLogicalW = NATIVE_FB_LOGICAL_W;
		fLogicalH = NATIVE_FB_LOGICAL_H;
		OSData *cw = OSDynamicCast(OSData, provider->getProperty("canvas-width"));
		OSData *ch = OSDynamicCast(OSData, provider->getProperty("canvas-height"));
		if (cw && ch && cw->getLength() == 4 && ch->getLength() == 4) {
			uint32_t w = *(const uint32_t *)cw->getBytesNoCopy(), h = *(const uint32_t *)ch->getBytesNoCopy();
			if (w >= 320 && w <= 4096 && h >= 320 && h <= 4096) {
				fLogicalW = w;
				fLogicalH = h;
			}
		}
		IOLog("NativeDisplay: canvas %ux%u\n", fLogicalW, fLogicalH);
	}
	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	int fit = 1;
	PE_parse_boot_argn("lumiafb_fit", &fit, sizeof(fit));
	fFit = !native && fit && fWidth <= 4096;
	if (fFit) {
		/* Largest canvas-shaped rectangle in the framebuffer, centred. */
		fFitH = fHeight;
		fFitW = (uint32_t)(((uint64_t)fLogicalW * fHeight) / fLogicalH);
		if (fFitW > fWidth) {
			fFitW = fWidth;
			fFitH = (uint32_t)(((uint64_t)fLogicalH * fWidth) / fLogicalW);
		}
		fFitX = (fWidth - fFitW) / 2;
		fFitY = (fHeight - fFitH) / 2;
	}
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = native_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, NATIVE_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("NativeDisplay: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("NativeDisplay: no vsync timer\n");
		return false;
	}
	/*
	 * backboardd renders a frame per vsync; on one CPU its software renderer
	 * is the largest load (boot 6: up to 13 of every 20 s), so the default
	 * vsync is 30 Hz, not the panel's 60 (boot-arg lumiafb_vsync=60 restores it).
	 */
	uint32_t vsyncHz = 30;
	PE_parse_boot_argn("lumiafb_vsync", &vsyncHz, sizeof(vsyncHz));
	vsyncHz = vsyncHz < 5 ? 5 : (vsyncHz > 60 ? 60 : vsyncHz);
	fVsyncUS = 1000000 / vsyncHz;
	uint32_t hz = 10;
	PE_parse_boot_argn("lumiafb_hz", &hz, sizeof(hz));
	fFlushTicks = (hz == 0 || hz >= vsyncHz) ? 1 : vsyncHz / hz;
	fVsync->setTimeoutUS(fVsyncUS);
	IOLog("NativeDisplay: vsync %u Hz, panel copy every %u vsync(s)\n", vsyncHz, fFlushTicks);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	gNativeCLCD = this;
	IOLog("NativeDisplay: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u%s\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH, fFit ? ", fitted" : "");
	if (fFit) {
		IOLog("NativeDisplay: canvas scaled to %ux%u at %u,%u\n", fFitW, fFitH, fFitX, fFitY);
	}
	return true;
}

/*
 * True while iOS believes the display is off (it asked for off; lumiafb_stayon kept the panel lit). iOS
 * then ignores touches until the side button wakes it; NativeHID turns a touch into that wake.
 */
extern "C" bool
native_display_off_requested(void)
{
	return gNativeCLCD != NULL && gNativeCLCD->fOffRequested;
}

/*
 * Where the presented canvas lands in the framebuffer, for the touch screen
 * (NativeHID): framebuffer size, the canvas rectangle in it, canvas size.
 * False until AppleCLCD has started.
 */
extern "C" bool
native_display_geometry(uint32_t *fbW, uint32_t *fbH, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h,
    uint32_t *canvasW, uint32_t *canvasH)
{
	AppleCLCD *fb = gNativeCLCD;
	if (fb == NULL) {
		return false;
	}
	*fbW = fb->fWidth;
	*fbH = fb->fHeight;
	*canvasW = fb->fLogicalW;
	*canvasH = fb->fLogicalH;
	if (fb->fFit) {
		*x = (int32_t)fb->fFitX;
		*y = (int32_t)fb->fFitY;
		*w = fb->fFitW;
		*h = fb->fFitH;
	} else {
		*x = -fb->fOffX;
		*y = -fb->fOffY;
		*w = fb->fLogicalW;
		*h = fb->fLogicalH;
	}
	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(NATIVE_FB_NOTIFY_VSYNC, values, 6);
	}
	if (fb->fPowerOff) {
		/* Fade out: halve every channel each vsync, then black, then idle. */
		uint32_t step = fb->fFadeStep;
		if (step <= 6) {
			fb->fFadeStep = step + 1;
		}
		IOLockUnlock(fb->fLock);
		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
		}
		sender->setTimeoutUS(fb->fVsyncUS);
		return;
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
	uint8_t *rec = NULL;
	if (flush) {
		rec = (uint8_t *)IOMallocData(NATIVE_FB_SWAP_REC_SIZE);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, NATIVE_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		fb->present(rec);
		IOFreeData(rec, NATIVE_FB_SWAP_REC_SIZE);
	}
	sender->setTimeoutUS(fb->fVsyncUS);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t native_fb_swaps_logged;
static uint64_t native_fb_swap_count;

/* Copy each enabled layer's surface into the panel memory (1:1, or scaled when fitted). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * shows the latest frame every fFlushTicks ticks (boot-arg lumiafb_hz,
 * default 10 Hz).
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
	IOLockLock(fLock);
	memcpy(fPending, rec, NATIVE_FB_SWAP_REC_SIZE);
	fDirty = true;
	IOLockUnlock(fLock);
	if ((++native_fb_swap_count % 1000) == 0) {
		IOLog("NativeDisplay: %llu swaps\n", native_fb_swap_count);
	}
}

void
AppleCLCD::present(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);


	if (!fConsolePaused) {
		int keep = 0;
		fConsolePaused = true;
		if (!PE_parse_boot_argn("lumiafb_console", &keep, sizeof(keep)) || keep == 0) {
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			native_console_muted = 1;
			/* The centred canvas never paints the bands around it: clear the
			 * console text out of them once. */
			for (uint32_t y = 0; y < fHeight; y++) {
				bzero((void *)(fPixels + (uint64_t)y * fRowBytes), (size_t)fWidth * 4);
			}
		}
	}

	bool clearBars = false;
	for (uint32_t layer = 0; layer < NATIVE_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		NativeSurface *s = native_surface_copy(id);
		if (native_fb_swaps_logged < 16) {
			native_fb_swaps_logged++;
			IOLog("NativeDisplay: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		if (fFit) {
			if (bpe == 4) {
				presentFitted(s, src, sx, sy, dx, dy, w, h);
			}
			s->release();
			clearBars = true;
			continue;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
		if (src != 0 && bpe == 4) {
			for (int32_t y = 0; y < h; y++) {
				memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
				    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
				    (size_t)w * 4);
			}
		}
		s->release();
	}
	/*
	 * The bars around the fitted canvas belong to no layer: clear them on every presented frame, so
	 * kernel console text written there (other CPUs, the time before the first frame) never lingers.
	 */
	if (clearBars) {
		for (uint32_t y = 0; y < fHeight; y++) {
			uint8_t *row = (uint8_t *)(fPixels + (uint64_t)y * fRowBytes);
			if (y < fFitY || y >= fFitY + fFitH) {
				bzero(row, (size_t)fWidth * 4);
				continue;
			}
			bzero(row, (size_t)fFitX * 4);
			bzero(row + (uint64_t)(fFitX + fFitW) * 4, (size_t)(fWidth - fFitX - fFitW) * 4);
		}
	}
}

/*
 * Nearest-neighbour scale of one layer (logical rectangle dx,dy,w,h, read
 * 1:1 from the surface at sx,sy) into the fitted canvas rectangle: every
 * framebuffer pixel of the rectangle takes the canvas pixel it covers.
 */
void
AppleCLCD::presentFitted(NativeSurface *s, uintptr_t src, int32_t sx, int32_t sy, int32_t dx, int32_t dy,
    int32_t w, int32_t h)
{
	static int32_t cols[4096];      /* surface column per framebuffer column, -1 = not this layer */

	if (w > (int32_t)s->width - sx) {
		w = (int32_t)s->width - sx;
	}
	if (h > (int32_t)s->height - sy) {
		h = (int32_t)s->height - sy;
	}
	if (src == 0 || w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 || s->memory == NULL ||
	    (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow + (uint64_t)(sx + w) * 4 >
	    s->memory->getLength()) {
		return;
	}
	for (uint32_t px = 0; px < fFitW; px++) {
		int32_t lx = (int32_t)(((uint64_t)px * fLogicalW) / fFitW);
		cols[px] = (lx >= dx && lx < dx + w) ? sx + (lx - dx) : -1;
	}
	for (uint32_t py = 0; py < fFitH; py++) {
		int32_t ly = (int32_t)(((uint64_t)py * fLogicalH) / fFitH);
		if (ly < dy || ly >= dy + h) {
			continue;
		}
		const uint32_t *in = (const uint32_t *)(src + s->offset + (uint64_t)(sy + (ly - dy)) * s->bytesPerRow);
		uint32_t *out = (uint32_t *)(fPixels + (uint64_t)(fFitY + py) * fRowBytes + (uint64_t)fFitX * 4);
		for (uint32_t px = 0; px < fFitW; px++) {
			if (cols[px] >= 0) {
				out[px] = in[cols[px]];
			}
		}
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= NATIVE_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t native_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kNativeFBGetLayerDefaultSurface:
		out[0] = native_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kNativeFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kNativeFBSwapEnd: {
		uint8_t rec[NATIVE_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < NATIVE_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kNativeFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kNativeFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kNativeFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kNativeFBGetDotPitch:
		out[0] = NATIVE_FB_DOT_PITCH * fFB->fLogicalW / NATIVE_FB_LOGICAL_W;   /* same physical size */
		outCount = 1;
		break;
	case kNativeFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < NATIVE_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("NativeDisplay: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kNativeFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			/* lumiafb_stayon=1: keep showing the last frame through display-off requests,
			 * while nothing can wake the display again (no buttons, touch or power source). */
			char who[32];
			proc_selfname(who, sizeof(who));
			uint32_t stayOn = 0;
			if (off && PE_parse_boot_argn("lumiafb_stayon", &stayOn, sizeof(stayOn)) && stayOn) {
				fFB->fOffRequested = true;
				IOLog("NativeDisplay: %s display off ignored (lumiafb_stayon)\n", who);
				break;
			}
			if (!off) {
				fFB->fOffRequested = false;
			}
			IOLockLock(fFB->fLock);
			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			IOLockUnlock(fFB->fLock);
			IOLog("NativeDisplay: %s display %s\n", who, off ? "off" : "on");
		}
		break;
	case kNativeFBSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(native_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			native_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("NativeDisplay: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
