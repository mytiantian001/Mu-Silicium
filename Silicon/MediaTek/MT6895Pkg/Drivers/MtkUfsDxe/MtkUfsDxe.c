/** @file
  MediaTek UFS host controller platform driver (MT6895 / pearl).

  Provides the EDKII_UFS_HOST_CONTROLLER_PROTOCOL consumed by the generic
  MdeModulePkg UfsPassThruDxe driver.

  The bootloader (LK) already brings up the UFS PHY, clocks and the UniPro
  link (it boots from UFS), and the mainline kernel's ufs-mediatek driver
  relies on the same setup (its device tree intentionally omits the clocks,
  resets and PHY for this board). This driver therefore only:

    - sets the MediaTek "CLK_CG early" bit in REG_UFS_XOUFS_CTRL (0x140),
      mirroring ufs_mtk_hce_enable_notify() of the mainline driver, which
      bypasses the abnormal ERR_CHK signal and prevents host hangs
    - exposes the UFSHCI MMIO registers at PcdUfsHostControllerBase
    - provides identity DMA mapping with cache maintenance (the UFS
      controller is not behind a programmed IOMMU on this platform)

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ArmSmcLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

#define MTK_UFS_HC_PRIVATE_SIGNATURE  SIGNATURE_32 ('M', 'T', 'K', 'U')

//
// MediaTek vendor registers inside the UFSHCI MMIO range.
//
#define MTK_REG_UFS_XOUFS_CTRL  0x140
#define MTK_XOUFS_CTRL_CLK_CG   BIT7

//
// MediaTek reference clock control (REG_UFS_REFCLK_CTRL).
//
#define MTK_REG_UFS_REFCLK_CTRL  0x144
#define MTK_REFCLK_REQUEST       BIT0
#define MTK_REFCLK_ACK           BIT1

//
// MediaTek SiP service (ufs-mediatek-sip.h):
//   arm_smccc_smc (MTK_SIP_UFS_CONTROL, UFS_MTK_SIP_DEVICE_RESET, high, ...)
// MTK_SIP_UFS_CONTROL = MTK_SIP_SMC_CMD(0x276) =
//   ARM_SMCCC_CALL_VAL (FAST_CALL, SMC_64, OWNER_SIP, 0x276) = 0xC2000276.
// The previous 0x81020276 encoding (SMC32, owner 1) made the ATF reject
// the call with SMC_UNK, so the device reset never happened and the UFS
// device stayed unresponsive (link startup 0x1, DP=0).
//
#define MTK_SIP_UFS_CONTROL        0xC2000276
#define UFS_MTK_SIP_DEVICE_RESET   0x2

//
// UFSHCI MMIO size (mainline mt6895.dtsi: reg = <0x112b0000 0x2300>).
//
#define MTK_UFS_HC_MMIO_SIZE  0x2300

//
// UFSHCI registers used by the platform hooks.
//
#define MTK_REG_UFS_AHIT     0x18
#define MTK_REG_UFS_IS       0x20
#define MTK_REG_UFS_HCS      0x30
#define MTK_REG_UFS_HCE      0x34
#define MTK_REG_UFS_UIC_CMD  0x90
#define MTK_REG_UFS_UIC_ARG1 0x94
#define MTK_REG_UFS_UIC_ARG2 0x98
#define MTK_REG_UFS_UIC_ARG3 0x9C

#define MTK_IS_UCCS  BIT10

//
// UIC command opcodes (UFSHCI spec).
//
#define MTK_UIC_DME_GET          0x01
#define MTK_UIC_DME_SET          0x02
#define MTK_UIC_DME_ENABLE       0x12
#define MTK_UIC_DME_RESET        0x14
#define MTK_UIC_DME_LINK_STARTUP 0x16

//
// UniPro MIB attributes (ufs-mediatek.h / unipro.h).
//
#define MTK_MIB_VS_UNIPROPOWERDOWNCONTROL  0xD0A8
#define MTK_MIB_VS_SAVEPOWERCONTROL        0xD0A6
#define MTK_MIB_PA_LOCAL_TX_LCC_ENABLE     0x155E

//
// UICCMDARG1 encoding for DME commands: the attribute ID lives in bits
// [31:16] and the selector in bits [15:0]. Mirrors the kernel's
// UIC_ARG_MIB_SEL macro and the Samsung UfsDxe's (0x1571 << 16) usage.
//
#define MTK_UIC_ARG_MIB(Mib)  ((UINT32)(Mib) << 16)

typedef struct {
  UINT32                              Signature;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  UfsHcProtocol;
  UINTN                               UfsHcBase;
} MTK_UFS_HC_PRIVATE_DATA;

#define MTK_UFS_HC_PRIVATE_DATA_FROM_THIS(a) \
  CR (a, MTK_UFS_HC_PRIVATE_DATA, UfsHcProtocol, MTK_UFS_HC_PRIVATE_SIGNATURE)

//
// Bookkeeping returned through the Mapping pointer of Map().
//
typedef struct {
  VOID                                *HostAddress;
  UINTN                               NumberOfBytes;
  EDKII_UFS_HOST_CONTROLLER_OPERATION Operation;
} MTK_UFS_MAPPING;

/**
  Get the MMIO base address of the UFS host controller.

  @param[in]  This      A pointer to the protocol instance.
  @param[out] MmioBar   The MMIO base address of the UFS host controller.

  @retval EFI_SUCCESS   The operation succeeds.
**/
EFI_STATUS
EFIAPI
MtkUfsGetMmioBar (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                                  *MmioBar
  )
{
  MTK_UFS_HC_PRIVATE_DATA  *Private;

  if ((This == NULL) || (MmioBar == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private  = MTK_UFS_HC_PRIVATE_DATA_FROM_THIS (This);
  *MmioBar = Private->UfsHcBase;

  return EFI_SUCCESS;
}

/**
  Allocate pages suitable for an EdkiiUfsHcOperationBusMasterCommonBuffer
  mapping.

  @param[in]  This         A pointer to the protocol instance.
  @param[in]  Type         The allocation type (passed to AllocatePages).
  @param[in]  MemoryType   The type of memory to allocate.
  @param[in]  Pages        The number of pages to allocate.
  @param[out] HostAddress  The base address of the allocated range.
  @param[in]  Attributes   Requested attributes (ignored; cache maintenance
                           is done in Map()/Unmap()).

  @retval EFI_SUCCESS           The requested memory pages were allocated.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES  The memory pages could not be allocated.
**/
EFI_STATUS
EFIAPI
MtkUfsAllocateBuffer (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN     EFI_ALLOCATE_TYPE                   Type,
  IN     EFI_MEMORY_TYPE                     MemoryType,
  IN     UINTN                               Pages,
  OUT VOID                                   **HostAddress,
  IN     UINT64                              Attributes
  )
{
  EFI_STATUS  Status;

  if ((This == NULL) || (HostAddress == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->AllocatePages (Type, MemoryType, Pages, (EFI_PHYSICAL_ADDRESS *)HostAddress);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Make sure no stale cache lines can be written back over the DMA data.
  //
  InvalidateDataCacheRange (*HostAddress, EFI_PAGES_TO_SIZE (Pages));

  //
  // The UFS host controller is a non-coherent bus master. The generic
  // UfsPassThruDxe driver fills the UTP transfer descriptors and command
  // descriptors with plain memory writes and rings the doorbell without any
  // cache maintenance (it was written for cache-coherent x86 platforms), so
  // on ARM the controller would read stale data and the CPU would read a
  // stale OCS. Mark these buffers non-cacheable.
  //
  // EFI_MEMORY_WC maps to Normal non-cacheable memory on ARM, which keeps
  // the DMA coherency but - unlike EFI_MEMORY_UC (Device-nGnRE) - tolerates
  // unaligned accesses. The Windows boot chain (and the FAT driver) process
  // file data in place, and an unaligned access to Device memory faults
  // with 'Data abort: Alignment fault' regardless of SCTLR.A.
  //
  Status = gDS->SetMemorySpaceAttributes (
                  (EFI_PHYSICAL_ADDRESS)(UINTN)*HostAddress,
                  EFI_PAGES_TO_SIZE (Pages),
                  EFI_MEMORY_WC
                  );
  DEBUG ((
    DEBUG_INFO,
    "MtkUfsDxe: AllocateBuffer(0x%lx, %d pages) normal-nc: %r\n",
    (UINT64)(UINTN)*HostAddress,
    (UINT32)Pages,
    Status
    ));

  return EFI_SUCCESS;
}

/**
  Frees memory that was allocated with AllocateBuffer().

  @param[in] This         A pointer to the protocol instance.
  @param[in] Pages        The number of pages to free.
  @param[in] HostAddress  The base address of the allocated range.

  @retval EFI_SUCCESS           The requested memory pages were freed.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
**/
EFI_STATUS
EFIAPI
MtkUfsFreeBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  UINTN                               Pages,
  IN  VOID                                *HostAddress
  )
{
  if ((This == NULL) || (HostAddress == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  return gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
}

/**
  Provides the UFS controller-specific addresses needed to access system
  memory. The controller is not behind an IOMMU on this platform, so the
  mapping is the identity; cache maintenance is performed according to the
  transfer direction.

  @param[in]     This           A pointer to the protocol instance.
  @param[in]     Operation      Read/write direction of the DMA transfer.
  @param[in]     HostAddress    The system memory address to map.
  @param[in,out] NumberOfBytes  On input the number of bytes to map, on
                                output the number of bytes mapped.
  @param[out]    DeviceAddress  The resulting device address.
  @param[out]    Mapping        A value to pass to Unmap().

  @retval EFI_SUCCESS           The range was mapped.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES  The request could not be completed.
**/
EFI_STATUS
EFIAPI
MtkUfsMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  )
{
  MTK_UFS_MAPPING  *MapInfo;

  if ((This == NULL) || (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  switch (Operation) {
    case EdkiiUfsHcOperationBusMasterRead:
      //
      // Device reads from system memory: flush the producer's writes so the
      // DMA sees them.
      //
      WriteBackDataCacheRange (HostAddress, *NumberOfBytes);
      break;
    case EdkiiUfsHcOperationBusMasterWrite:
      //
      // Device writes to system memory: drop stale cache lines before the
      // DMA, so they cannot be written back over the device's data.
      //
      InvalidateDataCacheRange (HostAddress, *NumberOfBytes);
      break;
    case EdkiiUfsHcOperationBusMasterCommonBuffer:
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  MapInfo = AllocatePool (sizeof (MTK_UFS_MAPPING));
  if (MapInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  MapInfo->HostAddress   = HostAddress;
  MapInfo->NumberOfBytes = *NumberOfBytes;
  MapInfo->Operation     = Operation;

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping       = MapInfo;

  return EFI_SUCCESS;
}

/**
  Completes the Map() operation and releases any corresponding resources.

  @param[in] This     A pointer to the protocol instance.
  @param[in] Mapping  The mapping value returned from Map().

  @retval EFI_SUCCESS           The range was unmapped.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
**/
EFI_STATUS
EFIAPI
MtkUfsUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  )
{
  MTK_UFS_MAPPING  *MapInfo;

  if ((This == NULL) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  MapInfo = (MTK_UFS_MAPPING *)Mapping;

  if (MapInfo->Operation == EdkiiUfsHcOperationBusMasterWrite) {
    //
    // Drop the stale lines again so the consumer reads the device's data.
    //
    InvalidateDataCacheRange (MapInfo->HostAddress, MapInfo->NumberOfBytes);
  }

  FreePool (MapInfo);

  return EFI_SUCCESS;
}

/**
  Flushes all posted write transactions from the UFS bus to the attached
  UFS device.

  @param[in] This  A pointer to the protocol instance.

  @retval EFI_SUCCESS  The posted writes were flushed.
**/
EFI_STATUS
EFIAPI
MtkUfsFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MemoryFence ();

  return EFI_SUCCESS;
}

/**
  Read from the UFS host controller MMIO space.

  @param[in]     This    A pointer to the protocol instance.
  @param[in]     Width   Width of the memory operations.
  @param[in]     Offset  Offset within the UFS host controller MMIO space.
  @param[in]     Count   Number of memory operations to perform.
  @param[in,out] Buffer  Destination buffer for the read.

  @retval EFI_SUCCESS           The data was read.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
**/
EFI_STATUS
EFIAPI
MtkUfsMmioRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  MTK_UFS_HC_PRIVATE_DATA  *Private;
  UINTN                    Address;
  UINTN                    Index;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = MTK_UFS_HC_PRIVATE_DATA_FROM_THIS (This);
  Address = Private->UfsHcBase + (UINTN)Offset;

  switch (Width) {
    case EfiUfsHcWidthUint8:
      for (Index = 0; Index < Count; Index++) {
        ((UINT8 *)Buffer)[Index] = MmioRead8 (Address + Index);
      }

      break;
    case EfiUfsHcWidthUint16:
      for (Index = 0; Index < Count; Index++) {
        ((UINT16 *)Buffer)[Index] = MmioRead16 (Address + Index * sizeof (UINT16));
      }

      break;
    case EfiUfsHcWidthUint32:
      for (Index = 0; Index < Count; Index++) {
        ((UINT32 *)Buffer)[Index] = MmioRead32 (Address + Index * sizeof (UINT32));
      }

      break;
    case EfiUfsHcWidthUint64:
      for (Index = 0; Index < Count; Index++) {
        ((UINT64 *)Buffer)[Index] = MmioRead64 (Address + Index * sizeof (UINT64));
      }

      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Write to the UFS host controller MMIO space.

  @param[in]     This    A pointer to the protocol instance.
  @param[in]     Width   Width of the memory operations.
  @param[in]     Offset  Offset within the UFS host controller MMIO space.
  @param[in]     Count   Number of memory operations to perform.
  @param[in,out] Buffer  Source buffer for the write.

  @retval EFI_SUCCESS           The data was written.
  @retval EFI_INVALID_PARAMETER One or more parameters are invalid.
**/
EFI_STATUS
EFIAPI
MtkUfsMmioWrite (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  MTK_UFS_HC_PRIVATE_DATA  *Private;
  UINTN                    Address;
  UINTN                    Index;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = MTK_UFS_HC_PRIVATE_DATA_FROM_THIS (This);
  Address = Private->UfsHcBase + (UINTN)Offset;

  switch (Width) {
    case EfiUfsHcWidthUint8:
      for (Index = 0; Index < Count; Index++) {
        MmioWrite8 (Address + Index, ((UINT8 *)Buffer)[Index]);
      }

      break;
    case EfiUfsHcWidthUint16:
      for (Index = 0; Index < Count; Index++) {
        MmioWrite16 (Address + Index * sizeof (UINT16), ((UINT16 *)Buffer)[Index]);
      }

      break;
    case EfiUfsHcWidthUint32:
      for (Index = 0; Index < Count; Index++) {
        MmioWrite32 (Address + Index * sizeof (UINT32), ((UINT32 *)Buffer)[Index]);
      }

      break;
    case EfiUfsHcWidthUint64:
      for (Index = 0; Index < Count; Index++) {
        MmioWrite64 (Address + Index * sizeof (UINT64), ((UINT64 *)Buffer)[Index]);
      }

      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC MTK_UFS_HC_PRIVATE_DATA  mMtkUfsHcPrivate = {
  MTK_UFS_HC_PRIVATE_SIGNATURE,
  {
    MtkUfsGetMmioBar,
    MtkUfsAllocateBuffer,
    MtkUfsFreeBuffer,
    MtkUfsMap,
    MtkUfsUnmap,
    MtkUfsFlush,
    MtkUfsMmioRead,
    MtkUfsMmioWrite
  },
  0
};

/**
  Pulse the UFS device hardware reset (RST_n) through the secure monitor.

  The generic driver never resets the attached device, but the mainline
  kernel always does it before enabling the host controller
  (ufshcd_device_reset() in ufshcd_init()) and MediaTek implements it as an
  SMC (ufs_mtk_device_reset()): RST_n low, host controller stopped, >= 10 us
  low pulse, RST_n high, then wait for the device to come back.
**/
STATIC
VOID
MtkUfsDeviceReset (
  VOID
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Base;
  UINT32        Retry;

  Base = mMtkUfsHcPrivate.UfsHcBase;

  //
  // Assert RST_n (active low).
  //
  Args.Arg0 = MTK_SIP_UFS_CONTROL;
  Args.Arg1 = UFS_MTK_SIP_DEVICE_RESET;
  Args.Arg2 = 0;
  Args.Arg3 = 0;
  Args.Arg4 = 0;
  Args.Arg5 = 0;
  Args.Arg6 = 0;
  Args.Arg7 = 0;
  ArmCallSmc (&Args);
  DEBUG ((DEBUG_INFO, "MtkUfsDxe: device reset assert -> 0x%lx\n", Args.Arg0));

  //
  // Stop the host controller in the middle of the device reset (mirrors
  // ufs_mtk_device_reset()).
  //
  MmioWrite32 (Base + MTK_REG_UFS_HCE, 0);
  for (Retry = 100; Retry > 0; Retry--) {
    if ((MmioRead32 (Base + MTK_REG_UFS_HCE) & 1) == 0) {
      break;
    }

    gBS->Stall (10);
  }

  //
  // RST_n must stay low for at least 10 us.
  //
  gBS->Stall (15);

  //
  // Deassert RST_n.
  //
  Args.Arg0 = MTK_SIP_UFS_CONTROL;
  Args.Arg1 = UFS_MTK_SIP_DEVICE_RESET;
  Args.Arg2 = 1;
  Args.Arg3 = 0;
  Args.Arg4 = 0;
  Args.Arg5 = 0;
  Args.Arg6 = 0;
  Args.Arg7 = 0;
  ArmCallSmc (&Args);
  DEBUG ((DEBUG_INFO, "MtkUfsDxe: device reset deassert -> 0x%lx\n", Args.Arg0));

  //
  // Give the device time to respond (the kernel waits 10-15 ms).
  //
  gBS->Stall (15000);
}

/**
  Request the MediaTek UFS reference clock and wait for the acknowledge.

  The reference clock is gated by default; the link cannot come up without
  it (mirrors ufs_mtk_setup_ref_clk() of the mainline driver).
**/
STATIC
VOID
MtkUfsRefClkRequest (
  VOID
  )
{
  UINTN   Base;
  UINT32  Value;
  UINT32  Retry;

  Base = mMtkUfsHcPrivate.UfsHcBase;

  Value = MmioRead32 (Base + MTK_REG_UFS_REFCLK_CTRL);
  MmioWrite32 (Base + MTK_REG_UFS_REFCLK_CTRL, Value | MTK_REFCLK_REQUEST);

  //
  // Wait until the acknowledge bit matches the request bit (3 ms timeout).
  //
  for (Retry = 300; Retry > 0; Retry--) {
    Value = MmioRead32 (Base + MTK_REG_UFS_REFCLK_CTRL);
    if (((Value & MTK_REFCLK_ACK) >> 1) == (Value & MTK_REFCLK_REQUEST)) {
      break;
    }

    gBS->Stall (10);
  }

  DEBUG ((
    DEBUG_INFO,
    "MtkUfsDxe: REFCLK request -> 0x%x (%a)\n",
    MmioRead32 (Base + MTK_REG_UFS_REFCLK_CTRL),
    (Retry > 0) ? "ACK" : "TIMEOUT"
    ));
}

/**
  Send a UIC command directly through the host controller registers.

  Mirrors the kernel ufshcd_send_uic_cmd: the command registers are written
  without waiting for HCS.UCRDY first (the UIC interface is usable while the
  host controller is disabled, e.g. for DME_RESET), then the completion
  status is polled.

  @param[in]  Opcode   UIC command opcode.
  @param[in]  Arg1     UIC command argument 1.
  @param[in]  Arg2     UIC command argument 2.
  @param[in]  Arg3     UIC command argument 3.
  @param[out] OutArg2  Optional buffer for UICCMDARG2 (result code in [7:0]).
  @param[out] OutArg3  Optional buffer for UICCMDARG3 (the DME_GET value).

  @retval EFI_SUCCESS  The command completed.
  @retval EFI_TIMEOUT  The command did not complete in time.
**/
STATIC
EFI_STATUS
MtkUfsSendUicCommand (
  IN  UINT32  Opcode,
  IN  UINT32  Arg1,
  IN  UINT32  Arg2,
  IN  UINT32  Arg3,
  OUT UINT32  *OutArg2,
  OUT UINT32  *OutArg3
  )
{
  UINTN   Base;
  UINT32  Value;
  UINT32  Retry;

  Base = mMtkUfsHcPrivate.UfsHcBase;

  //
  // Some UniPro implementations do not accept a DME command that follows
  // the previous one too closely (mirrors ufshcd_add_delay_before_dme_cmd).
  //
  gBS->Stall (1000);

  //
  // Clear a pending UIC command completion status.
  //
  Value = MmioRead32 (Base + MTK_REG_UFS_IS);
  if ((Value & MTK_IS_UCCS) != 0) {
    MmioWrite32 (Base + MTK_REG_UFS_IS, Value);
  }

  MmioWrite32 (Base + MTK_REG_UFS_UIC_ARG1, Arg1);
  MmioWrite32 (Base + MTK_REG_UFS_UIC_ARG2, Arg2);
  MmioWrite32 (Base + MTK_REG_UFS_UIC_ARG3, Arg3);
  MmioWrite32 (Base + MTK_REG_UFS_UIC_CMD, Opcode);

  //
  // Wait up to one second for the command to complete.
  //
  for (Retry = 1000; Retry > 0; Retry--) {
    Value = MmioRead32 (Base + MTK_REG_UFS_IS);
    if ((Value & MTK_IS_UCCS) != 0) {
      break;
    }

    gBS->Stall (1000);
  }

  if (Retry == 0) {
    return EFI_TIMEOUT;
  }

  if (OutArg2 != NULL) {
    //
    // UICCMDARG2 after completion carries the UIC command result in bits
    // [7:0] (the kernel checks it against MASK_UIC_COMMAND_RESULT).
    //
    *OutArg2 = MmioRead32 (Base + MTK_REG_UFS_UIC_ARG2);
  }

  if (OutArg3 != NULL) {
    //
    // UICCMDARG3 after completion carries the DME_GET attribute value
    // (mirrors the kernel's uic_cmd_compl() and the Samsung UfsDxe).
    //
    *OutArg3 = MmioRead32 (Base + MTK_REG_UFS_UIC_ARG3);
  }

  return EFI_SUCCESS;
}

/**
  Platform callback consumed by UfsPassThruDxe (EDKII_UFS_HC_PLATFORM_PROTOCOL).

  Implements the MediaTek host controller quirks that the generic driver does
  not know about, mirroring the mainline ufs-mediatek driver:

    - PreHce: turn on CLK_CG (cleared by every HCE cycle) and disable
      auto-hibernate; clear HCE and issue DME_RESET + DME_ENABLE as required
      by the UFS 2.0 spec 7.1.1 initialization sequence (the generic driver
      only toggles HCE).
    - PreLinkStartup: take the UniPro out of low power mode, disable host
      TX LCC and disable deep stall (ufs_mtk_pre_link).

  @param[in]     ControllerHandle  Handle of the UFS controller.
  @param[in]     CallbackPhase     When the callback is invoked.
  @param[in,out] CallbackData      EDKII_UFS_HC_DRIVER_INTERFACE.

  @retval EFI_SUCCESS  The platform specific handling completed.
**/
STATIC
EFI_STATUS
EFIAPI
MtkUfsHcPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT32      Value;
  UINT32      Result;
  UINT32      Retry;

  Base = mMtkUfsHcPrivate.UfsHcBase;

  switch (CallbackPhase) {
    case EdkiiUfsHcPreHce:
      //
      // Make sure the reference clock is running before the HCE cycle.
      //
      MtkUfsRefClkRequest ();

      //
      // Reset the attached device before enabling the host controller
      // (mirrors ufshcd_init(): ufshcd_device_reset() runs before
      // ufshcd_hba_enable()).
      //
      MtkUfsDeviceReset ();

      //
      // Turn on CLK_CG before every HCE enable (the HCE cycle clears the
      // bit). It bypasses an abnormal ERR_CHK signal that would otherwise
      // leave the controller unable to bring the link up.
      //
      Value = MmioRead32 (Base + MTK_REG_UFS_XOUFS_CTRL);
      MmioWrite32 (Base + MTK_REG_UFS_XOUFS_CTRL, Value | MTK_XOUFS_CTRL_CLK_CG);

      //
      // Stop the host controller (mirrors ufshcd_hba_stop). When the
      // generic driver sets HCE again the controller runs its own
      // initialization sequence (HCE 1->0->1) which resets and enables the
      // UniPro internally; the driver must NOT send DME_RESET/DME_ENABLE
      // itself (that is only for UFSHCI_QUIRK_BROKEN_HCE platforms such as
      // Exynos, and the UIC command engine does not complete commands while
      // HCE is 0 on this controller).
      //
      MmioWrite32 (Base + MTK_REG_UFS_HCE, 0);
      for (Retry = 100; Retry > 0; Retry--) {
        if ((MmioRead32 (Base + MTK_REG_UFS_HCE) & 1) == 0) {
          break;
        }

        gBS->Stall (10);
      }

      break;

    case EdkiiUfsHcPostHce:
      //
      // The controller initialization sequence (HCE 1->0->1) takes time
      // (MediaTek: ~600 us, ufs_mtk_hce_enable_notify sets
      // hba_enable_delay_us). The generic driver reads HCE back immediately
      // and can observe the stale 1, so wait here and make sure the
      // controller really finished before the link startup runs.
      //
      gBS->Stall (2000);
      for (Retry = 500; Retry > 0; Retry--) {
        if ((MmioRead32 (Base + MTK_REG_UFS_HCE) & 1) != 0) {
          break;
        }

        gBS->Stall (1000);
      }

      DEBUG ((
        DEBUG_INFO,
        "MtkUfsDxe: PostHce HCE=0x%x HCS=0x%x (%a)\n",
        MmioRead32 (Base + MTK_REG_UFS_HCE),
        MmioRead32 (Base + MTK_REG_UFS_HCS),
        (Retry > 0) ? "ready" : "TIMEOUT"
        ));

      //
      // Set the auto-hibernate idle timer to its maximum (timer 1023,
      // scale 5 = 100 ms per tick, ~102 s). AHIT=0 does NOT disable
      // auto-hibernate on this controller - it makes the link hibernate
      // immediately when idle (the previous boot showed link startup
      // errors with AHIT=0 while AHIT=150ms worked). The kernel's 150 ms
      // default is too short for the firmware flow (every DEBUG line takes
      // milliseconds on the framebuffer console), so use the maximum.
      //
      MmioWrite32 (Base + MTK_REG_UFS_AHIT, 0x17FF);
      DEBUG ((DEBUG_INFO, "MtkUfsDxe: PostHce AHIT=0x%x (auto-hibernate ~102 s)\n", MmioRead32 (Base + MTK_REG_UFS_AHIT)));
      break;

    case EdkiiUfsHcPreLinkStartup:
      //
      // ufs_mtk_pre_link: UniPro out of low power mode.
      //
      Result = 0;
      Status = MtkUfsSendUicCommand (MTK_UIC_DME_SET, MTK_UIC_ARG_MIB (MTK_MIB_VS_UNIPROPOWERDOWNCONTROL), 0, 0, &Result, NULL);
      DEBUG ((DEBUG_INFO, "MtkUfsDxe: PreLink DME_SET(0xD0A8)=0 -> %r (result=0x%x)\n", Status, Result & 0xFF));

      //
      // ufs_mtk_pre_link: disable host TX LCC.
      //
      Result = 0;
      Status = MtkUfsSendUicCommand (MTK_UIC_DME_SET, MTK_UIC_ARG_MIB (MTK_MIB_PA_LOCAL_TX_LCC_ENABLE), 0, 0, &Result, NULL);
      DEBUG ((DEBUG_INFO, "MtkUfsDxe: PreLink DME_SET(0x155E)=0 -> %r (result=0x%x)\n", Status, Result & 0xFF));

      //
      // ufs_mtk_pre_link: disable deep stall.
      //
      Result = 0;
      Value  = 0;
      Status = MtkUfsSendUicCommand (MTK_UIC_DME_GET, MTK_UIC_ARG_MIB (MTK_MIB_VS_SAVEPOWERCONTROL), 0, 0, &Result, &Value);
      DEBUG ((DEBUG_INFO, "MtkUfsDxe: PreLink DME_GET(0xD0A6) -> %r (result=0x%x val=0x%x)\n", Status, Result & 0xFF, Value));
      if (!EFI_ERROR (Status) && ((Result & 0xFF) == 0)) {
        Value  &= ~(UINT32)BIT6;
        Status  = MtkUfsSendUicCommand (MTK_UIC_DME_SET, MTK_UIC_ARG_MIB (MTK_MIB_VS_SAVEPOWERCONTROL), 0, Value, &Result, NULL);
        DEBUG ((DEBUG_INFO, "MtkUfsDxe: PreLink DME_SET(0xD0A6)=0x%x -> %r (result=0x%x)\n", Value, Status, Result & 0xFF));
      }

      //
      // The kernel issues the link startup twice on a device that is not
      // active yet ("to make sure the device state move to active") and the
      // generic driver's own attempt follows. The first attempts may report
      // a PHY adapter error - that is expected and deliberately ignored
      // here, the generic attempt is the one that counts.
      //
      Result = 0;
      Status = MtkUfsSendUicCommand (MTK_UIC_DME_LINK_STARTUP, 0, 0, 0, &Result, NULL);
      DEBUG ((DEBUG_INFO, "MtkUfsDxe: PreLink warm-up link startup #1 -> %r (result=0x%x)\n", Status, Result & 0xFF));

      Result = 0;
      Status = MtkUfsSendUicCommand (MTK_UIC_DME_LINK_STARTUP, 0, 0, 0, &Result, NULL);
      DEBUG ((DEBUG_INFO, "MtkUfsDxe: PreLink warm-up link startup #2 -> %r (result=0x%x)\n", Status, Result & 0xFF));

      break;

    case EdkiiUfsHcPostLinkStartup:
      break;

    default:
      return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

//
// Platform protocol consumed by UfsPassThruDxe.
//
STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL  mMtkUfsHcPlatform = {
  EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION,
  NULL,
  MtkUfsHcPlatformCallback,
  EdkiiUfsCardRefClkFreq26Mhz
};

/**
  Entry point of the MediaTek UFS host controller platform driver.

  @param[in] ImageHandle  The firmware allocated handle for the EFI image.
  @param[in] SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS  The protocol was installed.
  @retval others       The driver failed to initialize.
**/
EFI_STATUS
EFIAPI
MtkUfsDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  UINT32                     Value;
  MEMMAP_DEVICE_PATH  *MemMap;
  EFI_DEVICE_PATH_PROTOCOL   *End;

  mMtkUfsHcPrivate.UfsHcBase = FixedPcdGet32 (PcdUfsHostControllerBase);
  if (mMtkUfsHcPrivate.UfsHcBase == 0) {
    DEBUG ((DEBUG_ERROR, "MtkUfsDxe: PcdUfsHostControllerBase is not set\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // MediaTek early setting, mirroring ufs_mtk_hce_enable_notify(PRE_CHANGE):
  // turn on CLK_CG early to bypass an abnormal ERR_CHK signal and prevent a
  // host controller hang.
  //
  Value = MmioRead32 (mMtkUfsHcPrivate.UfsHcBase + MTK_REG_UFS_XOUFS_CTRL);
  MmioWrite32 (
    mMtkUfsHcPrivate.UfsHcBase + MTK_REG_UFS_XOUFS_CTRL,
    Value | MTK_XOUFS_CTRL_CLK_CG
    );

  //
  // Request the reference clock (the link cannot come up without it).
  //
  MtkUfsRefClkRequest ();

  DEBUG ((
    DEBUG_INFO,
    "MtkUfsDxe: UFS host controller at 0x%lx (XOUFS_CTRL=0x%x)\n",
    mMtkUfsHcPrivate.UfsHcBase,
    MmioRead32 (mMtkUfsHcPrivate.UfsHcBase + MTK_REG_UFS_XOUFS_CTRL)
    ));

  //
  // Build a device path for the controller handle. UfsPassThruDxe's
  // Supported() opens EFI_DEVICE_PATH_PROTOCOL on the controller handle, so
  // a platform (non-PCI) controller must carry one; the PCI variant inherits
  // it from the PCI I/O stack.
  //
  MemMap = AllocatePool (sizeof (MEMMAP_DEVICE_PATH) + sizeof (EFI_DEVICE_PATH_PROTOCOL));
  if (MemMap == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  MemMap->Header.Type    = HARDWARE_DEVICE_PATH;
  MemMap->Header.SubType = HW_MEMMAP_DP;
  SetDevicePathNodeLength (&MemMap->Header, sizeof (MEMMAP_DEVICE_PATH));
  MemMap->MemoryType      = EfiMemoryMappedIO;
  MemMap->StartingAddress = mMtkUfsHcPrivate.UfsHcBase;
  MemMap->EndingAddress   = mMtkUfsHcPrivate.UfsHcBase + MTK_UFS_HC_MMIO_SIZE - 1;

  End = (EFI_DEVICE_PATH_PROTOCOL *)((UINTN)MemMap + sizeof (MEMMAP_DEVICE_PATH));
  End->Type    = END_DEVICE_PATH_TYPE;
  End->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength (End, sizeof (EFI_DEVICE_PATH_PROTOCOL));

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  &mMtkUfsHcPrivate.UfsHcProtocol,
                  &gEfiDevicePathProtocolGuid,
                  (EFI_DEVICE_PATH_PROTOCOL *)MemMap,
                  &gEdkiiUfsHcPlatformProtocolGuid,
                  &mMtkUfsHcPlatform,
                  NULL
                  );
  DEBUG ((DEBUG_INFO, "MtkUfsDxe: protocol install: %r\n", Status));

  return Status;
}
