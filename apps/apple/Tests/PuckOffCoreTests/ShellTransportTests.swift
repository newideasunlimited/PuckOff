// SPDX-License-Identifier: Apache-2.0
import Foundation
import XCTest

@testable import PuckOffCore

final class ShellTransportTests: XCTestCase {
  private let response = Data(
    "PUCKOFF/1 {\"id\":\"test\",\"op\":\"info\",\"ok\":true}\r\n".utf8
  )

  func testCoordinatorObservedCancellationFixture() throws {
    struct Capture: Decodable {
      let request: [UInt8]
      let response: [UInt8]
    }
    let url = try XCTUnwrap(
      Bundle.module.url(
        forResource: "shell-cancel", withExtension: "json", subdirectory: "Fixtures"
      ))
    let capture = try JSONDecoder().decode(Capture.self, from: Data(contentsOf: url))
    XCTAssertEqual(capture.request, [21])
    XCTAssertEqual(capture.response, [27, 91, 74])
    let channel = FixtureByteChannel(reads: [Data(capture.response), response])
    var transport = ShellManagementTransport(channel: channel)
    _ = try transport.exchange(ManagementRequest(.info, id: "test"))
    XCTAssertEqual(channel.writes.first, Data(capture.request))
  }

  func testBothCancellationAcknowledgmentsAcrossEveryChunkBoundary() throws {
    for acknowledgment in [Data([27, 91, 74]), Data([27, 91, 48, 74])] {
      for split in 1..<acknowledgment.count {
        let channel = FixtureByteChannel(reads: [
          Data(acknowledgment.prefix(split)),
          Data(acknowledgment.dropFirst(split)),
          response,
        ])
        var transport = ShellManagementTransport(channel: channel)
        let request = try ManagementRequest(.info, id: "test")
        let started = ContinuousClock.now
        _ = try transport.exchange(request)
        let finished = ContinuousClock.now
        XCTAssertEqual(channel.writes, [Data([21]), request.bytes])
        XCTAssertEqual(
          channel.events, ["drain", "write", "read", "read", "drain", "write", "read"])
        XCTAssertEqual(Set(channel.deadlines).count, 1)
        let deadline = try XCTUnwrap(channel.deadlines.first)
        XCTAssertGreaterThanOrEqual(deadline, started + .seconds(3))
        XCTAssertLessThanOrEqual(deadline, finished + .seconds(3))
      }
    }
  }

  func testNoTerminatorIsWrittenWhenEchoOrMetakeysAreDisabled() throws {
    let channel = FixtureByteChannel(reads: [
      Data("unrelated synthetic console output".utf8)
    ])
    var transport = ShellManagementTransport(channel: channel)
    XCTAssertThrowsError(try transport.exchange(ManagementRequest(.info, id: "test"))) {
      XCTAssertEqual($0 as? ManagementError, .shellNotReady)
    }
    XCTAssertEqual(channel.writes, [Data([21])])
    XCTAssertFalse(channel.writes.joined().contains(13))
  }

  func testUnrelatedEscapeSequencesDoNotAuthorizeACommand() throws {
    let channel = FixtureByteChannel(reads: [Data([27, 91, 75, 27, 91, 50, 74])])
    var transport = ShellManagementTransport(channel: channel)
    XCTAssertThrowsError(try transport.exchange(ManagementRequest(.info, id: "test")))
    XCTAssertEqual(channel.writes, [Data([21])])
  }

  func testDrainFailureDoesNotSendAnyBytes() throws {
    let channel = FixtureByteChannel(reads: [], failedDrain: 1)
    var transport = ShellManagementTransport(channel: channel)
    XCTAssertThrowsError(try transport.exchange(ManagementRequest(.info, id: "test"))) {
      XCTAssertEqual($0 as? ManagementError, .shellNotReady)
    }
    XCTAssertTrue(channel.writes.isEmpty)
  }

  func testPostAcknowledgmentDrainFailureStillDoesNotSendATerminator() throws {
    let channel = FixtureByteChannel(reads: [Data([27, 91, 74])], failedDrain: 2)
    var transport = ShellManagementTransport(channel: channel)
    XCTAssertThrowsError(try transport.exchange(ManagementRequest(.info, id: "test")))
    XCTAssertEqual(channel.writes, [Data([21])])
  }

  func testCancellationNoiseHasABoundedBudget() throws {
    var collector = ShellCancellationCollector()
    XCTAssertFalse(try collector.append(Data(repeating: 65, count: 16_384)))
    XCTAssertThrowsError(try collector.append(Data([27]))) {
      XCTAssertEqual($0 as? ManagementError, .responseTooLarge)
    }
    let channel = FixtureByteChannel(reads: [Data(repeating: 65, count: 16_385)])
    var transport = ShellManagementTransport(channel: channel)
    XCTAssertThrowsError(try transport.exchange(ManagementRequest(.info, id: "test")))
    XCTAssertEqual(channel.writes, [Data([21])])
  }

  func testResponseTimeoutAfterSafeCancellationIsNotSuccess() throws {
    let channel = FixtureByteChannel(reads: [Data([27, 91, 74])])
    var transport = ShellManagementTransport(channel: channel)
    let request = try ManagementRequest(.status, id: "test")
    XCTAssertThrowsError(try transport.exchange(request)) {
      XCTAssertEqual($0 as? ManagementError, .timeout)
    }
    XCTAssertEqual(channel.writes, [Data([21]), request.bytes])
  }

  func testEveryRequestRequiresANewCancellationAcknowledgment() throws {
    let channel = FixtureByteChannel(reads: [Data([27, 91, 74]), response])
    var transport = ShellManagementTransport(channel: channel)
    let request = try ManagementRequest(.info, id: "test")
    _ = try transport.exchange(request)
    XCTAssertThrowsError(try transport.exchange(ManagementRequest(.status, id: "fresh"))) {
      XCTAssertEqual($0 as? ManagementError, .shellNotReady)
    }
    XCTAssertEqual(channel.writes, [Data([21]), request.bytes, Data([21])])
  }
}

private final class FixtureByteChannel: ManagementByteChannel {
  var reads: [Data]
  var writes: [Data] = []
  var events: [String] = []
  var deadlines: [ContinuousClock.Instant] = []
  let failedDrain: Int?
  private var drains = 0

  init(reads: [Data], failedDrain: Int? = nil) {
    self.reads = reads
    self.failedDrain = failedDrain
  }

  func drain(before deadline: ContinuousClock.Instant) throws {
    events.append("drain")
    deadlines.append(deadline)
    drains += 1
    if failedDrain == drains { throw ManagementError.timeout }
  }

  func write(_ data: Data, before deadline: ContinuousClock.Instant) throws {
    events.append("write")
    deadlines.append(deadline)
    writes.append(data)
  }

  func read(before deadline: ContinuousClock.Instant) throws -> Data {
    events.append("read")
    deadlines.append(deadline)
    guard !reads.isEmpty else { throw ManagementError.timeout }
    return reads.removeFirst()
  }
}
