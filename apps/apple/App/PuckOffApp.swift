// SPDX-License-Identifier: Apache-2.0
import SwiftUI

@main
struct PuckOffApp: App {
  var body: some Scene {
    #if os(macOS)
      Window("PuckOff", id: "companion") {
        CompanionView()
          .frame(minWidth: 700, minHeight: 620)
      }
      .defaultSize(width: 820, height: 760)
    #else
      WindowGroup {
        CompanionView()
      }
    #endif
  }
}
