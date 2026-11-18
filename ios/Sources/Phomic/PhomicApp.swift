import SwiftUI

@main
struct PhomicApp: App {
    @State private var model = MicController()
    @State private var updates = UpdateCheck()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(model)
                .environment(updates)
        }
    }
}
