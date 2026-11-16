import SwiftUI

@main
struct PhomicApp: App {
    @State private var model = MicController()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(model)
        }
    }
}
