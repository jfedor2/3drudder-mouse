// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include "rudder.h"

// The four axes the 3dRudder reports, in the order of RudderFrame::axes.
enum class RudderAxis : int {
    Rotation,
    ForwardBack,
    LeftRight,
    UpDown,
    Count
};

static_assert((int) RudderAxis::Count == RUDDER_AXIS_COUNT, "axis count");
extern const char* const RUDDER_AXIS_NAMES[RUDDER_AXIS_COUNT];         // "Rotation"
extern const char* const RUDDER_AXIS_KEYS[RUDDER_AXIS_COUNT];          // "rotation" (config)
extern const char* const RUDDER_AXIS_DESCRIPTIONS[RUDDER_AXIS_COUNT];  // "clockwise = +"

// Rotation and the tilts are in degrees, up / down is -1..1.
inline bool axis_in_degrees(RudderAxis a) {
    return a != RudderAxis::UpDown;
}

// Slider limits for each axis, in its units.
struct AxisLimits {
    float range_min, range_max;  // full output at
    float deadzone_max;
    float display;  // the bar shows +/- this much
};
extern const AxisLimits AXIS_LIMITS[RUDDER_AXIS_COUNT];

// What an axis drives. The + side (clockwise, toes down, tilt right, up)
// moves / positions the cursor right or up, and scrolls up or right.
enum class MouseTarget : int {
    None,
    MoveX,      // cursor speed
    MoveY,
    PositionX,  // cursor position in the absolute area
    PositionY,
    ScrollV,    // scrolling speed
    ScrollH,
    Count
};

constexpr int MOUSE_TARGET_COUNT = (int) MouseTarget::Count;
extern const char* const MOUSE_TARGET_NAMES[MOUSE_TARGET_COUNT];  // shown and saved

inline bool target_is_move(MouseTarget t) {
    return t == MouseTarget::MoveX || t == MouseTarget::MoveY;
}
inline bool target_is_position(MouseTarget t) {
    return t == MouseTarget::PositionX || t == MouseTarget::PositionY;
}
inline bool target_is_scroll(MouseTarget t) {
    return t == MouseTarget::ScrollV || t == MouseTarget::ScrollH;
}

enum class MouseButton : int {
    Left,
    Right,
    Middle,
    Back,
    Forward,
    Count
};

constexpr int MOUSE_BUTTON_COUNT = (int) MouseButton::Count;
extern const char* const MOUSE_BUTTON_NAMES[MOUSE_BUTTON_COUNT];
constexpr int NO_BUTTON = -1;

inline uint32_t mouse_button_mask(MouseButton b) {
    return 1u << (int) b;
}

struct AxisMapping {
    MouseTarget target = MouseTarget::None;
    bool invert = false;
    float range = 10.0f;    // from neutral for full output
    float deadzone = 1.0f;  // from neutral with no output
    float curve = 1.0f;     // response exponent, 1 = linear
    float move_speed = 800.0f;  // pixels per second at full output
    float scroll_speed = 10.0f;  // wheel notches per second at full output
    // Buttons held past a level (0..1 of the way from deadzone to full) on
    // either side, independent of the target.
    int button_pos = NO_BUTTON;
    int button_neg = NO_BUTTON;
    float button_level = 0.5f;
};

// Linear level (-1..1) for a value relative to neutral: 0 inside the
// deadzone, 1 at the range. Invert applied, curve not.
float axis_level(const AxisMapping& m, float value);

// The output (-1..1): the level through the response curve.
float axis_output(const AxisMapping& m, float value);

// Whether a level presses a button, with a little hysteresis so it doesn't
// chatter at the threshold.
bool button_pressed(bool was_pressed, float level, float threshold);

// One Euro filter (Casiez et al.): smooths jitter when still, with little lag
// when moving.
class OneEuroFilter {
public:
    void reset() { initialized_ = false; }
    // min_cutoff in Hz: lower = smoother when still. beta: how fast the
    // cutoff rises with speed.
    float filter(float value, double t_seconds, float min_cutoff, float beta);

private:
    bool initialized_ = false;
    float x_ = 0.0f;
    float dx_ = 0.0f;
    double t_ = 0.0;
};
