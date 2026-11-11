// The IOKit user-space calls these tools use. The iPhoneOS SDK ships IOKit.framework (its .tbd) but not its
// headers; these are the public macOS prototypes (IOKit/IOKitLib.h), which are the same on iOS.
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stdint.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_registry_entry_t;
typedef io_object_t io_connect_t;
typedef int IOReturn;
#ifndef kIOMapAnywhere
#define kIOMapAnywhere 0x00000001
#endif

extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator,
                                          uint32_t options);
kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *inputStruct,
                                        size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
                                   mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
                                     mach_vm_address_t atAddress);
