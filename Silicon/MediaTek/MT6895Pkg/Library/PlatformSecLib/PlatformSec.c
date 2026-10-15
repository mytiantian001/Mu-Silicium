/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PlatformSecLib.h>

#include "PlatformRegisters.h"

VOID
DisableWatchDogTimer ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR WDogRegion;

  // Locate "WatchDog Timer" Memory Region
  Status = LocateMemoryRegionByName ("WatchDog Timer", &WDogRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Disable WatchDog Timer
  MmioWrite32 (WDogRegion.Address, WDT_MODE_KEY);
}

STATIC
VOID
AdoptOverlay (
  IN UINT32                  OverlayBase,
  IN EFI_PHYSICAL_ADDRESS    FrameBufferAddress,
  IN UINT64                  FrameBufferLength
  )
{
  UINT32 Enabled;
  UINT32 Layer;
  UINT32 Address;
  UINT32 Con;

  Enabled = MmioRead32 (OverlayBase + MTK_OVL_ENABLE);
  for (Layer = 0; Layer < MTK_OVL_LAYER_COUNT; Layer++) {
    if ((Enabled & (1U << Layer)) == 0) {
      continue;
    }

    Address = MmioRead32 (OverlayBase + MTK_OVL_ADDR (Layer));
    if (Address < FrameBufferAddress ||
        Address >= FrameBufferAddress + FrameBufferLength) {
      continue;
    }

    Con = MmioRead32 (OverlayBase + MTK_OVL_CON (Layer));
    Con = (Con & ~MTK_OVL_FORMAT_MASK) | MTK_OVL_RGBA8888 | MTK_OVL_BYTE_SWAP;
    MmioWrite32 (OverlayBase + MTK_OVL_CON (Layer), Con);
    MmioWrite32 (OverlayBase + MTK_OVL_PITCH (Layer), 1088 * 4);
    MmioWrite32 (OverlayBase + MTK_OVL_ADDR (Layer), (UINT32)FrameBufferAddress);
  }
}

/**
  LK may leave an enabled overlay layer with a stale address, pitch or
  pixel format. Repoint the layers that belong to the framebuffer slots so
  the framebuffer console is visible from the SEC stage onwards (the DXE
  display driver runs the same takeover again later).
**/
VOID
AdoptDisplayOverlay ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR FbRegion;

  // Locate "Display Reserved" Memory Region
  Status = LocateMemoryRegionByName ("Display Reserved", &FbRegion);
  if (EFI_ERROR (Status) || FbRegion.Address == 0 || FbRegion.Length == 0) {
    DEBUG ((DEBUG_WARN, "Pearl: \"Display Reserved\" region not found - display stays as-is\n"));
    return;
  }

  // Adopt both overlay blocks feeding the panel
  AdoptOverlay (PEARL_OVL0_BASE, FbRegion.Address, FbRegion.Length);
  AdoptOverlay (PEARL_OVL0_2L_BASE, FbRegion.Address, FbRegion.Length);

  DEBUG ((
    DEBUG_WARN,
    "Pearl: display overlay adopted (FB 0x%lx, %ld bytes)\n",
    (UINT64)FbRegion.Address,
    FbRegion.Length
    ));
}

VOID
PlatformInitialize ()
{
  // Disable WatchDog Timer
  DisableWatchDogTimer ();

  // Adopt the LK display handoff before any firmware output is produced
  AdoptDisplayOverlay ();
}
