import ActivityKit
import SwiftUI
import WidgetKit
import PhomicActivity

@main
struct PhomicWidgetBundle: WidgetBundle {
    var body: some Widget {
        MicLiveActivity()
    }
}

/// Lock screen and Dynamic Island while Phomic is connected. Tapping it opens the app.
struct MicLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: MicActivityAttributes.self) { context in
            LockScreenView(state: context.state.shown(stale: context.isStale))
                .activityBackgroundTint(Color.black.opacity(0.6))
                .activitySystemActionForegroundColor(.white)
        } dynamicIsland: { context in
            let state = context.state.shown(stale: context.isStale)
            return DynamicIsland {
                // The badge and the transport icon sit in boxes of the same size, centered
                // vertically, with the same margin on every side, so they mirror each other.
                DynamicIslandExpandedRegion(.leading) {
                    MuteButton(state: state, size: Self.badge)
                        .frame(maxHeight: .infinity)
                }
                DynamicIslandExpandedRegion(.center) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(state.title)
                            .font(.headline)
                        Text(state.pcName)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    TransportIcon(transport: state.transport, size: 26)
                        .foregroundStyle(.secondary)
                        .frame(width: Self.badge, height: Self.badge)
                        .frame(maxHeight: .infinity)
                }
            } compactLeading: {
                Image(systemName: state.micSymbol)
                    .foregroundStyle(state.color)
            } compactTrailing: {
                TransportIcon(transport: state.transport, size: 18)
                    .foregroundStyle(.secondary)
            } minimal: {
                Image(systemName: state.micSymbol)
                    .foregroundStyle(state.color)
            }
            .contentMargins(.all, Self.margin, for: .expanded)
        }
    }

    private static let badge: CGFloat = 52
    private static let margin: CGFloat = 20
}

private struct LockScreenView: View {
    let state: MicActivityAttributes.ContentState

    var body: some View {
        HStack(spacing: 14) {
            MuteButton(state: state, size: 48)
            VStack(alignment: .leading, spacing: 2) {
                Text(state.title)
                    .font(.headline)
                HStack(spacing: 6) {
                    TransportIcon(transport: state.transport, size: 16)
                    Text(state.pcName)
                        .lineLimit(1)
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
        }
        .foregroundStyle(.white)
        .padding(16)
    }
}

/// The microphone badge; tapping it mutes or unmutes (except while reconnecting).
private struct MuteButton: View {
    let state: MicActivityAttributes.ContentState
    let size: CGFloat

    var body: some View {
        if state.status == .reconnecting {
            MicBadge(state: state, size: size)
        } else {
            Button(intent: ToggleMuteIntent()) {
                MicBadge(state: state, size: size)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(state.status == .muted ? "Unmute" : "Mute")
        }
    }
}

private struct MicBadge: View {
    let state: MicActivityAttributes.ContentState
    let size: CGFloat

    var body: some View {
        Image(systemName: state.micSymbol)
            .font(.system(size: size * 0.42, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background(Circle().fill(state.color))
    }
}

private extension MicActivityAttributes.ContentState {
    /// Phomic renews the activity while it runs; one gone stale means Phomic stopped without
    /// ending it (a crash, say), so it shows as stopped instead of live.
    func shown(stale: Bool) -> Self {
        guard stale else { return self }
        var state = self
        state.status = .reconnecting
        state.stopped = true
        return state
    }

    var title: String {
        if stopped { return "Phomic stopped" }
        switch status {
        case .live: return "Live"
        case .muted: return "Muted"
        case .reconnecting: return "Reconnecting…"
        }
    }

    var micSymbol: String {
        status == .muted ? "mic.slash.fill" : "mic.fill"
    }

    /// Same colors as the button in the app: green live, red muted.
    var color: Color {
        switch status {
        case .live: .green
        case .muted: .red
        case .reconnecting: .gray
        }
    }
}
