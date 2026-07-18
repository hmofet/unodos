// UnoDosFlasherCapture.app main executable (compiled).
// Captures the flasher window IN-PROCESS via ScreenCaptureKit, so the capture
// runs under THIS app bundle's Screen Recording grant (a shelled-out
// `screencapture` is evaluated as its own binary and would be denied).
// Downloads the latest macOS flasher, opens it, captures its window, quits it,
// and cleans up. Never clicks Install, so no drive is written.
import Foundation
import CoreGraphics
import AppKit
import ScreenCaptureKit

let home = FileManager.default.homeDirectoryForCurrentUser.path
let outDir = home + "/unodos-flasher-shots"
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
let logPath = outDir + "/capture.log"
func log(_ s: String) {
    let line = "\(Date()) \(s)\n"
    guard let d = line.data(using: .utf8) else { return }
    if FileManager.default.fileExists(atPath: logPath), let h = FileHandle(forWritingAtPath: logPath) {
        h.seekToEndOfFile(); h.write(d); try? h.close()
    } else {
        try? d.write(to: URL(fileURLWithPath: logPath))
    }
}

@discardableResult
func sh(_ path: String, _ args: [String]) -> Int32 {
    let p = Process(); p.executableURL = URL(fileURLWithPath: path); p.arguments = args
    do { try p.run() } catch { return -1 }
    p.waitUntilExit(); return p.terminationStatus
}

func findWindowID() -> CGWindowID {
    let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
    if let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] {
        for w in list {
            let owner = (w[kCGWindowOwnerName as String] as? String) ?? ""
            let layer = (w[kCGWindowLayer as String] as? Int) ?? -1
            if layer == 0 && owner.lowercased().contains("unodos") {
                return CGWindowID((w[kCGWindowNumber as String] as? Int) ?? 0)
            }
        }
    }
    return 0
}

func captureWindow(_ windowID: CGWindowID) -> CGImage? {
    let sem = DispatchSemaphore(value: 0)
    var result: CGImage?
    Task {
        do {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            if let win = content.windows.first(where: { $0.windowID == windowID }) {
                let filter = SCContentFilter(desktopIndependentWindow: win)
                let cfg = SCStreamConfiguration()
                let scale = win.owningApplication != nil ? 2 : 2   // capture at 2x for a crisp image
                cfg.width = Int(win.frame.width) * scale
                cfg.height = Int(win.frame.height) * scale
                cfg.showsCursor = false
                cfg.captureResolution = .best
                result = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: cfg)
            } else {
                log("SCK: window \(windowID) not in shareable content")
            }
        } catch {
            log("SCK error: \(error)")
        }
        sem.signal()
    }
    _ = sem.wait(timeout: .now() + 20)
    return result
}

// Crop away fully-transparent margins (SCK renders the window at native size into
// the requested canvas; on a 1x session that leaves transparent padding). Scans
// the image's own pixel data (top-left origin) and crops the same CGImage, so
// there is no coordinate-flip to get wrong.
func trimTransparent(_ img: CGImage) -> CGImage {
    guard let data = img.dataProvider?.data, let ptr = CFDataGetBytePtr(data) else { return img }
    let w = img.width, h = img.height, bpr = img.bytesPerRow, bpp = img.bitsPerPixel / 8
    let aOff: Int
    switch img.alphaInfo {
    case .premultipliedLast, .last:   aOff = bpp - 1     // RGBA / BGRA
    case .premultipliedFirst, .first: aOff = 0           // ARGB
    default: return img                                  // no alpha -> nothing to trim
    }
    var minX = w, minY = h, maxX = -1, maxY = -1
    for y in 0..<h {
        let row = y * bpr
        for x in 0..<w where ptr[row + x * bpp + aOff] != 0 {
            if x < minX { minX = x }; if x > maxX { maxX = x }
            if y < minY { minY = y }; if y > maxY { maxY = y }
        }
    }
    guard maxX >= minX, maxY >= minY else { return img }
    let rect = CGRect(x: minX, y: minY, width: maxX - minX + 1, height: maxY - minY + 1)
    return img.cropping(to: rect) ?? img
}

func writePNG(_ img: CGImage, _ path: String) -> Bool {
    let rep = NSBitmapImageRep(cgImage: img)
    guard let png = rep.representation(using: .png, properties: [:]) else { return false }
    do { try png.write(to: URL(fileURLWithPath: path)); return true } catch { return false }
}

log("=== capture start ===")
let tmp = NSTemporaryDirectory() + "unoflash-" + UUID().uuidString
try? FileManager.default.createDirectory(atPath: tmp, withIntermediateDirectories: true)
let url = "https://github.com/hmofet/unodos/releases/latest/download/UnoDosFlasher-macOS.zip"
sh("/usr/bin/curl", ["-sL", "-o", tmp + "/f.zip", url])
sh("/usr/bin/ditto", ["-x", "-k", tmp + "/f.zip", tmp])
let apps = ((try? FileManager.default.contentsOfDirectory(atPath: tmp)) ?? []).filter { $0.hasSuffix(".app") }
if let appName = apps.first {
    sh("/usr/bin/open", [tmp + "/" + appName])

    var wid: CGWindowID = 0
    for _ in 0..<24 { wid = findWindowID(); if wid != 0 { break }; Thread.sleep(forTimeInterval: 0.5) }
    log("windowID=\(wid)")
    Thread.sleep(forTimeInterval: 1.0)

    if wid != 0, let raw = captureWindow(wid) {
        let img = trimTransparent(raw)
        let out = outDir + "/flasher-macos.png"
        if writePNG(img, out) { log("captured \(img.width)x\(img.height) (raw \(raw.width)x\(raw.height)) -> \(out)") }
        else { log("PNG encode/write failed") }
    } else {
        log("capture FAILED (window not found or no image)")
    }

    sh("/usr/bin/osascript", ["-e", "tell application \"UnoDosFlasher\" to quit"])
} else {
    log("no .app found in the downloaded zip")
}
try? FileManager.default.removeItem(atPath: tmp)   // do not keep the download
log("=== done ===")
