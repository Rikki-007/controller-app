package dev.vcd.controller

/** TCP and UDP port vcd listens on by default (VC_DEFAULT_PORT). */
const val DEFAULT_PORT = 28960

/**
 * Controller buttons, by position rather than label: [SOUTH] is A on Xbox,
 * Cross on PlayStation and B on Switch. Same bits as VC_BTN_* in
 * include/controller.h; the wire format fixes them.
 */
object Buttons {
    const val SOUTH = 1 shl 0
    const val EAST = 1 shl 1
    const val WEST = 1 shl 2
    const val NORTH = 1 shl 3
    const val L1 = 1 shl 4
    const val R1 = 1 shl 5
    const val L3 = 1 shl 6
    const val R3 = 1 shl 7
    const val START = 1 shl 8
    const val SELECT = 1 shl 9
    const val HOME = 1 shl 10
    const val MISC = 1 shl 11
    const val DPAD_UP = 1 shl 12
    const val DPAD_DOWN = 1 shl 13
    const val DPAD_LEFT = 1 shl 14
    const val DPAD_RIGHT = 1 shl 15
    const val TOUCHPAD = 1 shl 16
}

/** Optional data the phone can provide (VC_CAP_*). */
object Capabilities {
    const val MOTION = 0x01
    const val TOUCH = 0x02
}

/** Layouts the host can emulate; [wire] is vc_controller_type_t. */
enum class ControllerType(val wire: Int) {
    XBOX(1),
    PS5(2),
    SWITCH(3),
    PS4(4),
}
