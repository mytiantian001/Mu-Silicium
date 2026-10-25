/** @file
  ACPI table defines for the Xiaomi Redmi Note 12T Pro (pearl, MT6895).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __PEARL_ACPITABLES_H__
#define __PEARL_ACPITABLES_H__

#include <IndustryStandard/Acpi.h>

#define EFI_ACPI_OEM_ID                       {'P','E','A','R','L',' '}
#define EFI_ACPI_OEM_TABLE_ID                 SIGNATURE_64 ('P','E','A','R','L','M','T','K')
#define EFI_ACPI_OEM_REVISION                 0x00000001
#define EFI_ACPI_CREATOR_ID                   SIGNATURE_32 ('E','D','K','2')
#define EFI_ACPI_CREATOR_REVISION             0x00010000

//
// A macro to initialise the common header part of EFI ACPI tables.
//
#define ACPI_HEADER(Signature, Type, Revision) {                  \
    Signature,                      /* UINT32  Signature */       \
    sizeof (Type),                  /* UINT32  Length */          \
    Revision,                       /* UINT8   Revision */        \
    0,                              /* UINT8   Checksum */        \
    EFI_ACPI_OEM_ID,                /* UINT8   OemId[6] */        \
    EFI_ACPI_OEM_TABLE_ID,          /* UINT64  OemTableId */      \
    EFI_ACPI_OEM_REVISION,          /* UINT32  OemRevision */     \
    EFI_ACPI_CREATOR_ID,            /* UINT32  CreatorId */       \
    EFI_ACPI_CREATOR_REVISION       /* UINT32  CreatorRevision */ \
  }

// 
// NULL_GAS: a generic address structure meaning "register not present".
// (The vendor tree gets this from EmbeddedPkg's AcpiLib.h; define it
// locally so the tables build in any tree.)
//
#define NULL_GAS  { EFI_ACPI_6_3_SYSTEM_MEMORY, 0, 0, EFI_ACPI_6_3_UNDEFINED, 0L }

//
// Platform hardware (mainline mt6895.dtsi / mt6895-xiaomi-pearl.dts).
//
#define PEARL_GIC_DISTRIBUTOR_BASE     0x0C000000
#define PEARL_GIC_REDISTRIBUTOR_BASE   0x0C040000
#define PEARL_GIC_REDISTRIBUTOR_SIZE   0x00200000
#define PEARL_GIC_REDISTRIBUTOR_STRIDE 0x00040000
#define PEARL_CPU_COUNT                8

//
// CPU MPIDRs (affinity order):
//   cluster 0: 4x Cortex-A55  -> 0x000..0x003
//   cluster 1: 3x Cortex-A78  -> 0x100..0x102
//   cluster 2: 1x Cortex-A78  -> 0x200
//
//
// MT6895 DTS: cpu0..cpu7 reg = 0x0000, 0x0100, ... 0x0700, i.e. the core
// number sits in Aff1.  Windows matches each GICC entry against MPIDR, so a
// wrong layout here makes it fault before the first thread exists.
//
#define PEARL_MPIDR(i)  (0x10000 + (i) * 0x100)

#endif // __PEARL_ACPITABLES_H__
