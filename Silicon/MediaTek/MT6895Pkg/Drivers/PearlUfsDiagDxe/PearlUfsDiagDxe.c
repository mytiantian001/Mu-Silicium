/** @file
  Pearl UFS read/write test application.

  This application deliberately does not depend on Windows or a BCD.  It
  enumerates every BlockIo handle, reads several LBAs from each medium, then
  exercises the writable FAT volume by creating, writing, closing, reopening,
  reading back, comparing and deleting a test file.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/FileSystemInfo.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

#define UFS_TEST_FILE_NAME  L"\\UFS_TEST.BIN"
#define UFS_TEST_FILE_SIZE  (64 * 1024)

STATIC UINTN       mBlockHandleCount;
STATIC UINTN       mBlockReadPass;
STATIC UINTN       mBlockReadFail;
STATIC UINTN       mFileSystemCount;
STATIC EFI_STATUS  mFileWriteStatus  = EFI_NOT_READY;
STATIC EFI_STATUS  mFileReadStatus   = EFI_NOT_READY;
STATIC EFI_STATUS  mFileCompareStatus = EFI_NOT_READY;
STATIC EFI_STATUS  mFileDeleteStatus = EFI_NOT_READY;
STATIC EFI_STATUS  mReportStatus     = EFI_NOT_READY;
STATIC UINTN       mLastBlockFailureIndex;
STATIC UINT64      mLastBlockFailureLba;
STATIC EFI_STATUS  mLastBlockFailureStatus = EFI_SUCCESS;
STATIC EFI_STATUS  mRawWriteStatus         = EFI_NOT_READY;
STATIC EFI_STATUS  mRawReadbackStatus      = EFI_NOT_READY;
STATIC EFI_STATUS  mRawRestoreStatus       = EFI_NOT_READY;
STATIC EFI_STATUS  mRawMarkerStatus        = EFI_NOT_READY;
STATIC UINT64      mRawMarkerLba;

STATIC
VOID
RecordBlockFailure (
  IN UINTN       Index,
  IN UINT64      Lba,
  IN EFI_STATUS  Status
  )
{
  mLastBlockFailureIndex  = Index;
  mLastBlockFailureLba    = Lba;
  mLastBlockFailureStatus = Status;
}

STATIC
VOID
PrintHexBytes (
  IN CONST UINT8  *Data,
  IN UINTN         Length
  )
{
  UINTN  Index;

  for (Index = 0; Index < Length; Index++) {
    Print (L"%02x", Data[Index]);
  }
}

STATIC
VOID
PrintDevicePath (
  IN EFI_HANDLE  Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  CHAR16                    *Text;

  DevicePath = DevicePathFromHandle (Handle);
  Text       = DevicePath == NULL ? NULL : ConvertDevicePathToText (DevicePath, TRUE, TRUE);
  Print (L"%s", Text == NULL ? L"<no device path>" : Text);
  if (Text != NULL) {
    FreePool (Text);
  }
}

STATIC
BOOLEAN
IsUfsTestVolume (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS            Status;
  EFI_FILE_SYSTEM_INFO  *Info;
  UINTN                 InfoSize;
  BOOLEAN               Match;

  InfoSize = SIZE_OF_EFI_FILE_SYSTEM_INFO + (64 * sizeof (CHAR16));
  Info     = AllocatePool (InfoSize);
  if (Info == NULL) {
    return FALSE;
  }

  Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &InfoSize, Info);
  Match  = !EFI_ERROR (Status) && (StrnCmp (Info->VolumeLabel, L"UFS_TEST", 8) == 0);
  FreePool (Info);
  return Match;
}

STATIC
VOID
WriteRawMarker (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  VOID                *Buffer;
  UINTN                Pages;
  UINTN                Alignment;
  UINT8               *Bytes;
  UINTN                Index;
  EFI_STATUS           Status;

  Media = BlockIo->Media;
  if ((Media == NULL) || (Media->BlockSize == 0) || (Media->LastBlock < 32)) {
    Print (L"  raw marker: media too small [SKIP]\r\n");
    return;
  }

  mRawMarkerLba = Media->LastBlock - 9;
  Alignment     = EFI_PAGE_SIZE;
  if (Media->IoAlign > Alignment) {
    Alignment = Media->IoAlign;
  }

  Pages  = EFI_SIZE_TO_PAGES (Media->BlockSize);
  Buffer = AllocateAlignedPages (Pages, Alignment);
  if (Buffer == NULL) {
    Print (L"  raw marker: allocation failed [FAIL]\r\n");
    mRawMarkerStatus = EFI_OUT_OF_RESOURCES;
    return;
  }

  Bytes = (UINT8 *)Buffer;
  for (Index = 0; Index < Media->BlockSize; Index++) {
    Bytes[Index] = (UINT8)(0x3C ^ (Index * 5));
  }

  CopyMem (Bytes, "PEARL-UFS-WRITE-MARKER-V3", 25);
  Bytes[25] = (UINT8)(mRawMarkerLba >> 0);
  Bytes[26] = (UINT8)(mRawMarkerLba >> 8);
  Bytes[27] = (UINT8)(mRawMarkerLba >> 16);
  Bytes[28] = (UINT8)(mRawMarkerLba >> 24);

  Print (L"  raw marker write: LBA 0x%lx block 0x%x\r\n", mRawMarkerLba, Media->BlockSize);
  Status = BlockIo->WriteBlocks (BlockIo, Media->MediaId, mRawMarkerLba, Media->BlockSize, Buffer);
  mRawMarkerStatus = Status;
  Print (L"    write: %r\r\n", Status);
  if (!EFI_ERROR (Status) && (BlockIo->FlushBlocks != NULL)) {
    Status = BlockIo->FlushBlocks (BlockIo);
    Print (L"    flush: %r\r\n", Status);
    if (EFI_ERROR (Status)) {
      mRawMarkerStatus = Status;
    }
  }

  FreePages (Buffer, Pages);
}

STATIC
VOID
RunRawBlockWriteTest (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  VOID                *Original;
  VOID                *Pattern;
  VOID                *Readback;
  UINTN                Pages;
  UINTN                Alignment;
  UINT64               Lba;
  UINT8               *Bytes;
  UINTN                Index;
  EFI_STATUS           Status;
  BOOLEAN              Match;

  Media = BlockIo->Media;
  if ((Media == NULL) || (Media->BlockSize == 0) || (Media->LastBlock < 16)) {
    Print (L"  raw write test: media too small [SKIP]\r\n");
    return;
  }

  //
  // UFS_TEST is an empty, dedicated volume. Use a block near the end so the
  // FAT metadata and the first data clusters are never touched. The original
  // contents are restored after the test.
  //
  Lba = Media->LastBlock - 8;
  if (Lba <= Media->LastBlock / 2) {
    Lba = Media->LastBlock / 2;
  }

  Alignment = EFI_PAGE_SIZE;
  if (Media->IoAlign > Alignment) {
    Alignment = Media->IoAlign;
  }

  Pages    = EFI_SIZE_TO_PAGES (Media->BlockSize);
  Original = AllocateAlignedPages (Pages, Alignment);
  Pattern  = AllocateAlignedPages (Pages, Alignment);
  Readback = AllocateAlignedPages (Pages, Alignment);
  if ((Original == NULL) || (Pattern == NULL) || (Readback == NULL)) {
    Print (L"  raw write test: allocation failed [FAIL]\r\n");
    mRawWriteStatus = EFI_OUT_OF_RESOURCES;
    if (Original != NULL) {
      FreePages (Original, Pages);
    }
    if (Pattern != NULL) {
      FreePages (Pattern, Pages);
    }
    if (Readback != NULL) {
      FreePages (Readback, Pages);
    }
    return;
  }

  Print (L"  raw block write test: LBA 0x%lx, block 0x%x\r\n", Lba, Media->BlockSize);

  Status = BlockIo->ReadBlocks (BlockIo, Media->MediaId, Lba, Media->BlockSize, Original);
  if (EFI_ERROR (Status)) {
    Print (L"    read original: %r [FAIL]\r\n", Status);
    mRawWriteStatus = Status;
    goto Done;
  }

  Bytes = (UINT8 *)Pattern;
  for (Index = 0; Index < Media->BlockSize; Index++) {
    Bytes[Index] = (UINT8)(0xA5 ^ (Index * 7) ^ (Index >> 4));
  }

  Status = BlockIo->WriteBlocks (BlockIo, Media->MediaId, Lba, Media->BlockSize, Pattern);
  mRawWriteStatus = Status;
  Print (L"    write pattern: %r\r\n", Status);
  if (EFI_ERROR (Status)) {
    goto Restore;
  }

  if (BlockIo->FlushBlocks != NULL) {
    Status = BlockIo->FlushBlocks (BlockIo);
    Print (L"    flush after write: %r\r\n", Status);
    if (EFI_ERROR (Status)) {
      mRawWriteStatus = Status;
      goto Restore;
    }
  }

  Status = BlockIo->ReadBlocks (BlockIo, Media->MediaId, Lba, Media->BlockSize, Readback);
  mRawReadbackStatus = Status;
  Match = !EFI_ERROR (Status) &&
          (CompareMem (Pattern, Readback, Media->BlockSize) == 0);
  Print (L"    read back and compare: %r [%s]\r\n", Status, Match ? L"PASS" : L"FAIL");
  if (!Match && !EFI_ERROR (Status)) {
    mRawReadbackStatus = EFI_DEVICE_ERROR;
  }

Restore:
  Status = BlockIo->WriteBlocks (BlockIo, Media->MediaId, Lba, Media->BlockSize, Original);
  mRawRestoreStatus = Status;
  Print (L"    restore original: %r\r\n", Status);
  if (!EFI_ERROR (Status) && (BlockIo->FlushBlocks != NULL)) {
    BlockIo->FlushBlocks (BlockIo);
  }

  if (!EFI_ERROR (mRawRestoreStatus)) {
    Status = BlockIo->ReadBlocks (BlockIo, Media->MediaId, Lba, Media->BlockSize, Readback);
    if (EFI_ERROR (Status) || (CompareMem (Original, Readback, Media->BlockSize) != 0)) {
      mRawRestoreStatus = EFI_DEVICE_ERROR;
      Print (L"    verify restore: %r [FAIL]\r\n", Status);
    } else {
      Print (L"    verify restore: %r [PASS]\r\n", Status);
    }
  }

Done:
  if (!EFI_ERROR (mRawWriteStatus) && EFI_ERROR (mRawReadbackStatus)) {
    mRawWriteStatus = mRawReadbackStatus;
  }
  if (!EFI_ERROR (mRawWriteStatus) && EFI_ERROR (mRawRestoreStatus)) {
    mRawWriteStatus = mRawRestoreStatus;
  }

  FreePages (Original, Pages);
  FreePages (Pattern, Pages);
  FreePages (Readback, Pages);
}

STATIC
VOID
RunBlockReadTest (
  IN EFI_HANDLE              Handle,
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINTN                   Index
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  VOID                *Buffer;
  UINTN                Pages;
  UINTN                Alignment;
  UINT64               Lba[3];
  UINTN                LbaCount;
  UINTN                LbaIndex;
  UINTN                Existing;
  EFI_STATUS           Status;

  Media = BlockIo->Media;
  if ((Media == NULL) || !Media->MediaPresent || (Media->BlockSize == 0)) {
    Print (L"  BLOCK%u: no media\n", Index);
    RecordBlockFailure (Index, MAX_UINT64, EFI_NO_MEDIA);
    mBlockReadFail++;
    return;
  }

  PrintDevicePath (Handle);
  Print (
    L"\n  BLOCK%u: size=0x%x last=0x%lx ro=%d logical=%d align=0x%x\n",
    Index,
    Media->BlockSize,
    Media->LastBlock,
    Media->ReadOnly,
    Media->LogicalPartition,
    Media->IoAlign
    );

  if (Media->BlockSize > 0x10000) {
    Print (L"  BLOCK%u: block size too large for test\n", Index);
    RecordBlockFailure (Index, MAX_UINT64, EFI_BAD_BUFFER_SIZE);
    mBlockReadFail++;
    return;
  }

  Alignment = EFI_PAGE_SIZE;
  if (Media->IoAlign > Alignment) {
    Alignment = Media->IoAlign;
  }

  Pages  = EFI_SIZE_TO_PAGES (Media->BlockSize);
  Buffer = AllocateAlignedPages (Pages, Alignment);
  if (Buffer == NULL) {
    Print (L"  BLOCK%u: buffer allocation failed\n", Index);
    RecordBlockFailure (Index, MAX_UINT64, EFI_OUT_OF_RESOURCES);
    mBlockReadFail++;
    return;
  }

  Lba[0]   = 0;
  Lba[1]   = Media->LastBlock / 2;
  Lba[2]   = Media->LastBlock;
  LbaCount = 0;
  for (LbaIndex = 0; LbaIndex < ARRAY_SIZE (Lba); LbaIndex++) {
    for (Existing = 0; Existing < LbaCount; Existing++) {
      if (Lba[Existing] == Lba[LbaIndex]) {
        break;
      }
    }

    if (Existing == LbaCount) {
      Lba[LbaCount++] = Lba[LbaIndex];
    }
  }

  for (LbaIndex = 0; LbaIndex < LbaCount; LbaIndex++) {
    ZeroMem (Buffer, Media->BlockSize);
    Status = BlockIo->ReadBlocks (
                               BlockIo,
                               Media->MediaId,
                               Lba[LbaIndex],
                               Media->BlockSize,
                               Buffer
                               );
    if (EFI_ERROR (Status)) {
      Print (L"    read LBA 0x%lx: %r [FAIL]\n", Lba[LbaIndex], Status);
      RecordBlockFailure (Index, Lba[LbaIndex], Status);
      mBlockReadFail++;
    } else {
      Print (L"    read LBA 0x%lx: %r head=", Lba[LbaIndex], Status);
      PrintHexBytes (Buffer, Media->BlockSize < 8 ? Media->BlockSize : 8);
      Print (L" [PASS]\n");
      mBlockReadPass++;
    }
  }

  FreePages (Buffer, Pages);
}

STATIC
EFI_STATUS
RunFileReadWriteTest (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS        Status;
  EFI_FILE_PROTOCOL *File;
  UINT8             *WriteBuffer;
  UINT8             *ReadBuffer;
  UINTN             Index;
  UINTN             Size;
  UINTN             ReadSize;
  BOOLEAN           Match;

  WriteBuffer = AllocatePool (UFS_TEST_FILE_SIZE);
  ReadBuffer  = AllocatePool (UFS_TEST_FILE_SIZE);
  if ((WriteBuffer == NULL) || (ReadBuffer == NULL)) {
    if (WriteBuffer != NULL) {
      FreePool (WriteBuffer);
    }
    if (ReadBuffer != NULL) {
      FreePool (ReadBuffer);
    }
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < UFS_TEST_FILE_SIZE; Index++) {
    WriteBuffer[Index] = (UINT8)(0x5A ^ (Index * 13) ^ (Index >> 8));
  }

  File   = NULL;
  Status = Root->Open (Root, &File, UFS_TEST_FILE_NAME, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    File->Delete (File);
  }

  File   = NULL;
  Status = Root->Open (
                    Root,
                    &File,
                    UFS_TEST_FILE_NAME,
                    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                    0
                    );
  if (EFI_ERROR (Status)) {
    Print (L"  file create: %r [FAIL]\n", Status);
    mFileWriteStatus = Status;
    FreePool (WriteBuffer);
    FreePool (ReadBuffer);
    return Status;
  }

  Size = UFS_TEST_FILE_SIZE;
  mFileWriteStatus = File->Write (File, &Size, WriteBuffer);
  Print (L"  file write 64 KiB: %r (%u bytes)\n", mFileWriteStatus, Size);
  if (!EFI_ERROR (mFileWriteStatus)) {
    Status = File->Flush (File);
    Print (L"  file flush: %r\n", Status);
    if (EFI_ERROR (Status)) {
      mFileWriteStatus = Status;
    }
  }

  Status = File->Close (File);
  if (EFI_ERROR (Status) && !EFI_ERROR (mFileWriteStatus)) {
    mFileWriteStatus = Status;
  }
  if (EFI_ERROR (mFileWriteStatus)) {
    FreePool (WriteBuffer);
    FreePool (ReadBuffer);
    return mFileWriteStatus;
  }

  File   = NULL;
  Status = Root->Open (Root, &File, UFS_TEST_FILE_NAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Print (L"  file reopen: %r [FAIL]\n", Status);
    mFileReadStatus = Status;
    FreePool (WriteBuffer);
    FreePool (ReadBuffer);
    return Status;
  }

  ReadSize         = UFS_TEST_FILE_SIZE;
  mFileReadStatus  = File->Read (File, &ReadSize, ReadBuffer);
  if (!EFI_ERROR (mFileReadStatus) && (ReadSize != UFS_TEST_FILE_SIZE)) {
    mFileReadStatus = EFI_DEVICE_ERROR;
  }
  Print (L"  file read 64 KiB: %r (%u bytes)\n", mFileReadStatus, ReadSize);

  Match = !EFI_ERROR (mFileReadStatus) &&
          (CompareMem (WriteBuffer, ReadBuffer, UFS_TEST_FILE_SIZE) == 0);
  mFileCompareStatus = Match ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  Print (L"  file compare: %r [%s]\n", mFileCompareStatus, Match ? L"PASS" : L"FAIL");

  File->Close (File);
  File   = NULL;
  Status = Root->Open (Root, &File, UFS_TEST_FILE_NAME, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    mFileDeleteStatus = File->Delete (File);
  } else {
    mFileDeleteStatus = Status;
  }
  Print (L"  file cleanup: %r\n", mFileDeleteStatus);

  FreePool (WriteBuffer);
  FreePool (ReadBuffer);
  return mFileCompareStatus;
}

STATIC
EFI_STATUS
WriteReport (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS        Status;
  EFI_FILE_PROTOCOL *File;
  CHAR16            Report[1024];
  UINTN             ReportSize;

  ReportSize = UnicodeSPrint (
                 Report,
                 sizeof (Report),
                 L"Pearl UFS test\r\n"
                 L"BlockIo handles: %u\r\n"
                 L"Block reads pass/fail: %u/%u\r\n"
                 L"File write: %r\r\n"
                 L"File read: %r\r\n"
                 L"File compare: %r\r\n"
                 L"File cleanup: %r\r\n"
                 L"Raw marker: %r at LBA 0x%lx\r\n"
                 L"Raw block write: %r\r\n"
                 L"Raw block readback: %r\r\n"
                 L"Raw block restore: %r\r\n"
                 L"Last block failure: index=%u lba=0x%lx status=%r\r\n",
                 mBlockHandleCount,
                 mBlockReadPass,
                 mBlockReadFail,
                 mFileWriteStatus,
                 mFileReadStatus,
                 mFileCompareStatus,
                 mFileDeleteStatus,
                 mRawMarkerStatus,
                 mRawMarkerLba,
                 mRawWriteStatus,
                 mRawReadbackStatus,
                 mRawRestoreStatus,
                 mLastBlockFailureIndex,
                 mLastBlockFailureLba,
                 mLastBlockFailureStatus
                 );

  //
  // UnicodeSPrint returns the number of UTF-16 characters, not bytes.
  // EFI_FILE_PROTOCOL.Write expects a byte count.
  //
  ReportSize = StrLen (Report) * sizeof (CHAR16);

  File   = NULL;
  Status = Root->Open (
                    Root,
                    &File,
                    L"\\ufs-test.txt",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                    0
                    );
  if (!EFI_ERROR (Status)) {
    File->Delete (File);
  }

  File   = NULL;
  Status = Root->Open (
                    Root,
                    &File,
                    L"\\ufs-test.txt",
                    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                    0
                    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->Write (File, &ReportSize, Report);
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }
  File->Close (File);
  return Status;
}

EFI_STATUS
EFIAPI
PearlUfsDiagEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_BLOCK_IO_PROTOCOL            *BlockIo;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_DEVICE_PATH_PROTOCOL         *DevicePath;
  CHAR16                           *Text;
  BOOLEAN                          FileTestDone;

  Print (L"\r\n===== Pearl UFS READ/WRITE TEST =====\r\n");
  Print (L"Windows/PE test is disabled; this is a storage-only test.\r\n\r\n");

  // The test application is itself the first boot option.  BDS has not
  // necessarily connected the UFS controller and its child buses yet.
  Print (L"Connecting UFS/SCSI controllers...\r\n");
  EfiBootManagerConnectAll ();
  Print (L"Controller connection complete.\r\n\r\n");

  Handles     = NULL;
  HandleCount = 0;
  Status      = gBS->LocateHandleBuffer (
                       ByProtocol,
                       &gEfiBlockIoProtocolGuid,
                       NULL,
                       &HandleCount,
                       &Handles
                       );
  if (EFI_ERROR (Status)) {
    Print (L"BlockIo enumeration: %r [FAIL]\r\n", Status);
  } else {
    mBlockHandleCount = HandleCount;
    Print (L"BlockIo handles: %u\r\n", HandleCount);
    for (Index = 0; Index < HandleCount; Index++) {
      BlockIo = NULL;
      Status  = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
      if (!EFI_ERROR (Status) && (BlockIo != NULL)) {
        RunBlockReadTest (Handles[Index], BlockIo, Index);
      } else {
        Print (L"BLOCK%u protocol: %r [FAIL]\r\n", Index, Status);
        RecordBlockFailure (Index, MAX_UINT64, Status);
        mBlockReadFail++;
      }
    }
    FreePool (Handles);
  }

  FileTestDone = FALSE;
  Handles      = NULL;
  HandleCount  = 0;
  Status       = gBS->LocateHandleBuffer (
                       ByProtocol,
                       &gEfiSimpleFileSystemProtocolGuid,
                       NULL,
                       &HandleCount,
                       &Handles
                       );
  if (EFI_ERROR (Status)) {
    Print (L"SimpleFileSystem enumeration: %r [FAIL]\r\n", Status);
  } else {
    mFileSystemCount = HandleCount;
    Print (L"SimpleFileSystem handles: %u\r\n", HandleCount);
    for (Index = 0; Index < HandleCount; Index++) {
      Fs = NULL;
      Status = gBS->HandleProtocol (
                          Handles[Index],
                          &gEfiSimpleFileSystemProtocolGuid,
                          (VOID **)&Fs
                          );
      if (EFI_ERROR (Status) || (Fs == NULL)) {
        continue;
      }

      DevicePath = DevicePathFromHandle (Handles[Index]);
      Text       = DevicePath == NULL ? NULL : ConvertDevicePathToText (DevicePath, TRUE, TRUE);
      Print (L"FS%u: %s\r\n", Index, Text == NULL ? L"<no device path>" : Text);
      if (Text != NULL) {
        FreePool (Text);
      }

      BlockIo = NULL;
      Status  = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
      if (FileTestDone || (EFI_ERROR (Status)) || (BlockIo == NULL) ||
          (BlockIo->Media == NULL) || BlockIo->Media->ReadOnly) {
        Print (L"  skip file write test: %r or read-only\r\n", Status);
        continue;
      }

      Root   = NULL;
      Status = Fs->OpenVolume (Fs, &Root);
      if (EFI_ERROR (Status)) {
        Print (L"  OpenVolume: %r [FAIL]\r\n", Status);
        continue;
      }

      if (!IsUfsTestVolume (Root)) {
        Print (L"  not UFS_TEST volume; skip write tests\r\n");
        Root->Close (Root);
        continue;
      }

      FileTestDone = TRUE;
      WriteRawMarker (BlockIo);
      RunRawBlockWriteTest (BlockIo);
      RunFileReadWriteTest (Root);
      mReportStatus = WriteReport (Root);
      Print (L"  report ufs-test.txt: %r\r\n", mReportStatus);
      if (BlockIo->FlushBlocks != NULL) {
        Status = BlockIo->FlushBlocks (BlockIo);
        Print (L"  flush volume: %r\r\n", Status);
      }
      Root->Close (Root);
    }
    FreePool (Handles);
  }

  if (!FileTestDone) {
    mFileWriteStatus = EFI_NOT_FOUND;
    Print (L"No writable SimpleFileSystem found; file test not run.\r\n");
  }

  Print (
    L"\r\nRESULT: block reads %u PASS / %u FAIL; raw write %r; file compare %r\r\n",
    mBlockReadPass,
    mBlockReadFail,
    mRawWriteStatus,
    mFileCompareStatus
    );
  Print (L"Test complete. Hold Power to reboot; output remains on screen.\r\n");

  // Do not fall through to another boot option and clear the test output.
  while (TRUE) {
    gBS->Stall (1000000);
  }
}
