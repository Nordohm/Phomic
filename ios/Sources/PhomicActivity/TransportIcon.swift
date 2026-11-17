import SwiftUI

/// How the iPhone reaches the PC. Shared by the app and the Live Activity.
public enum TransportKind: String, Codable, Hashable, Sendable {
    case wifi
    case bluetooth
    case usb
}

/// Wi-Fi, Bluetooth or USB, as an icon. SF Symbols has no Bluetooth logo, so that one is drawn.
public struct TransportIcon: View {
    let transport: TransportKind
    let size: CGFloat

    public init(transport: TransportKind, size: CGFloat) {
        self.transport = transport
        self.size = size
    }

    public var body: some View {
        switch transport {
        case .bluetooth:
            BluetoothRune()
                .stroke(style: StrokeStyle(lineWidth: size * 0.1, lineCap: .round, lineJoin: .round))
                .frame(width: size, height: size)
                .accessibilityLabel("Bluetooth")
        case .wifi:
            Image(systemName: "wifi")
                .font(.system(size: size * 0.8, weight: .semibold))
                .frame(width: size, height: size)
                .accessibilityLabel("Wi-Fi")
        case .usb:
            Image(systemName: "cable.connector")
                .font(.system(size: size * 0.75, weight: .semibold))
                .frame(width: size, height: size)
                .accessibilityLabel("USB")
        }
    }
}

/// The Bluetooth rune, on a 24-unit grid centered in the rect.
public struct BluetoothRune: Shape {
    public init() {}

    public func path(in rect: CGRect) -> Path {
        let unit = min(rect.width, rect.height) / 24
        func point(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
            CGPoint(x: rect.midX + (x - 12) * unit, y: rect.midY + (y - 12) * unit)
        }
        var path = Path()
        path.move(to: point(7, 7.5))
        path.addLine(to: point(16.5, 16.25))
        path.addLine(to: point(12, 20.5))
        path.addLine(to: point(12, 3.5))
        path.addLine(to: point(16.5, 7.75))
        path.addLine(to: point(7, 16.5))
        return path
    }
}
