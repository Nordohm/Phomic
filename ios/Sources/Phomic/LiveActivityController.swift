import ActivityKit
import Foundation
import PhomicActivity

/// Starts, updates and ends Phomic's Live Activity (see MicActivityAttributes).
///
/// It starts when a session goes live, follows mute and reconnects, and ends when the
/// session does. iOS only lets apps start one from the foreground, so a start that fails
/// (or was declined) is tried again with the next change while the app is open.
///
/// It must not outlive the app: when Phomic is swiped away in the app switcher it ends the
/// activity on the way out (endBeforeExit). If Phomic dies without a chance to (a crash, or
/// iOS reclaiming memory), the activity goes stale a few minutes later, because Phomic renews
/// its stale date only while it runs, and the widget then shows that Phomic stopped.
@MainActor
final class LiveActivityController {
    typealias State = MicActivityAttributes.ContentState

    private var activity: Activity<MicActivityAttributes>?
    private var shown: State?
    private var renewal: Task<Void, Never>?

    /// How long an activity counts as current without hearing from Phomic, and how often a
    /// running Phomic says it still is.
    nonisolated private static let staleAfter: TimeInterval = 180
    private static let renewEvery: Duration = .seconds(60)

    /// ActivityKit's Activity is thread-safe but not marked Sendable.
    private struct Handle: @unchecked Sendable {
        let activity: Activity<MicActivityAttributes>
        func update(_ state: State) {
            let content = LiveActivityController.content(state)
            Task.detached { await activity.update(content) }
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

    nonisolated private static func content(_ state: State) -> ActivityContent<State> {
        ActivityContent(state: state, staleDate: Date().addingTimeInterval(staleAfter))
    }

    /// `nil` ends the activity.
    func show(_ state: State?) {
        guard let state else {
            shown = nil
            renewal?.cancel()
            renewal = nil
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
            activity = try? Activity.request(attributes: MicActivityAttributes(), content: Self.content(state))
            shown = activity == nil ? nil : state
            if activity != nil {
                startRenewing()
            }
        }
    }

    /// Phomic is being closed (swiped away in the app switcher): end the activity before the
    /// process goes, or it would stay on the lock screen. iOS gives the app a moment for this,
    /// so this waits for ActivityKit, up to two seconds.
    func endBeforeExit() {
        renewal?.cancel()
        renewal = nil
        activity = nil
        shown = nil
        let handles = Activity<MicActivityAttributes>.activities.map(Handle.init)
        guard !handles.isEmpty else { return }
        let done = DispatchSemaphore(value: 0)
        Task.detached {
            for handle in handles {
                await handle.activity.end(nil, dismissalPolicy: .immediate)
            }
            done.signal()
        }
        _ = done.wait(timeout: .now() + 2)
    }

    private func startRenewing() {
        renewal?.cancel()
        renewal = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: Self.renewEvery)
                guard !Task.isCancelled, let self, let activity = self.activity, let shown = self.shown else { return }
                Handle(activity: activity).update(shown)
            }
        }
    }
}
