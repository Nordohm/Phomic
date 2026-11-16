import SwiftUI
import PhomicActivity

struct ContentView: View {
    @Environment(MicController.self) private var model
    @State private var pairingCode = ""
    @State private var showSettings = false

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            List {
                Section {
                    Picker("Connection", selection: $model.preferredTransport) {
                        ForEach(MicController.Transport.allCases) { transport in
                            Text(transport.title).tag(transport)
                        }
                    }
                    .pickerStyle(.segmented)
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets())
                }

                if model.isActive {
                    Section {
                        LiveCard()
                        Button {
                            model.showSystemMicModes()
                        } label: {
                            LabeledContent("Processing", value: model.systemMicMode ?? "—")
                        }
                        .disabled(!model.canShowMicModes)
                    }
                }

                if let error = model.lastError {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                    }
                }

                if !model.isActive {
                    switch model.preferredTransport {
                    case .wifi:
                        ComputersSection()
                    case .bluetooth:
                        NearbyComputersSection()
                    }
                }
            }
            .navigationTitle("Phomic")
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
}

// MARK: - Live session

private struct LiveCard: View {
    @Environment(MicController.self) private var model

    var body: some View {
        VStack(spacing: 16) {
            VStack(spacing: 4) {
                Text(title)
                    .font(.headline)
                if let subtitle {
                    Text(subtitle)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }

            MuteButton(isMuted: model.isMuted, isLive: isStreaming) {
                model.isMuted.toggle()
            }

            LevelMeter(level: model.level, dimmed: model.isMuted || !isStreaming)
                .padding(.horizontal, 24)

            Button("Disconnect", role: .destructive) {
                model.disconnect()
            }
            .buttonStyle(.bordered)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 12)
    }

    private var isStreaming: Bool {
        if case .streaming = model.phase { return true }
        return false
    }

    private var title: String {
        switch model.phase {
        case .idle: ""
        case .connecting(let name): "Connecting to \(name)…"
        case .streaming(let name): model.isMuted ? "Muted" : "Live on \(name)"
        case .reconnecting(let name): "Reconnecting to \(name)…"
        }
    }

    private var subtitle: String? {
        switch model.phase {
        case .streaming:
            if model.transport == .bluetooth {
                if let rate = model.bluetoothSampleRate { return "Bluetooth · \(rate / 1000) kHz" }
                return "Bluetooth"
            }
            if let rtt = model.roundTripMs { return "Wi-Fi · \(Int(rtt.rounded())) ms" }
            return "Wi-Fi"
        default:
            return nil
        }
    }
}

private struct MuteButton: View {
    let isMuted: Bool
    let isLive: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: isMuted ? "mic.slash.fill" : "mic.fill")
                .font(.system(size: 64, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 168, height: 168)
                .background(Circle().fill(color))
                .contentTransition(.symbolEffect(.replace))
                .animation(.easeInOut(duration: 0.2), value: isMuted)
        }
        .buttonStyle(.plain)
        .sensoryFeedback(.impact, trigger: isMuted)
        .accessibilityLabel(isMuted ? "Unmute" : "Mute")
    }

    /// Red: muted. Green: live. Gray: not connected yet.
    private var color: Color {
        if isMuted { return .red }
        return isLive ? .green : .gray
    }
}

private struct LevelMeter: View {
    let level: Float
    let dimmed: Bool

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(.quaternary)
                Capsule()
                    .fill(dimmed ? Color.secondary : (level > 0.97 ? Color.red : Color.green))
                    .frame(width: proxy.size.width * CGFloat(level))
            }
        }
        .frame(height: 8)
        .animation(.linear(duration: 0.08), value: level)
        .accessibilityHidden(true)
    }
}

// MARK: - Choosing a PC

private struct ComputerRow: View {
    let name: String
    let bluetooth: Bool
    let requiresCode: Bool

    var body: some View {
        HStack {
            Label {
                Text(name)
            } icon: {
                if bluetooth {
                    TransportIcon(bluetooth: true, size: 22)
                } else {
                    Image(systemName: "desktopcomputer")
                }
            }
            Spacer()
            if requiresCode {
                Image(systemName: "lock.fill")
                    .foregroundStyle(.secondary)
                    .accessibilityLabel("Needs a pairing code")
            }
        }
    }
}

private struct SearchingRow: View {
    var body: some View {
        HStack(spacing: 12) {
            ProgressView()
            Text("Looking for PCs…")
                .foregroundStyle(.secondary)
        }
    }
}

private struct ComputersSection: View {
    @Environment(MicController.self) private var model

    var body: some View {
        Section {
            if model.servers.isEmpty {
                SearchingRow()
            } else {
                ForEach(model.servers) { server in
                    Button {
                        model.connect(to: server)
                    } label: {
                        ComputerRow(name: server.name, bluetooth: false, requiresCode: server.requiresCode)
                    }
                }
            }
            NavigationLink("Connect by Address") {
                ManualConnectView()
            }
        } header: {
            Text("Computers")
        } footer: {
            if let problem = model.browseProblem {
                Text(problem)
            }
        }
    }
}

private struct NearbyComputersSection: View {
    @Environment(MicController.self) private var model

    var body: some View {
        Section {
            if model.nearbyPCs.isEmpty {
                SearchingRow()
            } else {
                ForEach(model.nearbyPCs) { pc in
                    Button {
                        model.connect(to: pc)
                    } label: {
                        ComputerRow(name: pc.name, bluetooth: true, requiresCode: pc.requiresCode)
                    }
                }
            }
        } header: {
            Text("Computers")
        } footer: {
            if let problem = model.bluetoothProblem {
                Text(problem)
            }
        }
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
                Text("In Phomic on the PC, click **Show details** under Connection.")
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
    @Environment(MicController.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var confirmForgetAll = false

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            List {
                Section {
                    Toggle("Live Activity", isOn: $model.liveActivityEnabled)
                } footer: {
                    Text("Lock Screen and Dynamic Island while connected, with a mute button.")
                }

                Section {
                    if model.savedPairings.isEmpty {
                        Text("None saved")
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
                    Text("Pairing codes")
                } footer: {
                    if !model.savedPairings.isEmpty {
                        Text("Swipe to forget a code. Phomic asks again next time.")
                    }
                }

                if !model.savedPairings.isEmpty {
                    Section {
                        Button("Forget All Codes", role: .destructive) {
                            confirmForgetAll = true
                        }
                    }
                }

                Section {
                    NavigationLink("Acknowledgements") {
                        AcknowledgementsView()
                    }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .confirmationDialog("Forget all pairing codes?", isPresented: $confirmForgetAll, titleVisibility: .visible) {
                Button("Forget All", role: .destructive) {
                    model.forgetAllPairings()
                }
            }
        }
    }
}
