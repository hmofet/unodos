/*
 * testlid.asl - synthetic ACPI lid that TOGGLES, for QEMU verification of the
 * lid-close -> sleep -> lid-open -> wake path.
 *
 * QEMU q35 has no lid, and a static _LID can't exercise the edge detector.
 * This _LID returns a value derived from a counter that increments on each
 * evaluation: acpi_power caches the lid ~1 s, and the shell evaluates it ~1 Hz,
 * so the lid reads OPEN for ~5 s, CLOSED for ~5 s, and repeats - producing a
 * clean CLOSE edge (enter sleep) then an OPEN edge (wake) inside a short run.
 *
 * Build:  iasl tools/testlid.asl   (-> tools/testlid.aml)
 * Boot :  qemu ... -acpitable file=testlid.aml   (harness.py lidsleep)
 */
DefinitionBlock ("", "SSDT", 2, "UNODOS", "TESTLID", 0x00000001)
{
    Scope (\_SB)
    {
        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Name (LCNT, 0x00)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Method (_LID, 0, NotSerialized)
            {
                Increment (LCNT)
                If (LGreaterEqual (LCNT, 0x0A))
                {
                    Store (Zero, LCNT)          /* wrap every ~10 evaluations */
                }
                /* OPEN for counts 0..4, CLOSED for 5..9 */
                If (LLess (LCNT, 0x05))
                {
                    Return (One)                /* open   */
                }
                Return (Zero)                   /* closed */
            }
        }
    }
}
