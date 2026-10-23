// SPDX-License-Identifier: Apache-2.0
#if os(macOS)
  import Foundation
  import Observation
  import PuckOffCore
  import PuckOffPlatform

  @MainActor
  @Observable
  final class ReceiverModel {
    private(set) var candidates: [ReceiverCandidate] = []
    private(set) var hasScanned = false
    private(set) var isScanning = false
    private(set) var isReading = false
    private(set) var snapshot: ReceiverSnapshot?
    private(set) var readAt: Date?
    private(set) var errorMessage: String?
    let hid = ReceiverHIDMonitor()
    var selectedID: UInt64? {
      didSet {
        if selectedID != oldValue {
          cancelRead()
          hid.stop()
          errorMessage = nil
        }
      }
    }
    var selected: ReceiverCandidate? { candidates.first { $0.id == selectedID } }
    var isBusy: Bool { isReading || isScanning }
    @ObservationIgnored private var scanTask: Task<Void, Never>?
    @ObservationIgnored private var readTask: Task<Void, Never>?
    @ObservationIgnored private var scanToken: UUID?
    @ObservationIgnored private var readToken: UUID?

    func scan() {
      stop()
      candidates = []
      selectedID = nil
      errorMessage = nil
      hasScanned = false
      isScanning = true
      let token = UUID()
      scanToken = token
      scanTask = Task { @MainActor [weak self] in
        do {
          let candidates = try await Task.detached {
            try ReceiverDiscovery.scan()
          }.value
          guard let self, scanToken == token, !Task.isCancelled else { return }
          self.candidates = candidates
          hasScanned = true
        } catch {
          guard let self, scanToken == token, !Task.isCancelled else { return }
          report(error)
        }
        guard let self, scanToken == token else { return }
        isScanning = false
        scanTask = nil
        scanToken = nil
      }
    }

    func readStatus() {
      guard let selected, !isBusy else {
        errorMessage = "Select a receiver and wait for any current operation to finish."
        return
      }
      cancelRead()
      errorMessage = nil
      isReading = true
      let token = UUID()
      readToken = token
      readTask = Task { @MainActor [weak self] in
        do {
          let result = try await ReceiverSerial.read(selected)
          guard let self, readToken == token, !Task.isCancelled else { return }
          snapshot = result
          readAt = Date()
        } catch {
          guard let self, readToken == token, !Task.isCancelled else { return }
          report(error)
        }
        guard let self, readToken == token else { return }
        isReading = false
        readTask = nil
        readToken = nil
      }
    }

    func cancelRead() {
      readToken = nil
      readTask?.cancel()
      readTask = nil
      isReading = false
      snapshot = nil
      readAt = nil
    }

    func startHID() {
      guard let selected, !isScanning else {
        errorMessage = "Select a receiver before starting HID diagnostics."
        return
      }
      errorMessage = nil
      do {
        try hid.start(selected)
      } catch {
        report(error)
      }
    }

    func stop() {
      scanToken = nil
      scanTask?.cancel()
      scanTask = nil
      isScanning = false
      cancelRead()
      hid.stop()
    }

    private func report(_ error: any Error) {
      if let error = error as? ManagementError {
        errorMessage = error.localizedDescription
      } else if let error = error as? ReceiverAccessError {
        errorMessage = error.localizedDescription
      } else if error is CancellationError {
        errorMessage = "The receiver operation was cancelled."
      } else {
        errorMessage = "The receiver operation failed unexpectedly. No result is available."
      }
    }

    deinit {
      scanTask?.cancel()
      readTask?.cancel()
    }
  }
#endif
