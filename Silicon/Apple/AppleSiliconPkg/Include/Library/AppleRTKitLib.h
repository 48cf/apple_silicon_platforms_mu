/**
 * @file AppleRTKitLib.h
 * @author 48cf <me@iretq.dev>
 * @brief
 *
 * AppleRTKitLib header file.
 * Contains the interface for talking to the Apple RTKit firmware.
 *
 * @date 2025-10-22
 * @copyright Copyright (c) 2025, AppleWOA Authors. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include <PiDxe.h>
#include <Library/AppleMailboxLib.h>

#ifndef APPLE_RTKIT_LIB_H
#define APPLE_RTKIT_LIB_H

typedef struct _APPLE_RTKIT APPLE_RTKIT;

typedef struct _APPLE_RTKIT_MESSAGE {
    UINT64 Message;
    UINT8 Endpoint;
} APPLE_RTKIT_MESSAGE;

typedef struct _APPLE_RTKIT_BUFFER {
    UINT64 Address;
    UINT64 DeviceAddress;
    UINTN Size;
    UINT8 Endpoint;
} APPLE_RTKIT_BUFFER;

typedef EFI_STATUS (EFIAPI *APPLE_RTKIT_SHMEM_SETUP)(
    IN APPLE_RTKIT *RtKit,
    IN OUT APPLE_RTKIT_BUFFER *Buffer,
    IN VOID *Context);

typedef EFI_STATUS (EFIAPI *APPLE_RTKIT_SHMEM_DESTROY)(
    IN APPLE_RTKIT *RtKit,
    IN OUT APPLE_RTKIT_BUFFER *Buffer,
    IN VOID *Context);

/**
 * Initialize an RTKit instance.
 * @param Mailbox - Pointer to the mailbox to use for communication with RTKit.
 * @param ShmemSetup - Callback to setup shared memory buffers for the co-processor.
 * @param ShmemDestroy - Callback to destroy previously allocated shared memory buffers.
 * @param ShmemContext - Opaque context pointer to passed to the shared memory callbacks.
 * @return APPLE_RTKIT * - Pointer to the initialized APPLE_RTKIT structure.
 */
APPLE_RTKIT *
EFIAPI
AppleRTKitInitialize(
    IN APPLE_MAILBOX *Mailbox,
    IN APPLE_RTKIT_SHMEM_SETUP ShmemSetup,
    IN APPLE_RTKIT_SHMEM_DESTROY ShmemDestroy,
    IN VOID *ShmemContext
);

/**
 * Deinitialize an RTKit instance.
 * @param RtKit - Pointer to the APPLE_RTKIT structure to deinitialize.
 */
VOID
EFIAPI
AppleRTKitDeinitialize(
    IN APPLE_RTKIT *RtKit
);

/**
 * Boot the RTKit firmware.
 * @param RtKit - Pointer to the APPLE_RTKIT structure.
 * @return EFI_STATUS - If the RTKit was booted successfully, returns EFI_SUCCESS; otherwise, returns an appropriate error code.
 */
EFI_STATUS
EFIAPI
AppleRTKitBoot(
    IN APPLE_RTKIT *RtKit
);

/**
 * Receive a message from the RTKit firmware.
 * @param RtKit - Pointer to the APPLE_RTKIT structure.
 * @param Message - Pointer to the APPLE_RTKIT_MESSAGE structure to receive the message.
 * @return EFI_STATUS - If a message was received successfully, returns EFI_SUCCESS; otherwise, returns an appropriate error code.
 */
EFI_STATUS
EFIAPI
AppleRTKitReceiveMessage(
    IN APPLE_RTKIT *RtKit,
    OUT APPLE_RTKIT_MESSAGE *Message
);

/**
 * Start an RTKit endpoint.
 * @param RtKit - Pointer to the APPLE_RTKIT structure.
 * @param Endpoint - The endpoint number to start.
 * @return EFI_STATUS - If the endpoint was started successfully, returns EFI_SUCCESS; otherwise, returns an appropriate error code.
 */
EFI_STATUS
EFIAPI
AppleRTKitStartEndpoint(
    IN APPLE_RTKIT *RtKit,
    IN UINT8 Endpoint
);

#endif // APPLE_RTKIT_LIB_H
