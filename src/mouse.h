// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "mapping.h"
#include "rudder.h"

// Where "Position" targets put the cursor: full output on either side
// reaches the edges.
enum class AbsoluteArea : int {
    PrimaryMonitor,
    AllMonitors,
    Count
};

constexpr int ABSOLUTE_AREA_COUNT = (int) AbsoluteArea::Count;
extern const char* const ABSOLUTE_AREA_NAMES[ABSOLUTE_AREA_COUNT];

// Clicking by holding the cursor still after moving it.
enum class DwellClick : int {
    Off,
    Left,
    Double,
    Right,
    Count
};

constexpr int DWELL_CLICK_COUNT = (int) DwellClick::Count;
extern const char* const DWELL_CLICK_NAMES[DWELL_CLICK_COUNT];

struct MouseSettings {
    AxisMapping axes[RUDDER_AXIS_COUNT];
    float smoothing = 0.3f;  // 0..1
    AbsoluteArea area = AbsoluteArea::PrimaryMonitor;
    bool smooth_scroll = false;  // send fractions of a wheel notch
    // Position targets: hold the cursor still after the target has stayed
    // within hold_radius for hold_time, until it moves beyond release_radius.
    bool hold = false;
    float hold_time = 0.3f;  // seconds
    int hold_radius = 12;    // pixels
    int release_radius = 40; // pixels
    // Position targets: keep the cursor still while any mouse button is
    // down (from any source), unless the position moves more than
    // drag_radius away, which lets it drag.
    bool freeze_on_click = true;
    int drag_radius = 60;  // pixels
    float freeze_linger = 0.4f;  // seconds frozen after the buttons are up, for double clicks
    DwellClick dwell = DwellClick::Off;
    float dwell_time = 0.8f;  // seconds
    int dwell_radius = 10;    // pixels the cursor can wander while dwelling
};

struct ScreenRect {
    int x = 0, y = 0, w = 1, h = 1;
};

// Where the engine's output goes. The real one uses SendInput; the tests
// record what it's asked to do.
class MouseSink {
public:
    virtual ~MouseSink() = default;
    virtual void move(int dx, int dy) = 0;
    virtual void move_to(int x, int y) = 0;  // virtual screen pixels
    virtual void wheel(int vertical, int horizontal) = 0;  // WHEEL_DELTA = 120 per notch
    virtual void button(MouseButton b, bool down) = 0;
    virtual void click(MouseButton b, int count) = 0;
    virtual bool cursor_pos(int& x, int& y) = 0;
    virtual ScreenRect area(AbsoluteArea a) = 0;
    // Whether any mouse button is down, whatever pressed it.
    virtual bool buttons_down() = 0;
};

class SendInputSink : public MouseSink {
public:
    void move(int dx, int dy) override;
    void move_to(int x, int y) override;
    void wheel(int vertical, int horizontal) override;
    void button(MouseButton b, bool down) override;
    void click(MouseButton b, int count) override;
    bool cursor_pos(int& x, int& y) override;
    ScreenRect area(AbsoluteArea a) override;
    bool buttons_down() override;
};

// What the UI shows.
struct MouseSnapshot {
    bool in_use = false;
    bool recentered = false;
    float relative[RUDDER_AXIS_COUNT] = {};  // to neutral, smoothed
    float levels[RUDDER_AXIS_COUNT] = {};
    float outputs[RUDDER_AXIS_COUNT] = {};
    float speed[2] = {};   // cursor, pixels per second
    float scroll[2] = {};  // vertical, horizontal, notches per second
    bool positioning = false;  // a Position target is in use
    int target[2] = {};        // where it puts the cursor
    bool holding = false;      // the position is held
    float hold_progress = -1.0f;  // 0..1 towards holding, -1 when not positioning
    bool frozen = false;          // kept still because a button is down
    uint32_t buttons = 0;      // held, mouse_button_mask()
    float dwell_progress = -1.0f;  // 0..1 while dwelling, -1 when not
    uint32_t dwell_clicks = 0;
};

// Turns 3dRudder frames into mouse input.
class MouseEngine {
public:
    explicit MouseEngine(MouseSink* sink);

    void set_settings(const MouseSettings& settings);
    void set_running(bool running);
    void recenter();
    void clear_neutral();
    std::shared_ptr<const MouseSnapshot> snapshot();

    // Reader thread: a frame, or nullptr when they stop. t in seconds.
    void process(const RudderFrame* frame, double t);

private:
    void release_all();
    void stop_motion();
    void reset_hold();
    void reset_freeze();
    void hold_position(const MouseSettings& s, const bool (&has_pos)[2], int (&target)[2], double t);
    void freeze_on_click(
        const MouseSettings& s, const bool (&has_pos)[2], const int (&cursor)[2], int (&target)[2], double t);
    void publish();

    MouseSink* sink_;

    std::mutex mutex_;
    MouseSettings settings_;
    bool running_ = false;
    bool recenter_requested_ = false;
    bool clear_neutral_requested_ = false;
    std::shared_ptr<const MouseSnapshot> snapshot_;

    // Processing thread only.
    MouseSnapshot current_;
    RudderNeutral neutral_;
    OneEuroFilter filters_[RUDDER_AXIS_COUNT];
    bool side_pressed_[RUDDER_AXIS_COUNT][2] = {};
    uint32_t held_ = 0;  // buttons we pressed through the sink
    float move_rem_[2] = {};
    float wheel_rem_[2] = {};  // in wheel units
    bool have_last_t_ = false;
    double last_t_ = 0.0;
    bool sent_target_ = false;
    int last_target_[2] = {};
    bool hold_anchored_ = false;
    bool holding_ = false;
    int hold_anchor_[2] = {};
    int hold_point_[2] = {};
    double hold_since_ = 0.0;
    bool frozen_ = false;
    bool dragging_ = false;  // unfrozen by a large movement until the buttons go up
    bool lingering_ = false;  // buttons up, still frozen until linger_until_
    double linger_until_ = 0.0;
    int freeze_point_[2] = {};
    bool dwell_anchored_ = false;
    bool dwell_armed_ = false;
    int dwell_anchor_[2] = {};
    double dwell_start_ = 0.0;
};
