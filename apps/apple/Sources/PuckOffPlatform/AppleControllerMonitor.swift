// SPDX-License-Identifier: Apache-2.0
#if canImport(GameController)
  import Foundation
  import GameController
  import Observation
  import PuckOffCore

  public struct ControllerDiagnostic: Identifiable {
    public let id: UUID
    public let label: String
    public let profile: String
    public let input: GamepadInput?
  }

  @MainActor
  @Observable
  public final class AppleControllerMonitor {
    public private(set) var isRunning = false
    public private(set) var controllers: [ControllerDiagnostic] = []
    public private(set) var errorMessage: String?
    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var localIDs: [ObjectIdentifier: UUID] = [:]

    public init() {}

    public func start() {
      guard !isRunning else { return }
      errorMessage = nil
      isRunning = true
      task = Task { @MainActor [weak self] in
        do {
          while !Task.isCancelled {
            self?.sample()
            try await Task.sleep(for: .milliseconds(50))
          }
        } catch is CancellationError {
          // Stopping diagnostics cancels the sampling task.
        } catch {
          self?.stop()
          self?.errorMessage = "Controller diagnostics stopped unexpectedly."
        }
      }
    }

    public func stop() {
      task?.cancel()
      task = nil
      isRunning = false
      controllers = []
      localIDs = [:]
    }

    private func sample() {
      let connected = GCController.controllers().filter { !$0.isSnapshot }
      let activeIDs = Set(connected.map(ObjectIdentifier.init))
      localIDs = localIDs.filter { activeIDs.contains($0.key) }
      controllers = connected.enumerated().map { index, controller in
        let key = ObjectIdentifier(controller)
        let id = localIDs[key] ?? UUID()
        localIDs[key] = id
        let snapshot = controller.capture()
        if let gamepad = snapshot.extendedGamepad {
          return ControllerDiagnostic(
            id: id, label: "Apple controller \(index + 1)",
            profile: "Extended gamepad", input: Self.input(gamepad)
          )
        }
        if let gamepad = snapshot.microGamepad {
          let buttons: [(GamepadButton, GCControllerButtonInput)] = [
            (.a, gamepad.buttonA), (.x, gamepad.buttonX),
          ]
          return ControllerDiagnostic(
            id: id, label: "Apple controller \(index + 1)",
            profile: "Micro gamepad",
            input: GamepadInput(
              buttons: buttons.compactMap { $0.1.isPressed ? $0.0 : nil },
              axes: [
                AxisReading(.dpadX, Double(gamepad.dpad.xAxis.value)),
                AxisReading(.dpadY, Double(gamepad.dpad.yAxis.value)),
              ]
            )
          )
        }
        return ControllerDiagnostic(
          id: id, label: "Apple controller \(index + 1)",
          profile: "Input profile not supported by this diagnostic view", input: nil
        )
      }
    }

    private static func input(_ gamepad: GCExtendedGamepad) -> GamepadInput {
      let buttons: [(GamepadButton, GCControllerButtonInput?)] = [
        (.a, gamepad.buttonA), (.b, gamepad.buttonB),
        (.x, gamepad.buttonX), (.y, gamepad.buttonY),
        (.leftShoulder, gamepad.leftShoulder), (.rightShoulder, gamepad.rightShoulder),
        (.view, gamepad.buttonOptions), (.menu, gamepad.buttonMenu),
        (.leftStick, gamepad.leftThumbstickButton), (.rightStick, gamepad.rightThumbstickButton),
        (.guide, gamepad.buttonHome),
      ]
      return GamepadInput(
        buttons: buttons.compactMap { $0.1?.isPressed == true ? $0.0 : nil },
        axes: [
          AxisReading(.leftX, Double(gamepad.leftThumbstick.xAxis.value)),
          AxisReading(.leftY, Double(gamepad.leftThumbstick.yAxis.value)),
          AxisReading(.rightX, Double(gamepad.rightThumbstick.xAxis.value)),
          AxisReading(.rightY, Double(gamepad.rightThumbstick.yAxis.value)),
          AxisReading(.leftTrigger, Double(gamepad.leftTrigger.value)),
          AxisReading(.rightTrigger, Double(gamepad.rightTrigger.value)),
          AxisReading(.dpadX, Double(gamepad.dpad.xAxis.value)),
          AxisReading(.dpadY, Double(gamepad.dpad.yAxis.value)),
        ]
      )
    }

    deinit {
      task?.cancel()
    }
  }
#endif
