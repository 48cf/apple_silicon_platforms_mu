/**
 * Copyright (c) 2023, amarioguy (AppleWOA authors).
 *
 * Module Name:
 *     AppleSARTLib.c
 *
 * Abstract:
 *     SART driver for Apple silicon platforms from Skye (A11) SoCs onwards.
 *     Required to bring up NVMe and permit DMA. Based off of the m1n1 and Linux driver.
 *     Currently the driver only supports Sicily (A14)/Tonga (M1) SoCs and newer.
 *
 * Environment:
 *     UEFI DXE (Driver Execution Environment) and runtime services.
 *
 * License:
 *     SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT OR GPL-2.0-only.
 *
 *     Original m1n1/Linux driver copyright (c) The Asahi Linux Contributors.
 *
*/

#include <Library/AppleSARTLib.h>
#include <Library/ConvenienceMacros.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>

typedef VOID (*APPLE_SART_GET_ENTRY)(
    IN APPLE_SART *Sart,
    IN UINTN Index,
    OUT APPLE_SART_ENTRY *Entry
);

typedef BOOLEAN (*APPLE_SART_SET_ENTRY)(
    IN APPLE_SART *Sart,
    IN UINTN Index,
    IN APPLE_SART_ENTRY *Entry
);

struct _APPLE_SART {
    UINT64 BaseAddress;
    UINT32 ProtectedEntries;

    APPLE_SART_GET_ENTRY GetEntry;
    APPLE_SART_SET_ENTRY SetEntry;
};

#define APPLE_SART_MAX_ENTRIES 16

// Definitions that apply to both SART v2 or v3.
#define APPLE_SART_V2_V3_CONFIG(index) (0x0 + 4 * (index))
#define APPLE_SART_V2_V3_SIZE_SHIFT 12

// SART v2 specific definitions.
#define APPLE_SART_V2_CONFIG_FLAGS GENMASK(31, 24)
#define APPLE_SART_V2_PHYS_ADDR(index) (0x40 + 4 * (index))
#define APPLE_SART_V2_PHYS_ADDR_SHIFT APPLE_SART_V2_V3_SIZE_SHIFT
#define APPLE_SART_V2_CONFIG_SIZE_SHIFT APPLE_SART_V2_V3_SIZE_SHIFT
#define APPLE_SART_V2_CONFIG_SIZE GENMASK(23, 0)
#define APPLE_SART_V2_CONFIG_SIZE_MAX APPLE_SART_V2_CONFIG_SIZE

// SART v3 specific definitions.
#define APPLE_SART_V3_PHYS_ADDR(index) (0x40 + 4 * (index))
#define APPLE_SART_V3_PHYS_ADDR_SHIFT APPLE_SART_V2_V3_SIZE_SHIFT
#define APPLE_SART_V3_MAX_SIZE GENMASK(29, 0)
#define APPLE_SART_V3_SIZE(index) (0x80 + 4 * (index))

STATIC
VOID
SARTV2GetEntry(
    IN APPLE_SART *Sart,
    IN UINTN Index,
    OUT APPLE_SART_ENTRY *Entry
) {
    UINT32 Config;
    UINT32 Address;

    Config = MmioRead32(Sart->BaseAddress + APPLE_SART_V2_V3_CONFIG(Index));
    Address = MmioRead32(Sart->BaseAddress + APPLE_SART_V2_PHYS_ADDR(Index));

    Entry->Address = (UINT64)(Address) << APPLE_SART_V2_PHYS_ADDR_SHIFT;
    Entry->Size = (UINT64)(FIELD_GET(APPLE_SART_V2_CONFIG_SIZE, Config)) << APPLE_SART_V2_CONFIG_SIZE_SHIFT;
    Entry->Flags = FIELD_GET(APPLE_SART_V2_CONFIG_FLAGS, Config);
}

STATIC
BOOLEAN
SARTV2SetEntry(
    IN APPLE_SART *Sart,
    IN UINTN Index,
    IN APPLE_SART_ENTRY *Entry
) {
    UINT32 Config;
    UINT32 Address;
    UINT32 Size;

    if ((Entry->Address & ((1 << APPLE_SART_V2_PHYS_ADDR_SHIFT) - 1)) != 0) {
        DEBUG((DEBUG_ERROR, "SARTV2SetEntry: Address 0x%lx is not properly aligned.\n", Entry->Address));
        return FALSE;
    }

    if ((Entry->Size & ((1 << APPLE_SART_V2_CONFIG_SIZE_SHIFT) - 1)) != 0) {
        DEBUG((DEBUG_ERROR, "SARTV2SetEntry: Size 0x%lx is not properly aligned.\n", Entry->Size));
        return FALSE;
    }

    Config = 0;
    Address = (UINT32)(Entry->Address >> APPLE_SART_V2_PHYS_ADDR_SHIFT);
    Size = (UINT32)(Entry->Size >> APPLE_SART_V2_CONFIG_SIZE_SHIFT);

    if (Size > APPLE_SART_V2_CONFIG_SIZE_MAX) {
        DEBUG((DEBUG_ERROR, "SARTV2SetEntry: Size 0x%lx exceeds maximum allowed size.\n", Entry->Size));
        return FALSE;
    }

    Config |= FIELD_PREP(APPLE_SART_V2_CONFIG_FLAGS, Entry->Flags);
    Config |= FIELD_PREP(APPLE_SART_V2_CONFIG_SIZE, Size);

    MmioWrite32(Sart->BaseAddress + APPLE_SART_V2_PHYS_ADDR(Index), Address);
    MmioWrite32(Sart->BaseAddress + APPLE_SART_V2_V3_CONFIG(Index), Config);

    return TRUE;
}

STATIC
VOID
SARTV3GetEntry(
    IN APPLE_SART *Sart,
    IN UINTN Index,
    OUT APPLE_SART_ENTRY *Entry
) {
    UINT32 Config;
    UINT32 Address;
    UINT32 Size;

    Config = MmioRead32(Sart->BaseAddress + APPLE_SART_V2_V3_CONFIG(Index));
    Address = MmioRead32(Sart->BaseAddress + APPLE_SART_V3_PHYS_ADDR(Index));
    Size = MmioRead32(Sart->BaseAddress + APPLE_SART_V3_SIZE(Index));

    Entry->Address = (UINT64)(Address) << APPLE_SART_V3_PHYS_ADDR_SHIFT;
    Entry->Size = (UINT64)(Size) << APPLE_SART_V2_V3_SIZE_SHIFT;
    Entry->Flags = Config;
}

STATIC
BOOLEAN
SARTV3SetEntry(
    IN APPLE_SART *Sart,
    IN UINTN Index,
    IN APPLE_SART_ENTRY *Entry
) {
    UINT32 Config;
    UINT32 Address;
    UINT32 Size;

    if ((Entry->Address & ((1 << APPLE_SART_V3_PHYS_ADDR_SHIFT) - 1)) != 0) {
        DEBUG((DEBUG_ERROR, "SARTV3GetEntry: Address 0x%lx is not properly aligned.\n", Entry->Address));
        return FALSE;
    }

    if ((Entry->Size & ((1 << APPLE_SART_V2_V3_SIZE_SHIFT) - 1)) != 0) {
        DEBUG((DEBUG_ERROR, "SARTV3GetEntry: Size 0x%lx is not properly aligned.\n", Entry->Size));
        return FALSE;
    }

    Config = Entry->Flags;
    Address = (UINT32)(Entry->Address >> APPLE_SART_V3_PHYS_ADDR_SHIFT);
    Size = (UINT32)(Entry->Size >> APPLE_SART_V2_V3_SIZE_SHIFT);

    if (Size > APPLE_SART_V3_MAX_SIZE) {
        DEBUG((DEBUG_ERROR, "SARTV3SetEntry: Size 0x%lx exceeds maximum allowed size.\n", Entry->Size));
        return FALSE;
    }

    MmioWrite32(Sart->BaseAddress + APPLE_SART_V3_PHYS_ADDR(Index), Address);
    MmioWrite32(Sart->BaseAddress + APPLE_SART_V3_SIZE(Index), Size);
    MmioWrite32(Sart->BaseAddress + APPLE_SART_V2_V3_CONFIG(Index), Config);

    return TRUE;
}

APPLE_SART *
EFIAPI
AppleSARTInitialize(
    IN UINT64 BaseAddress
) {
    APPLE_SART *Sart;

    Sart = AllocateZeroPool(sizeof(APPLE_SART));

    if (Sart == NULL) {
        return NULL;
    }

    switch (FixedPcdGet8(PcdAppleSartVersion)) {
        case 2:
            DEBUG((DEBUG_INFO, "AppleSARTInitialize: Initializing SARTv2 at 0x%llx.\n", BaseAddress));
            Sart->GetEntry = SARTV2GetEntry;
            Sart->SetEntry = SARTV2SetEntry;
            break;
        case 3:
            DEBUG((DEBUG_INFO, "AppleSARTInitialize: Initializing SARTv3 at 0x%llx.\n", BaseAddress));
            Sart->GetEntry = SARTV3GetEntry;
            Sart->SetEntry = SARTV3SetEntry;
            break;
        default:
            DEBUG((DEBUG_INFO, "AppleSARTInitialize: Unknown SART version %d.\n", FixedPcdGet8(PcdAppleSartVersion)));
            FreePool(Sart);
            return NULL;
    }

    Sart->BaseAddress = BaseAddress;
    Sart->ProtectedEntries = 0;

    // Find out which entries were initialized prior to us and mark them as protected.
    for (UINTN Index = 0; Index < APPLE_SART_MAX_ENTRIES; Index++) {
        APPLE_SART_ENTRY Entry;

        Sart->GetEntry(Sart, Index, &Entry);

        if (Entry.Flags != 0) {
            Sart->ProtectedEntries |= BIT(Index);
        }
    }

    return Sart;
}

VOID
EFIAPI
AppleSARTDeinitialize(
    IN APPLE_SART *Sart
) {
    if (Sart == NULL) {
        return;
    }

    // Clear all non-protected entries.
    for (UINTN Index = 0; Index < APPLE_SART_MAX_ENTRIES; Index++) {
        if ((Sart->ProtectedEntries & BIT(Index)) == 0) {
            APPLE_SART_ENTRY Entry;

            Entry.Address = 0;
            Entry.Size = 0;
            Entry.Flags = 0;

            if (!Sart->SetEntry(Sart, Index, &Entry)) {
                DEBUG((DEBUG_INFO, "AppleSARTDeinitialize: Failed to clear SART entry %d.\n", Index));
            }
        }
    }

    FreePool(Sart);
}

EFI_STATUS
EFIAPI
AppleSARTAddEntry(
    IN APPLE_SART *Sart,
    IN APPLE_SART_ENTRY *Entry
) {
    UINTN Index;

    // Find a free entry.
    for (Index = 0; Index < APPLE_SART_MAX_ENTRIES; Index++) {
        if ((Sart->ProtectedEntries & BIT(Index)) == 0) {
            break;
        }
    }

    if (Index == APPLE_SART_MAX_ENTRIES) {
        DEBUG((DEBUG_INFO, "AppleSARTAddEntry: No free SART entries available.\n"));
        return EFI_OUT_OF_RESOURCES;
    }

    // Set the entry.
    if (!Sart->SetEntry(Sart, Index, Entry)) {
        DEBUG((DEBUG_INFO, "AppleSARTAddEntry: Failed to set SART entry %d.\n", Index));
        return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AppleSARTRemoveEntry(
    IN APPLE_SART *Sart,
    IN APPLE_SART_ENTRY *Entry
) {
    UINTN Index;
    APPLE_SART_ENTRY CurrentEntry;

    // Find the entry.
    for (Index = 0; Index < APPLE_SART_MAX_ENTRIES; Index++) {
        Sart->GetEntry(Sart, Index, &CurrentEntry);

        if ((CurrentEntry.Address == Entry->Address) &&
            (CurrentEntry.Size == Entry->Size) &&
            (CurrentEntry.Flags == Entry->Flags)) {
            break;
        }
    }

    if (Index == APPLE_SART_MAX_ENTRIES) {
        DEBUG((DEBUG_INFO, "AppleSARTRemoveEntry: SART entry not found.\n"));
        return EFI_NOT_FOUND;
    }

    // Clear the entry.
    CurrentEntry.Address = 0;
    CurrentEntry.Size = 0;
    CurrentEntry.Flags = 0;

    EFI_STATUS Status = Sart->SetEntry(Sart, Index, &CurrentEntry);

    // This should never fail.
    ASSERT(Status == EFI_SUCCESS);

    return EFI_SUCCESS;
}
