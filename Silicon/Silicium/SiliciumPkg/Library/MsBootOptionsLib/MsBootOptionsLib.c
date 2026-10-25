/**
  Copyright (C) Microsoft Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/ArmLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PlatformBootOptionsLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/PcdLib.h>

#include <Protocol/FirmwareVolume2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/LoadedImage.h>

EFI_STATUS
BuildFwLoadOption (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption,
  IN     EFI_GUID                     *FwFile,
  IN     CHAR8                        *Parameter)
{
  EFI_STATUS                         Status               = EFI_SUCCESS;
  EFI_DEVICE_PATH_PROTOCOL          *DevicePath           = NULL;
  EFI_LOADED_IMAGE_PROTOCOL         *mLoadedImageProtocol = NULL;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode             = {0};
  CHAR16                            *Description          = NULL;
  UINTN                              DescriptionLength    = 0;

  // Get Defined Application Name from FV
  Status = GetSectionFromFv (FwFile, EFI_SECTION_USER_INTERFACE, 0, (VOID *)&Description, &DescriptionLength);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Get Application Name of %a! Status = %r\n", __FUNCTION__, FwFile, Status));
    return Status;
  }

  // Init FW Volume Device Path Node
  EfiInitializeFwVolDevicepathNode (&FileNode, FwFile);

  // Locate Loaded Image Protocol Handle
  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID *)&mLoadedImageProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate Loaded Image Protocol Handle! Status = %r\n", __FUNCTION__, Status));
    return Status;
  }

  // Get Device Path
  DevicePath = AppendDevicePathNode (DevicePathFromHandle (mLoadedImageProtocol->DeviceHandle), (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  if (DevicePath == NULL) {
    return EFI_NOT_FOUND;
  }

  // Init Boot Load Option
  Status = EfiBootManagerInitializeLoadOption (BootOption, LoadOptionNumberUnassigned, LoadOptionTypeBoot, LOAD_OPTION_CATEGORY_APP | LOAD_OPTION_ACTIVE | LOAD_OPTION_HIDDEN, Description, DevicePath, (UINT8 *)Parameter, (Parameter != NULL) ? (UINT32)AsciiStrSize (Parameter) : 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Initialize Boot Load Option! Status = %r\n", __FUNCTION__, Status));

    // Free Buffer
    FreePool (DevicePath);

    return Status;
  }

  // Free Buffers
  FreePool (DevicePath);
  FreePool (Description);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MsBootOptionsLibGetDefaultBootApp (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *LoadOption,
  IN     CHAR8                        *Parameter)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
MsBootOptionsLibGetBootManagerMenu (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *LoadOption,
  IN     CHAR8                        *Parameter)
{
  // Return Boot Manager Load Option
  return BuildFwLoadOption (LoadOption, PcdGetPtr (PcdBootManagerMenuFile), Parameter);
}

EFI_STATUS
CreateFvBootOption (
  IN     EFI_GUID                     *FileGuid,
  IN     CHAR16                       *Description,
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption,
  IN     UINT32                        Attributes,
  IN     UINT8                        *OptionalData,
  IN     UINT32                        OptionalDataSize OPTIONAL)
{
  EFI_STATUS                         Status               = EFI_SUCCESS;
  EFI_DEVICE_PATH_PROTOCOL          *DevicePath           = NULL;
  EFI_LOADED_IMAGE_PROTOCOL         *mLoadedImageProtocol = NULL;
  EFI_FIRMWARE_VOLUME2_PROTOCOL     *mFv2Protocol         = NULL;
  VOID                              *Buffer               = NULL;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode             = {0};
  UINTN                              Size                 = 0;
  UINT32                             AuthenticationStatus = 0;

  // Check Parameters
  if ((BootOption == NULL) || (FileGuid == NULL) || (Description == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Init FW Volume Device Path Node
  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);

  // Locate Loaded Image Handle Protocol
  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID *)&mLoadedImageProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate Loaded Image Protocol! Status = %r\n", __FUNCTION__, Status));
    return Status;
  }

  // Locate FV2 Handle Protocol
  Status = gBS->HandleProtocol (mLoadedImageProtocol->DeviceHandle, &gEfiFirmwareVolume2ProtocolGuid, (VOID *)&mFv2Protocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate FV2 Protocol! Status = %r\n", __FUNCTION__, Status));
    return Status;
  }

  // Read EFI Application Section
  Status = mFv2Protocol->ReadSection (mFv2Protocol, FileGuid, EFI_SECTION_PE32, 0, &Buffer, &Size, &AuthenticationStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Read EFI Application Section from %a! Status = %r\n", __FUNCTION__, FileGuid, Status));
    return Status;
  }

  // Get Device Path
  DevicePath = AppendDevicePathNode (DevicePathFromHandle (mLoadedImageProtocol->DeviceHandle), (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  if (DevicePath == NULL) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to get Device Path of EFI Application! Status = %r\n", __FUNCTION__, Status));

    // Free Buffer
    FreePool (Buffer);

    return EFI_OUT_OF_RESOURCES;
  }

  // Init Boot Option
  Status = EfiBootManagerInitializeLoadOption (BootOption, LoadOptionNumberUnassigned, LoadOptionTypeBoot, Attributes, Description, DevicePath, OptionalData, OptionalDataSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Init Boot Option for %a! Status = %r\n", __FUNCTION__, FileGuid, Status));

    // Free Buffers
    FreePool (Buffer);
    FreePool (DevicePath);

    return Status;
  }

  // Free Buffers
  FreePool (Buffer);
  FreePool (DevicePath);

  return EFI_SUCCESS;
}

STATIC
UINTN
RegisterFvBootOption (
  IN EFI_GUID *FileGuid,
  IN CHAR16   *Description,
  IN UINTN     Position,
  IN UINT32    Attributes,
  IN UINT8    *OptionalData, 
  IN UINT32    OptionalDataSize)
{
  EFI_STATUS                    Status          = EFI_SUCCESS;
  EFI_BOOT_MANAGER_LOAD_OPTION *BootOptions     = NULL;
  EFI_BOOT_MANAGER_LOAD_OPTION  NewOption       = {0};
  UINTN                         OptionIndex     = 0;
  UINTN                         BootOptionCount = 0;

  // Set Dummy Option Number
  NewOption.OptionNumber = LoadOptionNumberUnassigned;

  // Create a New FV Boot Option
  Status = CreateFvBootOption (FileGuid, Description, &NewOption, Attributes, OptionalData, OptionalDataSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Create a New FV Boot Option! Status = %r\n", __FUNCTION__, Status));
    return Status;
  }

  // Get Boot Load Options
  BootOptions = EfiBootManagerGetLoadOptions (&BootOptionCount, LoadOptionTypeBoot);

  // Find Boot Load Option
  OptionIndex = EfiBootManagerFindLoadOption (&NewOption, BootOptions, BootOptionCount);

  // Check Boot Option Index
  if (OptionIndex == -1) {
    // Update Boot Attribute
    NewOption.Attributes ^= LOAD_OPTION_ACTIVE;

    // Get new Boot Option Index
    OptionIndex = EfiBootManagerFindLoadOption (&NewOption, BootOptions, BootOptionCount);

    // Update Boot Attribute
    NewOption.Attributes ^= LOAD_OPTION_ACTIVE;
  }

  // Check Boot Option Index
  if (OptionIndex == -1) {
    // Add Boot Load Option Variable
    Status = EfiBootManagerAddLoadOptionVariable (&NewOption, Position);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Add New Boot Load Option Variable! Status = %r\n", __FUNCTION__, Status));

      // Free Buffers
      EfiBootManagerFreeLoadOption  (&NewOption);
      EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);

      return Status;
    }
  } else {
    // Update Boot Option Number
    NewOption.OptionNumber = BootOptions[OptionIndex].OptionNumber;
  }

  // Free Buffers
  EfiBootManagerFreeLoadOption  (&NewOption);
  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);

  return NewOption.OptionNumber;
}


/**
  Find a file on any FAT volume and return its full device path.
**/
//
// Shared boot log written by FrameBufferSerialPortLib (see its PearlLogAppend).
//
#define PEARL_LOG_ADDRESS  0x007F241000ULL
#define PEARL_LOG_SIZE     0x00010000U
#define PEARL_LOG_MAGIC    0x474F4C50U  // "PLOG"

typedef struct {
  UINT32  Magic;
  UINT32  Head;
  UINT32  Size;
  UINT32  Reserved;
} PEARL_LOG_HEADER;

/**
  Dump the captured boot log to \UEFIDEBUG.TXT on every FAT volume.
**/
STATIC
VOID
PearlLogDump (
  VOID
  )
{
  volatile PEARL_LOG_HEADER  *Hdr;
  volatile UINT8             *Data;
  PEARL_LOG_HEADER           Snapshot;
  UINTN                      Capacity;
  UINTN                      Length;
  UINTN                      Start;
  UINTN                      Index;
  UINT8                      *Linear;
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL          *Root;
  EFI_FILE_PROTOCOL          *File;
  UINTN                      WriteSize;

  Hdr      = (volatile PEARL_LOG_HEADER *)(UINTN)PEARL_LOG_ADDRESS;
  Capacity = PEARL_LOG_SIZE - sizeof (PEARL_LOG_HEADER);
  Data     = (volatile UINT8 *)(UINTN)(PEARL_LOG_ADDRESS + sizeof (PEARL_LOG_HEADER));

  CopyMem (&Snapshot, (VOID *)Hdr, sizeof (Snapshot));
  DEBUG ((DEBUG_ERROR, "PEARLDIAG: log head=%u magic=0x%x\n", Snapshot.Head, Snapshot.Magic));

  Start = (Snapshot.Head < Capacity) ? 0 : (Snapshot.Head % Capacity);
  if (Snapshot.Magic == PEARL_LOG_MAGIC) {
    Length = (Snapshot.Head < Capacity) ? Snapshot.Head : Capacity;
  } else {
    Length = 0;
  }

  Linear = AllocatePool (Length + 1);
  if (Linear == NULL) {
    return;
  }

  for (Index = 0; Index < Length; Index++) {
    Linear[Index] = Data[(Start + Index) % Capacity];
  }

  Linear[Length] = 0;

  DEBUG ((DEBUG_ERROR, "PEARLDIAG: captured log %u bytes\n", Length));

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    FreePool (Linear);
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
                     L"\\UEFIDEBUG.TXT",
                     EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ,
                     0
                     );
    if (!EFI_ERROR (Status)) {
      WriteSize = Length;
      Status    = File->Write (File, &WriteSize, Linear);
      DEBUG ((DEBUG_ERROR, "PEARLDIAG: vol%u \\UEFIDEBUG.TXT write %r (%u bytes)\n", Index, Status, WriteSize));
      File->Close (File);
    } else {
      DEBUG ((DEBUG_ERROR, "PEARLDIAG: vol%u \\UEFIDEBUG.TXT open %r\n", Index, Status));
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  FreePool (Linear);
}

/**
  Read 256 MiB of boot.wim through the firmware FAT stack and time it, so a
  stuck progress bar becomes a number instead of a guess.
**/
STATIC
VOID
PearlWimSpeedTest (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  VOID               *Buf;
  UINTN              BufSize;
  UINTN              Total;
  EFI_TIME           T0;
  EFI_TIME           T1;
  UINTN              Elapsed;

  Status = Root->Open (Root, &File, L"\\sources\\boot.wim", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return;
  }

  Buf = AllocatePool (1024 * 1024);
  if (Buf == NULL) {
    File->Close (File);
    return;
  }

  gST->RuntimeServices->GetTime (&T0, NULL);
  Total = 0;
  while (Total < (256U * 1024U * 1024U)) {
    BufSize = 1024 * 1024;
    Status  = File->Read (File, &BufSize, Buf);
    if (EFI_ERROR (Status) || (BufSize == 0)) {
      break;
    }
    Total += BufSize;
  }
  gST->RuntimeServices->GetTime (&T1, NULL);

  Elapsed = (UINTN)((T1.Hour * 3600 + T1.Minute * 60 + T1.Second) -
                    (T0.Hour * 3600 + T0.Minute * 60 + T0.Second));
  DEBUG ((
    DEBUG_ERROR,
    "PEARLDIAG: wim %u MiB read in %u s (%u MiB/s)\n",
    Total / (1024 * 1024),
    Elapsed,
    (Elapsed != 0) ? (UINT32)(Total / (1024 * 1024) / Elapsed) : 0
    ));

  FreePool (Buf);
  File->Close (File);
}

STATIC
EFI_DEVICE_PATH_PROTOCOL *
PearlFindEspFile (
  IN  CONST CHAR16  *FileName,
  OUT EFI_HANDLE    *VolumeHandle OPTIONAL
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_DEVICE_PATH_PROTOCOL         *Path;

  Path = NULL;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
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

    Status = Root->Open (Root, &File, (CHAR16 *)FileName, EFI_FILE_MODE_READ, 0);
    Root->Close (Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    File->Close (File);
    if (VolumeHandle != NULL) {
      *VolumeHandle = Handles[Index];
    }

    Path = FileDevicePath (Handles[Index], (CHAR16 *)FileName);
    break;
  }

  FreePool (Handles);
  return Path;
}

/**
  Print what BDS and bootmgr can see: every FAT volume, whether the BCD opens
  from it, and the current BootOrder / BootCurrent / Boot#### list.
**/
STATIC
VOID
PearlEspDiag (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  UINTN                         Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL             *Root;
  EFI_FILE_PROTOCOL             *File;
  EFI_DEVICE_PATH_PROTOCOL      *Dp;
  CHAR16                        *Text;
  UINT16                        BootCurrent;
  UINTN                         Size;
  UINT16                        *BootOrder;
  UINTN                         OrderSize;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         OptionCount;

  DEBUG ((DEBUG_ERROR, "PEARLDIAG: mpidr=0x%lx cntfrq=%u\n", ArmReadMpidr (), ArmReadCntFrq ()));

  Size = sizeof (BootCurrent);
  Status = gST->RuntimeServices->GetVariable (L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &Size, &BootCurrent);
  DEBUG ((DEBUG_ERROR, "PEARLDIAG: BootCurrent %r %04x\n", Status, (Status == EFI_SUCCESS) ? BootCurrent : 0xFFFF));

  BootOrder = NULL;
  OrderSize = 0;
  Status   = gST->RuntimeServices->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, NULL, &OrderSize, BootOrder);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    BootOrder = AllocatePool (OrderSize);
    if (BootOrder != NULL) {
      Status = gST->RuntimeServices->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, NULL, &OrderSize, BootOrder);
      if (!EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "PEARLDIAG: BootOrder (%u entries):", OrderSize / sizeof (UINT16)));
        for (Index = 0; Index < OrderSize / sizeof (UINT16); Index++) {
          DEBUG ((DEBUG_ERROR, " %04x", BootOrder[Index]));
        }
        DEBUG ((DEBUG_ERROR, "\n"));
      } else {
        DEBUG ((DEBUG_ERROR, "PEARLDIAG: BootOrder read %r\n", Status));
      }
      FreePool (BootOrder);
    }
  }

  Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
  for (Index = 0; Index < OptionCount; Index++) {
    Text = ConvertDevicePathToText (Options[Index].FilePath, TRUE, FALSE);
    DEBUG ((
      DEBUG_ERROR,
      "PEARLDIAG: Boot%04x \"%s\" attr=0x%x %s\n",
      Options[Index].OptionNumber,
      Options[Index].Description,
      Options[Index].Attributes,
      (Text != NULL) ? Text : L"(no path)"
      ));
    if (Text != NULL) {
      FreePool (Text);
    }
  }
  EfiBootManagerFreeLoadOptions (Options, OptionCount);

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PEARLDIAG: no SimpleFileSystem handles %r\n", Status));
    return;
  }

  DEBUG ((DEBUG_ERROR, "PEARLDIAG: %u SimpleFileSystem volume(s)\n", Count));
  for (Index = 0; Index < Count; Index++) {
    Dp   = DevicePathFromHandle (Handles[Index]);
    Text = (Dp != NULL) ? ConvertDevicePathToText (Dp, TRUE, FALSE) : NULL;
    DEBUG ((DEBUG_ERROR, "PEARLDIAG: vol%u %s\n", Index, (Text != NULL) ? Text : L"(no device path)"));
    if (Text != NULL) {
      FreePool (Text);
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "PEARLDIAG:   OpenVolume %r\n", Status));
      continue;
    }

    Status = Root->Open (Root, &File, L"\\EFI\\Microsoft\\Boot\\BCD", EFI_FILE_MODE_READ, 0);
    DEBUG ((DEBUG_ERROR, "PEARLDIAG:   \\EFI\\Microsoft\\Boot\\BCD -> %r\n", Status));
    if (!EFI_ERROR (Status)) {
      File->Close (File);
    }

    Status = Root->Open (Root, &File, L"\\EFI\\Boot\\BOOTAA64.EFI", EFI_FILE_MODE_READ, 0);
    DEBUG ((DEBUG_ERROR, "PEARLDIAG:   \\EFI\\Boot\\BOOTAA64.EFI -> %r\n", Status));
    if (!EFI_ERROR (Status)) {
      File->Close (File);
    }

    Status = Root->Open (Root, &File, L"\\sources\\boot.wim", EFI_FILE_MODE_READ, 0);
    DEBUG ((DEBUG_ERROR, "PEARLDIAG:   \\sources\\boot.wim -> %r\n", Status));
    if (!EFI_ERROR (Status)) {
      File->Close (File);
    }

    PearlWimSpeedTest (Root);

    Root->Close (Root);
  }

  FreePool (Handles);
  gBS->Stall (15 * 1000 * 1000);
}

/**
  Register "Windows Boot Manager" pointing at the ESP file, first in BootOrder.

  This is the layout a real Windows install uses, and it also makes BootCurrent
  point at a real Boot#### entry whose device path names the ESP, instead of
  MsBootPolicy's temporary in-memory option.
**/
STATIC
VOID
PearlRegisterEspBootOption (
  VOID
  )
{
  EFI_DEVICE_PATH_PROTOCOL      *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  EFI_STATUS                    Status;
  CHAR16                        *Text;
  BOOLEAN                       IsLinux;

  //
  // Prefer the Debian EFI-stub kernel when it is present: it exercises the
  // same UEFI -> ExitBootServices path as Windows but boots Linux (device
  // tree, no ACPI), which isolates a firmware handoff problem from a
  // Windows/ACPI specific one.
  //
  IsLinux = FALSE;
  Path = PearlFindEspFile (L"\\DEBLINUX.BIN", NULL);
  if (Path == NULL) {
    Path = PearlFindEspFile (L"\\deb-vmlinuz", NULL);
  }
  if (Path != NULL) {
    IsLinux = TRUE;
  }
  if (Path == NULL) {
    DEBUG ((DEBUG_WARN, "Pearl: no \\EFI\\Microsoft\\Boot\\bootmgfw.efi on any FAT volume\n"));
    return;
  }

  Text = ConvertDevicePathToText (Path, TRUE, FALSE);
  DEBUG ((DEBUG_ERROR, "Pearl: bootmgr path %s\n", (Text != NULL) ? Text : L"(null)"));
  if (Text != NULL) {
    FreePool (Text);
  }

  EfiBootManagerInitializeLoadOption (
    &Option,
    LoadOptionNumberUnassigned,
    LoadOptionTypeBoot,
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    IsLinux ? L"Debian Linux" : L"Windows Boot Manager",
    Path,
    NULL,
    0
    );
  Status = EfiBootManagerAddLoadOptionVariable (&Option, 0);
  DEBUG ((DEBUG_ERROR, "Pearl: add Windows Boot Manager -> %r option=%04x\n", Status, Option.OptionNumber));
  EfiBootManagerFreeLoadOption (&Option);
  FreePool (Path);
}

VOID
EFIAPI
MsBootOptionsLibRegisterDefaultBootOptions ()
{
  EFI_PLATFORM_BOOT_OPTION *BootOption;
  UINT8                     BootOptionCount;

  //
  // Disarm the firmware watchdog: bootmgr can take minutes to pull a large
  // boot.wim through the firmware FAT stack, and a watchdog reset looks
  // exactly like the machine "rebooting while loading".
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  DEBUG ((DEBUG_ERROR, "Pearl: watchdog disarmed\n"));

  //
  // Diagnostic aid: run the Pearl UFS/FAT diagnostic first, then keep the
  // UEFI Shell available for interactive inspection.
  //
  if (PcdGetBool (PcdUefiShellBootFirst)) {
    static EFI_GUID  mPearlPmicTestFileGuid = {
      0x8A3F6D52, 0x1C47, 0x4E90, { 0xB6, 0xD3, 0x5F, 0x2A, 0x9C, 0x7E, 0x4B, 0x18 }
    };

    RegisterFvBootOption (&mPearlPmicTestFileGuid, L"Pearl PMIC Test", 0, LOAD_OPTION_ACTIVE, NULL, 0);
    RegisterFvBootOption (&gUefiShellFileGuid, L"UEFI Shell", (UINTN)-1, LOAD_OPTION_ACTIVE, NULL, 0);
  }

  // Register Default Boot Options
  RegisterFvBootOption (&gMsBootPolicyFileGuid,            L"Internal Storage", (UINTN)-1, LOAD_OPTION_ACTIVE, (UINT8 *)"SSD", sizeof ("SSD"));
  RegisterFvBootOption (&gMsBootPolicyFileGuid,            L"USB Storage",      (UINTN)-1, LOAD_OPTION_ACTIVE, (UINT8 *)"USB", sizeof ("USB"));
  RegisterFvBootOption (FixedPcdGetPtr (PcdUfpLoaderFile), L"FFU Mode",         (UINTN)-1, LOAD_OPTION_HIDDEN, NULL, 0);

  // Get Platform Boot Options
  GetPlatformBootOptions (&BootOption, &BootOptionCount);

  // Register Platform Boot Options
  for (UINT8 i = 0; i < BootOptionCount; i++) {
    RegisterFvBootOption (BootOption[i].AppGuid, BootOption[i].AppName, (UINTN)-1, BootOption[i].LoadAttribute, NULL, 0);
  }

  //
  // Keep the Boot Manager menu last so it stays reachable from the menu but
  // never displaces the OS boot entry: with PcdPlatformBootTimeOut=0 the first
  // entry in BootOrder is what actually boots.
  //
  {
    static EFI_GUID  mBootManagerMenuFileGuid = {
      0xEEC25BDC, 0x67F2, 0x4D95, { 0xB1, 0xD5, 0xF8, 0x1B, 0x20, 0x39, 0xD1, 0x1D }
    };

    RegisterFvBootOption (&mBootManagerMenuFileGuid, L"Boot Manager", (UINTN)-1, LOAD_OPTION_ACTIVE, NULL, 0);
  }

  // Free Buffer
  if (BootOption != NULL) {
    FreePool (BootOption);
  }

  //
  // At this point in BDS the UFS stack has not been started yet (it comes up
  // when MsBootPolicy connects controllers), so there is not a single
  // SimpleFileSystem handle to scan.  Connect everything first, then the ESP
  // is visible for the boot-log dump and the Windows Boot Manager option.
  //
  EfiBootManagerConnectAll ();

  //
  // Boot Windows the way a real install does, then dump what BDS sees.
  //
  PearlEspDiag ();
  PearlLogDump ();
  PearlRegisterEspBootOption ();

  //
  // Re-dump so the file also contains the registration result.
  //
  PearlLogDump ();
}

EFI_BOOT_MANAGER_LOAD_OPTION*
EFIAPI
MsBootOptionsLibGetDefaultOptions (OUT UINTN *OptionCount)
{
  return NULL;
}
