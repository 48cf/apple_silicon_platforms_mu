/**
 *  Copyright (c) 2025, AppleWOA authors. All rights reserved.
 *
 *  Module Name:
 *    AppleMailboxLib.inf
 *  
 *  Abstract:
 *     Simple library to interface with the Apple Mailbox hardware.
 *
 *  Environment:
 *     UEFI DXE (Driver Execution Environment) and runtime services.
 *
 *  License:
 *    SPDX-License-Identifier: MIT
 * 
 */

// This driver only implements support for the ASC mailboxes for now.

#include <Library/AppleMailboxLib.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>

struct _APPLE_MAILBOX {
    UINTN BaseAddress;
};

// Register definitions borrowed from the m1n1 driver.
#define ASC_CPU_CONTROL_REG 0x44
#define ASC_CPU_CONTROL_START BIT4

// A2I registers (CPU to co-processor)
#define ASC_MBOX_A2I_CONTROL_REG 0x110
#define ASC_MBOX_A2I_SEND0_REG 0x800
#define ASC_MBOX_A2I_SEND1_REG 0x808

// I2A registers (co-processor to CPU)
#define ASC_MBOX_I2A_CONTROL_REG 0x114
#define ASC_MBOX_I2A_RECV0_REG 0x830
#define ASC_MBOX_I2A_RECV1_REG 0x838

#define ASC_MBOX_CONTROL_FULL BIT16
#define ASC_MBOX_CONTROL_EMPTY BIT17

STATIC
UINT32
MailboxRead32(
    IN APPLE_MAILBOX *Mailbox,
    IN UINTN Offset
) {
    return MmioRead32(Mailbox->BaseAddress + Offset);
}

STATIC
VOID
MailboxWrite32(
    IN APPLE_MAILBOX *Mailbox,
    IN UINTN Offset,
    IN UINT32 Value
) {
    MmioWrite32(Mailbox->BaseAddress + Offset, Value);
}

STATIC
UINT64
MailboxRead64(
    IN APPLE_MAILBOX *Mailbox,
    IN UINTN Offset
) {
    return MmioRead64(Mailbox->BaseAddress + Offset);
}

STATIC
VOID
MailboxWrite64(
    IN APPLE_MAILBOX *Mailbox,
    IN UINTN Offset,
    IN UINT64 Value
) {
    MmioWrite64(Mailbox->BaseAddress + Offset, Value);
}

APPLE_MAILBOX *
EFIAPI
AppleMailboxInitialize(
    IN UINTN BaseAddress
) {
    APPLE_MAILBOX *Mailbox;
    UINT32 CpuControl;

    Mailbox = AllocateZeroPool(sizeof(APPLE_MAILBOX));
    if (Mailbox == NULL) {
        DEBUG((DEBUG_ERROR, "AppleMailboxInitialize: Failed to allocate memory for the mailbox structure\n"));
        return NULL;
    }

    Mailbox->BaseAddress = BaseAddress;

    // Make sure the mailbox is started.
    CpuControl = MailboxRead32(Mailbox, ASC_CPU_CONTROL_REG);

    if ((CpuControl & ASC_CPU_CONTROL_START) == 0) {
        CpuControl |= ASC_CPU_CONTROL_START;
        MailboxWrite32(Mailbox, ASC_CPU_CONTROL_REG, CpuControl);
    }

    return Mailbox;
}

VOID
EFIAPI
AppleMailboxDeinitialize(
    IN APPLE_MAILBOX *Mailbox
) {
    UINT32 CpuControl;

    if (Mailbox == NULL) {
        return;
    }

    // Stop the mailbox if it's running.
    CpuControl = MailboxRead32(Mailbox, ASC_CPU_CONTROL_REG);

    if (CpuControl & ASC_CPU_CONTROL_START) {
        CpuControl &= ~ASC_CPU_CONTROL_START;
        MailboxWrite32(Mailbox, ASC_CPU_CONTROL_REG, CpuControl);
    }

    FreePool(Mailbox);
}

EFI_STATUS
EFIAPI
AppleMailboxSendMessage(
    IN APPLE_MAILBOX *Mailbox,
    IN APPLE_MAILBOX_MESSAGE *Message
) {
    if (Mailbox == NULL || Message == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    // Make sure the mailbox is not full.
    if (MailboxRead32(Mailbox, ASC_MBOX_A2I_CONTROL_REG) & ASC_MBOX_CONTROL_FULL) {
        return EFI_NOT_READY;
    }

    // Write the message to the mailbox.
    MailboxWrite64(Mailbox, ASC_MBOX_A2I_SEND0_REG, Message->Message0);
    MailboxWrite64(Mailbox, ASC_MBOX_A2I_SEND1_REG, Message->Message1);

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AppleMailboxReceiveMessage(
    IN APPLE_MAILBOX *Mailbox,
    OUT APPLE_MAILBOX_MESSAGE *Message
) {
    if (Mailbox == NULL || Message == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    // Make sure the mailbox is not empty.
    if (MailboxRead32(Mailbox, ASC_MBOX_I2A_CONTROL_REG) & ASC_MBOX_CONTROL_EMPTY) {
        return EFI_NOT_READY;
    }

    // Read the message from the mailbox.
    Message->Message0 = MailboxRead64(Mailbox, ASC_MBOX_I2A_RECV0_REG);
    Message->Message1 = (UINT32)MailboxRead64(Mailbox, ASC_MBOX_I2A_RECV1_REG);

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AppleMailboxReceiveMessageWithTimeout(
    IN APPLE_MAILBOX *Mailbox,
    OUT APPLE_MAILBOX_MESSAGE *Message,
    IN UINTN Timeout
) {
    EFI_STATUS Status;
    UINTN ElapsedTime;

    if (Mailbox == NULL || Message == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ElapsedTime = 0;
    Status = AppleMailboxReceiveMessage(Mailbox, Message);

    while (EFI_ERROR(Status) && ElapsedTime < Timeout) {
        // Wait up to 100 microseconds and try again until timeout.
        MicroSecondDelay(100);

        ElapsedTime += 100;
        Status = AppleMailboxReceiveMessage(Mailbox, Message);
    }

    return EFI_ERROR(Status) ? EFI_TIMEOUT : EFI_SUCCESS;
}
