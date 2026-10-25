/** @file
  Platform boot options for the Xiaomi Redmi Note 12T Pro (pearl).

  Exposes the UEFI Shell as a boot option so the UFS stack, the FAT driver
  and the Windows PE boot files can be inspected from the Shell when the
  Windows Boot Manager refuses to start.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootOptionsLib.h>

extern EFI_GUID  gUefiShellFileGuid;

/**
  Return the platform-specific boot options.

  @param[out] BootOption       Allocated array of boot options.
  @param[out] BootOptionCount  Number of entries in BootOption.
**/
VOID
GetPlatformBootOptions (
  OUT EFI_PLATFORM_BOOT_OPTION  **BootOption,
  OUT UINT8                     *BootOptionCount
  )
{
  EFI_PLATFORM_BOOT_OPTION  *Option;

  if ((BootOption == NULL) || (BootOptionCount == NULL)) {
    return;
  }

  *BootOption      = NULL;
  *BootOptionCount = 0;

  Option = AllocateZeroPool (sizeof (*Option));
  if (Option == NULL) {
    return;
  }

  Option->AppName       = L"UEFI Shell";
  Option->AppGuid       = &gUefiShellFileGuid;
  Option->LoadAttribute = LOAD_OPTION_ACTIVE;

  *BootOption      = Option;
  *BootOptionCount = 1;
}
