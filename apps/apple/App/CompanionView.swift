// SPDX-License-Identifier: Apache-2.0
import PuckOffCore
import PuckOffPlatform
import SwiftUI

struct CompanionView: View {
  @Environment(\.scenePhase) private var scenePhase
  @State private var controllers = AppleControllerMonitor()
  #if os(macOS)
    @State private var receiver = ReceiverModel()
  #endif

  var body: some View {
    TabView {
      #if os(macOS)
        ReceiverView(model: receiver)
          .tabItem { Label("Receiver", systemImage: "externaldrive") }
      #else
        IOSSetupView()
          .tabItem { Label("Setup", systemImage: "checklist") }
      #endif
      ControllerInputView(monitor: controllers)
        .tabItem { Label("Input", systemImage: "gamecontroller") }
      AboutView()
        .tabItem { Label("About", systemImage: "info.circle") }
    }
    .onChange(of: scenePhase) { _, phase in
      if phase != .active { stopDiagnostics() }
    }
    .onDisappear { stopDiagnostics() }
  }

  private func stopDiagnostics() {
    controllers.stop()
    #if os(macOS)
      receiver.stop()
    #endif
  }
}

struct ControllerInputView: View {
  let monitor: AppleControllerMonitor

  var body: some View {
    NavigationStack {
      Form {
        Section("Apple controller input") {
          Text(
            "Read controllers that this operating system already exposes through GameController. Input is displayed only inside PuckOff."
          )
          Button(monitor.isRunning ? "Stop diagnostics" : "Start diagnostics") {
            if monitor.isRunning { monitor.stop() } else { monitor.start() }
          }
          if let error = monitor.errorMessage {
            Text(error).foregroundStyle(.red)
          }
          if !monitor.isRunning {
            Text("Diagnostics are stopped. No controller input is being read.")
              .foregroundStyle(.secondary)
          } else if monitor.controllers.isEmpty {
            Text("No controllers are currently exposed to this app by GameController.")
            Text(
              "Pair a supported controller in system settings, or connect a controller supported by this OS. A USB connection alone does not establish compatibility."
            )
            .foregroundStyle(.secondary)
          }
        }
        ForEach(monitor.controllers) { controller in
          Section(controller.label) {
            LabeledContent("Profile", value: controller.profile)
            if let input = controller.input {
              InputValuesView(input: input)
            }
          }
        }
        Section("Scope") {
          Text(
            "Values refresh at 20 Hz with positive Y pointing up. This is an input diagnostic, not a latency test. Monitoring stops when the app leaves the foreground."
          )
          #if os(macOS)
            Text(
              "The early PuckOff generic USB HID device may not appear in GameController. Use the Receiver tab for its separate raw HID diagnostic. The two views may show the same physical device."
            )
          #else
            Text(
              "This app cannot inject a virtual controller into other iPhone or iPad apps. An unsupported receiver does not become system-compatible by opening PuckOff."
            )
          #endif
        }
      }
      .formStyle(.grouped)
      .navigationTitle("Input diagnostics")
    }
  }
}

struct InputValuesView: View {
  let input: GamepadInput

  var body: some View {
    LabeledContent(
      "Pressed buttons",
      value: input.buttons.isEmpty ? "None" : input.buttons.map(\.rawValue).joined(separator: ", ")
    )
    ForEach(input.axes) { reading in
      VStack(alignment: .leading, spacing: 4) {
        LabeledContent(
          reading.axis.rawValue,
          value: reading.value.formatted(.number.precision(.fractionLength(2)))
        )
        let range = reading.axis.range
        ProgressView(
          value: reading.value - range.lowerBound,
          total: range.upperBound - range.lowerBound
        )
        .accessibilityLabel(reading.axis.rawValue)
        .accessibilityValue(reading.value.formatted(.number.precision(.fractionLength(2))))
      }
    }
  }
}

private struct IOSSetupView: View {
  var body: some View {
    NavigationStack {
      Form {
        Section("Use an OS-supported controller") {
          Text(
            "1. Pair a compatible controller in Settings, or connect a wired controller that your device and OS support."
          )
          Text(
            "2. Open Input and choose Start diagnostics to check what GameController exposes to this app."
          )
          Text(
            "3. Use the controller in a game that supports it. PuckOff does not route input to that game."
          )
          Link(
            "Apple controller connection guidance",
            destination: URL(
              string: "https://support.apple.com/en-us/111099"
            )!)
        }
        Section("PuckOff receiver management") {
          LabeledContent("Generic USB CDC serial", value: "Not supported here")
          LabeledContent("Receiver BLE management", value: "Not implemented")
          Text(
            "Use the native macOS app to discover the USB receiver and read management-v1 status. This iOS app has no receiver transport, does not pair the receiver, and does not import controller bonds."
          )
        }
        Section("Platform boundary") {
          Text(
            "An ordinary iOS app cannot create a system-wide virtual gamepad for unrelated apps. Apple's GCVirtualController provides controls for the app that creates it; it is not an injection API."
          )
          Text(
            "Generic USB HID enumeration or a working browser test on a Mac is not evidence that the same receiver works with iPhone games."
          )
        }
      }
      .formStyle(.grouped)
      .navigationTitle("PuckOff setup")
    }
  }
}

private struct AboutView: View {
  var body: some View {
    NavigationStack {
      Form {
        Section("PuckOff companion - early native slice") {
          Text(
            "Independent open-source software from New Ideas Unlimited, LLC and the PuckOff contributors. Licensed under Apache-2.0."
          )
          Link(
            "Project and build instructions",
            destination: URL(
              string: "https://github.com/newideasunlimited/PuckOff/tree/main/apps/apple"
            )!)
        }
        Section("Privacy and control") {
          Text(
            "Device access starts only after an explicit action. The app does not store or upload input, serial transcripts, controller identifiers, or pairing records. Device labels are local session labels, not serial numbers."
          )
          Text(
            "Status reads use only management-v1 info and status. Pairing import, export, clear, firmware flashing, accounts, billing, and Android are not included."
          )
        }
        Section("Public APIs, not compatibility promises") {
          Text(
            "macOS uses I/O Kit for discovery and non-exclusive HID input, POSIX serial for read-only status, and GameController for supported controllers. No driver is installed."
          )
          Text(
            "iOS uses GameController for in-app diagnostics only. External Accessory is not an unrestricted generic USB serial API. Future BLE management would require a real, documented firmware service."
          )
          Text(
            "No virtual-controller output is implemented on either platform. macOS driver-based virtual HID is separate, entitlement-dependent work."
          )
          Link(
            "Apple GameController documentation",
            destination: URL(
              string: "https://developer.apple.com/documentation/gamecontroller/gccontroller"
            )!)
          Link(
            "Apple virtual controller scope",
            destination: URL(
              string: "https://developer.apple.com/documentation/gamecontroller/gcvirtualcontroller"
            )!)
          Link(
            "Apple DriverKit availability",
            destination: URL(
              string: "https://developer.apple.com/documentation/driverkit"
            )!)
        }
      }
      .formStyle(.grouped)
      .navigationTitle("About PuckOff")
    }
  }
}
