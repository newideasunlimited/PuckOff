// SPDX-License-Identifier: Apache-2.0
import Foundation

public enum GamepadButton: String, CaseIterable, Sendable {
  case a = "A"
  case b = "B"
  case x = "X"
  case y = "Y"
  case leftShoulder = "LB"
  case rightShoulder = "RB"
  case view = "View"
  case menu = "Menu"
  case leftStick = "L3"
  case rightStick = "R3"
  case guide = "Guide"
  case quickAccess = "Quick access"
  case leftGripUpper = "L4"
  case rightGripUpper = "R4"
  case leftGripLower = "L5"
  case rightGripLower = "R5"
}

public enum GamepadAxis: String, CaseIterable, Sendable {
  case leftX = "Left stick X"
  case leftY = "Left stick Y"
  case rightX = "Right stick X"
  case rightY = "Right stick Y"
  case leftTrigger = "Left trigger"
  case rightTrigger = "Right trigger"
  case dpadX = "D-pad X"
  case dpadY = "D-pad Y"

  public var range: ClosedRange<Double> {
    switch self {
    case .leftTrigger, .rightTrigger: 0...1
    default: -1...1
    }
  }
}

public struct AxisReading: Equatable, Identifiable, Sendable {
  public let axis: GamepadAxis
  public let value: Double
  public var id: GamepadAxis { axis }

  public init(_ axis: GamepadAxis, _ value: Double) {
    self.axis = axis
    self.value = value
  }
}

public struct GamepadInput: Equatable, Sendable {
  public let buttons: [GamepadButton]
  public let axes: [AxisReading]

  public init(buttons: [GamepadButton], axes: [AxisReading]) {
    self.buttons = buttons
    self.axes = axes
  }
}

public enum ReceiverUSB {
  // Experimental IDs are a filter, not proof of device identity.
  public static let vendorID = 0x1209
  public static let productID = 0x0001
  public static let usagePage = 0x01
  public static let usage = 0x05
}

public enum HIDReportError: Error, LocalizedError, Equatable {
  case unsupportedReport
  case invalidHat
  case invalidAxis

  public var errorDescription: String? {
    switch self {
    case .unsupportedReport:
      "Unsupported HID report. Expected the receiver's 13-byte report without a report ID."
    case .invalidHat:
      "The receiver sent an invalid D-pad value."
    case .invalidAxis:
      "The receiver sent a stick value outside its advertised range."
    }
  }
}

public enum ReceiverHIDReport {
  public static let byteCount = 13

  public static func decode(_ data: Data, reportID: UInt32 = 0) throws -> GamepadInput {
    guard reportID == 0, data.count == byteCount else {
      throw HIDReportError.unsupportedReport
    }
    let bytes = Array(data)
    guard bytes[2] <= 8 else { throw HIDReportError.invalidHat }
    let mask = UInt16(bytes[0]) | UInt16(bytes[1]) << 8
    let buttons = GamepadButton.allCases.enumerated().compactMap { index, button in
      mask & (1 << index) == 0 ? nil : button
    }
    func axis(at offset: Int) throws -> Double {
      let bits = UInt16(bytes[offset]) | UInt16(bytes[offset + 1]) << 8
      let value = Int16(bitPattern: bits)
      guard value != .min else { throw HIDReportError.invalidAxis }
      return Double(value) / Double(Int16.max)
    }
    let dpad: [(Double, Double)] = [
      (0, 1), (1, 1), (1, 0), (1, -1), (0, -1),
      (-1, -1), (-1, 0), (-1, 1), (0, 0),
    ]
    let direction = dpad[Int(bytes[2])]
    return try GamepadInput(
      buttons: buttons,
      axes: [
        AxisReading(.leftX, axis(at: 3)),
        AxisReading(.leftY, -axis(at: 5)),
        AxisReading(.rightX, axis(at: 7)),
        AxisReading(.rightY, -axis(at: 9)),
        AxisReading(.leftTrigger, Double(bytes[11]) / 255),
        AxisReading(.rightTrigger, Double(bytes[12]) / 255),
        AxisReading(.dpadX, direction.0),
        AxisReading(.dpadY, direction.1),
      ]
    )
  }
}
