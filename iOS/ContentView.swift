import SwiftUI

struct ContentView: View {
  @State private var controller = RigController()

  var body: some View {
    @Bindable var controller = controller
    NavigationStack {
      Form {
        Section {
          HStack(spacing: 10) {
            Circle()
              .fill(statusColor)
              .frame(width: 8, height: 8)
              .accessibilityHidden(true)
            Text(controller.statusText)
          }
          if let warning = controller.protocolWarning {
            Text(warning).font(.footnote)
          }
        }
        Section {
          AxisSlider(title: "Tilt", value: $controller.tilt,
                     range: controller.limits.tilt, reported: controller.reported?.tilt,
                     editingChanged: controller.editingChanged)
          AxisSlider(title: "Pan", value: $controller.pan,
                     range: controller.limits.pan, reported: controller.reported?.pan,
                     editingChanged: controller.editingChanged)
        } footer: {
          if let reported = controller.reported {
            Text(reported.moving ? "Rig moving" : "Rig holding position")
          }
        }
        Section {
          HStack(spacing: 16) {
            Button("Center", action: controller.center)
              .frame(maxWidth: .infinity, minHeight: 44)
            Button("Stop", action: controller.stop)
              .frame(maxWidth: .infinity, minHeight: 44)
          }
          .buttonStyle(.bordered)
        }
        Section {
          RigDebugView(controller: controller)
        }
      }
      .tint(.primary)
      .navigationTitle("Unhinged")
      .task { controller.start() }
      .onDisappear { controller.shutdown() }
    }
  }

  private var statusColor: Color {
    switch controller.link {
    case .connected: .green
    case .bridgeOnly, .rigSilent: .orange
    case .disconnected, .connecting: .gray
    }
  }
}

private struct AxisSlider: View {
  var title: String
  @Binding var value: Double
  var range: ClosedRange<Double>
  var reported: Double?
  var editingChanged: (Bool) -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("\(title) \(RigProtocol.number(value))°")
        .monospacedDigit()
      Text(reported.map { "rig \(RigProtocol.number($0))°" } ?? "rig unknown")
        .font(.subheadline)
        .foregroundStyle(.secondary)
        .monospacedDigit()
      Slider(value: $value, in: range, onEditingChanged: editingChanged) {
        Text(title)
      }
      .accessibilityValue("\(RigProtocol.number(value)) degrees")
    }
    .padding(.vertical, 8)
  }
}

private struct RigDebugView: View {
  @Bindable var controller: RigController

  var body: some View {
    DisclosureGroup("Debug") {
      TextField("Host", text: $controller.host)
        .textInputAutocapitalization(.never)
        .autocorrectionDisabled()
        .keyboardType(.URL)
      TextField("Port", text: $controller.port)
        .keyboardType(.numberPad)
      Button("Apply address", action: controller.reconnect)
      VStack(alignment: .leading, spacing: 12) {
        debugLine("Last line", value: controller.lastLine)
        debugLine("Last error", value: controller.lastError)
        debugLine("Last reset", value: controller.lastReset)
        if controller.lastReset == "BROWNOUT" {
          Text("Servo power dropped too low.")
        }
        Text("Reported angles are commanded positions, not measured feedback.")
          .foregroundStyle(.secondary)
      }
      .font(.footnote)
      .padding(.vertical, 8)
      .textSelection(.enabled)
    }
  }

  private func debugLine(_ title: String, value: String) -> some View {
    VStack(alignment: .leading, spacing: 4) {
      Text(title).foregroundStyle(.secondary)
      Text(value.isEmpty ? "None" : value).font(.system(.footnote, design: .monospaced))
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}
