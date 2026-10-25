#include <Library/DebugLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Configuration/BootDevices.h>

STATIC KEYPAD_DEVICE_PROTOCOL mKeypadDevice = {
  KeypadDeviceReset,
  KeypadDeviceGetKeys
};

EFI_STATUS
EFIAPI
InitializeKeypadDevice (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Handle = NULL;

  // Install Keypad Device Protocol on a dedicated controller handle so the
  // KeypadDxe driver binding can connect to it (the driver image handle is not
  // a controllable device on all platforms).
  Status = gBS->InstallMultipleProtocolInterfaces(&Handle, &gKeypadDeviceProtocolGuid, &mKeypadDevice, &gEfiDevicePathProtocolGuid, &KeypadDevicePath, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register Keypad Device Protocol!\n"));
    ASSERT_EFI_ERROR(Status);
  }

  return Status;
}
