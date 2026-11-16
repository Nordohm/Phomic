import SwiftUI

/// Wi-Fi or Bluetooth, as an icon. SF Symbols has no Bluetooth logo, so that one is drawn.
public struct TransportIcon: View {
    let bluetooth: Bool
    let size: CGFloat

    public init(bluetooth: Bool, size: CGFloat) {
        self.bluetooth = bluetooth
        self.size = size
    }

    public var body: some View {
        if bluetooth {
            BluetoothRune()
                .stroke(style: StrokeStyle(lineWidth: size * 0.1, lineCap: .round, lineJoin: .round))
                .frame(width: size, height: size)
                .accessibilityLabel("Bluetooth")
        } else {
            Image(systemName: "wifi")
                .font(.system(size: size * 0.8, weight: .semibold))
                .frame(width: size, height: size)
                .accessibilityLabel("Wi-Fi")
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
