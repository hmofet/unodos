import Foundation

// A removable USB / external physical disk, as enumerated by `diskutil`.
// Mirrors the Windows flasher's UsbDrive (Win32_DiskDrive WHERE InterfaceType='USB').
struct USBDrive: Identifiable, Hashable {
    let id: String          // BSD name, e.g. "disk4"
    let model: String
    let size: UInt64

    var device: String    { "/dev/\(id)" }     // whole disk (for diskutil unmount)
    var rawDevice: String { "/dev/r\(id)" }     // unbuffered char device (fast raw write)

    // "SanDisk Cruzer  (29.8 GB)   [disk4]"  — same shape as UsbDrive.ToString().
    var label: String {
        let gb = Double(size) / (1024.0 * 1024.0 * 1024.0)
        return String(format: "%@  (%.1f GB)   [%@]", model, gb, id)
    }
}

// Enumerate external physical disks via diskutil (plist output), skipping any
// internal disk so the user's system volume can never be selected.
enum DiskScanner {
    private static func diskutil(_ args: [String]) -> Data {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        p.arguments = args
        let out = Pipe()
        p.standardOutput = out
        p.standardError = Pipe()
        do { try p.run() } catch { return Data() }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return data
    }

    @discardableResult
    static func unmountDisk(_ device: String) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        p.arguments = ["unmountDisk", "force", device]
        p.standardOutput = Pipe(); p.standardError = Pipe()
        do { try p.run() } catch { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    static func externalDrives() -> [USBDrive] {
        let data = diskutil(["list", "-plist", "external", "physical"])
        guard
            let root = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any],
            let whole = root["WholeDisks"] as? [String]
        else { return [] }

        var drives: [USBDrive] = []
        for id in whole {
            let info = diskutil(["info", "-plist", id])
            guard let d = try? PropertyListSerialization.propertyList(from: info, options: [], format: nil) as? [String: Any]
            else { continue }
            if (d["Internal"] as? Bool) == true { continue }    // never an internal disk
            let size = (d["Size"] as? NSNumber)?.uint64Value
                ?? (d["TotalSize"] as? NSNumber)?.uint64Value ?? 0
            let name = (d["MediaName"] as? String)
                ?? (d["IORegistryEntryName"] as? String) ?? "USB drive"
            drives.append(USBDrive(id: id, model: name.trimmingCharacters(in: .whitespaces), size: size))
        }
        return drives
    }
}
