# uno2d — the 2D Primitive Vtable (CONTRACT-ARCH §5/§6, Phase 6)

Generalizes `fb.h` into one **tall** boundary: a mandatory software floor
(`put_pixel`) plus optional high-altitude accel overrides (`fill_rect`, `blit`,
`hline`/`vline`) that the core **synthesizes from the floor** when a backend omits
them. A new port runs everything by implementing only `put_pixel`; a port that can
accelerate (Amiga blitter, console GPU) overrides at the highest altitude it can.

The load-bearing invariant, verified by `uno2d_test`: **an accelerated override
produces pixel-identical output to the software synthesis** — so the software floor
is the conformance oracle for hardware backends. Run `sh uno2d/build.sh`: it renders
the same scene through the bare floor and through an accelerated backend and asserts
the framebuffers are byte-identical, then writes `build/uno2d_scene.ppm`.

PPM palette indices 0–3 come from the generated Contract (`uno_ui_palette_rgb24`).
Next: a real Amiga blitter backend (needs the 68K toolchain), and retargeting unoui
onto uno2d.
