import SwiftUI

@main
struct UnoDosFlasherApp: App {
    @StateObject private var model = FlashModel()

    var body: some Scene {
        Window("UnoDOS — USB Installer", id: "main") {
            FlashView(model: model)
                .frame(width: 560, height: 400)
                .onAppear { model.refresh() }
        }
        .windowResizability(.contentSize)
    }
}

struct FlashView: View {
    @ObservedObject var model: FlashModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Install UnoDOS/pc64 to a USB drive")
                .font(.system(size: 18, weight: .bold))

            VStack(alignment: .leading, spacing: 6) {
                Text("Target USB drive:")
                HStack(spacing: 8) {
                    Picker("", selection: $model.selection) {
                        if model.drives.isEmpty {
                            Text("No USB drives found").tag(USBDrive.ID?.none)
                        }
                        ForEach(model.drives) { d in
                            Text(d.label).tag(USBDrive.ID?.some(d.id))
                        }
                    }
                    .labelsHidden()
                    .disabled(model.busy)

                    if model.showAll || model.hiddenCount > 0 {
                        Button(model.showAll ? "Hide empty" : "Show all (\(model.hiddenCount))") {
                            model.toggleShowAll()
                        }
                        .disabled(model.busy)
                    }

                    Button("Refresh") { model.refresh() }
                        .disabled(model.busy)
                }
            }

            Text(model.notes)
                .font(.system(size: 12))
                .foregroundColor(Color(white: 0.30))
                .fixedSize(horizontal: false, vertical: true)
                .frame(height: 72, alignment: .top)

            Text("Everything on the selected drive will be erased.")
                .foregroundColor(Color(red: 176/255, green: 0, blue: 32/255))

            ProgressView(value: model.progress)
                .progressViewStyle(.linear)

            HStack {
                Text(model.status)
                    .font(.system(size: 12))
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                Spacer()
                Button(action: model.flash) {
                    Text("Install").frame(width: 64)
                }
                .keyboardShortcut(.defaultAction)
                .disabled(model.busy || model.selectedDrive == nil)
            }
        }
        .padding(20)
    }
}
