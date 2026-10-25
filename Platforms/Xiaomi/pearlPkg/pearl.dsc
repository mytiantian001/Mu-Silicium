##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = pearl
  PLATFORM_GUID                  = 5B3D8F21-7A64-4C9E-B02D-8E41C6A3F5D7
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/pearlPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = pearlPkg/pearl.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 1

!include MT6895Pkg/MT6895Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x40C00000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi Note 12T Pro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"pearl"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Redmi_Note_12T_Pro_pearl"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Redmi Note 12T Pro"

  #
  # Simple Frame Buffer (LK scanout: 1088 px pitch, 2460 px, 32 bpp).
  # Width is the scan-buffer pitch, VisibleWidth the real 1080 px panel width.
  #
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferAddress|0xfe14f000
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferWidth|1088
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferHeight|2460
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|1080
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferLayers|3
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferPixelFormat|1   # RGB (a8r8g8b8 word order)
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferOvl0Base|0x14002000
  gMediaTekTokenSpaceGuid.PcdMipiFrameBufferOvl0_2LBase|0x14003000

  #
  # Framebuffer console layout (SiliciumPkg FrameBufferSerialPortLib).
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1088
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2460
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # USB: MT6375 on i2c5 (0x11280000), SSUSB IPPC (0x11203e00), the MTK XHCI
  # controller (0x11200000) and the U2 PHY (t-phy@11e40000).
  #
  gMediaTekTokenSpaceGuid.PcdI2cBase|0x11280000
  gMediaTekTokenSpaceGuid.PcdIppcBase|0x11203e00
  gMediaTekTokenSpaceGuid.PcdXhciBase|0x11200000
  gMediaTekTokenSpaceGuid.PcdU2PhyBase|0x11e40000

  #
  # UFS host controller (mainline mt6895.dtsi ufshci@112b0000).
  #
  gMediaTekTokenSpaceGuid.PcdUfsHostControllerBase|0x112b0000

  #
  # Keypad: KPD controller (kp@1c00e000) and SPMI PMIF (spmi@1c804000).
  #
  gMediaTekTokenSpaceGuid.PcdKeypadBase|0x1c00e000
  gMediaTekTokenSpaceGuid.PcdPmifBase|0x1c804000

  #
  # Boot Manager: no countdown for bring-up
  #
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|0

  #
  # Normal boot order. The keypad driver now provides Volume Up, so the
  # Boot Manager can be entered with Volume Up.
  #
  gSiliciumPkgTokenSpaceGuid.PcdUefiShellBootFirst|FALSE

  #
  # Boot log: error-only for the interactive Boot Manager. Use 0x8000104F
  # when UFS/link-up diagnostics are needed.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000046

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|pearlPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Used by MtkUsbDxe to expose the XHCI as a non-discoverable PCI device.
  #
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf

  #
  # Used by the SCSI bus/disk drivers on top of the UFS pass-thru driver.
  #
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

  #
  # Pearl keypad: volume-down on the MT6895 KPD, volume-up/power on MT6363.
  #
  KeypadDeviceLib|pearlPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  #
  # Add the UEFI Shell to the Boot Manager for UFS/FAT diagnostics.
  #
  PlatformBootOptionsLib|pearlPkg/Library/PlatformBootOptionsLib/PlatformBootOptionsLib.inf

  #
  # UEFI Shell dependencies.
  #
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf

[Components]
!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  #
  # Custom display: MT6895 LK handoff takeover (OVL fixup)
  #
  MT6895Pkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  #
  # ACPI tables (MADT/GTDT/FADT/DSDT) for Windows and ACPI-aware OSes.
  #
  pearlPkg/AcpiTables/AcpiTables.inf

  #
  # Mask every GIC source and disarm the EL1 architected timers just before
  # ExitBootServices, so Windows inherits a CPU with nothing armed
  # (port of yunluoPkg/Drivers/HandoffQuiesceDxe).
  #
  pearlPkg/Drivers/HandoffQuiesceDxe/HandoffQuiesceDxe.inf

  #
  # USB host: non-discoverable XHCI registration + stock XHCI driver
  #
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf

  #
  # SCSI stack: the UFS pass-thru driver produces an ExtScsiPassThru
  # protocol; ScsiBus/ScsiDisk turn the LUNs into BlockIo devices.
  #
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf

  #
  # Keypad (volume down on KPD, volume up/power on MT6363) and its text input
  # front-end.
  #
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf

  #
  # UEFI Shell for UFS/FAT and Windows PE diagnostics.
  #
  ShellPkg/Application/Shell/Shell.inf
