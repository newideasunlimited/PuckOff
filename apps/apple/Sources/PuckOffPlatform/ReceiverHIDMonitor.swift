// SPDX-License-Identifier: Apache-2.0
#if os(macOS)
  import Foundation
  import IOKit.hid
  import Observation
  import OSLog
  import PuckOffCore

  @MainActor
  @Observable
  public final class ReceiverHIDMonitor {
    public private(set) var isRunning = false
    public private(set) var state = "HID diagnostics are stopped."
    public private(set) var input: GamepadInput?
    public private(set) var reportCount: UInt64 = 0
    public private(set) var errorMessage: String?
    @ObservationIgnored private var manager: IOHIDManager?
    @ObservationIgnored private var compatibleDeviceFound = false
    @ObservationIgnored private var received: UInt64 = 0
    @ObservationIgnored private var lastPublication: ContinuousClock.Instant?

    public init() {}

    public func start(_ candidate: ReceiverCandidate) throws {
      guard !isRunning else { return }
      try ReceiverDiscovery.validate(candidate)
      guard let location = candidate.locationID else { throw ReceiverAccessError.missingMetadata }
      errorMessage = nil
      input = nil
      reportCount = 0
      received = 0
      lastPublication = nil
      compatibleDeviceFound = false
      let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
      let match: [String: Any] = [
        kIOHIDVendorIDKey: ReceiverUSB.vendorID,
        kIOHIDProductIDKey: ReceiverUSB.productID,
        kIOHIDDeviceUsagePageKey: ReceiverUSB.usagePage,
        kIOHIDDeviceUsageKey: ReceiverUSB.usage,
        kIOHIDLocationIDKey: location,
      ]
      IOHIDManagerSetDeviceMatching(manager, match as CFDictionary)
      let context = Unmanaged.passUnretained(self).toOpaque()
      IOHIDManagerRegisterDeviceMatchingCallback(
        manager,
        { context, result, _, device in
          guard let context else { return }
          MainActor.assumeIsolated {
            let monitor = Unmanaged<ReceiverHIDMonitor>.fromOpaque(context).takeUnretainedValue()
            monitor.matched(device, result: result)
          }
        }, context)
      IOHIDManagerRegisterDeviceRemovalCallback(
        manager,
        { context, _, _, _ in
          guard let context else { return }
          MainActor.assumeIsolated {
            let monitor = Unmanaged<ReceiverHIDMonitor>.fromOpaque(context).takeUnretainedValue()
            monitor.fail(ReceiverAccessError.disconnected)
          }
        }, context)
      IOHIDManagerRegisterInputReportCallback(
        manager,
        {
          context, result, _, type, reportID, report, length in
          guard let context else { return }
          MainActor.assumeIsolated {
            let monitor = Unmanaged<ReceiverHIDMonitor>.fromOpaque(context).takeUnretainedValue()
            guard result == kIOReturnSuccess else {
              monitor.fail(ReceiverAccessError.hid(result))
              return
            }
            guard type == kIOHIDReportTypeInput else { return }
            guard length == ReceiverHIDReport.byteCount else {
              monitor.fail(HIDReportError.unsupportedReport)
              return
            }
            monitor.accept(Data(bytes: report, count: length), reportID: reportID)
          }
        }, context)
      self.manager = manager
      isRunning = true
      state = "Waiting for the selected receiver's HID interface."
      IOHIDManagerScheduleWithRunLoop(
        manager, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
      let result = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
      guard result == kIOReturnSuccess else {
        let error = ReceiverAccessError.hid(result)
        fail(error)
        throw error
      }
    }

    public func stop() {
      errorMessage = nil
      isRunning = false
      compatibleDeviceFound = false
      input = nil
      reportCount = 0
      state = "HID diagnostics are stopped."
      if let manager {
        let result = Self.close(manager)
        self.manager = nil
        if result != kIOReturnSuccess {
          errorMessage = ReceiverAccessError.hid(result).localizedDescription
        }
      }
    }

    private func matched(_ device: IOHIDDevice, result: IOReturn) {
      guard isRunning else { return }
      guard result == kIOReturnSuccess else {
        fail(ReceiverAccessError.hid(result))
        return
      }
      guard
        let size = IOHIDDeviceGetProperty(
          device, kIOHIDMaxInputReportSizeKey as CFString
        ) as? NSNumber, size.intValue == ReceiverHIDReport.byteCount
      else {
        fail(ReceiverAccessError.incompatibleHID)
        return
      }
      compatibleDeviceFound = true
      state = "HID interface opened without exclusive capture; waiting for input."
    }

    private func accept(_ report: Data, reportID: UInt32) {
      guard isRunning, compatibleDeviceFound else { return }
      do {
        let sample = try ReceiverHIDReport.decode(report, reportID: reportID)
        received &+= 1
        let now = ContinuousClock.now
        if let lastPublication, now - lastPublication < .milliseconds(50) { return }
        lastPublication = now
        input = sample
        reportCount = received
        state = "Receiving the experimental USB gamepad report."
      } catch let error as HIDReportError {
        fail(error)
      } catch {
        fail(HIDReportError.unsupportedReport)
      }
    }

    private func fail(_ error: any LocalizedError) {
      stop()
      if let cleanupError = errorMessage {
        errorMessage = "\(error.localizedDescription) \(cleanupError)"
      } else {
        errorMessage = error.localizedDescription
      }
    }

    private nonisolated static func close(_ manager: IOHIDManager) -> IOReturn {
      IOHIDManagerRegisterInputReportCallback(manager, nil, nil)
      IOHIDManagerRegisterDeviceMatchingCallback(manager, nil, nil)
      IOHIDManagerRegisterDeviceRemovalCallback(manager, nil, nil)
      IOHIDManagerUnscheduleFromRunLoop(
        manager, CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue)
      return IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    isolated deinit {
      if let manager {
        let result = Self.close(manager)
        if result != kIOReturnSuccess {
          Logger(subsystem: "com.newideasunlimited.puckoff", category: "HID")
            .error("HID cleanup failed (I/O Kit code \(result, privacy: .public)).")
        }
      }
    }
  }
#endif
