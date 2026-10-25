/** @file
  Pearl MT6363 PMIC SPMI read diagnostic.

  Tries each PMIF SWINF channel with a single-byte SPMI register read of the
  MT6363 TOPSTATUS register (SID 4, address 0x1e).  The valid channel is the
  one that returns a stable value instead of timing out or returning garbage.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define PMIF_BASE          0x1C804000

#define PMIF_INIT_DONE     0x0000
#define PMIF_INF_EN        0x0024
#define PMIF_ARB_EN        0x0150
#define PMIF_CMDISSUE_EN   0x03B8

#define SWINF_IDLE         0
#define SWINF_WFVLDCLR     6
#define GET_SWINF(x)       (((x) >> 1) & 0x7)

#define PMIF_CMD_REG       1
#define MT6363_SID         4
#define MT6363_TOPSTATUS   0x1E

typedef struct {
  CONST CHAR16  *Name;
  UINT16        Send;
  UINT16        Sta;
  UINT16        Rdata;
  UINT16        Rdy;
} SPMI_CANDIDATE;

STATIC SPMI_CANDIDATE mCandidates[] = {
  { L"SWINF0", 0x0800, 0x0828, 0x0814, 0x0824 },
  { L"SWINF1", 0x0840, 0x0868, 0x0854, 0x0864 },
  { L"SWINF2", 0x0880, 0x08A8, 0x0894, 0x08A4 },
  { L"SWINF3", 0x08C0, 0x08E8, 0x08D4, 0x08E4 },
  { L"FORMULA", 0x080A, 0x0832, 0x081E, 0x082E },
};

STATIC
EFI_STATUS
SpmiReadCandidate (
  IN  SPMI_CANDIDATE  *Candidate,
  OUT UINT8           *Value
  )
{
  UINTN   Timeout;
  UINT32  Status;
  UINT32  Command;

  Status = MmioRead32 (PMIF_BASE + Candidate->Sta);
  if (GET_SWINF (Status) == SWINF_WFVLDCLR) {
    MmioWrite32 (PMIF_BASE + Candidate->Rdy, 1);
    Status = MmioRead32 (PMIF_BASE + Candidate->Sta);
  }
  if (GET_SWINF (Status) != SWINF_IDLE) {
    return EFI_NOT_READY;
  }

  Command = ((UINT32)PMIF_CMD_REG << 30) |
            ((UINT32)MT6363_SID << 24) |
            MT6363_TOPSTATUS;
  MmioWrite32 (PMIF_BASE + Candidate->Send, Command);

  for (Timeout = 0; Timeout < 2000; Timeout++) {
    Status = MmioRead32 (PMIF_BASE + Candidate->Sta);
    if (GET_SWINF (Status) == SWINF_WFVLDCLR) {
      break;
    }
    MicroSecondDelay (10);
  }

  if (Timeout == 2000) {
    return EFI_TIMEOUT;
  }

  *Value = (UINT8)(MmioRead32 (PMIF_BASE + Candidate->Rdata) & 0xFF);
  MmioWrite32 (PMIF_BASE + Candidate->Rdy, 1);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PearlPmicKeysEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  UINT8           Value;
  UINTN           Index;
  UINTN           Round;
  SPMI_CANDIDATE  *Candidate;

  Print (L"\r\n===== Pearl MT6363 SPMI read test =====\r\n");
  Print (
    L"PMIF INIT_DONE=%08x INF_EN=%08x ARB_EN=%08x CMDISSUE=%08x\r\n",
    MmioRead32 (PMIF_BASE + PMIF_INIT_DONE),
    MmioRead32 (PMIF_BASE + PMIF_INF_EN),
    MmioRead32 (PMIF_BASE + PMIF_ARB_EN),
    MmioRead32 (PMIF_BASE + PMIF_CMDISSUE_EN)
    );
  Print (L"\r\n");

  for (Index = 0; Index < ARRAY_SIZE (mCandidates); Index++) {
    Candidate = &mCandidates[Index];
    Value     = 0xFF;
    Status    = SpmiReadCandidate (Candidate, &Value);
    Print (
      L"%s send=%04x sta=%04x rdata=%04x: status=%r value=%02x\r\n",
      Candidate->Name,
      Candidate->Send,
      Candidate->Sta,
      Candidate->Rdata,
      Status,
      Value
      );
  }

  Print (L"\r\nRepeat reads; press Volume Up / Power to see which candidate changes.\r\n");
  for (Round = 0; Round < 30; Round++) {
    Print (L"round %u:", Round);
    for (Index = 0; Index < ARRAY_SIZE (mCandidates); Index++) {
      Candidate = &mCandidates[Index];
      Value     = 0xFF;
      Status    = SpmiReadCandidate (Candidate, &Value);
      if (EFI_ERROR (Status)) {
        Print (L" %s=ERR(%r)", Candidate->Name, Status);
      } else {
        Print (L" %s=%02x", Candidate->Name, Value);
      }
    }
    Print (L"\r\n");
    gBS->Stall (500000);
  }

  Print (L"Test done. Hold Power to reboot.\r\n");
  for (;;) {
    gBS->Stall (1000000);
  }
}
