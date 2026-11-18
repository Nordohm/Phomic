import ActivityKit
import Foundation
import PhomicActivity
import UIKit
import os

/// Starts, updates and ends Phomic's Live Activity (see MicActivityAttributes).
///
/// It starts when a session goes live, follows mute and reconnects, and ends when the
/// session does. iOS only lets apps start one from the foreground, so a start that fails
/// (or was declined) is tried again with the next change while the app is open.
///
/// It must not outlive the app: when Phomic is swiped away in the app switcher it ends the
/// activity on the way out (endBeforeExit), and one left over from a crash goes at the next
/// launch.
///
/// It has no stale date. Phomic used to renew one every minute, so an activity whose app had
/// died would show that; but iOS rations updates from apps in the background and drops the rest
/// without a word, so a running Phomic's activity went stale too, and showed "Phomic stopped"
/// until Phomic came on screen.
///
/// Every step goes to the log (category LiveActivity; read it with the iPhone plugged in,
/// `idevicesyslog -p Phomic`), along with the app going to and from the background.
@MainActor
final class LiveActivityController {
    typealias State = MicActivityAttributes.ContentState

    private var activity: Activity<MicActivityAttributes>?
    private var shown: State?
    private var watch: Task<Void, Never>?
    private var observers: [NSObjectProtocol] = []

    nonisolated private static let log = Logger(subsystem: "io.github.nordohm.Phomic", category: "LiveActivity")

    /// ActivityKit's Activity is thread-safe but not marked Sendable.
    private struct Handle: @unchecked Sendable {
        let activity: Activity<MicActivityAttributes>
        func update(_ state: State) {
            let content = LiveActivityController.content(state)
            Task.detached {
                await activity.update(content)
                LiveActivityController.log.notice(
                    "changed: \(state.status.rawValue, privacy: .public), activity \(String(describing: activity.activityState), privacy: .public)")
            }
        }
        func end(why: String) {
            Task.detached {
                await activity.end(nil, dismissalPolicy: .immediate)
                LiveActivityController.log.notice("ended (\(why, privacy: .public))")
            }
        }
    }

    init() {
        // A run that was killed can leave one behind.
        let leftover = Activity<MicActivityAttributes>.activities
        if !leftover.isEmpty {
            Self.log.notice("ending \(leftover.count) left from an earlier run")
        }
        for stale in leftover {
            Handle(activity: stale).end(why: "left from an earlier run")
        }
        let center = NotificationCenter.default
        let moments: [(Notification.Name, String)] = [
            (UIApplication.didEnterBackgroundNotification, "app in the background"),
            (UIApplication.willEnterForegroundNotification, "app coming back"),
            (UIApplication.didBecomeActiveNotification, "app active"),
            (UIApplication.didReceiveMemoryWarningNotification, "memory warning"),
        ]
        for (name, text) in moments {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { _ in
                Self.log.notice("\(text, privacy: .public)")
            })
        }
    }

    nonisolated private static func content(_ state: State) -> ActivityContent<State> {
        ActivityContent(state: state, staleDate: nil)
    }

    /// `nil` ends the activity.
    func show(_ state: State?) {
        guard let state else {
            shown = nil
            watch?.cancel()
            watch = nil
            if let activity {
                self.activity = nil
                Handle(activity: activity).end(why: "session over")
            }
            return
        }
        guard state != shown else { return }
        if let activity {
            shown = state
            Handle(activity: activity).update(state)
        } else if state.status != .reconnecting {
            // Start only once actually live; keep it through reconnects after that.
            guard ActivityAuthorizationInfo().areActivitiesEnabled else {
                Self.log.notice("not started: Live Activities are off for Phomic")
                return
            }
            do {
                let started = try Activity.request(attributes: MicActivityAttributes(), content: Self.content(state))
                activity = started
                shown = state
                Self.log.notice("started: \(state.status.rawValue, privacy: .public)")
                watchState(of: started)
            } catch {
                Self.log.error("not started: \(error.localizedDescription, privacy: .public)")
            }
        }
    }

    /// Phomic is being closed (swiped away in the app switcher): end the activity before the
    /// process goes, or it would stay on the lock screen. iOS gives the app a moment for this,
    /// so this waits for ActivityKit, up to two seconds.
    func endBeforeExit() {
        Self.log.notice("app closing")
        watch?.cancel()
        watch = nil
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

    /// Logs what iOS does with the activity: it being ended or dismissed by someone else.
    private func watchState(of activity: Activity<MicActivityAttributes>) {
        watch?.cancel()
        let handle = Handle(activity: activity)
        watch = Task.detached {
            for await state in handle.activity.activityStateUpdates {
                Self.log.notice("iOS says the activity is \(String(describing: state), privacy: .public)")
            }
        }
    }
}
