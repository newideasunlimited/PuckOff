// SPDX-License-Identifier: Apache-2.0
import Foundation
import XCTest

@testable import PuckOffCore

final class HIDReportTests: XCTestCase {
  private let neutral = Data([0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])

  func testNeutralReport() throws {
    let input = try ReceiverHIDReport.decode(neutral)
    XCTAssertTrue(input.buttons.isEmpty)
    XCTAssertEqual(input.axes.count, 8)
    XCTAssertTrue(input.axes.allSatisfy { $0.value == 0 })
  }

  func testAllSixteenButtonBitsMatchFirmwareOrder() throws {
    for (index, button) in GamepadButton.allCases.enumerated() {
      var report = neutral
      let mask = UInt16(1) << index
      report[0] = UInt8(truncatingIfNeeded: mask)
      report[1] = UInt8(truncatingIfNeeded: mask >> 8)
      XCTAssertEqual(try ReceiverHIDReport.decode(report).buttons, [button])
    }
  }

  func testSignedLittleEndianAxesAndHIDYConversion() throws {
    let report = Data([
      0, 0, 8, 0xFF, 0x7F, 0x01, 0x80, 0x01, 0x80, 0xFF, 0x7F, 0, 255,
    ])
    let input = try ReceiverHIDReport.decode(report)
    XCTAssertEqual(input.axes.map(\.value), [1, 1, -1, -1, 0, 1, 0, 0])
  }

  func testEveryHatDirection() throws {
    let expected: [[Double]] = [
      [0, 1], [1, 1], [1, 0], [1, -1], [0, -1],
      [-1, -1], [-1, 0], [-1, 1], [0, 0],
    ]
    for hat in 0...8 {
      var report = neutral
      report[2] = UInt8(hat)
      let input = try ReceiverHIDReport.decode(report)
      XCTAssertEqual(Array(input.axes.suffix(2).map(\.value)), expected[hat])
    }
  }

  func testInvalidLengthsAndReportIDAreRejected() {
    for length in [0, 1, 12, 14, 64] {
      XCTAssertThrowsError(try ReceiverHIDReport.decode(Data(repeating: 0, count: length))) {
        XCTAssertEqual($0 as? HIDReportError, .unsupportedReport)
      }
    }
    XCTAssertThrowsError(try ReceiverHIDReport.decode(neutral, reportID: 1))
  }

  func testInvalidHatAndPaddingAreRejected() {
    for hat: UInt8 in [9, 15, 0x18, 255] {
      var report = neutral
      report[2] = hat
      XCTAssertThrowsError(try ReceiverHIDReport.decode(report)) {
        XCTAssertEqual($0 as? HIDReportError, .invalidHat)
      }
    }
  }

  func testOutOfDescriptorAxisRangeIsRejected() {
    for offset in [3, 5, 7, 9] {
      var report = neutral
      report[offset] = 0
      report[offset + 1] = 0x80
      XCTAssertThrowsError(try ReceiverHIDReport.decode(report)) {
        XCTAssertEqual($0 as? HIDReportError, .invalidAxis)
      }
    }
  }

  func testSlicedDataDoesNotAssumeZeroBasedIndices() throws {
    let data = Data([99, 99]) + neutral
    XCTAssertEqual(
      try ReceiverHIDReport.decode(data.dropFirst(2)),
      try ReceiverHIDReport.decode(neutral)
    )
  }
}
