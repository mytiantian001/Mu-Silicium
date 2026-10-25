/** @file
  Mask every GIC interrupt source and disarm both EL1 architected timers just
  before ExitBootServices, so the OS inherits a CPU with nothing armed.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>

#define GICD_TYPER      0x004
#define GICD_ICENABLER  0x180
#define GICD_ICPENDR    0x280
#define GICD_ICACTIVER  0x380
// The SGI/PPI bank sits in the 64 KiB SGI_base frame after RD_base; without
// this offset the three writes below land on reserved addresses and mask
// nothing at all.
#define GICR_SGI_FRAME  0x10000
#define GICR_ICENABLER  (GICR_SGI_FRAME + 0x180)
#define GICR_ICPENDR    (GICR_SGI_FRAME + 0x280)
#define GICR_ICACTIVER  (GICR_SGI_FRAME + 0x380)

#define GIC_MAX_BANKS   32

STATIC
UINTN
GicBase (
  IN CHAR8  *Name
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;

  if (EFI_ERROR (LocateMemoryRegionByName (Name, &Region))) {
    return 0;
  }

  return (UINTN)Region.Address;
}

STATIC
VOID
QuiesceGic (
  VOID
  )
{
  UINTN   Gicd;
  UINTN   Gicr;
  UINT32  Banks;
  UINT32  Index;

  Gicd  = GicBase ("GIC Distributor");
  Banks = ((MmioRead32 (Gicd + GICD_TYPER) & 0x1F) + 1);
  if (Banks > GIC_MAX_BANKS) {
    Banks = GIC_MAX_BANKS;
  }

  for (Index = 0; Index < Banks; Index++) {
    MmioWrite32 (Gicd + GICD_ICENABLER + (Index * 4), 0xFFFFFFFF);
    MmioWrite32 (Gicd + GICD_ICPENDR + (Index * 4), 0xFFFFFFFF);
    MmioWrite32 (Gicd + GICD_ICACTIVER + (Index * 4), 0xFFFFFFFF);
  }

  // Frame 0 carries the SGI/PPI bank, which is where the four architected
  // timer interrupts named by GTDT live.
  Gicr = GicBase ("GIC Redistributors");
  MmioWrite32 (Gicr + GICR_ICENABLER, 0xFFFFFFFF);
  MmioWrite32 (Gicr + GICR_ICPENDR, 0xFFFFFFFF);
  MmioWrite32 (Gicr + GICR_ICACTIVER, 0xFFFFFFFF);
}
/**
  Drop a marker file on every FAT volume.  A framebuffer line can be left over
  from a previous boot, a file cannot.
**/
STATIC
VOID
WriteEbsMarker (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINT8                            Text[] = "EBS reached\r\n";
  UINTN                            Size;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Root->Open (
                     Root,
                     &File,
                     L"\\EBS.TXT",
                     EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ,
                     0
                     );
    if (!EFI_ERROR (Status)) {
      Size = sizeof (Text) - 1;
      File->Write (File, &Size, Text);
      File->Close (File);
    }

    Root->Close (Root);
  }

  FreePool (Handles);
}

STATIC
VOID
EFIAPI
QuiesceOnBeforeExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DEBUG ((DEBUG_ERROR, "QUIESCE: EBS event fired, masking GIC and timers\n"));
  WriteEbsMarker ();

  ArmWriteCntpCtl (ARM_ARCH_TIMER_IMASK);
  ArmWriteCntpCval (0x7FFFFFFFFFFFFFFFULL);
  ArmWriteCntvCtl (ARM_ARCH_TIMER_IMASK);
  ArmWriteCntvCval (0x7FFFFFFFFFFFFFFFULL);
  QuiesceGic ();
}

EFI_STATUS
EFIAPI
HandoffQuiesceEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;
  EFI_STATUS Status;

  DEBUG ((DEBUG_ERROR, "QUIESCE: running on mpidr=0x%lx\n", ArmReadMpidr ()));

  Event  = NULL;
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  QuiesceOnBeforeExitBootServices,
                  NULL,
                  &gEfiEventBeforeExitBootServicesGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "QUIESCE: event registration %r\n", Status));
  }

  return Status;
}
