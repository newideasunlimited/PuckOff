// SPDX-License-Identifier: Apache-2.0
import Foundation
import XCTest

@testable import PuckOffCore

final class ManagementProtocolTests: XCTestCase {
  private func fixture(_ name: String) throws -> Data {
    let url = try XCTUnwrap(
      Bundle.module.url(forResource: name, withExtension: "json", subdirectory: "Fixtures")
    )
    return try Data(contentsOf: url)
  }

  private func request(_ operation: ManagementOperation) throws -> ManagementRequest {
    try ManagementRequest(operation, id: "fixture-\(operation.rawValue)")
  }

  private func frame(_ data: Data) -> Data {
    Data("PUCKOFF/1 ".utf8) + data + Data("\r\n".utf8)
  }

  func testOnlyReadOnlyWireCommandsExist() throws {
    XCTAssertEqual(
      try request(.info).bytes,
      Data("setup info fixture-info\r".utf8)
    )
    XCTAssertEqual(
      try request(.status).bytes,
      Data("setup status fixture-status\r".utf8)
    )
  }

  func testRejectsCommandInjectionAndInvalidIDs() {
    for id in ["", "a b", "a\rpair clear 0", "a\n", "\u{E9}", String(repeating: "a", count: 17)] {
      XCTAssertThrowsError(try ManagementRequest(.info, id: id)) {
        XCTAssertEqual($0 as? ManagementError, .invalidRequestID)
      }
    }
    XCTAssertNoThrow(try ManagementRequest(.info, id: "A-z_019"))
  }

  func testGeneratedIDsMeetContractAndAreDistinct() throws {
    let ids = (0..<100).map { _ in ManagementRequest.freshID() }
    XCTAssertEqual(Set(ids).count, 100)
    for id in ids {
      XCTAssertEqual(id.utf8.count, 16)
      XCTAssertNoThrow(try ManagementRequest(.info, id: id))
    }
  }

  func testChunkedCRLFWithPromptEchoAndUnrelatedLogs() throws {
    let payload = try fixture("info")
    let input =
      Data("\u{1B}[32muart:~$ setup info fixture-info\r\nlog: synthetic\r\n".utf8)
      + frame(payload)
    for chunkSize in [1, 2, 7, 128, input.count] {
      var collector = ManagementResponseCollector(request: try request(.info))
      var result: Data?
      for start in stride(from: 0, to: input.count, by: chunkSize) {
        let chunk = input.subdata(in: start..<min(start + chunkSize, input.count))
        if let response = try collector.append(chunk) { result = response }
      }
      let info = try ManagementResponse.info(XCTUnwrap(result), for: request(.info))
      XCTAssertEqual(info.protocolVersion, 1)
      XCTAssertEqual(info.slotCount, 4)
      XCTAssertFalse(info.nativePairing)
    }
  }

  func testStaleIDOrWrongOperationDoesNotCompleteRequest() throws {
    var collector = ManagementResponseCollector(request: try request(.info))
    let payload = try fixture("info")
    let text = try XCTUnwrap(String(data: payload, encoding: .utf8))
    XCTAssertNil(
      try collector.append(
        frame(
          Data(
            text.replacingOccurrences(
              of: "fixture-info", with: "stale-id"
            ).utf8))))
    XCTAssertNil(
      try collector.append(
        frame(
          Data(
            text.replacingOccurrences(
              of: "\"op\":\"info\"", with: "\"op\":\"status\""
            ).utf8))))
    XCTAssertNotNil(try collector.append(frame(payload)))
  }

  func testRequiresExactPrefixAndCompleteLine() throws {
    var collector = ManagementResponseCollector(request: try request(.info))
    let payload = try fixture("info").dropLast()
    XCTAssertNil(try collector.append(Data("echo PUCKOFF/1 ".utf8) + payload + Data([10])))
    XCTAssertNil(try collector.append(Data("PUCKOFF/1 ".utf8) + payload))
    XCTAssertNotNil(try collector.append(Data([13, 10])))
  }

  func testExactLineLimitAndCRLF() throws {
    let payload = try fixture("info").dropLast()
    let prefix = Data("PUCKOFF/1 ".utf8)
    let padding = 1_024 - prefix.count - payload.count
    var collector = ManagementResponseCollector(request: try request(.info))
    XCTAssertNotNil(
      try collector.append(
        prefix + payload + Data(repeating: 32, count: padding) + Data([13, 10])
      ))
    var oversized = ManagementResponseCollector(request: try request(.info))
    XCTAssertThrowsError(
      try oversized.append(
        prefix + payload + Data(repeating: 32, count: padding + 1) + Data([10])
      )
    ) {
      XCTAssertEqual($0 as? ManagementError, .responseTooLarge)
    }
  }

  func testBoundsBothUnterminatedLineAndTotalNoise() throws {
    var line = ManagementResponseCollector(request: try request(.info))
    XCTAssertThrowsError(try line.append(Data(repeating: 65, count: 1_025)))
    var noise = ManagementResponseCollector(request: try request(.info))
    XCTAssertNil(try noise.append(Data(repeating: 10, count: 16_384)))
    XCTAssertThrowsError(try noise.append(Data([10]))) {
      XCTAssertEqual($0 as? ManagementError, .responseTooLarge)
    }
  }

  func testMalformedJSONAndFirmwareErrorsAreSanitized() throws {
    for error in ["invalid_argument", "conflict", "storage_io", "PRIVATE-CONTROLLER-SERIAL"] {
      var collector = ManagementResponseCollector(request: try request(.info))
      let data = Data(
        "{\"id\":\"fixture-info\",\"op\":\"info\",\"ok\":false,\"error\":\"\(error)\"}".utf8
      )
      XCTAssertThrowsError(try collector.append(frame(data))) {
        XCTAssertEqual($0 as? ManagementError, .firmwareRejected)
        XCTAssertFalse($0.localizedDescription.contains(error))
      }
    }
    var invalid = ManagementResponseCollector(request: try request(.info))
    XCTAssertThrowsError(try invalid.append(frame(Data("PRIVATE-CONTROLLER-SERIAL".utf8)))) {
      XCTAssertEqual($0 as? ManagementError, .invalidResponse)
      XCTAssertFalse($0.localizedDescription.contains("PRIVATE-CONTROLLER-SERIAL"))
    }
  }

  func testStorageErrorIsNotReportedAsSuccess() throws {
    var collector = ManagementResponseCollector(request: try request(.status))
    let data = Data(
      """
      {"id":"fixture-status","op":"status","ok":false,"error":"storage_unavailable"}
      """.utf8
    )
    XCTAssertThrowsError(try collector.append(frame(data))) {
      XCTAssertEqual($0 as? ManagementError, .storageUnavailable)
    }
  }

  func testInfoRejectsWrongProductVersionLayoutAndMissingCapability() throws {
    let text = try XCTUnwrap(String(data: fixture("info"), encoding: .utf8))
    for (old, new) in [
      ("PuckOff Receiver", "Different device"),
      ("\"protocol\":1", "\"protocol\":2"),
      ("\"slots\":4", "\"slots\":8"),
      ("\"record_bytes\":24", "\"record_bytes\":32"),
      ("\"status\"", "\"future-command\""),
    ] {
      XCTAssertThrowsError(
        try ManagementResponse.info(
          Data(text.replacingOccurrences(of: old, with: new).utf8),
          for: request(.info)
        )
      ) {
        XCTAssertEqual($0 as? ManagementError, .unsupportedFirmware)
      }
    }
  }

  func testStatusIncludesAllFourStatesWithoutIdentifiers() throws {
    let status = try ManagementResponse.status(
      fixture("status"), for: request(.status), slotCount: 4)
    XCTAssertTrue(status.storageReady)
    XCTAssertEqual(
      status.slots.map(\.stateDescription),
      ["Connected", "Controller off", "Searching", "Empty"]
    )
    XCTAssertEqual(status.slots[0].rssi, -42)
    XCTAssertEqual(status.slots[0].reports, 247)
  }

  func testUnavailableStorageRemainsAnExplicitUnreadySnapshot() throws {
    let text = try XCTUnwrap(String(data: fixture("status"), encoding: .utf8))
      .replacingOccurrences(of: "\"storage_ready\":true", with: "\"storage_ready\":false")
    let status = try ManagementResponse.status(
      Data(text.utf8), for: request(.status), slotCount: 4
    )
    XCTAssertFalse(status.storageReady)
    XCTAssertEqual(status.slots.count, 4)
  }

  func testRejectsDuplicateSlotsNegativeCountersAndContradictoryStates() throws {
    let text = try XCTUnwrap(String(data: fixture("status"), encoding: .utf8))
    for (old, new) in [
      ("\"slot\":3", "\"slot\":0"),
      ("\"slot\":3", "\"slot\":4"),
      ("\"polls\":250", "\"polls\":-1"),
      ("\"polls\":250", "\"polls\":4294967296"),
      ("\"rssi\":-42", "\"rssi\":42"),
      ("\"connected\":true,\"powered_off\":false", "\"connected\":true,\"powered_off\":true"),
      ("\"used\":true,\"connected\":true", "\"used\":false,\"connected\":true"),
    ] {
      XCTAssertThrowsError(
        try ManagementResponse.status(
          Data(text.replacingOccurrences(of: old, with: new).utf8),
          for: request(.status), slotCount: 4
        )
      ) {
        XCTAssertEqual($0 as? ManagementError, .invalidResponse)
      }
    }
  }

  func testMockSessionProbesBeforeStatusAndNeverSendsMutations() throws {
    var transport = FixtureTransport(info: try fixture("info"), status: try fixture("status"))
    var ids = ["fixture-info", "fixture-status"].makeIterator()
    let snapshot = try ReceiverManagement.read(using: &transport, makeID: { ids.next()! })
    XCTAssertEqual(snapshot.status.slots.count, 4)
    XCTAssertEqual(transport.requests.map(\.operation), [.info, .status])
  }

  func testDeviceSuppliedStringsAreNotRetainedInInfoModel() throws {
    let text = try XCTUnwrap(String(data: fixture("info"), encoding: .utf8))
      .replacingOccurrences(of: "\"dev\"", with: "\"PRIVATE-CONTROLLER-SERIAL\"")
    let info = try ManagementResponse.info(Data(text.utf8), for: request(.info))
    XCTAssertFalse(String(reflecting: info).contains("PRIVATE-CONTROLLER-SERIAL"))
  }

  func testReusedIDStopsBeforeSendingStatus() throws {
    var transport = FixtureTransport(info: try fixture("info"), status: try fixture("status"))
    XCTAssertThrowsError(
      try ReceiverManagement.read(
        using: &transport, makeID: { "fixture-info" }
      )
    ) {
      XCTAssertEqual($0 as? ManagementError, .invalidRequestID)
    }
    XCTAssertEqual(transport.requests.map(\.operation), [.info])
  }

  func testUnsupportedFirmwareStopsBeforeStatusWithoutLegacyFallback() throws {
    var transport = FixtureTransport(
      info: Data("legacy prompt".utf8), status: try fixture("status"))
    XCTAssertThrowsError(
      try ReceiverManagement.read(
        using: &transport, makeID: { "fixture-info" }
      ))
    XCTAssertEqual(transport.requests.map(\.operation), [.info])
  }

  func testTimeoutIsSurfacedWithoutRetryOrFallback() throws {
    var transport = FixtureTransport(info: Data(), status: Data(), failure: .timeout)
    XCTAssertThrowsError(try ReceiverManagement.read(using: &transport)) {
      XCTAssertEqual($0 as? ManagementError, .timeout)
    }
    XCTAssertEqual(transport.requests.map(\.operation), [.info])
  }
}

private struct FixtureTransport: ManagementTransport {
  let info: Data
  let status: Data
  var failure: ManagementError?
  var requests: [ManagementRequest] = []

  mutating func exchange(_ request: ManagementRequest) throws -> Data {
    requests.append(request)
    if let failure { throw failure }
    return request.operation == .info ? info : status
  }
}
