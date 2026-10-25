/** @file
  Pearl UEFI keypad input test.

  The real keypad driver is produced by KeypadDeviceDxe/KeypadDxe.  This test
  application only reads the UEFI console input protocol, so seeing scan codes
  here proves the complete KPD/MT6363 -> KeypadDxe -> SimpleTextIn chain.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/SimpleTextIn.h>

EFI_STATUS
EFIAPI
PearlKeypadEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  EFI_INPUT_KEY Key;

  Print (L"\r\n===== Pearl keypad UEFI input test =====\r\n");
  Print (L"Press Volume Down, Volume Up and Power (short press).\r\n\r\n");

  for (;;) {
    ZeroMem (&Key, sizeof (Key));
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (!EFI_ERROR (Status)) {
      Print (
        L"KEY ScanCode=0x%04x Unicode=0x%04x (%s)\r\n",
        Key.ScanCode,
        Key.UnicodeChar,
        Key.ScanCode == SCAN_UP ? L"Volume Up" :
        Key.ScanCode == SCAN_DOWN ? L"Volume Down" :
        Key.UnicodeChar == CHAR_CARRIAGE_RETURN ? L"Power/Enter" : L"other"
        );
    }

    gBS->Stall (10000);
  }
}
