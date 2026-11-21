import SwiftUI
import UIKit
import PhomicActivity

/// One tab per connection (Wi-Fi, Bluetooth, USB) in the tab bar, Liquid Glass on iOS 26.
/// Picking a tab picks the connection, as the switch at the top did: while live, the session
/// moves over to it.
struct ContentView: View {
    @Environment(MicController.self) private var model
    @State private var pairingCode = ""
    @State private var showSettings = false

    var body: some View {
        @Bindable var model = model

        TabView(selection: $model.preferredTransport) {
            Tab("Wi-Fi", systemImage: "wifi", value: MicController.Transport.wifi) {
                ConnectionScreen(transport: .wifi, showSettings: $showSettings)
            }
            Tab(value: MicController.Transport.bluetooth) {
                ConnectionScreen(transport: .bluetooth, showSettings: $showSettings)
            } label: {
                Label {
                    Text("Bluetooth")
                } icon: {
                    Image(uiImage: BluetoothGlyph.image)
                }
            }
            Tab("USB", systemImage: "cable.connector", value: MicController.Transport.usb) {
                ConnectionScreen(transport: .usb, showSettings: $showSettings)
            }
        }
        .sensoryFeedback(.selection, trigger: model.preferredTransport)
        .overlay(alignment: .top) {
            ErrorBanner()
        }
        .sheet(isPresented: $showSettings) {
            SettingsView()
        }
        .alert(
            "Pairing code",
            isPresented: Binding(
                get: { model.pairingPrompt != nil },
                set: { if !$0 { model.cancelPairing() } }
            ),
            presenting: model.pairingPrompt
        ) { _ in
            TextField("4 digits", text: $pairingCode)
                .keyboardType(.numberPad)
            Button("Connect") {
                model.submitPairingCode(pairingCode)
                pairingCode = ""
            }
            Button("Cancel", role: .cancel) {
                model.cancelPairing()
                pairingCode = ""
            }
        } message: { prompt in
            Text(prompt.wrongCode
                 ? "Wrong code. It's shown in Phomic on \(prompt.pcName)."
                 : "It's shown in Phomic on \(prompt.pcName).")
        }
    }
}

/// The Bluetooth rune as a template image: tab bar icons have to be images, and SF Symbols
/// has no Bluetooth logo.
@MainActor
private enum BluetoothGlyph {
    static let image: UIImage = {
        let renderer = ImageRenderer(content: Rune())
        renderer.scale = 3
        return (renderer.uiImage ?? UIImage()).withRenderingMode(.alwaysTemplate)
    }()

    private struct Rune: View {
        var body: some View {
            BluetoothRune()
                .stroke(style: StrokeStyle(lineWidth: 2.4, lineCap: .round, lineJoin: .round))
                .foregroundStyle(.black)
                .frame(width: 26, height: 26)
        }
    }
}

/// What one tab shows: the live session, or how to connect over that transport.
private struct ConnectionScreen: View {
    let transport: MicController.Transport
    @Binding var showSettings: Bool
    @Environment(MicController.self) private var model
    @Environment(UpdateCheck.self) private var updates

    private var isStreaming: Bool {
        if case .streaming = model.phase { return true }
        return false
    }

    private static let verticalPadding: CGFloat = 32

    var body: some View {
        NavigationStack {
            // Centered on the screen: the live screen around the mute button exactly, the rest as
            // a group (a long list of PCs scrolls instead). The top bar (with the status bar) is
            // taller than the tab bar, so centering between the two would sit a little low;
            // instead the content keeps the same margin, the larger of the two, top and bottom.
            GeometryReader { screen in
                let margin = max(screen.safeAreaInsets.top, screen.safeAreaInsets.bottom)
                let height = max(0, screen.size.height - 2 * margin)
                ScrollView {
                    VStack(spacing: 24) {
                        Group {
                            if model.isActive {
                                LiveView()
                                    .frame(height: max(0, height - Self.verticalPadding))
                            } else {
                                if let version = updates.available {
                                    UpdateCard(version: version)
                                        .transition(.opacity)
                                }
                                switch transport {
                                case .wifi:
                                    WiFiComputers()
                                case .bluetooth:
                                    BluetoothComputers()
                                case .usb:
                                    UsbWaiting()
                                }
                            }
                        }
                        .transition(.opacity)
                    }
                    .padding(.horizontal, 20)
                    .padding(.vertical, Self.verticalPadding / 2)
                    .frame(minHeight: height)
                    .padding(.vertical, margin)
                    .animation(.snappy, value: model.isActive)
                    .animation(.snappy, value: updates.available)
                }
            }
            // The whole screen, so `screen` knows the bars; the margins above keep content clear
            // of them (and long lists scroll on under the glass).
            .ignoresSafeArea(.container, edges: .vertical)
            // Scroll only when there's more than fits, e.g. a long list of PCs.
            .scrollBounceBehavior(.basedOnSize)
            .backgroundPreferenceValue(MicButtonKey.self) { button in
                if let button, model.isActive {
                    VoiceGlow(button: button, isLive: isStreaming && !model.isMuted, isMuted: isStreaming && model.isMuted)
                }
            }
            .background(Color(.systemGroupedBackground))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        showSettings = true
                    } label: {
                        Image(systemName: "gearshape")
                    }
                    .accessibilityLabel("Settings")
                }
            }
        }
    }
}

// MARK: - Building blocks

private extension View {
    /// A rounded tile on the grouped background, like the rows of a list.
    func card() -> some View {
        padding(.horizontal, 16)
            .padding(.vertical, 14)
            .background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }

    /// Liquid Glass buttons on iOS 26, bordered ones before.
    @ViewBuilder
    func glassButton(prominent: Bool = false) -> some View {
        if #available(iOS 26, *) {
            if prominent {
                buttonStyle(.glassProminent)
            } else {
                buttonStyle(.glass)
            }
        } else if prominent {
            buttonStyle(.borderedProminent)
        } else {
            buttonStyle(.bordered)
        }
    }
}

/// Shrinks a little while pressed; for buttons that draw their own background.
private struct PressableStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .opacity(configuration.isPressed ? 0.85 : 1)
            .animation(.snappy(duration: 0.15), value: configuration.isPressed)
    }
}

/// A newer Phomic is on the App Store (UpdateCheck). Shown with the list of PCs, not during a
/// session.
private struct UpdateCard: View {
    let version: String
    @Environment(UpdateCheck.self) private var updates
    @Environment(\.openURL) private var openURL

    var body: some View {
        HStack(spacing: 12) {
            Button {
                openURL(UpdateCheck.appStore)
            } label: {
                HStack(spacing: 12) {
                    Image(systemName: "arrow.down.circle.fill")
                        .font(.title2)
                        .foregroundStyle(.tint)
                    Text("Phomic \(version) is available")
                        .font(.subheadline.weight(.semibold))
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(PressableStyle())
            .accessibilityHint("Opens the App Store")

            Button {
                updates.dismiss()
            } label: {
                Image(systemName: "xmark")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .padding(6)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Not now")
        }
        .card()
    }
}

/// MicController's last error, at the top of the screen like a notification, over every tab and
/// the live screen. It stays until it's swiped up or closed, or the next attempt clears it.
private struct ErrorBanner: View {
    @Environment(MicController.self) private var model

    var body: some View {
        ZStack {
            if let message = model.lastError {
                Banner(message: message) {
                    model.dismissError()
                }
                // A new message is a new banner, with its own drag.
                .id(message)
                .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .animation(.snappy, value: model.lastError)
    }

    private struct Banner: View {
        let message: String
        let dismiss: () -> Void
        @State private var drag: CGFloat = 0

        var body: some View {
            HStack(spacing: 12) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .font(.title3)
                    .foregroundStyle(.orange)
                    .accessibilityHidden(true)
                Text(message)
                    .font(.subheadline)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button(action: dismiss) {
                    Image(systemName: "xmark")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .frame(width: 44, height: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Close")
            }
            .padding(.leading, 16)
            .padding(.trailing, 4)
            .padding(.vertical, 6)
            .frame(minHeight: 56)
            .glassBackground(cornerRadius: 22)
            .padding(.horizontal, 12)
            // Follows the finger upwards only; a flick or a short pull up dismisses it.
            .offset(y: min(0, drag))
            .gesture(
                DragGesture(minimumDistance: 8)
                    .onChanged { drag = $0.translation.height }
                    .onEnded { value in
                        if value.translation.height < -30 || value.predictedEndTranslation.height < -80 {
                            dismiss()
                        } else {
                            withAnimation(.snappy) { drag = 0 }
                        }
                    }
            )
            .accessibilityElement(children: .combine)
            .accessibilityAction(named: "Close", dismiss)
            .onAppear {
                AccessibilityNotification.Announcement(message).post()
            }
        }
    }
}

private extension View {
    /// Liquid Glass on iOS 26, a material with a shadow before.
    @ViewBuilder
    func glassBackground(cornerRadius: CGFloat) -> some View {
        let shape = RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
        if #available(iOS 26, *) {
            glassEffect(.regular, in: shape)
        } else {
            background(.regularMaterial, in: shape)
                .shadow(color: .black.opacity(0.15), radius: 12, y: 4)
        }
    }
}

// MARK: - Live session

/// The iPhone's Mic Mode isn't Standard, so iOS processes the sound before Phomic sends it. Apps
/// can't change that themselves, so this stays as long as it's so; the button opens the system's
/// Mic Mode menu, where Standard turns it off.
private struct MicModeWarning: View {
    let title: String
    let turnOff: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.title3)
                .foregroundStyle(.orange)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.subheadline.weight(.semibold))
                Text("Choose Standard for unprocessed sound.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityElement(children: .combine)
            Button("Turn Off", action: turnOff)
                .glassButton()
                .accessibilityHint("Opens Mic Mode")
        }
        .padding(.leading, 14)
        .padding(.trailing, 10)
        .padding(.vertical, 10)
        .background(Color.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

/// Once, the first time live: Mic Mode reads Standard, but Automatic reads the same, so this says
/// which one to choose. Closing it, or opening Mic Mode from it, puts it away for good.
private struct MicModeReminder: View {
    let open: () -> Void
    let close: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "waveform")
                .font(.title3)
                .foregroundStyle(.tint)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text("Check Mic Mode")
                    .font(.subheadline.weight(.semibold))
                Text("Choose Standard for unprocessed sound.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityElement(children: .combine)
            Button("Open", action: open)
                .glassButton()
                .accessibilityHint("Opens Mic Mode")
            Button(action: close) {
                Image(systemName: "xmark")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .frame(width: 32, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close")
        }
        .padding(.leading, 14)
        .padding(.trailing, 4)
        .padding(.vertical, 6)
        .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

private struct LiveView: View {
    @Environment(MicController.self) private var model

    var body: some View {
        VStack(spacing: 0) {
            VStack(spacing: 24) {
                if let title = model.processingMicMode {
                    MicModeWarning(title: title) {
                        model.showSystemMicModes()
                    }
                    .transition(.opacity)
                } else if model.showsMicModeReminder {
                    MicModeReminder {
                        model.showSystemMicModes()
                        model.dismissMicModeReminder()
                    } close: {
                        model.dismissMicModeReminder()
                    }
                    .transition(.opacity)
                }
                VStack(spacing: 8) {
                    Text(title)
                        .font(.title2.weight(.semibold))
                        .multilineTextAlignment(.center)
                    if let chip {
                        Label(chip, systemImage: "desktopcomputer")
                            .font(.footnote.weight(.medium))
                            .foregroundStyle(.secondary)
                            .padding(.horizontal, 10)
                            .padding(.vertical, 5)
                            .background(Color(.tertiarySystemFill), in: Capsule())
                    }
                    if let note {
                        Text(note)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                }
                .accessibilityElement(children: .combine)
            }
            .animation(.snappy, value: model.processingMicMode)
            .animation(.snappy, value: model.showsMicModeReminder)
            .frame(maxHeight: .infinity, alignment: .bottom)
            .padding(.bottom, 32)

            // The two halves around it share the height equally, so it sits in the middle.
            MuteButton(isMuted: model.isMuted, isLive: isStreaming) {
                model.isMuted.toggle()
            }

            VStack(spacing: 18) {
                if model.transport == .usb {
                    // A cable is plugged in or not: no Disconnect, like a wired microphone.
                    if isStreaming {
                        Text("Unplug the cable to stop.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Button("Disconnect", role: .destructive) {
                        model.disconnect()
                    }
                    .buttonStyle(.bordered)
                    .tint(.red)
                    .controlSize(.large)
                }
            }
            .frame(maxHeight: .infinity, alignment: .top)
            .padding(.top, 32)
        }
        .frame(maxWidth: .infinity)
    }

    private var isStreaming: Bool {
        if case .streaming = model.phase { return true }
        return false
    }

    /// One word or two; the PC's name goes in the chip below, so long names don't wrap.
    private var title: String {
        switch model.phase {
        case .idle: return ""
        case .connecting: return "Connecting…"
        case .streaming: return model.isMuted ? "Muted" : "Live"
        case .reconnecting: return model.transport == .usb ? "Unplugged" : "Reconnecting…"
        }
    }

    /// Which PC, and how well: "ATT-DESKTOP · 4 ms", "ATT-DESKTOP · 48 kHz". The tab it's on
    /// already says Wi-Fi, Bluetooth or USB.
    private var chip: String? {
        var parts: [String] = []
        if let name = pcName {
            parts.append(name)
        }
        if isStreaming {
            if model.transport == .bluetooth {
                if let rate = model.bluetoothSampleRate { parts.append("\(rate / 1000) kHz") }
            } else if let rtt = model.roundTripMs {
                parts.append("\(Int(rtt.rounded())) ms")
            }
        }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }

    /// The PC's name, once known (over USB the PC says who it is when it connects).
    private var pcName: String? {
        let name: String
        switch model.phase {
        case .idle: return nil
        case .connecting(let n), .streaming(let n), .reconnecting(let n): name = n
        }
        return name == "your PC" ? nil : name
    }

    private var note: String? {
        if case .reconnecting = model.phase, model.transport == .usb {
            return "Plug the cable back in."
        }
        return nil
    }
}

/// The big mute button. Your voice shows in the background behind it (VoiceGlow), which
/// finds the button through MicButtonKey.
private struct MuteButton: View {
    let isMuted: Bool
    let isLive: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack {
                Circle()
                    .fill(color)
                    .anchorPreference(key: MicButtonKey.self, value: .bounds) { $0 }
                Image(systemName: isMuted ? "mic.slash.fill" : "mic.fill")
                    .font(.system(size: 60, weight: .semibold))
                    .foregroundStyle(.white)
                    .contentTransition(.symbolEffect(.replace))
            }
            .frame(width: 172, height: 172)
            .animation(.snappy(duration: 0.12), value: isMuted) // quick, so a tap feels instant
            .animation(.easeInOut(duration: 0.2), value: isLive)
        }
        .buttonStyle(PressableStyle())
        .sensoryFeedback(.impact, trigger: isMuted)
        .accessibilityLabel(isMuted ? "Unmute" : "Mute")
    }

    /// Red: muted. Green: live. Gray: not connected yet.
    private var color: Color {
        if isMuted { return .red }
        return isLive ? .green : Color(.systemGray3)
    }
}

/// Where the mute button is, for the glow behind the whole screen.
private struct MicButtonKey: PreferenceKey {
    static let defaultValue: Anchor<CGRect>? = nil
    static func reduce(value: inout Anchor<CGRect>?, nextValue: () -> Anchor<CGRect>?) {
        value = nextValue() ?? value
    }
}

/// Your voice as light: a gradient from the mute button's edge out to the screen's edges, which
/// brightens and reaches further while you talk and settles when you're quiet. It rises fast and
/// fades slowly, so it breathes rather than flickers. As the sound nears full scale it warms up,
/// like a level meter: from green through yellow and orange (-9 dB) to red (0 dB). A still,
/// faint red while muted.
///
/// It's drawn every frame from the latest level (TimelineView), not when the level changes: in
/// silence the level stays at the same value, and a glow that only moved on changes would stop
/// wherever it was. While muted it stands still, so drawing stops once the fade to red is done.
/// It reads the level itself, so the screen around it doesn't redraw 20 times a second too.
private struct VoiceGlow: View {
    let button: Anchor<CGRect>
    let isLive: Bool
    let isMuted: Bool

    @Environment(MicController.self) private var model
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var smoother = GlowSmoother()
    /// Still fading after muting or unmuting: keep drawing a little longer.
    @State private var fading = false

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30, paused: !isLive && !fading)) { context in
            let look = smoother.next(level: isLive ? target : 0, muted: isMuted, loudness: isLive ? loudness : 0,
                                     at: context.date)
            let tint = Self.warm(look.heat, colorScheme).mix(with: .red, by: look.muted)
            let amount = strength * look.amount
            GeometryReader { proxy in
                let rect = proxy[button]
                let size = proxy.size
                let center = CGPoint(x: rect.midX, y: rect.midY)
                // Out to the farthest corner, so the whole screen is in it.
                let far = [CGPoint.zero, CGPoint(x: size.width, y: 0), CGPoint(x: 0, y: size.height),
                           CGPoint(x: size.width, y: size.height)]
                    .map { hypot($0.x - center.x, $0.y - center.y) }
                    .max() ?? 1
                RadialGradient(
                    stops: [
                        .init(color: tint.opacity(amount), location: 0),
                        .init(color: tint.opacity(amount * 0.35), location: look.middle),
                        .init(color: tint.opacity(0), location: 1),
                    ],
                    center: UnitPoint(x: center.x / max(size.width, 1), y: center.y / max(size.height, 1)),
                    startRadius: rect.width / 2,
                    endRadius: max(far, rect.width / 2 + 1)
                )
            }
        }
        .task(id: isMuted) {
            fading = true
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
            fading = false
        }
        .ignoresSafeArea()
        .opacity(isLive || isMuted ? 1 : 0)
        .animation(.easeInOut(duration: 0.4), value: isLive || isMuted)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    /// Where the glow is heading, 0...1. `level` is -60...0 dB as 0...1. A room's own noise sits
    /// around -45 dB (0.25) and talking at arm's length around -30 to -20 dB, so -45 dB is dark
    /// and -21 dB full.
    private var target: CGFloat {
        reduceMotion ? 0.4 : CGFloat(min(1, max(0, (model.level - 0.25) / 0.4)))
    }

    /// How close to full scale, 0...1: 0 up to -9 dB, 1 at 0 dB, smooth in between.
    private var loudness: Double {
        let x = min(1, max(0, (Double(model.level) - 0.85) / 0.15))
        return x * x * (3 - 2 * x)
    }

    /// The same color reads stronger on black (it's the only light there) than as a tint on
    /// a light background, so dark mode gets less of it.
    private var strength: Double { colorScheme == .dark ? 0.34 : 0.6 }

    /// Green (0) to red (1) the way a level meter goes: around the color wheel, through yellow
    /// and orange. (Mixing the two directly passes through a muddy olive.) Red's hue can be just
    /// under 1 rather than just over 0 (iOS 26's red has a touch more blue than green), and going
    /// up to it from green would pass through blue, so the way round is always down through 0.
    private static func warm(_ heat: Double, _ scheme: ColorScheme) -> Color {
        let traits = UITraitCollection(userInterfaceStyle: scheme == .dark ? .dark : .light)
        func hsb(_ color: UIColor) -> (Double, Double, Double) {
            var h: CGFloat = 0, s: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
            color.resolvedColor(with: traits).getHue(&h, saturation: &s, brightness: &b, alpha: &a)
            return (Double(h), Double(s), Double(b))
        }
        let (gh, gs, gb) = hsb(.systemGreen)
        var (rh, rs, rb) = hsb(.systemRed)
        if rh > gh { rh -= 1 }
        var hue = gh + (rh - gh) * heat
        if hue < 0 { hue += 1 }
        return Color(hue: hue, saturation: gs + (rs - gs) * heat, brightness: gb + (rb - gb) * heat)
    }
}

/// Eases the glow toward where it's heading, frame by frame. The voice goes up in about 0.1 s
/// and down in about 0.7 s, and so does the warming up when it's loud (back in about 1.5 s); the
/// change between live and muted takes about half a second. A class, so the values can change
/// while the view is drawn.
private final class GlowSmoother {
    struct Look {
        /// 0...1, times the color scheme's strength.
        var amount: Double
        /// Where the gradient is at a third of its brightness, 0...1 of the way out.
        var middle: Double
        /// 0 green ... 1 red, through yellow and orange: how loud.
        var heat: Double
        /// 0 live ... 1 muted.
        var muted: Double
    }

    private var glow: Double = 0
    private var muted: Double = 0
    private var heat: Double = 0
    private var last: Date?

    func next(level target: CGFloat, muted isMuted: Bool, loudness: Double, at now: Date) -> Look {
        // After a pause (muted, nothing drawn), carry on from here rather than jump ahead.
        let gap = last.map { now.timeIntervalSince($0) } ?? 0
        let elapsed = gap > 0 && gap < 0.1 ? gap : 1.0 / 60
        last = now
        func ease(_ value: inout Double, toward target: Double, _ timeConstant: Double) {
            value += (target - value) * (1 - exp(-elapsed / timeConstant))
        }
        ease(&glow, toward: Double(target), Double(target) > glow ? 0.08 : 0.7)
        ease(&muted, toward: isMuted ? 1 : 0, 0.15)
        ease(&heat, toward: loudness, loudness > heat ? 0.08 : 0.5)

        // Square root: ordinary talking already shows well; shouting only adds a little more.
        let voice = sqrt(max(0, glow))
        let live = (amount: 0.15 + 0.85 * voice, middle: 0.2 + 0.4 * voice)
        let still = (amount: 0.35, middle: 0.22)
        return Look(
            amount: live.amount + (still.amount - live.amount) * muted,
            middle: live.middle + (still.middle - live.middle) * muted,
            heat: heat,
            muted: muted
        )
    }
}

// MARK: - Choosing a PC

/// The microphone emblem above the list, with rings going out while Phomic looks for PCs.
private struct Hero: View {
    let title: String
    let subtitle: String?
    let searching: Bool

    var body: some View {
        VStack(spacing: 10) {
            ZStack {
                if searching {
                    PulseRings()
                }
                Circle()
                    .fill(Color.accentColor.opacity(0.14))
                    .frame(width: 88, height: 88)
                Image(systemName: "mic.fill")
                    .font(.system(size: 34, weight: .semibold))
                    .foregroundStyle(Color.accentColor)
            }
            .frame(width: 150, height: 150)
            VStack(spacing: 4) {
                Text(title)
                    .font(.title3.weight(.semibold))
                if let subtitle {
                    Text(subtitle)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }
        }
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .combine)
    }
}

private struct PulseRings: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        TimelineView(.animation(minimumInterval: nil, paused: reduceMotion)) { context in
            let time = context.date.timeIntervalSinceReferenceDate
            ZStack {
                ForEach(0..<2, id: \.self) { ring in
                    let phase = (time / 2.4 + Double(ring) / 2).truncatingRemainder(dividingBy: 1)
                    Circle()
                        .stroke(Color.accentColor.opacity(0.35 * (1 - phase)), lineWidth: 1.5)
                        .frame(width: 88 + 62 * phase, height: 88 + 62 * phase)
                }
            }
        }
        .accessibilityHidden(true)
    }
}

private struct PCCard: View {
    let name: String
    let transport: TransportKind
    let requiresCode: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Group {
                    if transport == .bluetooth {
                        TransportIcon(transport: .bluetooth, size: 22)
                    } else {
                        Image(systemName: "desktopcomputer")
                            .font(.system(size: 18, weight: .medium))
                    }
                }
                .foregroundStyle(Color.accentColor)
                .frame(width: 40, height: 40)
                .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 10, style: .continuous))

                Text(name)
                    .font(.body.weight(.medium))
                    .foregroundStyle(.primary)
                    .lineLimit(1)
                Spacer(minLength: 8)
                if requiresCode {
                    Image(systemName: "lock.fill")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("Needs a pairing code")
                }
                Image(systemName: "chevron.right")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.tertiary)
                    .accessibilityHidden(true)
            }
            .card()
            .contentShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
        }
        .buttonStyle(PressableStyle())
    }
}

private struct WiFiComputers: View {
    @Environment(MicController.self) private var model

    var body: some View {
        VStack(spacing: 18) {
            Hero(title: model.servers.isEmpty ? "Looking for PCs…" : "Pick your PC",
                 subtitle: model.browseProblem ?? "On this Wi-Fi network",
                 searching: model.servers.isEmpty)
            VStack(spacing: 10) {
                ForEach(model.servers) { server in
                    PCCard(name: server.name, transport: .wifi, requiresCode: server.requiresCode) {
                        model.connect(to: server)
                    }
                }
            }
            NavigationLink {
                ManualConnectView()
            } label: {
                Label("Connect by Address", systemImage: "keyboard")
                    .font(.subheadline.weight(.medium))
            }
            if model.servers.isEmpty {
                GetWindowsAppLink()
            }
        }
    }
}

/// Where to get the PC app, for whoever finds no PC because it isn't installed yet.
private struct GetWindowsAppLink: View {
    var body: some View {
        Link(destination: URL(string: "https://nordohm.com/Phomic")!) {
            Label("Get Phomic for Windows", systemImage: "arrow.up.right.square")
                .font(.subheadline.weight(.medium))
        }
        .accessibilityHint("Opens nordohm.com/Phomic")
    }
}

private struct BluetoothComputers: View {
    @Environment(MicController.self) private var model

    var body: some View {
        VStack(spacing: 18) {
            Hero(title: model.nearbyPCs.isEmpty ? "Looking for PCs…" : "Pick your PC",
                 subtitle: model.bluetoothProblem ?? "Nearby, over Bluetooth",
                 searching: model.nearbyPCs.isEmpty)
            VStack(spacing: 10) {
                ForEach(model.nearbyPCs) { pc in
                    PCCard(name: pc.name, transport: .bluetooth, requiresCode: pc.requiresCode) {
                        model.connect(to: pc)
                    }
                }
            }
            if model.nearbyPCs.isEmpty {
                GetWindowsAppLink()
            }
        }
    }
}

/// USB: the iPhone waits, and the PC connects through the cable by itself.
private struct UsbWaiting: View {
    @Environment(MicController.self) private var model

    var body: some View {
        VStack(spacing: 20) {
            CableIllustration(waiting: !model.usbPaused)
            VStack(spacing: 4) {
                Text(model.usbPaused ? "Not waiting for a PC" : "Waiting for your PC")
                    .font(.title3.weight(.semibold))
                Text("Plug this iPhone into a PC running Phomic.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .accessibilityElement(children: .combine)
            if model.usbPaused {
                Button {
                    model.connectOverUsb()
                } label: {
                    Text("Connect")
                        .frame(minWidth: 140)
                }
                .glassButton(prominent: true)
                .controlSize(.large)
            } else {
                GetWindowsAppLink()
            }
        }
        .frame(maxWidth: .infinity)
    }
}

/// iPhone, a cable of dots, PC. The dots run while the iPhone waits.
private struct CableIllustration: View {
    let waiting: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        HStack(spacing: 14) {
            tile("iphone", highlighted: true)
            TimelineView(.periodic(from: .now, by: 0.35)) { context in
                let step = Int(context.date.timeIntervalSinceReferenceDate / 0.35) % 3
                HStack(spacing: 8) {
                    ForEach(0..<3, id: \.self) { dot in
                        Circle()
                            .fill(Color.accentColor)
                            .frame(width: 7, height: 7)
                            .opacity(!waiting ? 0.25 : reduceMotion ? 0.6 : (dot == step ? 1 : 0.3))
                    }
                }
            }
            tile("desktopcomputer", highlighted: false)
        }
        .accessibilityHidden(true)
    }

    private func tile(_ symbol: String, highlighted: Bool) -> some View {
        Image(systemName: symbol)
            .font(.system(size: 28, weight: .medium))
            .foregroundStyle(highlighted ? Color.accentColor : Color.secondary)
            .frame(width: 72, height: 72)
            .background(highlighted ? Color.accentColor.opacity(0.12) : Color(.secondarySystemGroupedBackground),
                        in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

private struct ManualConnectView: View {
    @Environment(MicController.self) private var model
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        @Bindable var model = model

        Form {
            Section {
                TextField("Address, e.g. 192.168.1.20", text: $model.manualHost)
                    .keyboardType(.numbersAndPunctuation)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .submitLabel(.go)
                    .onSubmit(connect)
                LabeledContent("Port") {
                    TextField("Port", text: $model.manualPort)
                        .keyboardType(.numberPad)
                        .multilineTextAlignment(.trailing)
                }
            } footer: {
                Text("Shown under **Show details** in Phomic on the PC.")
            }

            Section {
                Button("Connect", action: connect)
                    .disabled(model.manualHost.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .navigationTitle("Connect by Address")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func connect() {
        model.connectManually()
        if model.isActive {
            dismiss()
        }
    }
}

// MARK: - Settings

private struct SettingsView: View {
    /// The installed version, e.g. "1.1.0", from the app's Info.plist (MARKETING_VERSION on
    /// App Store builds), so it never needs updating here.
    static let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"

    @Environment(MicController.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var confirmForgetAll = false

    /// A new issue on GitHub with Phomic's feedback form, this app's version and the iPhone's
    /// filled in ("iOS 27.2.1, iPhone16,1").
    private var feedbackURL: URL {
        var system = utsname()
        uname(&system)
        let machine = withUnsafeBytes(of: &system.machine) { bytes in
            String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        }
        var url = URLComponents(string: "https://github.com/Nordohm/Phomic/issues/new")!
        url.queryItems = [
            URLQueryItem(name: "template", value: "feedback.yml"),
            URLQueryItem(name: "app", value: "iPhone \(Self.version)"),
            URLQueryItem(name: "device", value: "iOS \(UIDevice.current.systemVersion), \(machine)"),
        ]
        return url.url!
    }

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            List {
                Section {
                    Toggle("Live Activity", isOn: $model.liveActivityEnabled)
                }

                Section {
                    if model.savedPairings.isEmpty {
                        Text("None yet")
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(model.savedPairings) { pairing in
                            Label(pairing.name, systemImage: "desktopcomputer")
                                .swipeActions {
                                    Button("Forget", role: .destructive) {
                                        model.forgetPairing(pairing.id)
                                    }
                                }
                        }
                    }
                } header: {
                    Text("Paired PCs")
                } footer: {
                    Text(model.savedPairings.isEmpty
                         ? "PCs show up here once you enter their code."
                         : "Swipe to forget a PC.")
                }

                if !model.savedPairings.isEmpty {
                    Section {
                        Button("Forget All PCs", role: .destructive) {
                            confirmForgetAll = true
                        }
                    }
                }

                Section {
                    Link("Send Feedback", destination: feedbackURL)
                        .accessibilityHint("Opens a new issue on GitHub")
                    NavigationLink("Acknowledgements") {
                        AcknowledgementsView()
                    }
                } footer: {
                    Text("Phomic \(Self.version)")
                }
            }
            .scrollBounceBehavior(.basedOnSize)
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .confirmationDialog("Forget all paired PCs?", isPresented: $confirmForgetAll, titleVisibility: .visible) {
                Button("Forget All", role: .destructive) {
                    model.forgetAllPairings()
                }
            }
        }
    }
}
