import AppIntents
import PhomicActivity

/// Xcode builds: puts PhomicActivity's intents (ToggleMuteIntent) into the app's App Intents
/// metadata. See PhomicActivityIntents.
struct PhomicAppIntents: AppIntentsPackage {
    static var includedPackages: [any AppIntentsPackage.Type] { [PhomicActivityIntents.self] }
}
