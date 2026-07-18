// winid - print the CGWindow id of the first on-screen UnoDOS flasher window.
// Window number + geometry are available without Screen Recording permission;
// only the pixels (screencapture) need it.
import CoreGraphics
import Foundation

let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
var found = -1
if let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] {
    for w in list {
        let owner = (w[kCGWindowOwnerName as String] as? String) ?? ""
        let layer = (w[kCGWindowLayer as String] as? Int) ?? -1
        if layer == 0 && owner.lowercased().contains("unodos") {
            found = (w[kCGWindowNumber as String] as? Int) ?? -1
            break
        }
    }
}
print(found)
