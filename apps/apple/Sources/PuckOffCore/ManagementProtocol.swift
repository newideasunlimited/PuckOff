// SPDX-License-Identifier: Apache-2.0
import Foundation

public enum ManagementError: Error, LocalizedError, Equatable {
  case invalidRequestID
  case invalidResponse
  case unsupportedFirmware
  case responseTooLarge
  case timeout
  case shellNotReady
  case storageUnavailable
  case firmwareRejected

  public var errorDescription: String? {
    switch self {
    case .invalidRequestID:
      "The management request ID is invalid."
    case .invalidResponse:
      "The receiver returned an invalid management response."
    case .unsupportedFirmware:
      "This device does not advertise the supported PuckOff management v1 contract."
    case .responseTooLarge:
      "The receiver exceeded the management response safety limits."
    case .timeout:
      "No matching management v1 response arrived before the deadline. Older firmware may not support setup info/status. No legacy commands were sent."
    case .shellNotReady:
      "The shell did not acknowledge safe line cancellation, so this request was not sent. Restart the receiver and use firmware with shell echo, metakeys, and VT100 enabled."
    case .storageUnavailable:
      "Receiver pairing storage is unavailable. No pairing data was changed."
    case .firmwareRejected:
      "The receiver rejected the read-only management request."
    }
  }
}

public enum ManagementOperation: String, Sendable {
  case info
  case status
}

public struct ManagementRequest: Equatable, Sendable {
  public let operation: ManagementOperation
  public let id: String

  public init(_ operation: ManagementOperation, id: String) throws {
    let bytes = Array(id.utf8)
    guard (1...16).contains(bytes.count),
      bytes.allSatisfy({
        (65...90).contains($0) || (97...122).contains($0)
          || (48...57).contains($0) || $0 == 95 || $0 == 45
      })
    else {
      throw ManagementError.invalidRequestID
    }
    self.operation = operation
    self.id = id
  }

  public var bytes: Data { Data("setup \(operation.rawValue) \(id)\r".utf8) }

  public static func freshID() -> String {
    String(UUID().uuidString.replacingOccurrences(of: "-", with: "").prefix(16))
  }
}

private struct Envelope: Decodable {
  let id: String
  let op: String
  let ok: Bool
  let error: String?

  func matches(_ request: ManagementRequest) -> Bool {
    id == request.id && op == request.operation.rawValue
  }

  func validate(_ request: ManagementRequest) throws {
    guard matches(request) else { throw ManagementError.invalidResponse }
    guard ok else {
      if error == "storage_unavailable" { throw ManagementError.storageUnavailable }
      throw ManagementError.firmwareRejected
    }
    guard error == nil else { throw ManagementError.invalidResponse }
  }
}

private func decode<T: Decodable>(_ type: T.Type, from data: Data) throws -> T {
  do {
    return try JSONDecoder().decode(type, from: data)
  } catch is DecodingError {
    // Decoder errors can contain receiver-supplied strings. Never expose them.
    throw ManagementError.invalidResponse
  }
}

public struct ManagementResponseCollector {
  public static let maximumLineBytes = 1_024
  public static let maximumReceiveBytes = 16_384
  private static let prefix = Data("PUCKOFF/1 ".utf8)

  private let request: ManagementRequest
  private var line = Data()
  private var received = 0

  public init(request: ManagementRequest) {
    self.request = request
  }

  public mutating func append(_ data: Data) throws -> Data? {
    guard data.count <= Self.maximumReceiveBytes - received else {
      throw ManagementError.responseTooLarge
    }
    received += data.count
    for byte in data {
      if byte == 10 {
        if line.last == 13 { line.removeLast() }
        let completeLine = line
        line.removeAll(keepingCapacity: true)
        guard completeLine.starts(with: Self.prefix) else { continue }
        let payload = Data(completeLine.dropFirst(Self.prefix.count))
        let envelope = try decode(Envelope.self, from: payload)
        guard envelope.matches(request) else { continue }
        try envelope.validate(request)
        return payload
      }
      line.append(byte)
      guard
        line.count <= Self.maximumLineBytes
          || (line.count == Self.maximumLineBytes + 1 && byte == 13)
      else {
        throw ManagementError.responseTooLarge
      }
    }
    return nil
  }
}

public struct ReceiverInfo: Equatable, Sendable {
  public let protocolVersion: Int
  public let slotCount: Int
  public let nativePairing: Bool
}

public struct ReceiverSlot: Decodable, Equatable, Identifiable, Sendable {
  public let slot: Int
  public let used: Bool
  public let connected: Bool
  public let poweredOff: Bool
  public let rssi: Int
  public let polls: UInt32
  public let replies: UInt32
  public let reports: UInt32
  public let crcErrors: UInt32
  public var id: Int { slot }

  public var stateDescription: String {
    if !used { return "Empty" }
    if connected { return "Connected" }
    return poweredOff ? "Controller off" : "Searching"
  }

  enum CodingKeys: String, CodingKey {
    case slot, used, connected, rssi, polls, replies, reports
    case poweredOff = "powered_off"
    case crcErrors = "crc_errors"
  }
}

public struct ReceiverStatus: Equatable, Sendable {
  public let storageReady: Bool
  public let slots: [ReceiverSlot]
}

public struct ReceiverSnapshot: Equatable, Sendable {
  public let info: ReceiverInfo
  public let status: ReceiverStatus
}

public enum ManagementResponse {
  public static func info(_ data: Data, for request: ManagementRequest) throws -> ReceiverInfo {
    guard request.operation == .info else { throw ManagementError.invalidResponse }
    try decode(Envelope.self, from: data).validate(request)
    struct Info: Decodable {
      let protocolVersion: Int
      let product: String
      let firmware: String
      let slots: Int
      let recordBytes: Int
      let capabilities: [String]
      let nativePairing: Bool

      enum CodingKeys: String, CodingKey {
        case product, firmware, slots, capabilities
        case protocolVersion = "protocol"
        case recordBytes = "record_bytes"
        case nativePairing = "native_pairing"
      }
    }
    let response = try decode(Info.self, from: data)
    guard response.protocolVersion == 1, response.product == "PuckOff Receiver",
      response.slots == 4, response.recordBytes == 24,
      response.capabilities.contains("status")
    else {
      throw ManagementError.unsupportedFirmware
    }
    // Only fixed-schema, non-identifying fields cross into the app model.
    return ReceiverInfo(
      protocolVersion: response.protocolVersion,
      slotCount: response.slots,
      nativePairing: response.nativePairing
    )
  }

  public static func status(
    _ data: Data, for request: ManagementRequest, slotCount: Int
  ) throws -> ReceiverStatus {
    guard request.operation == .status, slotCount == 4 else {
      throw ManagementError.invalidResponse
    }
    try decode(Envelope.self, from: data).validate(request)
    struct Status: Decodable {
      let storageReady: Bool
      let slots: [ReceiverSlot]

      enum CodingKeys: String, CodingKey {
        case slots
        case storageReady = "storage_ready"
      }
    }
    let response = try decode(Status.self, from: data)
    guard response.slots.count == slotCount,
      Set(response.slots.map(\.slot)) == Set(0..<slotCount),
      response.slots.allSatisfy({
        (-255...0).contains($0.rssi)
          && !($0.connected && $0.poweredOff)
          && (!$0.connected || $0.used)
      })
    else {
      throw ManagementError.invalidResponse
    }
    return ReceiverStatus(
      storageReady: response.storageReady,
      slots: response.slots.sorted { $0.slot < $1.slot }
    )
  }
}

public protocol ManagementTransport {
  mutating func exchange(_ request: ManagementRequest) throws -> Data
}

public enum ReceiverManagement {
  public static func read<T: ManagementTransport>(
    using transport: inout T,
    makeID: () -> String = ManagementRequest.freshID
  ) throws -> ReceiverSnapshot {
    let infoRequest = try ManagementRequest(.info, id: makeID())
    let info = try ManagementResponse.info(transport.exchange(infoRequest), for: infoRequest)
    let statusRequest = try ManagementRequest(.status, id: makeID())
    guard statusRequest.id != infoRequest.id else { throw ManagementError.invalidRequestID }
    let status = try ManagementResponse.status(
      transport.exchange(statusRequest), for: statusRequest, slotCount: info.slotCount
    )
    return ReceiverSnapshot(info: info, status: status)
  }
}
