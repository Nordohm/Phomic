import ActivityKit
import Foundation

/// The Live Activity Phomic shows while it's connected to a PC (lock screen and Dynamic
/// Island). Shared by the app, which starts and updates it, and the widget, which draws it.
public struct MicActivityAttributes: ActivityAttributes {
    public struct ContentState: Codable, Hashable, Sendable {
        public enum Status: String, Codable, Hashable, Sendable {
            case live
            case muted
            case reconnecting
        }

        public var status: Status
        public var pcName: String
        public var bluetooth: Bool

        public init(status: Status, pcName: String, bluetooth: Bool) {
            self.status = status
            self.pcName = pcName
            self.bluetooth = bluetooth
        }
    }

    public init() {}
}
