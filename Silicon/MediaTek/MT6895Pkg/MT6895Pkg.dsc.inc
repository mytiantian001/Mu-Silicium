##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # Timer
  #
  USE_PHYSICAL_TIMER                         = 1

!include SiliciumPkg/SiliciumPkg.dsc.inc

[BuildOptions]
  #
  # -mstrict-align: the AArch64 runs with alignment checking enabled
  # (SCTLR.A=1) and the default CLANGPDB flags let the compiler emit
  # unaligned accesses for structs it assumes aligned (e.g. MBR/GPT
  # partition records), which traps. The vendor tree always built with
  # -mstrict-align; restore it here.
  #
  *_*_AARCH64_CC_FLAGS = -march=armv8.2-a+crypto+rcpc -mstrict-align

[PcdsFixedAtBuild]
  #
  # ACPI
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x202035393836544D # "MT6895  "
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x6895

  #
  # CPU
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|3

  #
  # GIC
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0C000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C040000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"MediaTek (TM) Dimensity 8200 @ 3.10 GHz"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"MT6895"

[LibraryClasses]
  #
  # SEC Libraries
  #
  PlatformSecLib|MT6895Pkg/Library/PlatformSecLib/PlatformSecLib.inf

[Components]
  #
  # Storage: MediaTek UFS host controller + generic UFS pass-thru
  #
  MT6895Pkg/Drivers/MtkUfsDxe/MtkUfsDxe.inf
  MT6895Pkg/Drivers/MtkUfsPassThruDxe/UfsPassThruDxe.inf

  #
  # USB host: MediaTek platform bring-up (MT6375 + SSUSB)
  #
  MT6895Pkg/Drivers/MtkUsbDxe/MtkUsbDxe.inf
