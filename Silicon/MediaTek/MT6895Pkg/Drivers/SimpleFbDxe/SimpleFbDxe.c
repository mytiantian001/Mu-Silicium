/* SimpleFbDxe: Simple FrameBuffer */
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <PiDxe.h>
#include <Protocol/GraphicsOutput.h>
#include <Uefi.h>

/// Defines
/*
 * Convert enum video_log2_bpp to bytes and bits. Note we omit the outer
 * brackets to allow multiplication by fractional pixels.
 */
#define VNBYTES(bpix) (1 << (bpix)) / 8
#define VNBITS(bpix) (1 << (bpix))

#define FB_BITS_PER_PIXEL (32)
#define FB_BYTES_PER_PIXEL (FB_BITS_PER_PIXEL / 8)

/*
 * Bits per pixel selector. Each value n is such that the bits-per-pixel is
 * 2 ^ n
 */
enum video_log2_bpp {
  VIDEO_BPP1 = 0,
  VIDEO_BPP2,
  VIDEO_BPP4,
  VIDEO_BPP8,
  VIDEO_BPP16,
  VIDEO_BPP32,
};

typedef struct {
  VENDOR_DEVICE_PATH DisplayDevicePath;
  EFI_DEVICE_PATH    EndDevicePath;
} DISPLAY_DEVICE_PATH;

DISPLAY_DEVICE_PATH mDisplayDevicePath = {
    {{HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
          (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
          (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      }},
     EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID},
    {END_DEVICE_PATH_TYPE,
     END_ENTIRE_DEVICE_PATH_SUBTYPE,
     {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};

/// Declares

STATIC FRAME_BUFFER_CONFIGURE *mFrameBufferBltLibConfigure;
STATIC UINTN                   mFrameBufferBltLibConfigureSize;

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber);

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *            BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL);

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mDisplay = {
    DisplayQueryMode, DisplaySetMode, DisplayBlt, NULL};

/* MTK display OVL register layout (shared by the mt6789/mt6895/mt8183
 * families). The OVL base addresses are platform PCDs. */
#define MTK_OVL_LAYER_COUNT 4U
#define MTK_OVL_ENABLE      0x2CU
#define MTK_OVL_CON(n)      (0x30U + (0x20U * (n)))
#define MTK_OVL_PITCH(n)    (0x44U + (0x20U * (n)))
#define MTK_OVL_ADDR(n)     (0xF40U + (0x20U * (n)))
#define MTK_OVL_FORMAT_MASK 0x010F000U
#define MTK_OVL_RGBA8888    (2U << 12)
#define MTK_OVL_BYTE_SWAP   (1U << 24)

STATIC
VOID
MtkAdoptOverlay(
    IN UINT32                  OverlayBase,
    IN EFI_PHYSICAL_ADDRESS    FrameBufferAddress,
    IN UINT32                  FrameBufferSize,
    IN UINT32                  FrameBufferStride)
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
        Address >= FrameBufferAddress + (3ULL * FrameBufferSize)) {
      continue;
    }

    Con = MmioRead32 (OverlayBase + MTK_OVL_CON (Layer));
    Con = (Con & ~MTK_OVL_FORMAT_MASK) |
          MTK_OVL_RGBA8888 | MTK_OVL_BYTE_SWAP;
    MmioWrite32 (OverlayBase + MTK_OVL_CON (Layer), Con);
    MmioWrite32 (OverlayBase + MTK_OVL_PITCH (Layer), FrameBufferStride);
    MmioWrite32 (OverlayBase + MTK_OVL_ADDR (Layer), (UINT32)FrameBufferAddress);
  }
}

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;
  Status = gBS->AllocatePool(
      EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
      (VOID **)Info);

  ASSERT_EFI_ERROR(Status);

  *SizeOfInfo                   = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *            BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL)
{
  RETURN_STATUS Status;
  EFI_TPL       Tpl;
  UINT32        *FrameBuffer;
  UINT32         PixelsPerScanLine;
  UINTN          Row;
  UINTN          Column;

  __builtin_prefetch((const void *)(UINTN)mDisplay.Mode->FrameBufferBase, 1, 3);

  //
  // We have to raise to TPL_NOTIFY, so we make an atomic write to the frame
  // buffer. We would not want a timer based event (Cursor, ...) to come in
  // while we are doing this operation.
  //
  Tpl = gBS->RaiseTPL(TPL_NOTIFY);

  Status = FrameBufferBlt(
      mFrameBufferBltLibConfigure, BltBuffer, BltOperation, SourceX, SourceY,
      DestinationX, DestinationY, Width, Height, Delta);

  gBS->RestoreTPL(Tpl);

  // a8b8g8r8 has the reserved/alpha byte at bits 31:24.  The generic
  // FrameBufferBltLib conversion writes the RGB channels but leaves this byte
  // zero; MTK OVL then treats the text as transparent.  Only repair the
  // region actually written to video.  Never use DestinationX/Y for
  // VideoToBuffer, where they refer to the caller's BltBuffer.
  if (!RETURN_ERROR(Status) &&
      BltOperation == EfiBltBufferToVideo &&
      mDisplay.Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
    FrameBuffer = (UINT32 *)(UINTN)mDisplay.Mode->FrameBufferBase;
    PixelsPerScanLine = mDisplay.Mode->Info->PixelsPerScanLine;
    for (Row = 0; Row < Height; Row++) {
      for (Column = 0; Column < Width; Column++) {
        FrameBuffer[(DestinationY + Row) * PixelsPerScanLine + DestinationX + Column] |= 0xFF000000;
      }
    }
  }

  return RETURN_ERROR(Status) ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SimpleFbDxeInitialize(
    IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status             = EFI_SUCCESS;
  EFI_HANDLE hUEFIDisplayHandle = NULL;

  /* Retrieve simple frame buffer from pre-SEC bootloader */
  DEBUG(
      (EFI_D_ERROR,
       "SimpleFbDxe: Retrieve MIPI FrameBuffer parameters from PCD\n"));
  UINT32 MipiFrameBufferAddr   = FixedPcdGet32(PcdMipiFrameBufferAddress);
  UINT32 MipiFrameBufferWidth  = FixedPcdGet32(PcdMipiFrameBufferWidth);
  UINT32 MipiFrameBufferHeight = FixedPcdGet32(PcdMipiFrameBufferHeight);
  UINT32 MipiFrameBufferVisibleWidth = FixedPcdGet32(PcdMipiFrameBufferVisibleWidth);
  UINT32 MipiFrameBufferPixelFormat = FixedPcdGet32(PcdMipiFrameBufferPixelFormat);
  UINT32 MipiFrameBufferLayers = FixedPcdGet32(PcdMipiFrameBufferLayers);

  if (MipiFrameBufferVisibleWidth == 0 || MipiFrameBufferVisibleWidth > MipiFrameBufferWidth) {
    MipiFrameBufferVisibleWidth = MipiFrameBufferWidth;
  }

  /* Sanity check */
  if (MipiFrameBufferAddr == 0 || MipiFrameBufferWidth == 0 ||
      MipiFrameBufferHeight == 0) {
    DEBUG((EFI_D_ERROR, "SimpleFbDxe: Invalid FrameBuffer parameters\n"));
    return EFI_DEVICE_ERROR;
  }

  /* Prepare struct */
  if (mDisplay.Mode == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE),
        (VOID **)&mDisplay.Mode);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));
  }

  if (mDisplay.Mode->Info == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
        (VOID **)&mDisplay.Mode->Info);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode->Info, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));
  }

  /* Set information */
  mDisplay.Mode->MaxMode       = 1;
  mDisplay.Mode->Mode          = 0;
  mDisplay.Mode->Info->Version = 0;

  mDisplay.Mode->Info->HorizontalResolution = MipiFrameBufferVisibleWidth;
  mDisplay.Mode->Info->VerticalResolution   = MipiFrameBufferHeight;

  /* SimpleFB runs on a8r8g8b8 (VIDEO_BPP32) for DB410c */
  UINT32               LineLength = MipiFrameBufferWidth * VNBYTES(VIDEO_BPP32);
  UINT32               FrameBufferSize    = LineLength * MipiFrameBufferHeight;
  EFI_PHYSICAL_ADDRESS FrameBufferAddress = MipiFrameBufferAddr;

  /* MipiFrameBufferWidth is the scan-buffer pitch (pixels per scanline). */
  mDisplay.Mode->Info->PixelsPerScanLine = MipiFrameBufferWidth;
  mDisplay.Mode->Info->PixelFormat =
      (MipiFrameBufferPixelFormat == 1) ? PixelRedGreenBlueReserved8BitPerColor
                                        : PixelBlueGreenRedReserved8BitPerColor;
  mDisplay.Mode->SizeOfInfo      = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  mDisplay.Mode->FrameBufferBase = FrameBufferAddress;
  mDisplay.Mode->FrameBufferSize = FrameBufferSize;

  //
  // Create the FrameBufferBltLib configuration.
  //
  Status = FrameBufferBltConfigure(
      (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
      mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);
  if (Status == RETURN_BUFFER_TOO_SMALL) {
    mFrameBufferBltLibConfigure = AllocatePool(mFrameBufferBltLibConfigureSize);
    if (mFrameBufferBltLibConfigure != NULL) {
      Status = FrameBufferBltConfigure(
          (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
          mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);
    }
  }
  ASSERT_EFI_ERROR(Status);

  // Match the mainline simplefb handoff: LK may leave an enabled overlay
  // layer with a stale address, pitch, or pixel format.  Repoint only
  // layers belonging to the three framebuffer slots.  The OVL bases are
  // platform PCDs; 0 disables the takeover.
  UINT32 Ovl0Base    = FixedPcdGet32 (PcdMipiFrameBufferOvl0Base);
  UINT32 Ovl0_2LBase = FixedPcdGet32 (PcdMipiFrameBufferOvl0_2LBase);
  if (Ovl0Base != 0) {
    MtkAdoptOverlay (Ovl0Base, FrameBufferAddress,
                     FrameBufferSize, LineLength);
  }
  if (Ovl0_2LBase != 0) {
    MtkAdoptOverlay (Ovl0_2LBase, FrameBufferAddress,
                     FrameBufferSize, LineLength);
  }

  // zhuowei: clear the screen to black
  // UEFI standard requires this, since text is white - see
  // OvmfPkg/QemuVideoDxe/Gop.c
  for (UINT32 i = 0; i < MipiFrameBufferLayers; i++) {
    ZeroMem((void *)(FrameBufferAddress + (FrameBufferSize * i)), FrameBufferSize);
  }

  /* Register handle */
  Status = gBS->InstallMultipleProtocolInterfaces(
      &hUEFIDisplayHandle, &gEfiDevicePathProtocolGuid, &mDisplayDevicePath,
      &gEfiGraphicsOutputProtocolGuid, &mDisplay, NULL);

  ASSERT_EFI_ERROR(Status);

  return Status;
}
