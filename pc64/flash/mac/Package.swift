// swift-tools-version:5.9
import PackageDescription

// The UnoDOS/pc64 USB Installer for macOS — a self-contained .app that embeds
// the one bootable UEFI disk image and writes it to a USB drive. The macOS twin
// of the Windows flasher (pc64/flash/UnoDosFlash.cs); ported from the Writer's
// Unlock mac flasher, simplified to pc64's UEFI-only, single-image world.
//
//   CFlash            — tiny C shim over Authorization Services (run the raw-disk
//                       writer as root after an admin prompt; the macOS analogue
//                       of the Windows UAC manifest).
//   UnoDosFlasher     — the SwiftUI/AppKit GUI executable.
//
// build-app.sh wraps the executable into UnoDosFlasher.app, compiles the
// privileged writer (writer.c), and bundles the gzip image.
let package = Package(
    name: "UnoDosFlasher",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "CFlash"),
        .executableTarget(
            name: "UnoDosFlasher",
            dependencies: ["CFlash"],
            linkerSettings: [.linkedFramework("Security")]
        )
    ]
)
