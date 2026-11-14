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
 * Built-in IOSurface for the Lumia 950.
 *
 * Apple's IOSurface kext is closed. IOSurface.framework (iOS 16.7) finds
 * IOServiceNameMatching("IOCoreSurfaceRoot"), opens it with type 0 and
 * talks to the user client below:
 *
 *   sel 13  limits: struct out 0x18, u16 at +0x16 must be 1 (ABI version),
 *           +0 offset alignment mask, +4 bytes-per-row alignment mask
 *   sel 0   create: scalar [parent id], struct in = serialized property
 *           dictionary (binary or XML), struct out = lock result (0xa68)
 *   sel 4   lookup by id: scalar [id], struct out = lock result
 *   sel 34  lookup by mach port: scalar [port name], struct out = lock result
 *   sel 35  create mach port: scalars [id, options], scalar out [port name]
 *   traps   0/1 use count +/- (id, category), 2 lock / 3 unlock
 *           (id, options, &seed), 4/5 release (id)
 *
 * The lock result tells the library where the surface lives in the
 * caller's address space: +0 pixels, +8 shared state page (registry id,
 * seed at +0xc), +0x10 timestamps, then id, sizes and geometry (layout in
 * native_surface_fill_result). Every surface is wired kernel memory mapped
 * into each task that creates or looks it up; mappings are counted per
 * client and dropped on release or close.
 *
 * Values, notifications, accelerators and shared events are accepted and
 * ignored (logged once per selector).
 */
#include "NativeSurface.h"

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/system.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSUnserialize.h>
#include <sys/proc.h>

enum {
	kNativeSurfaceCreate             = 0,
	kNativeSurfaceLookup             = 4,
	kNativeSurfaceWrapClientImage    = 6,
	kNativeSurfaceWrapClientMemory   = 7,
	kNativeSurfaceLimits             = 13,
	kNativeSurfaceLookupFromPort     = 34,
	kNativeSurfaceCreatePort         = 35,
};

enum {
	kNativeSurfaceTrapIncrementUse   = 0,
	kNativeSurfaceTrapDecrementUse   = 1,
	kNativeSurfaceTrapLock           = 2,
	kNativeSurfaceTrapUnlock         = 3,
	kNativeSurfaceTrapRelease        = 4,
	kNativeSurfaceTrapReleaseFailed  = 5,
};

#define NATIVE_SURFACE_RESULT_SIZE       0xa68
#define NATIVE_SURFACE_MAX_ID            4096
#define NATIVE_SURFACE_MAX_DIM           16384
#define NATIVE_SURFACE_MAX_ALLOC         (256u << 20)
#define NATIVE_SURFACE_MAX_PROPS         (64u << 10)
#define NATIVE_SURFACE_ROW_ALIGN         64
#define NATIVE_SURFACE_TIMESTAMPS        0x800   /* offset in the state page */
#define NATIVE_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define kIOSurfaceLockReadOnly          0x1

static IOLock       *native_surface_lock;
static NativeSurface *native_surface_table[NATIVE_SURFACE_MAX_ID];
static uint32_t      native_surface_users[NATIVE_SURFACE_MAX_ID];
static uint32_t      native_surface_next_id = 1;
static uint32_t      native_surface_creates_logged;
static uint64_t      native_surface_wired;       /* bytes of buffer surfaces */
#define NATIVE_SURFACE_WIRED_MAX         (1536ull << 20)

OSDefineMetaClassAndStructors(NativeSurface, OSObject);

void
NativeSurface::free()
{
	if (OSDynamicCast(IOBufferMemoryDescriptor, memory) != NULL) {
		OSAddAtomic64(-(SInt64)allocSize, (volatile SInt64 *)&native_surface_wired);
	}
	OSSafeReleaseNULL(kernelMap);
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(state);
	OSObject::free();
}

uintptr_t
NativeSurface::kernelAddress()
{
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer != NULL) {
		return (uintptr_t)buffer->getBytesNoCopy();
	}
	IOLockLock(native_surface_lock);
	if (kernelMap == NULL && memory != NULL) {
		kernelMap = memory->map(kIOMapWriteCombineCache);
	}
	IOLockUnlock(native_surface_lock);
	return kernelMap != NULL ? (uintptr_t)kernelMap->getVirtualAddress() : 0;
}

static uint32_t *
native_surface_seed(NativeSurface *s)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + NATIVE_SURFACE_STATE_SEED);
}

/*
 * Give the surface an id and a table slot; the table holds one retain and a
 * user count (creators, client mappings, kernel holders). Lock held.
 */
static bool
native_surface_insert_locked(NativeSurface *s)
{
	for (uint32_t n = 0; n < NATIVE_SURFACE_MAX_ID - 1; n++) {
		uint32_t id = native_surface_next_id;
		native_surface_next_id = id + 1 < NATIVE_SURFACE_MAX_ID ? id + 1 : 1;
		if (native_surface_table[id] == NULL) {
			s->id = id;
			s->retain();
			native_surface_table[id] = s;
			native_surface_users[id] = 1;
			*(uint64_t *)s->state->getBytesNoCopy() = id;   /* registry id */
			return true;
		}
	}
	return false;
}

/* Add a user to a surface that may have left the table (port lookups). */
static bool
native_surface_get(NativeSurface *s)
{
	bool ok = true;
	IOLockLock(native_surface_lock);
	if (native_surface_table[s->id] == s) {
		native_surface_users[s->id]++;
	} else if (native_surface_table[s->id] == NULL) {
		s->retain();
		native_surface_table[s->id] = s;
		native_surface_users[s->id] = 1;
	} else {
		ok = false;
	}
	IOLockUnlock(native_surface_lock);
	return ok;
}

/* Drop a user; the last one removes the surface from the table. */
static void
native_surface_put(NativeSurface *s)
{
	bool drop = false;
	IOLockLock(native_surface_lock);
	if (native_surface_table[s->id] == s && --native_surface_users[s->id] == 0) {
		native_surface_table[s->id] = NULL;
		drop = true;
	}
	IOLockUnlock(native_surface_lock);
	if (drop) {
		s->release();
	}
}

NativeSurface *
native_surface_copy(uint32_t id)
{
	NativeSurface *s = NULL;
	if (id == 0 || id >= NATIVE_SURFACE_MAX_ID || native_surface_lock == NULL) {
		return NULL;
	}
	IOLockLock(native_surface_lock);
	s = native_surface_table[id];
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(native_surface_lock);
	return s;
}

/* The table lock; the framebuffer may publish a surface before the root starts. */
static void
native_surface_init_lock(void)
{
	if (native_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &native_surface_lock)) {
			IOLockFree(lock);
		}
	}
}

static NativeSurface *
native_surface_alloc(void)
{
	native_surface_init_lock();
	NativeSurface *s = OSTypeAlloc(NativeSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	if (s->state == NULL) {
		s->release();
		return NULL;
	}
	bzero(s->state->getBytesNoCopy(), page_size);
	return s;
}

static bool
native_surface_publish(NativeSurface *s)
{
	bool ok;
	IOLockLock(native_surface_lock);
	ok = native_surface_insert_locked(s);
	IOLockUnlock(native_surface_lock);
	return ok;
}

NativeSurface *
native_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	NativeSurface *s = native_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page(bytesPerRow * height);
	s->memory = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	s->mapCache = kIOMapWriteCombineCache;
	if (s->memory == NULL || !native_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

mach_port_name_t
native_surface_port_in_task(NativeSurface *s, task_t task)
{
	mach_port_name_t name = 0;
	IOUserClient::copyPortNameForObjectInTask(task, s, &name);
	return name;
}

static uint32_t
native_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

static uint32_t
native_div_up(uint32_t a, uint32_t b)
{
	return b > 1 ? (a + b - 1) / b : a;
}

static uint32_t
native_row_bytes(uint32_t width, uint32_t ew, uint32_t bpe)
{
	uint64_t row = (uint64_t)native_div_up(width, ew) * bpe;
	return (uint32_t)((row + NATIVE_SURFACE_ROW_ALIGN - 1) & ~(uint64_t)(NATIVE_SURFACE_ROW_ALIGN - 1));
}

/* Geometry from the creation properties (IOSurface.h keys). */
static IOReturn
native_surface_layout(NativeSurface *s, OSDictionary *props)
{
	uint64_t end;

	s->width = native_dict_u32(props, "IOSurfaceWidth", 0);
	s->height = native_dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)native_dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)native_dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)native_dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = native_dict_u32(props, "IOSurfacePixelFormat", 0);
	s->cacheMode = native_dict_u32(props, "IOSurfaceCacheMode", 0);
	s->offset = native_dict_u32(props, "IOSurfaceOffset", 0);
	if (s->width > NATIVE_SURFACE_MAX_DIM || s->height > NATIVE_SURFACE_MAX_DIM ||
	    s->bytesPerElement > 64 || s->elementWidth == 0 || s->elementHeight == 0) {
		return kIOReturnBadArgument;
	}
	s->bytesPerRow = native_dict_u32(props, "IOSurfaceBytesPerRow",
	    native_row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->bytesPerRow < (uint64_t)native_div_up(s->width, s->elementWidth) * s->bytesPerElement) {
		return kIOReturnBadArgument;
	}
	end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * native_div_up(s->height, s->elementHeight);

	OSArray *planes = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	s->planeCount = 0;
	if (planes != NULL) {
		uint64_t next = s->offset;
		if (planes->getCount() > NATIVE_SURFACE_MAX_PLANES) {
			return kIOReturnBadArgument;
		}
		for (unsigned int i = 0; i < planes->getCount(); i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			struct native_surface_plane *p = &s->planes[i];
			if (pd == NULL) {
				return kIOReturnBadArgument;
			}
			p->width = native_dict_u32(pd, "IOSurfacePlaneWidth", s->width);
			p->height = native_dict_u32(pd, "IOSurfacePlaneHeight", s->height);
			p->bytesPerElement = (uint16_t)native_dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)native_dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)native_dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			if (p->width > NATIVE_SURFACE_MAX_DIM || p->height > NATIVE_SURFACE_MAX_DIM ||
			    p->bytesPerElement > 64 || p->elementWidth == 0 || p->elementHeight == 0) {
				return kIOReturnBadArgument;
			}
			p->bytesPerRow = native_dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    native_row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = native_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = native_dict_u32(pd, "IOSurfacePlaneSize",
			    p->bytesPerRow * native_div_up(p->height, p->elementHeight));
			next = round_page((uint64_t)p->offset + p->size);
			if ((uint64_t)p->offset + p->size > end) {
				end = (uint64_t)p->offset + p->size;
			}
		}
		s->planeCount = planes->getCount();
	}

	uint64_t alloc = native_dict_u32(props, "IOSurfaceAllocSize", 0);
	if (alloc < end) {
		alloc = end;
	}
	alloc = round_page(alloc);
	if (alloc == 0 || alloc > NATIVE_SURFACE_MAX_ALLOC) {
		return kIOReturnNoMemory;
	}
	s->allocSize = (uint32_t)alloc;
	return kIOReturnSuccess;
}

/* Per-client mapping of one surface. */
class NativeSurfaceRef : public OSObject {
	OSDeclareDefaultStructors(NativeSurfaceRef);

public:
	NativeSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(NativeSurfaceRef, OSObject);

void
NativeSurfaceRef::free()
{
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL) {
		native_surface_put(surface);
		surface->release();
		surface = NULL;
	}
	OSObject::free();
}

class IOCoreSurfaceRoot : public IOService {
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	native_surface_init_lock();
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	registerService();
	IOLog("NativeSurface: IOCoreSurfaceRoot registered\n");
	return true;
}

class IOSurfaceRootUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fRefs;

	NativeSurfaceRef *findRef(uint32_t id, unsigned int *index);
	IOReturn addRef(NativeSurface *s, NativeSurfaceRef **refOut, bool *existing);
	IOReturn returnSurface(NativeSurface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

static uint64_t native_surf_logged;      /* selectors / traps already logged (< 64) */

static void
native_surf_log_once(const char *what, uint32_t n)
{
	uint32_t bit = (what[0] == 't' ? 48 : 0) + n;
	if (bit >= 64 || (native_surf_logged & (1ULL << bit))) {
		return;
	}
	native_surf_logged |= 1ULL << bit;
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("NativeSurface: %s %s %u\n", name, what, n);
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(16);
	return fLock != NULL && fRefs != NULL;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

/* Lock held. */
NativeSurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		NativeSurfaceRef *ref = (NativeSurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id) {
			if (index != NULL) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

/*
 * Count one more create/lookup of s by this client, mapping it into the
 * task the first time. Takes over the caller's user count on s.
 *
 * A lookup of a surface this client already has maps nothing and counts
 * nothing (*existing): IOSurface.framework then gets base address -1 in the
 * lock result and reuses its cached IOSurfaceClient. Counting it instead
 * made the framework build a second client for the same id; releasing that
 * one (traps 4 + 5) unmapped the memory under the first (backboardd
 * snapshot crash in CGDataProviderCreateWithData).
 */
IOReturn
IOSurfaceRootUserClient::addRef(NativeSurface *s, NativeSurfaceRef **refOut, bool *existing)
{
	*existing = false;
	IOLockLock(fLock);
	NativeSurfaceRef *ref = findRef(s->id, NULL);
	if (ref != NULL && ref->surface == s) {
		IOLockUnlock(fLock);
		native_surface_put(s);
		*refOut = ref;
		*existing = true;
		return kIOReturnSuccess;
	}
	IOLockUnlock(fLock);

	ref = OSTypeAlloc(NativeSurfaceRef);
	if (ref == NULL || !ref->init()) {
		OSSafeReleaseNULL(ref);
		native_surface_put(s);
		return kIOReturnNoMemory;
	}
	s->retain();
	ref->surface = s;
	ref->count = 1;
	ref->data = s->memory->createMappingInTask(fTask, 0, kIOMapAnywhere | s->mapCache);
	ref->state = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (ref->data == NULL || ref->state == NULL) {
		ref->release();
		return kIOReturnVMError;
	}
	IOLockLock(fLock);
	NativeSurfaceRef *raced = findRef(s->id, NULL);
	if (raced != NULL && raced->surface == s) {
		/* Another thread mapped it meanwhile: count there, drop ours. */
		raced->count++;
		IOLockUnlock(fLock);
		ref->release();
		*refOut = raced;
		return kIOReturnSuccess;
	}
	fRefs->setObject(ref);
	IOLockUnlock(fLock);
	ref->release();         /* fRefs holds it */
	*refOut = ref;
	return kIOReturnSuccess;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, 4);
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, 8);
}

/* IOSurface.framework's lock result (copied to IOSurfaceClient + 0x68). */
static void
native_surface_fill_result(NativeSurface *s, NativeSurfaceRef *ref, uint8_t *out)
{
	bzero(out, NATIVE_SURFACE_RESULT_SIZE);
	put64(out, 0x00, ref->data->getAddress());
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + NATIVE_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, 2);
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x68 + i * 0x80;
		const struct native_surface_plane *pl = &s->planes[i];
		put32(p, 0x00, pl->width);
		put32(p, 0x04, pl->height);
		/* (bytes a row at 0x10, as IOSurfaceClientGetBytesPerRowOfPlane reads them; 0x08 is not used: they were
		 * there until boot 54, and the framework said 0 -- a slanted, then black screen through OpenGL ES) */
		put32(p, 0x0c, pl->offset);
		put32(p, 0x10, pl->bytesPerRow);
		put32(p, 0x14, pl->size);
		memcpy(p + 0x18, &pl->bytesPerElement, 2);
		p[0x1a] = pl->elementWidth;
		p[0x1b] = pl->elementHeight;
	}
}

static IOReturn
native_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Map s for this client and answer with its lock result. Takes s's user count. */
IOReturn
IOSurfaceRootUserClient::returnSurface(NativeSurface *s, IOExternalMethodArguments *args)
{
	NativeSurfaceRef *ref = NULL;
	bool existing = false;
	IOReturn ret = addRef(s, &ref, &existing);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	uint8_t *out = (uint8_t *)IOMallocZeroData(NATIVE_SURFACE_RESULT_SIZE);
	if (out == NULL) {
		ret = kIOReturnNoMemory;
	} else {
		native_surface_fill_result(s, ref, out);
		if (existing) {
			put64(out, 0x00, ~0ULL);        /* "already yours": use the cached client */
		}
		ret = native_struct_out(args, out, NATIVE_SURFACE_RESULT_SIZE);
		IOFreeData(out, NATIVE_SURFACE_RESULT_SIZE);
	}
	if (ret != kIOReturnSuccess && !existing) {
		/* The caller never learns the id: undo this create/lookup. */
		trapRelease((void *)(uintptr_t)s->id, NULL, NULL, NULL, NULL, NULL);
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
	const char *buf = (const char *)args->structureInput;
	uint32_t len = args->structureInputSize;
	char *copy = NULL;
	IOReturn ret;

	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len == 0 || len > NATIVE_SURFACE_MAX_PROPS) {
			return kIOReturnBadArgument;
		}
		copy = (char *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		args->structureInputDescriptor->readBytes(0, copy, len);
		buf = copy;
	}
	OSObject *obj = (buf != NULL && len > 0) ? OSUnserializeXML(buf, len) : NULL;
	if (copy != NULL) {
		IOFreeData(copy, len);
	}
	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		IOLog("NativeSurface: create: bad property dictionary (%u bytes)\n", len);
		return kIOReturnBadArgument;
	}

	NativeSurface *s = native_surface_alloc();
	if (s == NULL) {
		props->release();
		return kIOReturnNoMemory;
	}
	s->parentID = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	ret = native_surface_layout(s, props);
	if (ret == kIOReturnSuccess) {
		IOBufferMemoryDescriptor *mem = NULL;
		if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&native_surface_wired) +
		    s->allocSize <= NATIVE_SURFACE_WIRED_MAX) {
			mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
		}
		if (mem == NULL) {
			OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&native_surface_wired);
			ret = kIOReturnNoMemory;
		} else {
			bzero(mem->getBytesNoCopy(), s->allocSize);
			s->memory = mem;
			s->mapCache = kIOMapDefaultCache;
			if (!native_surface_publish(s)) {
				ret = kIOReturnNoResources;
			}
		}
	}
	if (ret != kIOReturnSuccess || native_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		native_surface_creates_logged++;
		IOLog("NativeSurface: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
		for (uint32_t i = 0; i < s->planeCount && ret == kIOReturnSuccess; i++) {
			IOLog("NativeSurface:   plane %u %ux%u bpe %u elem %ux%u bpr %u offset %u size %u\n", i,
			    s->planes[i].width, s->planes[i].height, s->planes[i].bytesPerElement, s->planes[i].elementWidth,
			    s->planes[i].elementHeight, s->planes[i].bytesPerRow, s->planes[i].offset, s->planes[i].size);
		}
	}
	props->release();
	if (ret != kIOReturnSuccess) {
		s->release();
		return ret;
	}
	ret = returnSurface(s, args);   /* takes the creator's user count */
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kNativeSurfaceLimits: {
		uint8_t limits[0x18] = { 0 };
		put32(limits, 0x0, NATIVE_SURFACE_ROW_ALIGN - 1);
		put32(limits, 0x4, NATIVE_SURFACE_ROW_ALIGN - 1);
		limits[0x16] = 1;
		native_surf_log_once("sel", selector);
		return native_struct_out(args, limits, sizeof(limits));
	}
	case kNativeSurfaceCreate:
		return create(args);
	case kNativeSurfaceLookup: {
		NativeSurface *s = native_surface_copy((uint32_t)arg0);
		IOReturn ret = kIOReturnNotFound;
		if (s != NULL) {
			ret = native_surface_get(s) ? returnSurface(s, args) : kIOReturnNotFound;
			s->release();
		}
		native_surf_log_once("sel", selector);
		return ret;
	}
	case kNativeSurfaceLookupFromPort: {
		OSObject *obj = NULL;
		IOReturn ret = IOUserClient::copyObjectForPortNameInTask(fTask, (mach_port_name_t)arg0, &obj);
		NativeSurface *s = OSDynamicCast(NativeSurface, obj);
		if (ret == kIOReturnSuccess) {
			ret = (s != NULL && native_surface_get(s)) ? returnSurface(s, args) : kIOReturnBadArgument;
		}
		OSSafeReleaseNULL(obj);
		native_surf_log_once("sel", selector);
		return ret;
	}
	case kNativeSurfaceCreatePort: {
		NativeSurface *s = native_surface_copy((uint32_t)arg0);
		if (s == NULL || args->scalarOutputCount < 1) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = native_surface_port_in_task(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		native_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	case kNativeSurfaceWrapClientImage:
	case kNativeSurfaceWrapClientMemory:
		/*
		 * A surface around memory of the client's own (IOSurfaceClientWrapClientImage: CoreAnimation's
		 * OpenGL ES renderer asks for one for each picture it draws from). Answered with success and
		 * no lock result, the framework made a client with no state page and QuartzCore read through
		 * it (iosurface_get_edr_factor -> IOSurfaceClientGetBulkAttachments, far 0x30: boots 49-52).
		 * Refused, the renderer draws those pictures another way (HakiOS's MiniIOSurface did so before
		 * it made them).
		 */
		native_surf_log_once("sel", selector);
		return kIOReturnUnsupported;
	default:
		/* Values, notifications, ownership, timestamps...: accept and ignore. */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		native_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

static IOReturn
native_surface_seed_out(NativeSurface *s, void *seedOut, bool bump)
{
	uint32_t seed = bump ? OSIncrementAtomic((volatile SInt32 *)native_surface_seed(s)) + 1 :
	    *native_surface_seed(s);
	if (seedOut != NULL && copyout(&seed, (user_addr_t)seedOut, sizeof(seed)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

/* Retained surface this client created or looked up, or NULL. */
static NativeSurface *
native_surface_held(IOLock *lock, OSArray *refs, uint32_t id)
{
	NativeSurface *s = NULL;
	IOLockLock(lock);
	for (unsigned int i = 0; i < refs->getCount(); i++) {
		NativeSurfaceRef *ref = (NativeSurfaceRef *)refs->getObject(i);
		if (ref->surface->id == id) {
			s = ref->surface;
			s->retain();
			break;
		}
	}
	IOLockUnlock(lock);
	return s;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, __unused void *options, void *seedOut,
    void *, void *, void *)
{
	NativeSurface *s = native_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = native_surface_seed_out(s, seedOut, false);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	NativeSurface *s = native_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = native_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	IOLockLock(fLock);
	NativeSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index);
	if (ref != NULL && --ref->count == 0) {
		fRefs->removeObject(index);     /* unmaps and drops the user count */
	}
	IOLockUnlock(fLock);
	return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
}

static IOExternalTrap native_surface_traps[] = {
	[kNativeSurfaceTrapIncrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kNativeSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kNativeSurfaceTrapLock]          = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	[kNativeSurfaceTrapUnlock]        = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	[kNativeSurfaceTrapRelease]       = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
	[kNativeSurfaceTrapReleaseFailed] = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(native_surface_traps) / sizeof(native_surface_traps[0])) {
		native_surf_log_once("trap", index);
		return NULL;
	}
	native_surf_log_once("trap", index);
	*targetP = this;
	return &native_surface_traps[index];
}
