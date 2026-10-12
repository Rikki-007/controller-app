import Foundation
import VCCore

/// One controller's link to vcd.
///
/// The protocol work (TCP handshake, keepalive, the UDP send loop, the wire
/// format) happens in the shared C engine, `src/vc_client.c`, the same code
/// the Android app runs and the desktop tests exercise. This class adds what
/// is specific to iOS:
///
/// - **The sender thread.** A dedicated `Thread` at `.userInteractive` QoS
///   runs `vc_client_run()`. It never touches the main thread, so a busy
///   render loop cannot delay input, and vice versa.
/// - **A non-blocking input API.** `update(_:)` and the helpers write the
///   state and hand it to the sender through a lock-free triple buffer. A
///   change is on the wire within `min_interval_us` (2 ms), and the current
///   state is re-sent at `rateHz` regardless.
/// - **Events on the main queue** (or `eventQueue`). Connected, feedback and closed.
///
/// Threading: call everything on the main thread. iOS suspends sockets in
/// the background, so call `stop()` when the app leaves the foreground and
/// `start()` again when it returns.
///
/// The app needs `NSLocalNetworkUsageDescription` in its Info.plist: talking
/// to a PC on the LAN triggers iOS's local network permission prompt.
public final class ControllerLink {

    public struct Configuration: Sendable {
        public var host: String
        public var port: UInt16
        public var type: ControllerType
        public var capabilities: Capabilities
        public var name: String
        public var rateHz: UInt32

        public init(host: String,
                    port: UInt16 = UInt16(VC_DEFAULT_PORT),
                    type: ControllerType,
                    capabilities: Capabilities = [],
                    name: String = "iPhone",
                    rateHz: UInt32 = 120) {
            self.host = host
            self.port = port
            self.type = type
            self.capabilities = capabilities
            self.name = name
            self.rateHz = rateHz
        }
    }

    public enum Event: Sendable {
        /// The host accepted the controller; input is flowing.
        case connected(slot: Int, capabilities: Capabilities, hostTimeoutMs: Int)
        /// Rumble and light bar state to render.
        case feedback(Feedback)
        /// The run has ended. Always the last event of a `start()`.
        case closed(Closure)
    }

    public struct Feedback: Sendable {
        public let rumbleLow: UInt8
        public let rumbleHigh: UInt8
        public let red: UInt8
        public let green: UInt8
        public let blue: UInt8
        public let player: UInt8
    }

    public struct Closure: Sendable {
        public let reason: Reason
        /// `vc_error_code_t` when the host refused or ended the session, else 0.
        public let hostError: UInt8
        public let message: String
    }

    /// Mirrors `vc_client_end_t`.
    public enum Reason: Sendable {
        case stopped, connectFailed, refused, hostError, hostBye
        case connectionLost, protocolError, timeout, system

        init(_ end: vc_client_end_t) {
            switch end {
            case VC_CLIENT_END_STOPPED:         self = .stopped
            case VC_CLIENT_END_CONNECT_FAILED:  self = .connectFailed
            case VC_CLIENT_END_REFUSED:         self = .refused
            case VC_CLIENT_END_HOST_ERROR:      self = .hostError
            case VC_CLIENT_END_HOST_BYE:        self = .hostBye
            case VC_CLIENT_END_CONNECTION_LOST: self = .connectionLost
            case VC_CLIENT_END_PROTOCOL:        self = .protocolError
            case VC_CLIENT_END_TIMEOUT:         self = .timeout
            default:                            self = .system
            }
        }
    }

    public enum LinkError: Error {
        /// `host` is not a dotted IPv4 address.
        case invalidHost(String)
    }

    public struct Statistics: Sendable {
        public let datagramsSent: UInt32
        public let sendErrors: UInt32
        /// Last control-channel round trip; 0 before the first.
        public let roundTripMicroseconds: UInt32
    }

    public let configuration: Configuration
    private let onEvent: (Event) -> Void
    private let eventQueue: DispatchQueue

    // Main thread only.
    private var client: OpaquePointer?          // vc_client_t *
    private var thread: Thread?
    private var state = vc_input_state_t()      // the input side's working copy

    /// A serial queue at `.userInteractive` QoS for events that should not
    /// wait behind UI work on the main thread, rumble in particular: pass it
    /// as `eventQueue` and drive Core Haptics from there.
    public static let feedbackQueue = DispatchQueue(label: "vcd.feedback", qos: .userInteractive)

    /// `onEvent` is called on `eventQueue` (main by default).
    ///
    /// The send loop itself does not run on a dispatch queue: it blocks for
    /// the whole session, and blocking GCD worker threads makes GCD spawn
    /// more of them. It gets its own thread at the same `.userInteractive`
    /// class instead (set here and again by the engine).
    public init(configuration: Configuration, eventQueue: DispatchQueue = .main,
                onEvent: @escaping (Event) -> Void) {
        self.configuration = configuration
        self.eventQueue = eventQueue
        self.onEvent = onEvent
    }

    public var isRunning: Bool { thread != nil }

    // MARK: - Lifecycle

    /// Connects and starts sending. Returns at once; `.connected` or
    /// `.closed` follows.
    public func start() throws {
        dispatchPrecondition(condition: .onQueue(.main))
        guard thread == nil else { return }

        var config = vc_client_config_t()
        config.port = configuration.port
        config.controller_type = configuration.type.rawValue
        config.caps = configuration.capabilities.rawValue
        config.rate_hz = configuration.rateHz
        // A C callback cannot capture context; `user` carries self. The
        // sender thread's closure below keeps self alive while it runs.
        config.on_event = { event, user in
            guard let event, let user else { return }
            Unmanaged<ControllerLink>.fromOpaque(user).takeUnretainedValue().forward(event.pointee)
        }
        config.user = Unmanaged.passUnretained(self).toOpaque()

        // The engine copies the strings, so they need only live for the call.
        let created: OpaquePointer? = configuration.host.withCString { host in
            configuration.name.withCString { name in
                config.host = host
                config.name = name
                return vc_client_create(&config)
            }
        }
        guard let client = created else {
            throw LinkError.invalidHost(configuration.host)
        }
        self.client = client
        vc_client_set_state(client, &state)   // whatever was set before start()

        let link = self
        let thread = Thread {
            _ = vc_client_run(client)          // blocks for the whole session
            DispatchQueue.main.async {
                link.finished(client)
            }
        }
        thread.name = "vcd.sender"
        thread.qualityOfService = .userInteractive
        self.thread = thread
        thread.start()
    }

    /// Sends BYE and ends the session; `.closed(.stopped)` follows.
    public func stop() {
        dispatchPrecondition(condition: .onQueue(.main))
        if let client { vc_client_stop(client) }
    }

    private func finished(_ finishedClient: OpaquePointer) {
        if client == finishedClient { client = nil }
        vc_client_destroy(finishedClient)
        thread = nil
    }

    // MARK: - Input (main thread)

    /// Edits the controller state and publishes it. Never blocks.
    public func update(_ body: (inout vc_input_state_t) -> Void) {
        dispatchPrecondition(condition: .onQueue(.main))
        body(&state)
        if let client { vc_client_set_state(client, &state) }
    }

    /// The common case: buttons, sticks in -1...1 (+y up), triggers in 0...1.
    public func set(buttons: Buttons,
                    leftStick: SIMD2<Float>, rightStick: SIMD2<Float>,
                    leftTrigger: Float, rightTrigger: Float) {
        update { s in
            s.buttons = buttons.rawValue
            s.left = vc_stick_t(x: Self.axis(leftStick.x), y: Self.axis(leftStick.y))
            s.right = vc_stick_t(x: Self.axis(rightStick.x), y: Self.axis(rightStick.y))
            s.trigger_left = Self.trigger(leftTrigger)
            s.trigger_right = Self.trigger(rightTrigger)
        }
    }

    /// Gyro in degrees per second and acceleration in g, controller axes
    /// (+x right, +y up, +z toward the player). Pass nil to stop sending motion.
    public func setMotion(gyro: SIMD3<Float>?, acceleration: SIMD3<Float>) {
        let flag = UInt8(truncatingIfNeeded: VC_INPUT_HAS_MOTION)
        update { s in
            guard let gyro else {
                s.flags &= ~flag
                return
            }
            s.flags |= flag
            s.motion.gyro = (Self.gyro(gyro.x), Self.gyro(gyro.y), Self.gyro(gyro.z))
            s.motion.accel = (Self.accel(acceleration.x), Self.accel(acceleration.y),
                              Self.accel(acceleration.z))
        }
    }

    public struct TouchPoint: Sendable {
        public var id: UInt8
        /// 0...1 across the pad, origin top-left.
        public var position: SIMD2<Float>
        public init(id: UInt8, position: SIMD2<Float>) {
            self.id = id
            self.position = position
        }
    }

    /// Up to two touchpad contacts; pass nil for a lifted finger.
    public func setTouch(_ first: TouchPoint?, _ second: TouchPoint?) {
        let flag = UInt8(truncatingIfNeeded: VC_INPUT_HAS_TOUCH)
        update { s in
            s.flags |= flag
            s.touch = (Self.touchPoint(first), Self.touchPoint(second))
        }
    }

    public var statistics: Statistics? {
        guard let client else { return nil }
        var stats = vc_client_stats_t()
        vc_client_get_stats(client, &stats)
        return Statistics(datagramsSent: stats.sent, sendErrors: stats.send_errors,
                          roundTripMicroseconds: stats.rtt_us)
    }

    // MARK: - Sender thread

    /// Called by the engine on the sender thread: copy, then hop to main.
    private func forward(_ e: vc_client_event_t) {
        let event: Event
        if e.type == VC_CLIENT_EVENT_CONNECTED {
            event = .connected(slot: Int(e.slot), capabilities: Capabilities(rawValue: e.caps),
                               hostTimeoutMs: Int(e.timeout_ms))
        } else if e.type == VC_CLIENT_EVENT_FEEDBACK {
            let f = e.feedback
            event = .feedback(Feedback(rumbleLow: f.rumble_low, rumbleHigh: f.rumble_high,
                                       red: f.led.0, green: f.led.1, blue: f.led.2,
                                       player: f.player))
        } else {
            let message = e.message.map { String(cString: $0) } ?? ""
            event = .closed(Closure(reason: Reason(e.end), hostError: e.error, message: message))
        }
        eventQueue.async { self.onEvent(event) }
    }

    // MARK: - Unit conversion (see controller.h)

    private static func axis(_ v: Float) -> Int16 {
        Int16((max(-1, min(1, v)) * 32767).rounded())
    }

    private static func trigger(_ v: Float) -> UInt8 {
        UInt8((max(0, min(1, v)) * 255).rounded())
    }

    /// 2000 deg/s full scale.
    private static func gyro(_ dps: Float) -> Int16 {
        Int16((max(-2000, min(2000, dps)) * (32767 / 2000)).rounded())
    }

    /// 4096 per g, so +-8 g.
    private static func accel(_ g: Float) -> Int16 {
        Int16((max(-7.99, min(7.99, g)) * 4096).rounded())
    }

    private static func touchPoint(_ p: TouchPoint?) -> vc_touch_point_t {
        guard let p else { return vc_touch_point_t() }
        func unit(_ v: Float) -> UInt16 { UInt16((max(0, min(1, v)) * 65535).rounded()) }
        return vc_touch_point_t(active: 1, id: p.id & 0x7F,
                                x: unit(p.position.x), y: unit(p.position.y))
    }
}
