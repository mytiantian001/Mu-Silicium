/** @file
  Differentiated System Definition Table (DSDT) for pearl (MT6895).

  Minimal bring-up DSDT: the 8 processor devices (ACPI0007) with _UIDs
  matching the MADT GICC entries. Platform devices (UFS, USB, keys, RTC)
  will be added incrementally.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "PEARLM", "PEARL ", 1)
{
  Scope (\_SB_)
  {
    Device (CPU0)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x0)
      Method (_STA) { Return (0xf) }
    }

    Device (CPU1)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x1)
      Method (_STA) { Return (0xf) }
    }

    Device (CPU2)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x2)
      Method (_STA) { Return (0xf) }
    }

    Device (CPU3)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x3)
      Method (_STA) { Return (0xf) }
    }

    Device (CPU4)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x4)
      Method (_STA) { Return (0xf) }
    }

    Device (CPU5)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x5)
      Method (_STA) { Return (0xf) }
    }

    Device (CPU6)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x6)
      Method (_STA) { Return (0xf) }
    }

    Device (CPU7)
    {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x7)
      Method (_STA) { Return (0xf) }
    }
  }
}
