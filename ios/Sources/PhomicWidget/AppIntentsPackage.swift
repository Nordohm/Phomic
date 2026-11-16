import AppIntents
import PhomicActivity

/// Xcode builds: puts PhomicActivity's intents (ToggleMuteIntent, the mute button) into the
/// widget's App Intents metadata. See PhomicActivityIntents.
struct PhomicWidgetIntents: AppIntentsPackage {
    static var includedPackages: [any AppIntentsPackage.Type] { [PhomicActivityIntents.self] }
}
