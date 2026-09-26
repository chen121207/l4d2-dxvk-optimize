#pragma once
/*
 * FGDS Vulkan 0.1 experimental contract.
 *
 * This header intentionally avoids a hard dependency on the Vulkan SDK so
 * that a producer and a consumer can share the ABI from x86 or x64 builds.
 * Vulkan handles are encoded as their native 64-bit handle value. The
 * producer remains responsible for device ownership and synchronization.
 */
#include <stdint.h>

#define FGDS_VK_VERSION 0x00010000u
#define FGDS_VK_CAMERA_CUT 1u

typedef struct FgdsVkImage
{
    uint64_t image;
    uint64_t view;
    uint32_t format;
    uint32_t layout;
} FgdsVkImage;

typedef struct FgdsVkSync
{
    uint64_t semaphore;
    uint64_t value;
} FgdsVkSync;

typedef struct FgdsVkFrame
{
    uint32_t structSize, version, width, height;
    uint64_t frameId, timestampNs;
    float worldToClip[16];
    float jitterPixels[2];
    uint32_t flags, reserved;
    uint64_t device;
    uint64_t physicalDevice;
    uint32_t queueFamily;
    uint32_t reserved2;
    FgdsVkImage color;
    FgdsVkImage depth;
    FgdsVkImage motionToOther;
    FgdsVkImage objectId;
    FgdsVkImage hudMask;
    FgdsVkImage transparencyMask;
    FgdsVkSync ready;
} FgdsVkFrame;

typedef struct FgdsVkPair
{
    uint32_t structSize, version;
    FgdsVkFrame frames[2];
    float alpha;
    uint32_t reserved;
} FgdsVkPair;
