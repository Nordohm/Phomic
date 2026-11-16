import ActivityKit
import Foundation
import PhomicActivity

/// Starts, updates and ends Phomic's Live Activity (see MicActivityAttributes).
///
/// It starts when a session goes live, follows mute and reconnects, and ends when the
/// session does. iOS only lets apps start one from the foreground, so a start that fails
/// (or was declined) is tried again with the next change while the app is open.
@MainActor
final class LiveActivityController {
    typealias State = MicActivityAttributes.ContentState

    private var activity: Activity<MicActivityAttributes>?
    private var shown: State?

    /// ActivityKit's Activity is thread-safe but not marked Sendable.
    private struct Handle: @unchecked Sendable {
        let activity: Activity<MicActivityAttributes>
        func update(_ state: State) {
            Task.detached { await activity.update(ActivityContent(state: state, staleDate: nil)) }
        }
        func end() {
            Task.detached { await activity.end(nil, dismissalPolicy: .immediate) }
        }
    }

    init() {
        // A run that was killed can leave one behind.
        for stale in Activity<MicActivityAttributes>.activities {
            Handle(activity: stale).end()
        }
    }

    /// `nil` ends the activity.
    func show(_ state: State?) {
        guard let state else {
            shown = nil
            if let activity {
                self.activity = nil
                Handle(activity: activity).end()
            }
            return
        }
        guard state != shown else { return }
        if let activity {
            shown = state
            Handle(activity: activity).update(state)
        } else if state.status != .reconnecting, ActivityAuthorizationInfo().areActivitiesEnabled {
            // Start only once actually live; keep it through reconnects after that.
            activity = try? Activity.request(attributes: MicActivityAttributes(),
                                             content: ActivityContent(state: state, staleDate: nil))
            shown = activity == nil ? nil : state
        }
    }
}
