/**
 *  Copyright (c) 2025, AppleWOA authors. All rights reserved.
 *
 *  Module Name:
 *    AppleRTKitLib.inf
 *
 *  Abstract:
 *     Simple library to interface with the Apple RTKit firmware.
 *
 *  Environment:
 *     UEFI DXE (Driver Execution Environment) and runtime services.
 *
 *  License:
 *    SPDX-License-Identifier: MIT
 *
 */

// This code is heavily inspired by the m1n1 rtkit implementation.

#include <Library/AppleRTKitLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>

#define RTKIT_MIN_VERSION 11
#define RTKIT_MAX_VERSION 12

#define RTKIT_MANAGEMENT_MESSAGE_TYPE GENMASK(59, 52)
#define RTKIT_OSLOG_MESSAGE_TYPE GENMASK(63, 56)

#define RTKIT_HELLO_MIN_VERSION GENMASK(15, 0)
#define RTKIT_HELLO_MAX_VERSION GENMASK(31, 16)

#define RTKIT_ENDPOINT_MAP_MORE BIT0
#define RTKIT_ENDPOINT_MAP_DONE BIT51
#define RTKIT_ENDPOINT_MAP_BASE GENMASK(34, 32)
#define RTKIT_ENDPOINT_MAP_BITMAP GENMASK(31, 0)

#define RTKIT_START_ENDPOINT_FLAG BIT1
#define RTKIT_START_ENDPOINT_INDEX GENMASK(39, 32)

#define RTKIT_BUFFER_REQUEST 1
#define RTKIT_BUFFER_REQUEST_SIZE GENMASK(51, 44)
#define RTKIT_BUFFER_REQUEST_IOVA GENMASK(41, 0)

#define RTKIT_OSLOG_BUFFER_REQUEST_SIZE GENMASK(55, 36)
#define RTKIT_OSLOG_BUFFER_REQUEST_IOVA GENMASK(35, 0)


typedef enum {
    RTKIT_ENDPOINT_MANAGEMENT = 0,
    RTKIT_ENDPOINT_CRASHLOG = 1,
    RTKIT_ENDPOINT_SYSLOG = 2,
    RTKIT_ENDPOINT_DEBUG = 3,
    RTKIT_ENDPOINT_IOREPORT = 4,
    RTKIT_ENDPOINT_OSLOG = 8,
    RTKIT_ENDPOINT_TRACEKIT = 10,
} RTKIT_ENDPOINT;

typedef enum {
    RTKIT_MANAGEMENT_MESSAGE_HELLO = 1,
    RTKIT_MANAGEMENT_MESSAGE_HELLO_ACK = 2,
    RTKIT_MANAGEMENT_MESSAGE_START_ENDPOINT = 5,
    RTKIT_MANAGEMENT_MESSAGE_IOP_POWER_STATE = 6,
    RTKIT_MANAGEMENT_MESSAGE_IOP_POWER_STATE_ACK = 7,
    RTKIT_MANAGEMENT_MESSAGE_ENDPOINT_MAP = 8,
    RTKIT_MANAGEMENT_MESSAGE_ENDPOINT_MAP_ACK = 8,
    RTKIT_MANAGEMENT_MESSAGE_AP_POWER_STATE = 11,
    RTKIT_MANAGEMENT_MESSAGE_AP_POWER_STATE_ACK = 11,
} RTKIT_MANAGEMENT_MESSAGE;

typedef enum {
    RTKIT_POWER_STATE_OFF = 0x00,
    RTKIT_POWER_STATE_SLEEP = 0x01,
    RTKIT_POWER_STATE_QUIESCED = 0x10,
    RTKIT_POWER_STATE_ON = 0x20,
    RTKIT_POWER_STATE_INIT = 0x220,
} RTKIT_POWER_STATE;

struct _APPLE_RTKIT {
    APPLE_MAILBOX *Mailbox;
    APPLE_RTKIT_SHMEM_SETUP ShmemSetup;
    APPLE_RTKIT_SHMEM_DESTROY ShmemDestroy;

    VOID *ShmemContext;

    UINT64 Endpoints[4]; // 4 * 64 = 256
    BOOLEAN Crashed;

    UINT32 IopPowerState;
    UINT32 ApPowerState;

    APPLE_RTKIT_BUFFER SyslogBuffer;
    APPLE_RTKIT_BUFFER CrashlogBuffer;
    APPLE_RTKIT_BUFFER IoreportBuffer;
};

STATIC
EFI_STATUS
RTKitSendMessage(
    IN APPLE_RTKIT *RtKit,
    IN RTKIT_ENDPOINT Endpoint,
    IN UINT64 Message
) {
    APPLE_MAILBOX_MESSAGE MailboxMessage;

    MailboxMessage.Message0 = Message;
    MailboxMessage.Message1 = Endpoint;

    return AppleMailboxSendMessage(RtKit->Mailbox, &MailboxMessage);
}

STATIC
EFI_STATUS
RTKitReceiveMessage(
    IN APPLE_RTKIT *RtKit,
    OUT APPLE_RTKIT_MESSAGE *Message,
    IN UINTN Timeout
) {
    EFI_STATUS Status;
    APPLE_MAILBOX_MESSAGE MailboxMessage;

    Status = AppleMailboxReceiveMessageWithTimeout(RtKit->Mailbox, &MailboxMessage, Timeout);

    if (Status == EFI_SUCCESS) {
        Message->Message = MailboxMessage.Message0;
        Message->Endpoint = MailboxMessage.Message1;
    }

    return Status;
}

STATIC
EFI_STATUS
HandleBufferRequest(
    IN APPLE_RTKIT *RtKit,
    IN APPLE_RTKIT_MESSAGE *Message,
    IN OUT APPLE_RTKIT_BUFFER *Buffer
) {
    EFI_STATUS Status;

    UINT64 DeviceAddress;
    UINT32 Size;

    if (Message->Endpoint == RTKIT_ENDPOINT_OSLOG) {
        DeviceAddress = FIELD_GET(RTKIT_OSLOG_BUFFER_REQUEST_IOVA, Message->Message) << 12;
        Size = FIELD_GET(RTKIT_OSLOG_BUFFER_REQUEST_SIZE, Message->Message);
    } else {
        DeviceAddress = FIELD_GET(RTKIT_BUFFER_REQUEST_IOVA, Message->Message);
        Size = FIELD_GET(RTKIT_BUFFER_REQUEST_SIZE, Message->Message) << 12;
    }

    Buffer->Endpoint = Message->Endpoint;
    Buffer->DeviceAddress = DeviceAddress;
    Buffer->Size = Size;

    if (RtKit->ShmemSetup != NULL) {
        Status = RtKit->ShmemSetup(RtKit, Buffer, RtKit->ShmemContext);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "HandleBufferRequest: Shared memory setup failed: %r\n", Status));
            return Status;
        }
    } else {
        DEBUG((DEBUG_ERROR, "HandleBufferRequest: No shared memory setup function provided\n"));
        return EFI_UNSUPPORTED;
    }

    UINT64 ReplyMessage;

    if (Message->Endpoint == RTKIT_ENDPOINT_OSLOG) {
        ReplyMessage =
            FIELD_PREP(RTKIT_OSLOG_MESSAGE_TYPE, RTKIT_BUFFER_REQUEST) |
            FIELD_PREP(RTKIT_OSLOG_BUFFER_REQUEST_IOVA, Buffer->DeviceAddress) |
            FIELD_PREP(RTKIT_OSLOG_BUFFER_REQUEST_SIZE, Buffer->Size >> 12);
    } else {
        ReplyMessage =
            FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, RTKIT_BUFFER_REQUEST) |
            FIELD_PREP(RTKIT_BUFFER_REQUEST_IOVA, Buffer->DeviceAddress) |
            FIELD_PREP(RTKIT_BUFFER_REQUEST_SIZE, Buffer->Size >> 12);
    }

    Status = RTKitSendMessage(RtKit, Message->Endpoint, ReplyMessage);

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "HandleBufferRequest: Failed to send buffer request reply: %r\n", Status));
        return Status;
    }

    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
HandleManagementMessage(
    IN APPLE_RTKIT *RtKit,
    IN APPLE_RTKIT_MESSAGE *Message
) {
    UINT8 MessageType = FIELD_GET(RTKIT_MANAGEMENT_MESSAGE_TYPE, Message->Message);

    if (MessageType == RTKIT_MANAGEMENT_MESSAGE_HELLO) {
        UINT16 MinVersion = FIELD_GET(RTKIT_HELLO_MIN_VERSION, Message->Message);
        UINT16 MaxVersion = FIELD_GET(RTKIT_HELLO_MAX_VERSION, Message->Message);

        if (MinVersion > RTKIT_MAX_VERSION || MaxVersion < RTKIT_MIN_VERSION) {
            DEBUG((DEBUG_ERROR, "HandleManagementMessage: Incompatible RTKit version: min %u, max %u\n", MinVersion, MaxVersion));
            return EFI_UNSUPPORTED;
        }

        UINT16 Version = MIN(MaxVersion, RTKIT_MAX_VERSION);
        UINT64 ReplyMessage =
            FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, RTKIT_MANAGEMENT_MESSAGE_HELLO_ACK) |
            FIELD_PREP(RTKIT_HELLO_MIN_VERSION, Version) |
            FIELD_PREP(RTKIT_HELLO_MAX_VERSION, Version);

        return RTKitSendMessage(RtKit, RTKIT_ENDPOINT_MANAGEMENT, ReplyMessage);
    } else if (MessageType == RTKIT_MANAGEMENT_MESSAGE_ENDPOINT_MAP) {
        UINT32 Bitmap = FIELD_GET(RTKIT_ENDPOINT_MAP_BITMAP, Message->Message);
        UINT32 Base = FIELD_GET(RTKIT_ENDPOINT_MAP_BASE, Message->Message);
        BOOLEAN IsLast = (Message->Message & RTKIT_ENDPOINT_MAP_DONE) != 0;

        for (UINT32 Index = 0; Index < 32; Index++) {
            if ((Bitmap & BIT(Index)) == 0) {
                continue;
            }

            UINT32 EndpointIndex = Base * 32 + Index;

            RtKit->Endpoints[EndpointIndex / 64] |= BIT(EndpointIndex % 64);
        }

        UINT64 ReplyMessage =
            FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, RTKIT_MANAGEMENT_MESSAGE_ENDPOINT_MAP_ACK) |
            FIELD_PREP(RTKIT_ENDPOINT_MAP_BASE, Base) |
            (IsLast ? RTKIT_ENDPOINT_MAP_DONE : BIT0);
        
        EFI_STATUS Status = RTKitSendMessage(RtKit, RTKIT_ENDPOINT_MANAGEMENT, ReplyMessage);

        if (EFI_ERROR(Status)) {
            return Status;
        }

        if (!IsLast) {
            return EFI_SUCCESS;
        }

        for (UINT32 Endpoint = 0; Endpoint < 32; Endpoint++) {
            if (Endpoint != RTKIT_ENDPOINT_MANAGEMENT && (RtKit->Endpoints[Endpoint / 64] & BIT(Endpoint % 64)) == 0) {
                continue;
            }

            Status = AppleRTKitStartEndpoint(RtKit, Endpoint);

            if (EFI_ERROR(Status)) {
                DEBUG((DEBUG_ERROR, "HandleManagementMessage: Failed to start endpoint %u: %r\n", Endpoint, Status));
                return Status;
            }
        }

        return EFI_SUCCESS;
    } else if (MessageType == RTKIT_MANAGEMENT_MESSAGE_IOP_POWER_STATE_ACK) {
        UINT32 PowerState = Message->Message & 0xFFFF;

        RtKit->IopPowerState = PowerState;

        DEBUG((DEBUG_INFO, "HandleManagementMessage: IOP power state changed to %u\n", PowerState));

        return EFI_SUCCESS;
    } else if (MessageType == RTKIT_MANAGEMENT_MESSAGE_AP_POWER_STATE_ACK) {
        UINT32 PowerState = Message->Message & 0xFFFF;

        RtKit->ApPowerState = PowerState;

        DEBUG((DEBUG_INFO, "HandleManagementMessage: AP power state changed to %u\n", PowerState));

        return EFI_SUCCESS;
    }

    DEBUG((DEBUG_ERROR, "HandleManagementMessage: Unknown management message type %u\n", MessageType));

    return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
HandleCrashlogMessage(
    IN APPLE_RTKIT *RtKit,
    IN APPLE_RTKIT_MESSAGE *Message
) {
    UINT32 MessageType = FIELD_GET(RTKIT_MANAGEMENT_MESSAGE_TYPE, Message->Message);

    if (MessageType != RTKIT_BUFFER_REQUEST) {
        DEBUG((DEBUG_ERROR, "HandleCrashlogMessage: Unknown crashlog message type %u\n", MessageType));
        return EFI_UNSUPPORTED;
    }

    if (RtKit->CrashlogBuffer.Size == 0) {
        return HandleBufferRequest(RtKit, Message, &RtKit->CrashlogBuffer);
    }

    RtKit->Crashed = TRUE;

    // TODO: Implement dumping of the crash log buffer.
    DEBUG((DEBUG_ERROR, "HandleCrashlogMessage: RTKit has crashed :(\n"));

    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
HandleSyslogMessage(
    IN APPLE_RTKIT *RtKit,
    IN APPLE_RTKIT_MESSAGE *Message
) {
    UINT32 MessageType = FIELD_GET(RTKIT_MANAGEMENT_MESSAGE_TYPE, Message->Message);

    if (MessageType == RTKIT_BUFFER_REQUEST) {
        return HandleBufferRequest(RtKit, Message, &RtKit->SyslogBuffer);
    } else if (MessageType == 5) {
        // System log message, unhandled for now but must be acked to avoid issues.
        return RTKitSendMessage(RtKit, RTKIT_ENDPOINT_SYSLOG, Message->Message);
    }

    DEBUG((DEBUG_ERROR, "HandleSyslogMessage: Unknown syslog message type %u\n", MessageType));

    return EFI_UNSUPPORTED;
}

APPLE_RTKIT *
EFIAPI
AppleRTKitInitialize(
    IN APPLE_MAILBOX *Mailbox,
    IN APPLE_RTKIT_SHMEM_SETUP ShmemSetup,
    IN APPLE_RTKIT_SHMEM_DESTROY ShmemDestroy,
    IN VOID *ShmemContext
) {
    APPLE_RTKIT *RtKit;

    RtKit = AllocateZeroPool(sizeof(APPLE_RTKIT));

    if (RtKit == NULL) {
        DEBUG((DEBUG_ERROR, "AppleRTKitInitialize: Failed to allocate memory for the RTKit structure\n"));
        return NULL;
    }

    RtKit->Mailbox = Mailbox;
    RtKit->ShmemSetup = ShmemSetup;
    RtKit->ShmemDestroy = ShmemDestroy;
    RtKit->ShmemContext = ShmemContext;

    return RtKit;
}

VOID
EFIAPI
AppleRTKitDeinitialize(
    IN APPLE_RTKIT *RtKit
) {
    // TODO: Free shared memory buffers.

    if (RtKit != NULL) {
        FreePool(RtKit);
    }
}

EFI_STATUS
EFIAPI
AppleRTKitBoot(
    IN APPLE_RTKIT *RtKit
) {
    EFI_STATUS Status;

    if (RtKit == NULL || RtKit->Mailbox == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    RtKit->IopPowerState = RTKIT_POWER_STATE_SLEEP;
    RtKit->ApPowerState = RTKIT_POWER_STATE_QUIESCED;

    Status = RTKitSendMessage(RtKit, RTKIT_ENDPOINT_MANAGEMENT,
        FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, RTKIT_MANAGEMENT_MESSAGE_IOP_POWER_STATE) |
        RTKIT_POWER_STATE_ON);

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to send IOP_POWER_STATE message: %r\n", Status));
        return Status;
    }

    // Wait for the RTKit to transition to the power on state.
    while (RtKit->IopPowerState != RTKIT_POWER_STATE_ON) {
        APPLE_RTKIT_MESSAGE RtKitMessage;

        // Wait for the IOP to signal it is powered on.
        Status = AppleRTKitReceiveMessage(RtKit, &RtKitMessage);

        if (Status == EFI_SUCCESS) {
            DEBUG((DEBUG_INFO, "AppleRTKitBoot: Received unexpected message on endpoint %u during power on wait\n", RtKitMessage.Endpoint));
            return EFI_DEVICE_ERROR;
        } else if (Status != EFI_TIMEOUT) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to receive power state message: %r\n", Status));
            return Status;
        } else {
            MicroSecondDelay(100000);
        }
    }

    Status = RTKitSendMessage(RtKit, RTKIT_ENDPOINT_MANAGEMENT,
         FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, RTKIT_MANAGEMENT_MESSAGE_AP_POWER_STATE) |
         RTKIT_POWER_STATE_ON);

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to send AP_PWR_STATE message: %r\n", Status));
        return Status;
    }

    DEBUG((DEBUG_INFO, "AppleRTKitBoot: RTKit booted successfully\n"));

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AppleRTKitReceiveMessage(
    IN APPLE_RTKIT *RtKit,
    OUT APPLE_RTKIT_MESSAGE *Message
) {
    EFI_STATUS Status;

    if (RtKit == NULL || RtKit->Mailbox == NULL || Message == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ASSERT(!RtKit->Crashed);

    while (TRUE) {
        Status = RTKitReceiveMessage(RtKit, Message, 100000);

        if (EFI_ERROR(Status)) {
            return Status;
        }

        // Reject messages from invalid endpoints.
        if (Message->Endpoint >= 0x100) {
            DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Received message from invalid endpoint %llu\n", Message->Endpoint));
            continue;
        }

        // If the endpoint is an application endpoint, return to the caller.
        if (Message->Endpoint >= 0x20) {
            return EFI_SUCCESS;
        }

        DEBUG((DEBUG_INFO, "AppleRTKitReceiveMessage: Received message from system endpoint %u: 0x%016llx\n", Message->Endpoint, Message->Message));

        if (Message->Endpoint == RTKIT_ENDPOINT_MANAGEMENT) {
            Status = HandleManagementMessage(RtKit, Message);
        } else if (Message->Endpoint == RTKIT_ENDPOINT_SYSLOG) {
            Status = HandleSyslogMessage(RtKit, Message);
        } else if (Message->Endpoint == RTKIT_ENDPOINT_CRASHLOG) {
            Status = HandleCrashlogMessage(RtKit, Message);
        } else if (Message->Endpoint == RTKIT_ENDPOINT_IOREPORT) {
            UINT8 MessageType = FIELD_GET(RTKIT_MANAGEMENT_MESSAGE_TYPE, Message->Message);

            if (MessageType == RTKIT_BUFFER_REQUEST) {
                Status = HandleBufferRequest(RtKit, Message, &RtKit->IoreportBuffer);
            } else if (MessageType == 8 || MessageType == 12) {
                // Unknown messages but they must be acked to avoid issues.
                Status = RTKitSendMessage(RtKit, RTKIT_ENDPOINT_IOREPORT, Message->Message);
            } else {
                DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Received unknown ioreport message type %u\n", MessageType));
                Status = EFI_UNSUPPORTED;
            }
        } else if (Message->Endpoint == RTKIT_ENDPOINT_OSLOG) {
            UINT8 MessageType = FIELD_GET(RTKIT_OSLOG_MESSAGE_TYPE, Message->Message);

            if (MessageType == RTKIT_BUFFER_REQUEST) {
                Status = HandleBufferRequest(RtKit, Message, &RtKit->SyslogBuffer);
            } else {
                DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Received unknown oslog message type %u\n", MessageType));
                Status = EFI_UNSUPPORTED;
            }
        } else {
            DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Received message from unknown system endpoint %u\n", Message->Endpoint));
            continue;
        }

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Unable to handle message from endpoint %u: %r\n", Message->Endpoint, Status));
            return Status;
        }
    }

    // No message from application endpoint was received.
    return EFI_TIMEOUT;
}

EFI_STATUS
AppleRTKitStartEndpoint(
    IN APPLE_RTKIT *RtKit,
    IN UINT8 Endpoint
) {
    return RTKitSendMessage(RtKit, RTKIT_ENDPOINT_MANAGEMENT,
        FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, RTKIT_MANAGEMENT_MESSAGE_START_ENDPOINT) |
        FIELD_PREP(RTKIT_START_ENDPOINT_INDEX, Endpoint) |
        RTKIT_START_ENDPOINT_FLAG);
}
