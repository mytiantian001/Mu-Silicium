#ifndef _PLATFORM_REGISTERS_H_
#define _PLATFORM_REGISTERS_H_

//
// Display OVL Registers
//
#define OVL_PITCH_OFFSET(x)           (0x44 + 0x20 * (x))

//
// Display OVL Bits
//
#define OVL_CONST_BLEND                BIT28

//
// pearl display overlay bases (disp_ovl0 / disp_ovl0_2l, mainline mt6895.dtsi)
//
#define PEARL_OVL0_BASE                0x14002000
#define PEARL_OVL0_2L_BASE             0x14003000

//
// MTK display OVL register layout (shared by the mt6789/mt6895/mt8183
// families) - keep in sync with the DXE display driver.
//
#define MTK_OVL_LAYER_COUNT            4
#define MTK_OVL_ENABLE                 0x2C
#define MTK_OVL_CON(n)                 (0x30 + (0x20 * (n)))
#define MTK_OVL_PITCH(n)               (0x44 + (0x20 * (n)))
#define MTK_OVL_ADDR(n)                (0xF40 + (0x20 * (n)))
#define MTK_OVL_FORMAT_MASK            0x010F000
#define MTK_OVL_RGBA8888               (2 << 12)
#define MTK_OVL_BYTE_SWAP              (1 << 24)

//
// WatchDog Timer Values
//
#define WDT_MODE_KEY                   0x22000000

#endif /* _PLATFORM_REGISTERS_H_ */
