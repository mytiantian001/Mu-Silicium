//
// Copyright (c) 2012-2013, ARM Limited. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

ASM_FUNC (PlatformAssemblyInitialize)
  EL1_OR_EL2 (x0)
1:
  // At EL1: disable alignment checking (SCTLR_EL1.A = 0). The ARM64
  // Windows ABI (bootmgfw/winload/kernel) freely performs unaligned
  // accesses; with alignment checking enabled they trap as data aborts.
  mrs x1, sctlr_el1
  bic x1, x1, #(1 << 1)
  msr sctlr_el1, x1
  isb
  ret
// Switch to EL1
2:
  mov x0, #(1 << 31)
  msr hcr_el2, x0

  // Disable alignment checking for EL1 before dropping down (EL2 may
  // write SCTLR_EL1).
  mrs x1, sctlr_el1
  bic x1, x1, #(1 << 1)
  msr sctlr_el1, x1
  isb

  msr elr_el2, x30

  mov x0, #0x3C5 // EL1h
  msr spsr_el2, x0
  isb
  eret
