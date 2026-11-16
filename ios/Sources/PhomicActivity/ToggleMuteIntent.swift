import AppIntents

/// The mute button on Phomic's Live Activity (and a "Toggle Mute" action in Shortcuts).
///
/// iOS runs it in the app, even when the app is in the background (LiveActivityIntent);
/// AudioRecordingIntent is what allows turning the microphone on from there. The app sets
/// `handler` at launch.
///
/// After changing this type, regenerate Resources/Metadata.appintents (Tools/appintents-metadata.sh):
/// iOS only runs intents that the bundle's metadata describes.
public struct ToggleMuteIntent: LiveActivityIntent, AudioRecordingIntent {
    public static let title: LocalizedStringResource = "Toggle Mute"

    public init() {}

    @MainActor public static var handler: (@MainActor () -> Void)?

    @MainActor
    public func perform() async throws -> some IntentResult {
        Self.handler?()
        return .result()
    }
}

/// Lets Xcode builds (Xcode/project.yml) find the intents in this module: the app and the widget
/// include it (PhomicAppIntents, PhomicWidgetIntents), and Xcode writes their metadata. xtool
/// builds don't use this; they ship Resources/Metadata.appintents instead.
public struct PhomicActivityIntents: AppIntentsPackage {}
