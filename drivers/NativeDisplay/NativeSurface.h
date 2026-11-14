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
 * Built-in IOSurface for the Lumia 950 (see NativeSurface.cpp).
 *
 * Shared with NativeDisplay.cpp: the framebuffer looks surfaces up by
 * id when a swap names them and publishes the panel memory itself as the
 * default framebuffer surface.
 */
#ifndef _NATIVE_SURFACE_H
#define _NATIVE_SURFACE_H


#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <mach/mach_types.h>

#define NATIVE_SURFACE_MAX_PLANES        20

struct native_surface_plane {
	uint32_t width, height, bytesPerRow, offset, size;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
};

class NativeSurface : public OSObject {
	OSDeclareDefaultStructors(NativeSurface);

public:
	uint32_t id;
	uint32_t width, height, bytesPerRow, offset, pixelFormat, allocSize;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
	uint32_t cacheMode, planeCount, parentID;
	struct native_surface_plane planes[NATIVE_SURFACE_MAX_PLANES];

	IOMemoryDescriptor       *memory;       /* pixels (buffer or physical) */
	IOOptionBits              mapCache;     /* cache mode for user mappings */
	IOBufferMemoryDescriptor *state;        /* shared state page, read-only for users */
	IOMemoryMap              *kernelMap;    /* lazily created for kernel copies */
	uint32_t                  gpuPhys;      /* memory from the GPU's pool (native_gpu_memory()): its address, or 0 */
	uint32_t                  gpuPlace;     /* ... and its place there, to give it back with */
	uint8_t                  *gpuKva;
	IOMemoryMap              *gpuCachedMap;  /* (unused since gl11: a cached alias of write-combined pages is not cached) */
	bool                      gpuPool;      /* gpuPhys is the GPU's pool's (given back when the surface goes) */
	bool                      gpuPanel;     /* the panel's own memory (NativeDisplay's default surface) */

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;

	/* Kernel address of the pixels (mapped on first use), or 0. */
	uintptr_t kernelAddress();
	uintptr_t cachedAddress();      /* GPU memory: a cached mapping of it, its lines invalidated first; else 0 */
};

/* Retained surface for an id, or NULL. */
NativeSurface *native_surface_copy(uint32_t id);
extern bool native_surface_gpu_presents;     /* the GPU draws the frames onto the panel itself (NativeSurface.cpp) */

/*
 * Publish physical memory (the panel framebuffer) as a surface. Returns a
 * retained surface that stays in the id table until released by the caller
 * and every user.
 */
NativeSurface *native_surface_create_physical(uint64_t phys, uint32_t width,
    uint32_t height, uint32_t bytesPerRow, uint32_t pixelFormat);

/* Send right for the surface in task (IOSurfaceLookupFromMachPort). */
mach_port_name_t native_surface_port_in_task(NativeSurface *surface, task_t task);

#endif /* _NATIVE_SURFACE_H */
