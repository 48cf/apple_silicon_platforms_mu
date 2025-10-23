/**
 * @file AppleSARTLib.h
 * @author 48cf <me@iretq.dev>
 * @brief
 *
 * AppleSARTLib header file.
 * Contains the interface for programming the Apple SART hardware.
 * The SART is a simple address filtering unit required for the ANS NVMe controller.
 * 
 * @date 2025-10-22
 * @copyright Copyright (c) 2025, AppleWOA Authors. All rights reserved.
 * 
 * SPDX-License-Identifier: MIT
 *
 */

#include <PiDxe.h>

#ifndef APPLE_SART_LIB_H
#define APPLE_SART_LIB_H

#define APPLE_SART_ALLOW_ALL_FLAG 0xff

typedef struct _APPLE_SART APPLE_SART;

typedef struct {
    UINT64 Address;
    UINT64 Size;
    UINT64 Flags;
} APPLE_SART_ENTRY;

/**
 * Initialize a SART instance.
 * @param BaseAddress - The base address of the SART hardware.
 * @return APPLE_SART * - Pointer to the initialized APPLE_SART structure.
 */
APPLE_SART *
EFIAPI
AppleSARTInitialize(
    IN UINT64 BaseAddress
);

/**
 * Deinitialize a SART instance.
 * @param Sart - Pointer to the APPLE_SART structure to deinitialize.
 */
VOID
EFIAPI
AppleSARTDeinitialize(
    IN APPLE_SART *Sart
);

/**
 * Add an entry to the SART. This programs the SART to allow DMA access to the specified memory region.
 * @param Sart - Pointer to the APPLE_SART structure.
 * @param Entry - Pointer to the APPLE_SART_ENTRY structure.
 * @return EFI_STATUS - If the entry was added successfully, returns EFI_SUCCESS; otherwise,
                        returns EFI_OUT_OF_RESOURCES if no free entries are available or EFI_INVALID_PARAMETER if the parameters are invalid.
 */
EFI_STATUS
EFIAPI
AppleSARTAddEntry(
    IN APPLE_SART *Sart,
    IN APPLE_SART_ENTRY *Entry
);

/**
 * Remove an entry from the SART. This programs the SART to no longer allow DMA access to the specified memory region.
 * @param Sart - Pointer to the APPLE_SART structure.
 * @param Entry - Pointer to the APPLE_SART_ENTRY structure.
 * @return EFI_STATUS - If the entry was removed successfully, returns EFI_SUCCESS; otherwise, returns EFI_NOT_FOUND.
 */
EFI_STATUS
EFIAPI
AppleSARTRemoveEntry(
    IN APPLE_SART *Sart,
    IN APPLE_SART_ENTRY *Entry
);

#endif // APPLE_SART_LIB_H
