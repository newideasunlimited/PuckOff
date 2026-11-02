// SPDX-License-Identifier: Apache-2.0
#if os(macOS)
  import PuckOffCore
  import PuckOffPlatform
  import SwiftUI

  struct ReceiverView: View {
    @Bindable var model: ReceiverModel
    @State private var confirmStatus = false

    var body: some View {
      NavigationStack {
        Form {
          Section("USB discovery") {
            Text(
              "Find CDC serial interfaces with experimental VID 1209 / PID 0001. These test IDs are not proof that a device is PuckOff. Disconnect unrelated devices that use the same IDs."
            )
            Button("Scan USB interfaces") { model.scan() }
              .disabled(model.isBusy)
            if model.isScanning {
              ProgressView("Inspecting USB metadata...")
            } else if !model.hasScanned {
              Text("No discovery scan has completed. No receiver is selected.")
                .foregroundStyle(.secondary)
            } else if model.candidates.isEmpty {
              Text("No matching USB serial interfaces found. Connect a receiver and scan again.")
            }
            Picker("Receiver", selection: $model.selectedID) {
              Text("Select an interface").tag(nil as UInt64?)
              ForEach(model.candidates) { candidate in
                Text(candidate.label).tag(Optional(candidate.id))
              }
            }
            .disabled(model.isBusy || model.candidates.isEmpty)
            Text(
              "Scanning inspects registry metadata; it does not open a serial port. A single result is not selected automatically."
            )
            .font(.caption)
            .foregroundStyle(.secondary)
          }
          Section("Read-only setup status") {
            Text(
              "Requires firmware with the PuckOff management-v1 contract. The original receiver firmware is not supported by this status reader. No legacy shell fallback is attempted."
            )
            Button("Read receiver status...") { confirmStatus = true }
              .disabled(model.selected == nil || model.isBusy)
            if model.isReading {
              ProgressView("Requesting info and status...")
              Button("Cancel status read") { model.cancelRead() }
            }
            if let snapshot = model.snapshot, let readAt = model.readAt {
              ReceiverSnapshotView(snapshot: snapshot, readAt: readAt)
            } else if !model.isReading {
              Text("No receiver status has been read.")
                .foregroundStyle(.secondary)
            }
          }
          if let error = model.errorMessage {
            Section("Receiver error") {
              Text(error).foregroundStyle(.red)
            }
          }
          Section("Raw receiver HID input") {
            Text(
              "Open only the selected USB location's gamepad interface, without exclusive capture. This decodes the early firmware's 13-byte input report inside PuckOff; it does not create a virtual gamepad."
            )
            if model.hid.isRunning {
              Button("Stop receiver HID diagnostics") { model.hid.stop() }
            } else {
              Button("Start receiver HID diagnostics") { model.startHID() }
                .disabled(
                  model.selected?.supportsHIDSelection != true || model.isBusy
                )
            }
            if let selected = model.selected, !selected.supportsHIDSelection {
              Text("USB location metadata is missing, so HID selection is unavailable.")
            }
            Text(model.hid.state)
            if let error = model.hid.errorMessage {
              Text(error).foregroundStyle(.red)
            }
            if let input = model.hid.input {
              LabeledContent("Reports received", value: model.hid.reportCount.formatted())
              InputValuesView(input: input)
              Text("Display refresh is limited to 20 Hz; input is not recorded.")
                .font(.caption)
                .foregroundStyle(.secondary)
            }
          }
          Section("Pairing and firmware") {
            Text(
              "Bond import, export, overwrite, clear, native pairing, and bootloader entry are not implemented in this app. Status never includes controller IDs or serial numbers."
            )
          }
        }
        .formStyle(.grouped)
        .navigationTitle("PuckOff receiver")
      }
      .confirmationDialog("Read the selected receiver?", isPresented: $confirmStatus) {
        Button("Read status without pairing changes") { model.readStatus() }
        Button("Cancel", role: .cancel) {}
      } message: {
        Text(
          "Use management-v1 firmware and close other serial tools. This opens CDC at 115200 baud and asserts DTR. The app waits for a shell-cancellation acknowledgment before sending info/status; without it, no command terminator is sent. No pairing or bootloader commands are sent. Opening a port can affect device control lines. The port closes after the read."
        )
      }
    }
  }

  private struct ReceiverSnapshotView: View {
    let snapshot: ReceiverSnapshot
    let readAt: Date

    var body: some View {
      LabeledContent("Management protocol", value: "v\(snapshot.info.protocolVersion)")
      LabeledContent("Last read", value: readAt.formatted(date: .omitted, time: .standard))
      LabeledContent(
        "Pairing storage",
        value: snapshot.status.storageReady ? "Ready at last read" : "Unavailable"
      )
      if !snapshot.status.storageReady {
        Text(
          "Receiver storage is not ready. The snapshot is not evidence of persisted pairing records."
        )
        .foregroundStyle(.red)
      }
      Text("This is a snapshot, not a live serial connection. The port has been closed.")
        .font(.caption)
        .foregroundStyle(.secondary)
      ForEach(snapshot.status.slots) { slot in
        LabeledContent("Slot \(slot.slot)", value: slot.stateDescription)
        if slot.used {
          LabeledContent(
            "Signal",
            value: slot.connected && slot.rssi != 0 ? "\(slot.rssi) dBm" : "Unavailable"
          )
          Text(
            "Polls \(slot.polls) | Replies \(slot.replies) | Reports \(slot.reports) | CRC errors \(slot.crcErrors)"
          )
          .font(.caption.monospacedDigit())
        }
      }
    }
  }
#endif
