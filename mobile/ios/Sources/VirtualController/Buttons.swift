import VCCore

/// Controller buttons, by position rather than label: `.south` is A on Xbox,
/// Cross on PlayStation and B on Switch.
///
/// Same bit values as `VC_BTN_*` in `include/controller.h`, which Swift
/// cannot import directly (they are built with `UINT32_C`). The wire format
/// fixes them, so they never change.
public struct Buttons: OptionSet, Hashable, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let south     = Buttons(rawValue: 1 << 0)
    public static let east      = Buttons(rawValue: 1 << 1)
    public static let west      = Buttons(rawValue: 1 << 2)
    public static let north     = Buttons(rawValue: 1 << 3)
    public static let l1        = Buttons(rawValue: 1 << 4)
    public static let r1        = Buttons(rawValue: 1 << 5)
    public static let l3        = Buttons(rawValue: 1 << 6)
    public static let r3        = Buttons(rawValue: 1 << 7)
    public static let start     = Buttons(rawValue: 1 << 8)
    public static let select    = Buttons(rawValue: 1 << 9)
    public static let home      = Buttons(rawValue: 1 << 10)
    public static let misc      = Buttons(rawValue: 1 << 11)
    public static let dpadUp    = Buttons(rawValue: 1 << 12)
    public static let dpadDown  = Buttons(rawValue: 1 << 13)
    public static let dpadLeft  = Buttons(rawValue: 1 << 14)
    public static let dpadRight = Buttons(rawValue: 1 << 15)
    public static let touchpad  = Buttons(rawValue: 1 << 16)
}

/// Controller layouts the host can emulate; raw values are the wire values
/// of `vc_controller_type_t`.
public enum ControllerType: UInt8, Sendable {
    case xbox = 1
    case ps5 = 2
    case nintendoSwitch = 3
    case ps4 = 4
}

/// Optional data the phone can provide; raw values are `VC_CAP_*`.
public struct Capabilities: OptionSet, Hashable, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }

    public static let motion = Capabilities(rawValue: UInt8(truncatingIfNeeded: VC_CAP_MOTION))
    public static let touch  = Capabilities(rawValue: UInt8(truncatingIfNeeded: VC_CAP_TOUCH))
}
