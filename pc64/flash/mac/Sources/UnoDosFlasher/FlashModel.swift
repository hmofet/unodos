import Foundation
import AppKit
import Security
import CFlash

@MainActor
final class FlashModel: ObservableObject {
    @Published var drives: [USBDrive] = []       // visible (filtered) list
    @Published var selection: USBDrive.ID?
    @Published var progress: Double = 0          // 0...1
    @Published var status: String = ""
    @Published var busy = false
    @Published var showAll = false               // include 0 GB (empty) drives?

    private var allDrives: [USBDrive] = []        // full scan, smallest-first
    var hiddenCount: Int { allDrives.filter { $0.size == 0 }.count }

    var selectedDrive: USBDrive? { drives.first { $0.id == selection } }

    // pc64 is UEFI-only and ships a single image, so — unlike the Writer's Unlock
    // flasher — there is no boot-mode / filesystem choice. One fixed blurb.
    var notes: String {
        "UnoDOS/pc64 is UEFI / x86-64 only (PCs from roughly 2012 on).\n\n"
        + "Boot the target machine from this USB: enter firmware, turn Secure Boot "
        + "OFF, and pick the USB from the boot menu."
    }

    func refresh() {
        // Smallest-first: a USB stick / SD card is usually the smallest external
        // disk, so it sorts to the top and is auto-selected; a big external backup
        // drive sinks to the bottom where it's harder to pick by accident.
        allDrives = DiskScanner.externalDrives().sorted { $0.size < $1.size }
        applyFilter()
    }

    func toggleShowAll() {
        showAll.toggle()
        applyFilter()
    }

    // Hide 0 GB drives (empty card-reader slots, no media) unless Show all is on,
    // then auto-select the top (smallest) entry as the most likely flash target.
    private func applyFilter() {
        drives = showAll ? allDrives : allDrives.filter { $0.size > 0 }
        if selection == nil || !drives.contains(where: { $0.id == selection }) {
            selection = drives.first?.id
        }
        let hidden = allDrives.count - drives.count
        if drives.isEmpty {
            status = hidden > 0 ? "\(hidden) empty drive(s) hidden — click Show all." : "No USB drives found."
        } else {
            status = "\(drives.count) external USB drive(s) found"
                + (hidden > 0 ? ", \(hidden) empty hidden." : ".")
        }
    }

    // The single embedded image: images/unodos.img.gz.
    private let resourceName = "unodos"

    func flash() {
        guard let drive = selectedDrive else { status = "Select a USB drive first."; return }

        let confirm = NSAlert()
        confirm.alertStyle = .warning
        confirm.messageText = "Confirm erase"
        confirm.informativeText =
            "This will ERASE everything on:\n\n    \(drive.label)\n\n"
            + "and write UnoDOS/pc64 (UEFI)."
            + "\n\nContinue?"
        confirm.addButton(withTitle: "Cancel")          // default (matches Win's Button2=No)
        confirm.addButton(withTitle: "Erase & Flash")
        guard confirm.runModal() == .alertSecondButtonReturn else { return }

        guard let gz = Bundle.main.resourceURL?
            .appendingPathComponent("images/\(resourceName).img.gz"),
            FileManager.default.fileExists(atPath: gz.path) else {
            report(title: "Install failed",
                   text: "The bundled UnoDOS image is missing from this app.",
                   ok: false)
            return
        }
        guard let writer = Bundle.main.url(forResource: "uno-writer", withExtension: nil) else {
            report(title: "Install failed", text: "The privileged writer is missing from this app.", ok: false)
            return
        }

        busy = true
        progress = 0
        status = "Unmounting volumes…"

        let dev = drive.rawDevice
        let whole = drive.device
        Task.detached { [weak self] in
            await self?.doFlash(writer: writer.path, image: gz.path, rawDevice: dev, wholeDisk: whole)
        }
    }

    private nonisolated func doFlash(writer: String, image: String, rawDevice: String, wholeDisk: String) async {
        // Free the drive's volumes first (an ESP / hidden partition would block the raw write).
        _ = DiskScanner.unmountDisk(wholeDisk)

        var pipe: UnsafeMutablePointer<FILE>? = nil
        let st = writer.withCString { w in
            image.withCString { g in
                rawDevice.withCString { d in
                    uno_authorized_exec(w, g, d, &pipe)
                }
            }
        }

        if st != errSecSuccess || pipe == nil {
            let cancelled = (st == errAuthorizationCanceled)
            await MainActor.run {
                self.busy = false
                if cancelled {
                    self.status = "Cancelled."
                } else {
                    self.status = "Failed: could not get administrator privileges."
                    self.report(title: "Install failed",
                                text: "macOS denied administrator privileges, so the drive can't be written.\n\nTry again and enter an administrator password.",
                                ok: false)
                }
            }
            return
        }

        // Read the writer's progress lines: "P done total", then "DONE" / "ERR …".
        var ok = false
        var errMsg = ""
        let line = UnsafeMutablePointer<CChar>.allocate(capacity: 1024)
        defer { line.deallocate() }
        while fgets(line, 1024, pipe) != nil {
            let s = String(cString: line).trimmingCharacters(in: .whitespacesAndNewlines)
            if s.hasPrefix("P ") {
                let parts = s.split(separator: " ")
                if parts.count == 3, let done = Double(parts[1]), let total = Double(parts[2]), total > 0 {
                    let frac = min(1.0, done / total)
                    let mb = Int(done) / (1024 * 1024)
                    await MainActor.run { self.progress = frac; self.status = "Writing… \(mb) MB" }
                }
            } else if s == "DONE" {
                ok = true
            } else if s.hasPrefix("ERR") {
                errMsg = String(s.dropFirst(3)).trimmingCharacters(in: .whitespaces)
            }
        }
        fclose(pipe)

        let succeeded = ok
        let msg = errMsg            // immutable snapshots for the @Sendable closure
        await MainActor.run {
            self.busy = false
            if succeeded {
                self.progress = 1
                self.status = "Done — you can remove the drive."
                self.report(title: "Finished",
                            text: "UnoDOS was written successfully.\n\nBoot the target machine from this USB.",
                            ok: true)
            } else {
                self.status = "Failed: " + (msg.isEmpty ? "the write did not complete." : msg)
                let denied = msg.range(of: "permission", options: .caseInsensitive) != nil
                    || msg.range(of: "denied", options: .caseInsensitive) != nil
                let help = denied
                    ? "macOS denied writing to the drive.\n\n• If it's an SD card, slide the write-protect (lock) switch on the side OFF.\n• Unmount the drive in Disk Utility and try again.\n\n(\(msg))"
                    : (msg.isEmpty ? "The write did not complete." : msg)
                        + "\n\nMake sure the drive is plugged in and not write-protected, then try again."
                self.report(title: "Install failed", text: help, ok: false)
            }
        }
    }

    private func report(title: String, text: String, ok: Bool) {
        let a = NSAlert()
        a.alertStyle = ok ? .informational : .critical
        a.messageText = title
        a.informativeText = text
        a.addButton(withTitle: "OK")
        a.runModal()
    }
}
