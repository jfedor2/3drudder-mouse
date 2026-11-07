// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "mapping.h"

#include <algorithm>
#include <cmath>

const char* const RUDDER_AXIS_NAMES[RUDDER_AXIS_COUNT] = {
    "Rotation",
    "Forward / back",
    "Left / right",
    "Up / down",
};

const char* const RUDDER_AXIS_KEYS[RUDDER_AXIS_COUNT] = {
    "rotation",
    "forward_back",
    "left_right",
    "up_down",
};

const char* const RUDDER_AXIS_DESCRIPTIONS[RUDDER_AXIS_COUNT] = {
    "clockwise = +",
    "toes down = +",
    "tilt right = +",
    "",
};

const AxisLimits AXIS_LIMITS[RUDDER_AXIS_COUNT] = {
    { 2.0f, 30.0f, 10.0f, 30.0f },
    { 2.0f, 25.0f, 8.0f, 25.0f },
    { 2.0f, 20.0f, 8.0f, 20.0f },
    { 0.1f, 1.0f, 0.5f, 1.0f },
};

const char* const MOUSE_TARGET_NAMES[MOUSE_TARGET_COUNT] = {
    "None",
    "Move left / right",
    "Move up / down",
    "Position left / right",
    "Position up / down",
    "Scroll up / down",
    "Scroll left / right",
};

const char* const MOUSE_BUTTON_NAMES[MOUSE_BUTTON_COUNT] = {
    "Left",
    "Right",
    "Middle",
    "Back",
    "Forward",
};

float axis_level(const AxisMapping& m, float value) {
    if (m.invert) {
        value = -value;
    }
    const float mag = std::abs(value);
    if (mag <= m.deadzone) {
        return 0.0f;
    }
    const float span = std::max(m.range - m.deadzone, 1e-3f);
    const float out = std::min((mag - m.deadzone) / span, 1.0f);
    return value < 0 ? -out : out;
}

float axis_output(const AxisMapping& m, float value) {
    const float level = axis_level(m, value);
    if (m.curve == 1.0f || level == 0.0f) {
        return level;
    }
    const float out = std::pow(std::abs(level), m.curve);
    return level < 0 ? -out : out;
}

bool button_pressed(bool was_pressed, float level, float threshold) {
    static constexpr float HYSTERESIS = 0.05f;
    if (was_pressed) {
        return level > std::max(threshold - HYSTERESIS, 0.0f);
    }
    return level >= threshold && level > 0.0f;
}

static float smoothing_alpha(float cutoff, double dt) {
    const double tau = 1.0 / (2 * 3.14159265358979 * cutoff);
    return (float) (1.0 / (1.0 + tau / dt));
}

float OneEuroFilter::filter(float value, double t, float min_cutoff, float beta) {
    if (!initialized_ || t <= t_) {
        if (!initialized_) {
            x_ = value;
            dx_ = 0.0f;
        }
        initialized_ = true;
        t_ = t;
        return x_;
    }
    const double dt = std::min(t - t_, 0.5);
    t_ = t;
    const float dx = (value - x_) / (float) dt;
    dx_ += smoothing_alpha(1.0f, dt) * (dx - dx_);
    const float cutoff = min_cutoff + beta * std::abs(dx_);
    x_ += smoothing_alpha(cutoff, dt) * (value - x_);
    return x_;
}
