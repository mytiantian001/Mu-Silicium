/** @file
  MediaTek USB host controller platform driver (MT6895 / pearl).

  Brings up the MTK USB stack for host operation:
    - initialises the SSUSB IPPC block (host IP power, port enable in host
      mode, clock-stability check) following xhci-mtk.c
    - switches the U2 PHY to host mode (IDDIG force) following
      phy-mtk-tphy.c
    - switches the MT6375 Type-C port to host mode over I2C (TCPCI
      ROLE_CONTROL: CC1/CC2 = Rp)
    - registers the XHCI controller (0x11200000) as a non-discoverable
      PCI device, so the stock edk2 XhciDxe driver binds to it unchanged

  References (mainline): drivers/usb/host/xhci-mtk.c (IPPC),
  drivers/phy/mediatek/phy-mtk-tphy.c (U2 PHY),
  drivers/usb/typec/tcpm/tcpci_mt6375.c (TCPC),
  drivers/i2c/busses/i2c-mt65xx.c (I2C).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

//
// IPPC (SSUSB IP port control, xhci-mtk layout)
//
#define IPPC_IP_PW_CTR0   0x00
#define IPPC_IP_PW_CTR1   0x04
#define IPPC_IP_PW_CTR2   0x08
#define IPPC_IP_PW_STS1   0x10
#define IPPC_IP_XHCI_CAP  0x24
#define IPPC_U3_CTRL_P    0x30   /* +8 per port, 64-bit */
#define IPPC_U2_CTRL_P    0x50   /* +8 per port, 64-bit */

#define CTRL0_IP_SW_RST        BIT0
#define CTRL1_IP_HOST_PDN      BIT0
#define CTRL2_IP_DEV_PDN       BIT0
#define CTRL_U3_PORT_HOST_SEL  BIT2
#define CTRL_U3_PORT_PDN       BIT1
#define CTRL_U3_PORT_DIS       BIT0
#define CTRL_U2_PORT_HOST_SEL  BIT2
#define CTRL_U2_PORT_PDN       BIT1
#define CTRL_U2_PORT_DIS       BIT0

#define CAP_U3_PORT_NUM(p)  ((p) & 0xFF)
#define CAP_U2_PORT_NUM(p)  (((p) >> 8) & 0xFF)

#define STS1_SYSPLL_STABLE  BIT0
#define STS1_REF_RST        BIT8
#define STS1_SYS125_RST     BIT10
#define STS1_XHCI_RST       BIT11
#define STS1_U3_MAC_RST     BIT16

//
// U2 PHY (t-phy, V2 banks; com bank at +0x300)
//
#define U2PHY_COM_OFFSET  0x300
#define U2PHYDTM1         0x06C
#define P2C_FORCE_IDDIG   BIT9
#define P2C_RG_IDDIG      BIT1

//
// MTK I2C (i2c-mt65xx register offsets)
//
#define I2C_DATA_PORT     0x00
#define I2C_SLAVE_ADDR    0x04
#define I2C_INTR_STAT     0x0C
#define I2C_CONTROL       0x10
#define I2C_TRANSFER_LEN  0x14
#define I2C_TRANSAC_LEN   0x18
#define I2C_DELAY_LEN     0x1C
#define I2C_TIMING        0x20
#define I2C_START         0x24

#define I2C_STAT_TRANSAC_COMP  BIT0
#define I2C_STAT_ACKERR        BIT1
#define I2C_STAT_HS_NACKERR    BIT2
#define I2C_STAT_ARB_LOST      BIT3

#define I2C_START_TRANSAC      BIT0
#define I2C_WRRD_TRANAC_VALUE  0x0002

//
// MT6375: the chip answers on several I2C addresses; the register's high
// byte selects the bank (drivers/mfd/mt6375.c: bank_to_i2c):
//   bank 0x00 -> 0x4E (TCPC), bank 0x01 -> 0x34 (PMU/charger).
// On the wire the register is the low byte of the 16-bit address.
//
#define MT6375_TCPC_ADDR    0x4E
#define MT6375_PMU_ADDR     0x34

//
// MT6375 TCPC registers (bank 0x00, TCPCI)
//
#define TCPC_ROLE_CONTROL   0x1A
#define TCPC_CC_STATUS      0x1D
#define TCPC_COMMAND        0x23
#define TCPC_ALERT          0x10
#define TCPC_ALERT_MASK     0x12
#define TCPC_ROLE_CTRL_CC_RP      0x1
#define TCPC_ROLE_CTRL_RP_VAL_DEF 0x0
#define TCPC_CMD_DISABLE_SINK_VBUS  0x44

//
// MT6375 charger/PMU registers (bank 0x01, wire register = low byte)
// (drivers/power/supply/mediatek/mt6375-charger.c)
//
#define MT6375_REG_TM_PAS_CODE1  0x07   /* 0x0107 hidden-mode password */
#define MT6375_REG_CHG_TOP1      0x20   /* 0x0120 CHG_EN/BUCK_EN/OTG_EN */
#define MT6375_REG_CHG_TOP2      0x21   /* 0x0121 CLK_FREQ */
#define MT6375_REG_OTG_V         0x31   /* 0x0131 boost output voltage */
#define MT6375_REG_OTG_C         0x32   /* 0x0132 boost current limit */
#define MT6375_REG_CHG_DUMY0     0x35   /* 0x0135 COMP_CLAMP */
#define MT6375_REG_CHG_HD_BUCK5  0x40   /* 0x0140 BUCK_RAMPOFT */
#define MT6375_REG_BC12_FUNC     0x50   /* 0x0150 BC12_EN */
#define MT6375_REG_DPDM_CTRL1    0x53   /* 0x0153 manual mode / detects */
#define MT6375_REG_DPDM_CTRL2    0x54   /* 0x0154 DP/DM LDOs */
#define MT6375_REG_DPDM_CTRL4    0x56   /* 0x0156 DP pull */
#define MT6375_REG_DPDM_CTRL5    0x57   /* 0x0157 DM pull */

#define MT6375_MSK_CHG_EN        0x01
#define MT6375_MSK_BUCK_EN       0x02
#define MT6375_MSK_OTG_EN        0x04
#define MT6375_MSK_CLK_FREQ      0xC0
#define MT6375_MSK_COMP_CLAMP    0x03
#define MT6375_MSK_BUCK_RAMPOFT  0xC0
#define MT6375_MSK_BC12_EN       0x80
#define MT6375_MSK_OTG_CV        0x3F
#define MT6375_MSK_OTG_CC        0x07

//
// U2 PHY BC1.1 switch (t-phy com bank +0x018, bit 23). When set the charger
// detection owns D+/D-; the SoC USB controller owns them when clear
// (phy-mtk-tphy.c clears it during PHY init, "DP/DM BC1.1 path Disable").
//
#define U2PHY_USBPHYACR6         0x018
#define PA6_RG_U2_BC11_SW_EN     BIT23

/**
  Polled MTK I2C write: [slave][data...]
**/
STATIC
EFI_STATUS
MtkI2cWrite (
  IN UINT8  Slave,
  IN UINT8  *Data,
  IN UINT32  Length
  )
{
  UINTN   Base = FixedPcdGet32 (PcdI2cBase);
  UINT32  Stat;
  UINT32  Timeout;
  UINT32  Index;

  /* Wait for the bus to be idle */
  for (Timeout = 10000; Timeout > 0; Timeout--) {
    if ((MmioRead32 (Base + I2C_INTR_STAT) & I2C_STAT_TRANSAC_COMP) != 0) {
      break;
    }

    MicroSecondDelay (10);
  }

  /* Clear status, set slave address and timing */
  MmioWrite32 (Base + I2C_INTR_STAT, I2C_STAT_TRANSAC_COMP | I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | I2C_STAT_ARB_LOST);
  MmioWrite32 (Base + I2C_SLAVE_ADDR, (Slave << 1) | 0);
  MmioWrite32 (Base + I2C_DELAY_LEN, 0x0002);
  MmioWrite32 (Base + I2C_TIMING, 0x0003);

  /* Push the data bytes */
  for (Index = 0; Index < Length; Index++) {
    MmioWrite8 (Base + I2C_DATA_PORT, Data[Index]);
  }

  MmioWrite16 (Base + I2C_TRANSFER_LEN, (UINT16)Length);
  MmioWrite16 (Base + I2C_TRANSAC_LEN, 1);
  MmioWrite16 (Base + I2C_CONTROL, 0x0000);   /* push-pull, no DMA */
  MmioWrite16 (Base + I2C_START, I2C_START_TRANSAC);

  /* Wait for completion */
  for (Timeout = 10000; Timeout > 0; Timeout--) {
    Stat = MmioRead32 (Base + I2C_INTR_STAT);
    if ((Stat & I2C_STAT_TRANSAC_COMP) != 0) {
      MmioWrite32 (Base + I2C_INTR_STAT, Stat);
      return EFI_SUCCESS;
    }

    if ((Stat & (I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | I2C_STAT_ARB_LOST)) != 0) {
      MmioWrite32 (Base + I2C_INTR_STAT, Stat);
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

/**
  Polled MTK I2C write-then-read: [slave][reg][restart][slave+1][data...]
**/
STATIC
EFI_STATUS
MtkI2cRead (
  IN  UINT8  Slave,
  IN  UINT8  Reg,
  OUT UINT8  *Data
  )
{
  UINTN   Base = FixedPcdGet32 (PcdI2cBase);
  UINT32  Stat;
  UINT32  Timeout;

  for (Timeout = 10000; Timeout > 0; Timeout--) {
    if ((MmioRead32 (Base + I2C_INTR_STAT) & I2C_STAT_TRANSAC_COMP) != 0) {
      break;
    }

    MicroSecondDelay (10);
  }

  MmioWrite32 (Base + I2C_INTR_STAT, I2C_STAT_TRANSAC_COMP | I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | I2C_STAT_ARB_LOST);
  MmioWrite32 (Base + I2C_SLAVE_ADDR, (Slave << 1) | 0);
  MmioWrite32 (Base + I2C_DELAY_LEN, 0x0002);
  MmioWrite32 (Base + I2C_TIMING, 0x0003);

  MmioWrite8 (Base + I2C_DATA_PORT, Reg);
  MmioWrite16 (Base + I2C_TRANSFER_LEN, 1);
  MmioWrite16 (Base + I2C_TRANSAC_LEN, I2C_WRRD_TRANAC_VALUE);
  MmioWrite16 (Base + I2C_CONTROL, 0x0000);
  MmioWrite16 (Base + I2C_START, I2C_START_TRANSAC);

  for (Timeout = 10000; Timeout > 0; Timeout--) {
    Stat = MmioRead32 (Base + I2C_INTR_STAT);
    if ((Stat & I2C_STAT_TRANSAC_COMP) != 0) {
      MmioWrite32 (Base + I2C_INTR_STAT, Stat);
      *Data = MmioRead8 (Base + I2C_DATA_PORT);
      return EFI_SUCCESS;
    }

    if ((Stat & (I2C_STAT_ACKERR | I2C_STAT_HS_NACKERR | I2C_STAT_ARB_LOST)) != 0) {
      MmioWrite32 (Base + I2C_INTR_STAT, Stat);
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

/**
  Write one byte to an MT6375 register.

  @param[in] Slave  I2C slave address (bank selector).
  @param[in] Reg    Register (low byte of the 16-bit register address).
  @param[in] Value  Value to write.
**/
STATIC
EFI_STATUS
MtkI2cWr (
  IN UINT8  Slave,
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  UINT8  Payload[2];

  Payload[0] = Reg;
  Payload[1] = Value;

  return MtkI2cWrite (Slave, Payload, sizeof (Payload));
}

/**
  Read-modify-write one MT6375 register.

  @param[in] Slave  I2C slave address (bank selector).
  @param[in] Reg    Register (low byte of the 16-bit register address).
  @param[in] Mask   Bits to change.
  @param[in] Value  New value of the masked bits.
**/
STATIC
EFI_STATUS
MtkI2cRmw (
  IN UINT8  Slave,
  IN UINT8  Reg,
  IN UINT8  Mask,
  IN UINT8  Value
  )
{
  EFI_STATUS  Status;
  UINT8       Val;

  Status = MtkI2cRead (Slave, Reg, &Val);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Val = (UINT8)((Val & ~Mask) | (Value & Mask));

  return MtkI2cWr (Slave, Reg, Val);
}

/**
  Bring up the MT6375 for USB host operation:

    - TCPC: advertise Rp on both CC lines, stop the sink (charge) path
    - charger: stop CHG/BUCK/BC1.2, load the OTG boost parameters and
      enable the boost (5 V on VBUS for the attached device)
    - U2 PHY: clear the BC1.1 switch so D+/D- belong to the SoC

  Mirrors the kernel's mt6375_tcpm_source_enable() and
  mt6375_chg_set_usbsw(USBSW_USB).
**/
STATIC
VOID
Mtk6375HostPowerOn (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       CcStatus = 0;
  UINT8       Payload[3];
  UINTN       Base;
  UINT32      Value;

  //
  // TCPC: CC1/CC2 = Rp (the host advertises its power role).
  //
  Value  = (TCPC_ROLE_CTRL_CC_RP << 0) |
           (TCPC_ROLE_CTRL_CC_RP << 2) |
           (TCPC_ROLE_CTRL_RP_VAL_DEF << 4);
  Status = MtkI2cWr (MT6375_TCPC_ADDR, TCPC_ROLE_CONTROL, (UINT8)Value);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: TCPC ROLE_CONTROL=0x%02x -> %r\n", Value, Status));

  Status = MtkI2cWr (MT6375_TCPC_ADDR, TCPC_COMMAND, TCPC_CMD_DISABLE_SINK_VBUS);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: TCPC disable sink -> %r\n", Status));

  //
  // Mask every TCPC alert and clear any pending one. The firmware has no
  // TCPC interrupt handler and an unhandled alert (for example the CC
  // status change when a device is attached) keeps the MT6375 interrupt
  // line asserted, which hangs the boot.
  //
  Payload[0] = TCPC_ALERT_MASK;
  Payload[1] = 0x00;
  Payload[2] = 0x00;
  Status     = MtkI2cWrite (MT6375_TCPC_ADDR, Payload, sizeof (Payload));
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: TCPC alerts masked -> %r\n", Status));

  Payload[0] = TCPC_ALERT;
  Payload[1] = 0xFF;
  Payload[2] = 0xFF;
  Status     = MtkI2cWrite (MT6375_TCPC_ADDR, Payload, sizeof (Payload));
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: TCPC alert cleared -> %r\n", Status));

  Status = MtkI2cRead (MT6375_TCPC_ADDR, TCPC_CC_STATUS, &CcStatus);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: TCPC CC_STATUS=0x%02x -> %r\n", CcStatus, Status));

  //
  // Charger: stop everything that could drive the port (mirrors the
  // kernel's mt6375_tcpm_quiesce; the VSYS bleed register is skipped for
  // the tcpm-managed configuration exactly like the kernel does).
  //
  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_CHG_TOP1, MT6375_MSK_CHG_EN | MT6375_MSK_BUCK_EN, 0);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: CHG/BUCK off -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_BC12_FUNC, MT6375_MSK_BC12_EN, 0);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: BC1.2 engine off -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_DPDM_CTRL2, BIT7 | BIT3, 0);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: DP/DM LDOs off -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_DPDM_CTRL4, BIT7 | BIT3, 0);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: DP pull off -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_DPDM_CTRL5, BIT7 | BIT3, 0);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: DM pull off -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_DPDM_CTRL1, BIT7 | BIT5 | BIT1 | BIT0, 0);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: DPDM manual/detects off -> %r\n", Status));

  //
  // Boost parameters (hidden mode), mirroring mt6375_set_boost_param(bst=true).
  //
  Status = MtkI2cWr (MT6375_PMU_ADDR, MT6375_REG_TM_PAS_CODE1, 0x69);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: hidden mode unlock -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_CHG_TOP2, MT6375_MSK_CLK_FREQ, 0x00);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: boost TOP2 -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_CHG_DUMY0, MT6375_MSK_COMP_CLAMP, 0x03);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: boost DUMY0 -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_CHG_HD_BUCK5, MT6375_MSK_BUCK_RAMPOFT, 0xC0);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: boost HD_BUCK5 -> %r\n", Status));

  Status = MtkI2cWr (MT6375_PMU_ADDR, MT6375_REG_TM_PAS_CODE1, 0x00);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: hidden mode lock -> %r\n", Status));

  //
  // Boost output: 5.0 V (sel 20 + 6) and 1.8 A limit (current table sel 4).
  //
  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_OTG_V, MT6375_MSK_OTG_CV, 26);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: OTG_V=5.0V -> %r\n", Status));

  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_OTG_C, MT6375_MSK_OTG_CC, 4);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: OTG_C=1.8A -> %r\n", Status));

  //
  // Enable the boost: 5 V on VBUS for the attached device.
  //
  Status = MtkI2cRmw (MT6375_PMU_ADDR, MT6375_REG_CHG_TOP1, MT6375_MSK_OTG_EN, MT6375_MSK_OTG_EN);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: OTG boost enable (VBUS on) -> %r\n", Status));

  //
  // Read the enable back: the kernel verifies that the bit really latched.
  //
  CcStatus = 0;
  Status   = MtkI2cRead (MT6375_PMU_ADDR, MT6375_REG_CHG_TOP1, &CcStatus);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: CHG_TOP1 readback=0x%02x (OTG_EN=%d) -> %r\n", CcStatus, (CcStatus >> 2) & 1, Status));

  //
  // U2 PHY: clear the BC1.1 switch so D+/D- belong to the SoC USB controller
  // (charger detection shares the lines with USB).
  //
  Base  = FixedPcdGet32 (PcdU2PhyBase) + U2PHY_COM_OFFSET;
  Value = MmioRead32 (Base + U2PHY_USBPHYACR6);
  Value &= ~(UINT32)PA6_RG_U2_BC11_SW_EN;
  MmioWrite32 (Base + U2PHY_USBPHYACR6, Value);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: BC1.1 switch cleared (DP/DM -> SoC): 0x%08x\n", Value));
}

/**
  Initialise the SSUSB IPPC block for host operation (xhci_mtk_host_enable).
**/
STATIC
VOID
MtkIppcHostEnable (
  VOID
  )
{
  UINTN   Base = FixedPcdGet32 (PcdIppcBase);
  UINT32  Value;
  UINT32  Check;
  UINT32  NumU2;
  UINT32  NumU3;
  UINT32  Index;
  UINT32  Timeout;

  /*
   * Power down the device IP: it is powered on by default (the LK left it
   * in gadget mode), and only one side may drive the port.
   */
  Value  = MmioRead32 (Base + IPPC_IP_PW_CTR2);
  Value |= CTRL2_IP_DEV_PDN;
  MmioWrite32 (Base + IPPC_IP_PW_CTR2, Value);

  /* Port counts come from the IP capability register */
  Value = MmioRead32 (Base + IPPC_IP_XHCI_CAP);
  NumU3 = CAP_U3_PORT_NUM (Value);
  NumU2 = CAP_U2_PORT_NUM (Value);
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: IPPC u2p=%d u3p=%d\n", NumU2, NumU3));

  /* Power on the host IP */
  Value  = MmioRead32 (Base + IPPC_IP_PW_CTR1);
  Value &= ~CTRL1_IP_HOST_PDN;
  MmioWrite32 (Base + IPPC_IP_PW_CTR1, Value);

  /* Enable all U3 ports in host mode */
  for (Index = 0; Index < NumU3; Index++) {
    Value  = MmioRead32 (Base + IPPC_U3_CTRL_P + Index * 8);
    Value &= ~(CTRL_U3_PORT_PDN | CTRL_U3_PORT_DIS);
    Value |= CTRL_U3_PORT_HOST_SEL;
    MmioWrite32 (Base + IPPC_U3_CTRL_P + Index * 8, Value);
  }

  /* Enable all U2 ports in host mode */
  for (Index = 0; Index < NumU2; Index++) {
    Value  = MmioRead32 (Base + IPPC_U2_CTRL_P + Index * 8);
    Value &= ~(CTRL_U2_PORT_PDN | CTRL_U2_PORT_DIS);
    Value |= CTRL_U2_PORT_HOST_SEL;
    MmioWrite32 (Base + IPPC_U2_CTRL_P + Index * 8, Value);
  }

  /* Wait for the clocks to become stable */
  Check = STS1_SYSPLL_STABLE | STS1_REF_RST | STS1_SYS125_RST | STS1_XHCI_RST;
  if (NumU3 > 0) {
    Check |= STS1_U3_MAC_RST;
  }

  for (Timeout = 200; Timeout > 0; Timeout--) {
    Value = MmioRead32 (Base + IPPC_IP_PW_STS1);
    if ((Value & Check) == Check) {
      break;
    }

    MicroSecondDelay (100);
  }

  DEBUG ((
    DEBUG_INFO,
    "MtkUsbDxe: IPPC sts1=0x%08x want=0x%08x %a\n",
    Value,
    Check,
    (Timeout > 0) ? "OK" : "TIMEOUT"
    ));
}

/**
  Switch the U2 PHY to host mode (force IDDIG = 0), mirroring
  u2_phy_instance_set_mode(PHY_MODE_USB_HOST).
**/
STATIC
VOID
MtkU2PhySetHostMode (
  VOID
  )
{
  UINTN   Base = FixedPcdGet32 (PcdU2PhyBase) + U2PHY_COM_OFFSET;
  UINT32  Value;

  Value  = MmioRead32 (Base + U2PHYDTM1);
  Value |= P2C_FORCE_IDDIG;
  Value &= ~P2C_RG_IDDIG;
  MmioWrite32 (Base + U2PHYDTM1, Value);

  DEBUG ((
    DEBUG_INFO,
    "MtkUsbDxe: U2 PHY DTM1=0x%08x (host mode)\n",
    MmioRead32 (Base + U2PHYDTM1)
    ));
}

/**
  Entry point: bring up the MTK USB host stack.
**/
EFI_STATUS
EFIAPI
MtkUsbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS XhciBase;

  DEBUG ((DEBUG_INFO, "MtkUsbDxe: initialising MT6895 USB host\n"));

  MtkIppcHostEnable ();
  MtkU2PhySetHostMode ();
  Mtk6375HostPowerOn ();

  /*
   * Register the XHCI controller as a non-discoverable PCI device so the
   * stock MdeModulePkg XhciDxe driver binds to it (the MTK XHCI is MMIO
   * only, no PCI config space).
   */
  XhciBase = FixedPcdGet32 (PcdXhciBase);
  Status   = RegisterNonDiscoverableMmioDevice (
               NonDiscoverableDeviceTypeXhci,
               NonDiscoverableDeviceDmaTypeNonCoherent,
               NULL,
               NULL,
               1,
               XhciBase, (UINTN)SIZE_4KB
               );
  DEBUG ((DEBUG_INFO, "MtkUsbDxe: XHCI registration: %r\n", Status));

  return EFI_SUCCESS;
}
