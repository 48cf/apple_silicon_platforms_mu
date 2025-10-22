/**
 * @file AppleMailboxLib.h
 * @author 48cf <me@iretq.dev>
 * @brief
 * 
 * AppleMailboxLib header file.
 * Contains the interface for interfacing with the Apple Mailbox hardware.
 * 
 * @date 2025-10-22
 * @copyright Copyright (c) 2025, AppleWOA Authors. All rights reserved.
 * 
 * SPDX-License-Identifier: MIT
 *
 */

#include <PiDxe.h>
#include <Library/ConvenienceMacros.h>

#ifndef APPLE_MAILBOX_LIB_H
#define APPLE_MAILBOX_LIB_H

typedef struct _APPLE_MAILBOX {
    UINTN BaseAddress;
} APPLE_MAILBOX;

typedef struct _APPLE_MAILBOX_MESSAGE {
    UINT64 Message0;
    UINT32 Message1;
} APPLE_MAILBOX_MESSAGE;

/**
 * Initialize a mailbox instance.
 * @param BaseAddress - The base address of the mailbox hardware.
 * @return APPLE_MAILBOX * - Pointer to the initialized APPLE_MAILBOX structure.
 */
APPLE_MAILBOX *
EFIAPI
AppleMailboxInitialize(
    IN UINTN BaseAddress
);

/**
 * Deinitialize a mailbox instance.
 * @param Mailbox - Pointer to the APPLE_MAILBOX structure to deinitialize.
 */
VOID
EFIAPI
AppleMailboxDeinitialize(
    IN APPLE_MAILBOX *Mailbox
);

/**
 * Send a message to the specified mailbox.
 * @param Mailbox - Pointer to the APPLE_MAILBOX structure representing the mailbox.
 * @param Message - Pointer to the APPLE_MAILBOX_MESSAGE structure containing the message to send.
 * @return EFI_STATUS - If the message was sent successfully, returns EFI_SUCCESS; otherwise, returns EFI_NOT_READY.
 */
EFI_STATUS
EFIAPI
AppleMailboxSendMessage(
    IN APPLE_MAILBOX *Mailbox,
    IN APPLE_MAILBOX_MESSAGE *Message
);

/**
 * Receive a message from the specified mailbox.
 * @param Mailbox - Pointer to the APPLE_MAILBOX structure to initialize.
 * @param Message - Pointer to the APPLE_MAILBOX_MESSAGE structure to store the received message.
 * @return EFI_STATUS - If a message was received successfully, returns EFI_SUCCESS; otherwise, returns EFI_NOT_READY.
 */
EFI_STATUS
EFIAPI
AppleMailboxReceiveMessage(
    IN APPLE_MAILBOX *Mailbox,
    OUT APPLE_MAILBOX_MESSAGE *Message
);

#endif // APPLE_MAILBOX_LIB_H
