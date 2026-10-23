// SPDX-License-Identifier: Apache-2.0
#if os(macOS)
  import Foundation
  import IOKit
  import IOKit.serial
  import PuckOffCore

  public enum SerialStage: String, Sendable {
    case opening = "opening the serial port"
    case exclusivity = "reserving the serial port"
    case configuring = "configuring the serial port"
    case reading = "reading the serial port"
    case writing = "writing the read-only request"
    case restoring = "restoring the serial port settings"
    case closing = "closing the serial port"
  }

  public enum ReceiverAccessError: Error, LocalizedError {
    case discovery(Int32)
    case missingMetadata
    case candidateGone
    case serial(SerialStage, Int32)
    case disconnected
    case hid(Int32)
    case incompatibleHID

    public var errorDescription: String? {
      switch self {
      case .discovery(let code):
        "USB serial discovery failed (I/O Kit code \(code))."
      case .missingMetadata:
        "A matching USB serial interface could not be safely identified."
      case .candidateGone:
        "The selected USB interface changed or was removed. Scan and select it again."
      case .serial(let stage, let code):
        "An error occurred while \(stage.rawValue) (system code \(code)). Close other serial apps and check device access."
      case .disconnected:
        "The receiver disconnected before a complete response arrived."
      case .hid(let code):
        "HID diagnostics failed (I/O Kit code \(code)). Check macOS device-access permissions."
      case .incompatibleHID:
        "The selected HID interface does not advertise the supported 13-byte receiver report."
      }
    }
  }

  public struct ReceiverCandidate: Identifiable, Equatable, Sendable {
    public let id: UInt64
    public let label: String
    public var supportsHIDSelection: Bool { locationID != nil }
    let serialPath: String
    let locationID: UInt32?
  }

  public enum ReceiverDiscovery {
    public static func scan() throws -> [ReceiverCandidate] {
      guard let matching = IOServiceMatching(kIOSerialBSDServiceValue) else {
        throw ReceiverAccessError.missingMetadata
      }
      var iterator: io_iterator_t = 0
      let result = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
      guard result == KERN_SUCCESS else { throw ReceiverAccessError.discovery(result) }
      defer { IOObjectRelease(iterator) }
      var entries: [(UInt64, String, UInt32?)] = []
      while true {
        let service = IOIteratorNext(iterator)
        guard service != 0 else { break }
        defer { IOObjectRelease(service) }
        guard number("idVendor", service: service)?.intValue == ReceiverUSB.vendorID,
          number("idProduct", service: service)?.intValue == ReceiverUSB.productID
        else { continue }
        guard
          let path = IORegistryEntryCreateCFProperty(
            service, kIOCalloutDeviceKey as CFString, kCFAllocatorDefault, 0
          )?.takeRetainedValue() as? String,
          validCalloutPath(path)
        else { throw ReceiverAccessError.missingMetadata }
        var id: UInt64 = 0
        let identityResult = IORegistryEntryGetRegistryEntryID(service, &id)
        guard identityResult == KERN_SUCCESS else {
          throw ReceiverAccessError.discovery(identityResult)
        }
        entries.append((id, path, number("locationID", service: service)?.uint32Value))
      }
      return entries.sorted { $0.0 < $1.0 }.enumerated().map { index, entry in
        ReceiverCandidate(
          id: entry.0, label: "USB receiver candidate \(index + 1)",
          serialPath: entry.1, locationID: entry.2
        )
      }
    }

    static func validate(_ candidate: ReceiverCandidate) throws {
      guard
        try scan().contains(where: {
          $0.id == candidate.id && $0.serialPath == candidate.serialPath
            && $0.locationID == candidate.locationID
        })
      else {
        throw ReceiverAccessError.candidateGone
      }
    }

    private static func number(_ key: String, service: io_registry_entry_t) -> NSNumber? {
      IORegistryEntrySearchCFProperty(
        service, kIOServicePlane, key as CFString, kCFAllocatorDefault,
        IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents)
      ) as? NSNumber
    }

    private static func validCalloutPath(_ path: String) -> Bool {
      let prefix = "/dev/cu."
      guard path.hasPrefix(prefix), path.utf8.count <= 256 else { return false }
      let suffix = path.dropFirst(prefix.count)
      return !suffix.isEmpty
        && suffix.utf8.allSatisfy {
          (65...90).contains($0) || (97...122).contains($0)
            || (48...57).contains($0) || [45, 46, 95].contains($0)
        }
    }
  }
#endif
