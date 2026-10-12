#if canImport(UIKit)
import UIKit
import VCCore

/// The on-screen gamepad.
///
/// Touches go straight to the shared C layout (`include/layout.h`), the same
/// hit-testing, deadzones and floating stick as on Android, and the
/// resulting state is published on `link` from inside the touch handler.
///
/// **Display link at 120 Hz.** On ProMotion screens iOS lowers the refresh
/// rate when little is animating, and UIKit delivers touches once per
/// display frame, so an idle-looking gamepad would sample touches at 60 Hz
/// or less. A `CADisplayLink` asking for 120 Hz keeps the panel, and so
/// touch delivery, at 120 Hz while the pad is on screen. It only drives
/// redraws: input is published as each touch event arrives, never held for
/// the next tick, which would add up to a frame of latency. On iPhone the
/// app's Info.plist needs `CADisableMinimumFrameDurationOnPhone = YES`, or
/// iOS caps it at 60 Hz.
///
/// Labels, accent colours and which buttons exist follow `controllerType`'s
/// profile: the bottom face button reads A on Xbox, ✕ on PlayStation and B
/// on Switch. The host fixes the type at HELLO, so pair a change with a new
/// `ControllerLink`.
///
/// Main thread only, like every view.
public final class GamepadView: UIView {

    /// Where input goes. May be set or replaced at any time; nil drops input.
    public var link: ControllerLink?

    public var controllerType: ControllerType = .xbox {
        didSet { if oldValue != controllerType { rebuild() } }
    }

    public var hapticsEnabled = true

    /// Radial deadzone and saturation (fractions of the stick radius); side 0 is left.
    public func setStick(side: Int, deadzone: Float, saturation: Float) {
        let index = side == 0 ? 0 : 1
        stickSettings[index] = (deadzone, saturation)
        vc_layout_set_stick(layout, Int32(index), deadzone, saturation)
    }

    // The C layout lives on the heap so its address is stable: Swift may
    // hand C a temporary copy of a stored struct property.
    private let layout = UnsafeMutablePointer<vc_layout_t>.allocate(capacity: 1)
    private var builtSize = CGSize.zero
    private var stickSettings: [(Float, Float)] = [(0.12, 0.85), (0.10, 0.90)]
    private var pressedMask: UInt32 = 0
    private var needsRedraw = true
    private var displayLink: CADisplayLink?
    private let haptics = UIImpactFeedbackGenerator(style: .light)

    public override init(frame: CGRect) {
        super.init(frame: frame)
        configure()
    }

    public required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    deinit {
        displayLink?.invalidate()
        layout.deallocate()
    }

    private func configure() {
        layout.initialize(to: vc_layout_t())
        isMultipleTouchEnabled = true
        isOpaque = false
        backgroundColor = .clear
        contentMode = .redraw
        isAccessibilityElement = true
        accessibilityTraits = .allowsDirectInteraction   // VoiceOver passes touches through
    }

    // MARK: - Layout

    public override func layoutSubviews() {
        super.layoutSubviews()
        if bounds.size != builtSize {
            rebuild()
        }
    }

    private func rebuild() {
        let size = bounds.size
        guard size.height > 0 else { return }
        builtSize = size
        let type = vc_controller_type_t(rawValue: UInt32(controllerType.rawValue))
        vc_layout_init_default(layout, type, Float(size.width / size.height))
        for side in 0..<2 {
            vc_layout_set_stick(layout, Int32(side), stickSettings[side].0, stickSettings[side].1)
        }
        let names = (0..<Int(layout.pointee.count)).map {
            String(cString: vc_layout_name(layout, Int32($0)))
        }
        accessibilityLabel = "Gamepad: " + names.joined(separator: ", ")
        pressedMask = 0
        publish()
    }

    // MARK: - Display link

    public override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            startDisplayLink()
        } else {
            stopDisplayLink()
            vc_layout_reset(layout)   // release everything this view held
            publish()
        }
    }

    private func startDisplayLink() {
        guard displayLink == nil else { return }
        // CADisplayLink retains its target; a weak proxy avoids a cycle.
        let displayLink = CADisplayLink(target: DisplayLinkTarget(self),
                                        selector: #selector(DisplayLinkTarget.tick(_:)))
        displayLink.preferredFrameRateRange = CAFrameRateRange(minimum: 80, maximum: 120,
                                                               preferred: 120)
        displayLink.add(to: .main, forMode: .common)
        self.displayLink = displayLink
    }

    private func stopDisplayLink() {
        displayLink?.invalidate()
        displayLink = nil
    }

    fileprivate func displayTick() {
        if needsRedraw {
            needsRedraw = false
            setNeedsDisplay()
        }
    }

    // MARK: - Touch

    public override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if hapticsEnabled { haptics.prepare() }
        feed(touches, VC_TOUCH_DOWN)
    }

    public override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        feed(touches, VC_TOUCH_MOVE)
    }

    public override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        feed(touches, VC_TOUCH_UP)
    }

    public override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        feed(touches, VC_TOUCH_CANCEL)
    }

    private func feed(_ touches: Set<UITouch>, _ phase: vc_touch_phase_t) {
        let unit = bounds.height
        guard unit > 0 else { return }
        for touch in touches {
            // A UITouch object lives as long as the touch: its address is the id.
            let id = UInt64(UInt(bitPattern: Unmanaged.passUnretained(touch).toOpaque()))
            let point = touch.location(in: self)
            vc_layout_touch(layout, id, phase, Float(point.x / unit), Float(point.y / unit))
        }
        publish()
    }

    /// State to the sender now, then haptics for new presses, then a redraw
    /// on the next display frame.
    private func publish() {
        link?.update { state in vc_layout_state(layout, &state) }

        var mask: UInt32 = 0
        var visual = vc_control_visual_t()
        for i in 0..<Int(layout.pointee.count) {
            vc_layout_visual(layout, Int32(i), &visual)
            if visual.pressed != 0 { mask |= 1 << UInt32(i) }
        }
        if hapticsEnabled && (mask & ~pressedMask) != 0 {
            haptics.impactOccurred()
        }
        pressedMask = mask
        needsRedraw = true
    }

    // MARK: - Drawing

    public override func draw(_ rect: CGRect) {
        let unit = bounds.height
        let font = UIFont.systemFont(ofSize: unit * 0.045, weight: .bold)
        var visual = vc_control_visual_t()

        for i in 0..<Int(layout.pointee.count) {
            guard let control = vc_layout_control(layout, Int32(i))?.pointee else { continue }
            vc_layout_visual(layout, Int32(i), &visual)
            let pressed = visual.pressed != 0
            let center = CGPoint(x: CGFloat(visual.base_x) * unit, y: CGFloat(visual.base_y) * unit)
            let knob = CGPoint(x: CGFloat(visual.knob_x), y: CGFloat(visual.knob_y))
            let w = CGFloat(control.w) * unit
            let h = CGFloat(control.h) * unit

            switch control.kind {
            case VC_CONTROL_STICK:
                let r = w / 2
                let base = UIBezierPath(ovalIn: CGRect(x: center.x - r, y: center.y - r, width: w, height: w))
                Self.baseFill.setFill()
                base.fill()
                Self.outline.setStroke()
                base.lineWidth = 2
                base.stroke()
                let k = r * 0.42
                Self.knobFill.setFill()
                UIBezierPath(ovalIn: CGRect(x: center.x + knob.x * r - k, y: center.y + knob.y * r - k,
                                            width: 2 * k, height: 2 * k)).fill()

            case VC_CONTROL_DPAD:
                let r = w / 2
                let arm = r * 0.36
                Self.baseFill.setFill()
                UIBezierPath(roundedRect: CGRect(x: center.x - arm, y: center.y - r, width: 2 * arm, height: w),
                             cornerRadius: arm * 0.4).fill()
                UIBezierPath(roundedRect: CGRect(x: center.x - r, y: center.y - arm, width: w, height: 2 * arm),
                             cornerRadius: arm * 0.4).fill()
                if pressed {
                    Self.pressedFill.setFill()
                    UIBezierPath(ovalIn: CGRect(x: center.x + knob.x * r * 0.7 - arm,
                                                y: center.y + knob.y * r * 0.7 - arm,
                                                width: 2 * arm, height: 2 * arm)).fill()
                }

            default:   // buttons, triggers, touchpad
                let frame = CGRect(x: center.x - w / 2, y: center.y - h / 2, width: w, height: h)
                let path = control.round != 0 ? UIBezierPath(ovalIn: frame)
                                               : UIBezierPath(roundedRect: frame, cornerRadius: h * 0.3)
                (pressed ? Self.pressedFill : Self.baseFill).setFill()
                path.fill()
                let rgb = vc_layout_color(layout, Int32(i))
                let accent: UIColor? = rgb == 0 ? nil : UIColor(rgb: rgb)
                (accent ?? Self.outline).setStroke()
                path.lineWidth = 2
                path.stroke()

                let label = String(cString: vc_layout_label(layout, Int32(i)))
                if !label.isEmpty {
                    let attributes: [NSAttributedString.Key: Any] = [
                        .font: font,
                        .foregroundColor: accent ?? UIColor.white,
                    ]
                    let text = label as NSString
                    let size = text.size(withAttributes: attributes)
                    text.draw(at: CGPoint(x: center.x - size.width / 2, y: center.y - size.height / 2),
                              withAttributes: attributes)
                }
            }
        }
    }

    private static let baseFill = UIColor(white: 1, alpha: 0.2)
    private static let pressedFill = UIColor(white: 1, alpha: 0.6)
    private static let knobFill = UIColor(white: 1, alpha: 0.8)
    private static let outline = UIColor(white: 1, alpha: 0.55)
}

/// Weak target for the display link, which retains its target.
private final class DisplayLinkTarget: NSObject {
    weak var view: GamepadView?

    init(_ view: GamepadView) {
        self.view = view
    }

    @objc func tick(_ link: CADisplayLink) {
        view?.displayTick()
    }
}

private extension UIColor {
    convenience init(rgb: UInt32) {
        self.init(red: CGFloat((rgb >> 16) & 0xFF) / 255,
                  green: CGFloat((rgb >> 8) & 0xFF) / 255,
                  blue: CGFloat(rgb & 0xFF) / 255,
                  alpha: 1)
    }
}
#endif
