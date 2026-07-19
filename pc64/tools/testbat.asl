/*
 * testbat.asl - synthetic ACPI battery + lid for QEMU verification.
 *
 * QEMU q35 has no battery/lid, so the unoacpi read path can only be exercised
 * end-to-end by injecting this SSDT:
 *
 *     iasl testbat.asl                        (acpica-tools)
 *     qemu-system-x86_64 ... -acpitable file=tools/testbat.aml
 *
 * A PNP0C0A control-method battery whose _BIF[2] (last full = 9000) and
 * _BST[2] (remaining = 4506) read back as **50%** in the tray chip, proving
 * device discovery -> package indexing -> integer extraction -> percentage;
 * plus a PNP0C0D lid reporting open.  (Same recipe the writers-unlock
 * reference host was verified with; see docs/ACPI-POWER-INTERFACE.md there.)
 */
DefinitionBlock ("testbat.aml", "SSDT", 2, "UNODOS", "TESTBAT", 0x00000001)
{
    Scope (\_SB)
    {
        Device (BATT)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x1F)                   /* present + enabled + shown */
            }
            Method (_BIF, 0, NotSerialized)
            {
                Return (Package (0x0D)
                {
                    One,                        /* Power Unit: mA/mAh         */
                    9000,                       /* Design Capacity            */
                    9000,                       /* Last Full Charge  [2]      */
                    One,                        /* Technology: rechargeable   */
                    12000,                      /* Design Voltage (mV)        */
                    900,                        /* Capacity Warning           */
                    450,                        /* Capacity Low               */
                    64, 64,                     /* capacity granularity 1/2   */
                    "UNO-BAT-1",                /* Model Number               */
                    "0001",                     /* Serial Number              */
                    "LION",                     /* Battery Type               */
                    "UnoDOS"                    /* OEM Information            */
                })
            }
            Method (_BST, 0, NotSerialized)
            {
                Return (Package (0x04)
                {
                    One,                        /* state: discharging         */
                    500,                        /* present rate (mA)          */
                    4506,                       /* remaining capacity [2]     */
                    11800                       /* present voltage (mV)       */
                })
            }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Method (_LID, 0, NotSerialized)
            {
                Return (One)                    /* open                       */
            }
        }
    }
}
