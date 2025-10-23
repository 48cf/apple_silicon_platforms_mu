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
#define RTKIT_BUFFER_REQUEST_IOVA GENMASK(43, 0)

typedef enum {
    RTKIT_ENDPOINT_MANAGEMENT = 0,
    RTKIT_ENDPOINT_CRASHLOG = 1,
    RTKIT_ENDPOINT_SYSLOG = 2,
    RTKIT_ENDPOINT_DEBUG = 3,
    RTKIT_ENDPOINT_IOREPORT = 4,
    RTKIT_ENDPOINT_OSLOG = 8,
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

    BOOLEAN Crashed;

    UINT32 IopPowerState;
    UINT32 ApPowerState;

    APPLE_RTKIT_BUFFER SyslogBuffer;
    APPLE_RTKIT_BUFFER CrashlogBuffer;
    APPLE_RTKIT_BUFFER IoreportBuffer;
};

STATIC
VOID
PrepareMessage(
    OUT APPLE_MAILBOX_MESSAGE *MailboxMessage,
    IN RTKIT_ENDPOINT Endpoint,
    IN UINT64 Message
) {
    MailboxMessage->Message0 = Message;
    MailboxMessage->Message1 = Endpoint;
}

STATIC
VOID
PrepareManagementMessage(
    OUT APPLE_MAILBOX_MESSAGE *MailboxMessage,
    IN RTKIT_MANAGEMENT_MESSAGE Type,
    IN UINT64 Message
) {
    UINT64 Payload = 0;

    Payload |= FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, (UINT64)Type);
    Payload |= Message;

    PrepareMessage(MailboxMessage, RTKIT_ENDPOINT_MANAGEMENT, Payload);
}

STATIC
EFI_STATUS
HandleBufferRequest(
    IN APPLE_RTKIT *RtKit,
    IN APPLE_RTKIT_MESSAGE *Message,
    IN OUT APPLE_RTKIT_BUFFER *Buffer
) {
    EFI_STATUS Status;
    APPLE_MAILBOX_MESSAGE Reply;
    UINT64 DeviceAddress;
    UINTN Size;

    DeviceAddress = FIELD_GET(RTKIT_BUFFER_REQUEST_IOVA, Message->Message);
    Size = FIELD_GET(RTKIT_BUFFER_REQUEST_SIZE, Message->Message) << 12; // Size in 4KiB pages

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

    UINT64 ReplyPayload = 0;

    ReplyPayload |= FIELD_PREP(RTKIT_MANAGEMENT_MESSAGE_TYPE, RTKIT_BUFFER_REQUEST);
    ReplyPayload |= FIELD_PREP(RTKIT_BUFFER_REQUEST_IOVA, Buffer->DeviceAddress);
    ReplyPayload |= FIELD_PREP(RTKIT_BUFFER_REQUEST_SIZE, Buffer->Size >> 12);

    PrepareMessage(&Reply, Message->Endpoint, ReplyPayload);

    Status = AppleMailboxSendMessage(RtKit->Mailbox, &Reply);

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "HandleBufferRequest: Failed to send buffer request reply for endpoint %u: %r\n", Message->Endpoint, Status));
        return Status;
    }

    return EFI_SUCCESS;
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
    APPLE_MAILBOX_MESSAGE Message;

    if (RtKit == NULL || RtKit->Mailbox == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    // Send a message to wake up a possibly sleeping IOP.
    PrepareManagementMessage(
        &Message,
        RTKIT_MANAGEMENT_MESSAGE_IOP_POWER_STATE,
        RTKIT_POWER_STATE_ON);

    Status = AppleMailboxSendMessage(RtKit->Mailbox, &Message);

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to send IOP_PWR_STATE message: %r\n", Status));
        return Status;
    }

    // Wait for a reply, we should receive a HELLO message.
    Status = AppleMailboxReceiveMessageWithTimeout(RtKit->Mailbox, &Message, 1000000);

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Timed out waiting for a reply from RTKit\n"));
        return Status;
    }

    if (Message.Message1 != RTKIT_ENDPOINT_MANAGEMENT) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Received message from unexpected endpoint %u\n", Message.Message1));
        return EFI_DEVICE_ERROR;
    }

    UINT8 MessageType = FIELD_GET(RTKIT_MANAGEMENT_MESSAGE_TYPE, Message.Message0);

    if (MessageType != RTKIT_MANAGEMENT_MESSAGE_HELLO) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Expected HELLO message, got %u\n", MessageType));
        return EFI_DEVICE_ERROR;
    }

    // Figure out a common protocol version, we support versions 11 and 12.
    UINT16 MinVersion = FIELD_GET(RTKIT_HELLO_MIN_VERSION, Message.Message0);
    UINT16 MaxVersion = FIELD_GET(RTKIT_HELLO_MAX_VERSION, Message.Message0);

    if (MinVersion > RTKIT_MAX_VERSION || MaxVersion < RTKIT_MIN_VERSION) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: RTKit version incompatible (device supports %u to %u, we support %u to %u)\n",
            MinVersion, MaxVersion, RTKIT_MIN_VERSION, RTKIT_MAX_VERSION));
        return EFI_UNSUPPORTED;
    }

    // Let's pick the highest common version both sides support.
    UINT32 ProtocolVersion = MIN(MaxVersion, RTKIT_MAX_VERSION);

    DEBUG((DEBUG_INFO, "AppleRTKitBoot: Booting RTKit, version %u\n", ProtocolVersion));

    UINT64 HelloReply = 0;

    HelloReply |= FIELD_PREP(RTKIT_HELLO_MIN_VERSION, ProtocolVersion);
    HelloReply |= FIELD_PREP(RTKIT_HELLO_MAX_VERSION, ProtocolVersion);

    PrepareManagementMessage(
        &Message,
        RTKIT_MANAGEMENT_MESSAGE_HELLO_ACK,
        HelloReply);

    Status = AppleMailboxSendMessage(RtKit->Mailbox, &Message);

    if (EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to send HELLO_REPLY message: %r\n", Status));
        return Status;
    }

    // Map needed system endpoints.
    BOOLEAN HasCrashlogEndpoint = FALSE;
    BOOLEAN HasDebugEndpoint = FALSE;
    BOOLEAN HasIoReportEndpoint = FALSE;
    BOOLEAN HasSyslogEndpoint = FALSE;
    BOOLEAN HasOsLogEndpoint = FALSE;
    BOOLEAN HasEpMapDone = FALSE;

    while (!HasEpMapDone) {
        Status = AppleMailboxReceiveMessageWithTimeout(RtKit->Mailbox, &Message, 1000000);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Timed out waiting for endpoint map request\n"));
            return Status;
        }

        if (Message.Message1 != RTKIT_ENDPOINT_MANAGEMENT) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Received message from unexpected endpoint %u\n", Message.Message1));
            return EFI_DEVICE_ERROR;
        }

        UINT8 MessageType = FIELD_GET(RTKIT_MANAGEMENT_MESSAGE_TYPE, Message.Message0);

        if (MessageType != RTKIT_MANAGEMENT_MESSAGE_ENDPOINT_MAP) {
            DEBUG((DEBUG_INFO, "AppleRTKitBoot: Received unexpected message type %u\n", MessageType));
            return EFI_DEVICE_ERROR;
        }

        // Extract the base and bitmap from the map request message.
        UINT32 MapEndpointBase = FIELD_GET(RTKIT_ENDPOINT_MAP_BASE, Message.Message0);
        UINT32 MapEndpointBitmap = FIELD_GET(RTKIT_ENDPOINT_MAP_BITMAP, Message.Message0);

        for (UINT32 Index = 0; Index < 32; Index++) {
            if ((MapEndpointBitmap & (1 << Index)) == 0) {
                continue;
            }

            UINT8 EndpointNumber = (UINT8)((MapEndpointBase << 5) + Index);

            if (EndpointNumber >= 0x20) {
                // Skip application endpoints for now.
                continue;
            }

            switch (EndpointNumber) {
                case RTKIT_ENDPOINT_CRASHLOG:
                    HasCrashlogEndpoint = TRUE;
                    break;
                case RTKIT_ENDPOINT_DEBUG:
                    HasDebugEndpoint = TRUE;
                    break;
                case RTKIT_ENDPOINT_IOREPORT:
                    HasIoReportEndpoint = TRUE;
                    break;
                case RTKIT_ENDPOINT_SYSLOG:
                    HasSyslogEndpoint = TRUE;
                    break;
                case RTKIT_ENDPOINT_OSLOG:
                    HasOsLogEndpoint = TRUE;
                    break;
                case RTKIT_ENDPOINT_MANAGEMENT:
                    // Ignore management endpoint, it's started by default.
                    break;
                default:
                    DEBUG((DEBUG_INFO, "AppleRTKitBoot: Unhandled RTKit endpoint %u\n", EndpointNumber));
                    break;
            }
        }

        if ((Message.Message0 & BIT51) != 0) {
            // This is the last endpoint map message.
            HasEpMapDone = TRUE;
        }

        UINT64 MapEndpointReply = 0;

        MapEndpointReply |= FIELD_PREP(RTKIT_ENDPOINT_MAP_BASE, MapEndpointBase);

        if (HasEpMapDone) {
            MapEndpointReply |= RTKIT_ENDPOINT_MAP_DONE;
        } else {
            MapEndpointReply |= RTKIT_ENDPOINT_MAP_MORE;
        }

        // Send an acknowledgment.
        PrepareManagementMessage(
            &Message,
            RTKIT_MANAGEMENT_MESSAGE_ENDPOINT_MAP_ACK,
            MapEndpointReply);

        Status = AppleMailboxSendMessage(RtKit->Mailbox, &Message);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to send EPMAP_REPLY message: %r\n", Status));
            return Status;
        }
    }

    if (HasDebugEndpoint) {
        Status = AppleRTKitStartEndpoint(RtKit, RTKIT_ENDPOINT_DEBUG);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to start debug endpoint: %r\n", Status));
            return Status;
        }
    }

    if (HasCrashlogEndpoint) {
        Status = AppleRTKitStartEndpoint(RtKit, RTKIT_ENDPOINT_CRASHLOG);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to start crashlog endpoint: %r\n", Status));
            return Status;
        }
    }

    if (HasSyslogEndpoint) {
        Status = AppleRTKitStartEndpoint(RtKit, RTKIT_ENDPOINT_SYSLOG);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to start syslog endpoint: %r\n", Status));
            return Status;
        }
    }

    if (HasIoReportEndpoint) {
        Status = AppleRTKitStartEndpoint(RtKit, RTKIT_ENDPOINT_IOREPORT);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to start ioreport endpoint: %r\n", Status));
            return Status;
        }
    }

    if (HasOsLogEndpoint) {
        Status = AppleRTKitStartEndpoint(RtKit, RTKIT_ENDPOINT_OSLOG);

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to start oslog endpoint: %r\n", Status));
            return Status;
        }
    }

    while (RtKit->IopPowerState != RTKIT_POWER_STATE_ON) {
        APPLE_RTKIT_MESSAGE RtKitMessage;

        // Wait for the IOP to signal it is powered on.
        Status = AppleRTKitReceiveMessage(RtKit, &RtKitMessage);

        if (Status == EFI_SUCCESS) {
            DEBUG((DEBUG_INFO, "AppleRTKitBoot: Received unexpected message on endpoint %u during power on wait\n", RtKitMessage.Endpoint));
            return EFI_DEVICE_ERROR;
        } else if (Status != EFI_NOT_READY) {
            DEBUG((DEBUG_ERROR, "AppleRTKitBoot: Failed to receive power state message: %r\n", Status));
            return Status;
        } else {
            DEBUG((DEBUG_INFO, "AppleRTKitBoot: Waiting for IOP to power on...\n"));
            MicroSecondDelay(10000);
        }
    }

    PrepareManagementMessage(
        &Message,
        RTKIT_MANAGEMENT_MESSAGE_AP_POWER_STATE,
        RTKIT_POWER_STATE_ON);

    Status = AppleMailboxSendMessage(RtKit->Mailbox, &Message);

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
    APPLE_MAILBOX_MESSAGE MailboxMessage;

    if (RtKit == NULL || RtKit->Mailbox == NULL || Message == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ASSERT(!RtKit->Crashed);

    while (TRUE) {
        // Receive a message from the mailbox.
        Status = AppleMailboxReceiveMessage(RtKit->Mailbox, &MailboxMessage);

        if (EFI_ERROR(Status)) {
            return Status;
        }

        // Reject messages from invalid endpoints.
        if (MailboxMessage.Message1 >= 0x100) {
            DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Received message from invalid endpoint %llu\n", MailboxMessage.Message1));
            continue;
        }

        // Populate the output structure.
        Message->Message = MailboxMessage.Message0;
        Message->Endpoint = (UINT8)MailboxMessage.Message1;

        // If the endpoint is an application endpoint, return to the caller.
        if (Message->Endpoint >= 0x20) {
            return EFI_SUCCESS;
        }

        // Extract the message type from the message.
        UINT8 MessageType = (UINT8)(Message->Message >> 52);

        DEBUG((DEBUG_INFO, "AppleRTKitReceiveMessage: Received message from system endpoint %u, type %u\n", Message->Endpoint, MessageType));
        DEBUG((DEBUG_INFO, "AppleRTKitReceiveMessage: Message payload: 0x%016lx\n", Message->Message));

        switch (Message->Endpoint) {
            case RTKIT_ENDPOINT_MANAGEMENT: {
                switch (MessageType) {
                    case RTKIT_MANAGEMENT_MESSAGE_IOP_POWER_STATE_ACK:
                        RtKit->IopPowerState = (UINT32)(Message->Message & 0xFFFF);
                        break;
                    case RTKIT_MANAGEMENT_MESSAGE_AP_POWER_STATE_ACK:
                        RtKit->ApPowerState = (UINT32)(Message->Message & 0xFFFF);
                        break;
                    default:
                        DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Received unknown management message type %u\n", MessageType));
                        break;
                }
                break;
            }
            case RTKIT_ENDPOINT_SYSLOG: {
                switch (MessageType) {
                    case 1:
                        // Buffer request
                        Status = HandleBufferRequest(RtKit, Message, &RtKit->SyslogBuffer);
                        break;
                    case 5:
                        // System log message, unhandled for now but must be acked to avoid issues.
                        Status = AppleMailboxSendMessage(RtKit->Mailbox, &MailboxMessage);
                        break;
                    default:
                        DEBUG((DEBUG_INFO, "AppleRTKitReceiveMessage: Received unknown syslog message type %u\n", MessageType));
                        break;
                }
                break;
            }
            case RTKIT_ENDPOINT_CRASHLOG: {
                switch (MessageType) {
                    case 1:
                        // Buffer request
                        if (RtKit->CrashlogBuffer.DeviceAddress != 0) {
                            ASSERT(FALSE && "RTKit crash!!!");
                        }
                        Status = HandleBufferRequest(RtKit, Message, &RtKit->CrashlogBuffer);
                        break;
                    default:
                        DEBUG((DEBUG_INFO, "AppleRTKitReceiveMessage: Received unknown crashlog message type %u\n", MessageType));
                        break;
                }
                break;
            }
            case RTKIT_ENDPOINT_IOREPORT: {
                switch (MessageType) {
                    case 1:
                        // Buffer request
                        Status = HandleBufferRequest(RtKit, Message, &RtKit->IoreportBuffer);
                        break;
                    case 8:
                    case 12:
                        // Unknown messages but they must be acked to avoid issues.
                        Status = AppleMailboxSendMessage(RtKit->Mailbox, &MailboxMessage);
                        break;
                    default:
                        DEBUG((DEBUG_INFO, "AppleRTKitReceiveMessage: Received unknown ioreport message type %u\n", MessageType));
                        break;
                }
                break;
            }
            case RTKIT_ENDPOINT_OSLOG: {
                DEBUG((DEBUG_INFO, "AppleRTKitReceiveMessage: Received unknown oslog message type %u\n", MessageType));
                break;
            }
            default:
                DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Received message from unknown system endpoint %u\n", Message->Endpoint));
                continue;
        }

        if (EFI_ERROR(Status)) {
            DEBUG((DEBUG_ERROR, "AppleRTKitReceiveMessage: Unable to handle message from endpoint %u: %r\n", Message->Endpoint, Status));
            return Status;
        }
    }

    // No message from application endpoint was received.
    return EFI_NOT_READY;
}

EFI_STATUS
AppleRTKitStartEndpoint(
    IN APPLE_RTKIT *RtKit,
    IN UINT8 Endpoint
) {
    APPLE_MAILBOX_MESSAGE Message;
    UINT64 Payload = 0;

    Payload |= FIELD_PREP(RTKIT_START_ENDPOINT_INDEX, (UINT64)Endpoint);
    Payload |= RTKIT_START_ENDPOINT_FLAG;

    PrepareManagementMessage(
        &Message,
        RTKIT_MANAGEMENT_MESSAGE_START_ENDPOINT,
        Payload);

    return AppleMailboxSendMessage(RtKit->Mailbox, &Message);
}
