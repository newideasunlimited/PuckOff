// SPDX-License-Identifier: Apache-2.0
import Foundation

public protocol ManagementByteChannel {
  mutating func drain(before deadline: ContinuousClock.Instant) throws
  mutating func write(_ data: Data, before deadline: ContinuousClock.Instant) throws
  mutating func read(before deadline: ContinuousClock.Instant) throws -> Data
}

public struct ShellManagementTransport<Channel: ManagementByteChannel>: ManagementTransport {
  private var channel: Channel

  public init(channel: Channel) {
    self.channel = channel
  }

  public mutating func exchange(_ request: ManagementRequest) throws -> Data {
    let deadline = ContinuousClock.now + .seconds(3)
    do {
      try channel.drain(before: deadline)
      try channel.write(Data([0x15]), before: deadline)
      var cancellation = ShellCancellationCollector()
      while true {
        let data = try channel.read(before: deadline)
        guard !data.isEmpty else { throw ManagementError.shellNotReady }
        if try cancellation.append(data) { break }
      }
      try channel.drain(before: deadline)
    } catch ManagementError.timeout {
      throw ManagementError.shellNotReady
    }
    // Never send CR until the shell has proved it handled Ctrl-U.
    try channel.write(request.bytes, before: deadline)
    var response = ManagementResponseCollector(request: request)
    while true {
      let data = try channel.read(before: deadline)
      guard !data.isEmpty else { throw ManagementError.invalidResponse }
      if let payload = try response.append(data) { return payload }
    }
  }
}

struct ShellCancellationCollector {
  private var suffix: [UInt8] = []
  private var received = 0

  mutating func append(_ data: Data) throws -> Bool {
    guard data.count <= ManagementResponseCollector.maximumReceiveBytes - received else {
      throw ManagementError.responseTooLarge
    }
    received += data.count
    for byte in data {
      suffix.append(byte)
      if suffix.count > 4 { suffix.removeFirst() }
      if suffix.suffix(3).elementsEqual([27, 91, 74]) || suffix == [27, 91, 48, 74] {
        return true
      }
    }
    return false
  }
}
