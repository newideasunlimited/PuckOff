// SPDX-License-Identifier: Apache-2.0
#if os(macOS)
  import Darwin
  import Foundation
  import OSLog
  import PuckOffCore

  public enum ReceiverSerial {
    public static func read(_ candidate: ReceiverCandidate) async throws -> ReceiverSnapshot {
      let operation = Task.detached(priority: .userInitiated) {
        try readBlocking(candidate)
      }
      return try await withTaskCancellationHandler {
        try await operation.value
      } onCancel: {
        operation.cancel()
      }
    }

    private static func readBlocking(_ candidate: ReceiverCandidate) throws -> ReceiverSnapshot {
      try Task.checkCancellation()
      try ReceiverDiscovery.validate(candidate)
      try Task.checkCancellation()
      let descriptor = Darwin.open(
        candidate.serialPath, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW
      )
      guard descriptor >= 0 else { throw ReceiverAccessError.serial(.opening, errno) }
      var original = termios()
      var originalAvailable = false
      let outcome = Result {
        guard ioctl(descriptor, TIOCEXCL) == 0 else {
          throw ReceiverAccessError.serial(.exclusivity, errno)
        }
        guard tcgetattr(descriptor, &original) == 0 else {
          throw ReceiverAccessError.serial(.configuring, errno)
        }
        originalAvailable = true
        var attributes = original
        cfmakeraw(&attributes)
        attributes.c_cflag |= tcflag_t(CLOCAL | CREAD)
        attributes.c_cflag &= ~tcflag_t(CRTSCTS)
        guard cfsetispeed(&attributes, speed_t(B115200)) == 0,
          cfsetospeed(&attributes, speed_t(B115200)) == 0,
          tcsetattr(descriptor, TCSANOW, &attributes) == 0,
          tcflush(descriptor, TCIFLUSH) == 0
        else { throw ReceiverAccessError.serial(.configuring, errno) }
        var dtr = TIOCM_DTR
        guard ioctl(descriptor, TIOCMBIS, &dtr) == 0 else {
          throw ReceiverAccessError.serial(.configuring, errno)
        }
        var transport = ShellManagementTransport(channel: SerialChannel(descriptor: descriptor))
        return try ReceiverManagement.read(using: &transport)
      }
      let restoreError: Int32? =
        originalAvailable && tcsetattr(descriptor, TCSANOW, &original) != 0 ? errno : nil
      let closeError: Int32? = Darwin.close(descriptor) != 0 ? errno : nil
      let logger = Logger(subsystem: "com.newideasunlimited.puckoff", category: "Serial")
      if let restoreError {
        logger.error(
          "Serial settings restoration failed (system code \(restoreError, privacy: .public)).")
      }
      if let closeError {
        logger.error("Serial close failed (system code \(closeError, privacy: .public)).")
      }
      if let restoreError { throw ReceiverAccessError.serial(.restoring, restoreError) }
      if let closeError { throw ReceiverAccessError.serial(.closing, closeError) }
      try Task.checkCancellation()
      return try outcome.get()
    }
  }

  private struct SerialChannel: ManagementByteChannel {
    let descriptor: Int32

    func drain(before deadline: ContinuousClock.Instant) throws {
      var received = 0
      while try wait(for: Int16(POLLIN), before: deadline, untilQuiet: true) {
        let data = try read(before: deadline)
        guard data.count <= ManagementResponseCollector.maximumReceiveBytes - received else {
          throw ManagementError.responseTooLarge
        }
        received += data.count
      }
    }

    func read(before deadline: ContinuousClock.Instant) throws -> Data {
      while true {
        _ = try wait(for: Int16(POLLIN), before: deadline)
        var buffer = [UInt8](repeating: 0, count: 256)
        let count = buffer.withUnsafeMutableBytes {
          Darwin.read(descriptor, $0.baseAddress, $0.count)
        }
        if count < 0 {
          if errno == EAGAIN || errno == EINTR { continue }
          throw ReceiverAccessError.serial(.reading, errno)
        }
        guard count > 0 else { throw ReceiverAccessError.disconnected }
        return Data(buffer.prefix(count))
      }
    }

    func write(_ data: Data, before deadline: ContinuousClock.Instant) throws {
      var offset = 0
      while offset < data.count {
        _ = try wait(for: Int16(POLLOUT), before: deadline)
        let count = data.withUnsafeBytes {
          Darwin.write(descriptor, $0.baseAddress?.advanced(by: offset), data.count - offset)
        }
        if count < 0 {
          if errno == EAGAIN || errno == EINTR { continue }
          throw ReceiverAccessError.serial(.writing, errno)
        }
        guard count > 0 else { throw ReceiverAccessError.disconnected }
        offset += count
      }
    }

    private func wait(
      for events: Int16, before deadline: ContinuousClock.Instant, untilQuiet: Bool = false
    ) throws -> Bool {
      while true {
        try Task.checkCancellation()
        let now = ContinuousClock.now
        guard now < deadline else { throw ManagementError.timeout }
        let duration = now.duration(to: deadline).components
        let remaining = duration.seconds * 1_000 + duration.attoseconds / 1_000_000_000_000_000
        let milliseconds = Int32(max(1, min(50, remaining)))
        var entry = pollfd(fd: descriptor, events: events, revents: 0)
        let result = Darwin.poll(&entry, 1, milliseconds)
        try Task.checkCancellation()
        guard ContinuousClock.now < deadline else { throw ManagementError.timeout }
        if result < 0 {
          if errno == EINTR { continue }
          throw ReceiverAccessError.serial(events == Int16(POLLIN) ? .reading : .writing, errno)
        }
        if result == 0 {
          if untilQuiet { return false }
          continue
        }
        if entry.revents & Int16(POLLERR | POLLHUP | POLLNVAL) != 0 {
          throw ReceiverAccessError.disconnected
        }
        if entry.revents & events != 0 { return true }
      }
    }
  }
#endif
