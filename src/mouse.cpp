// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "mouse.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

const char* const ABSOLUTE_AREA_NAMES[ABSOLUTE_AREA_COUNT] = {
    "Primary monitor",
    "All monitors",
};

const char* const DWELL_CLICK_NAMES[DWELL_CLICK_COUNT] = {
    "Off",
    "Left click",
    "Double click",
    "Right click",
};

// ---------------------------------------------------------------------------

static void send_mouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.mouseData = data;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static void button_flags(MouseButton b, DWORD& down, DWORD& up, DWORD& data) {
    data = 0;
    switch (b) {
        case MouseButton::Right:
            down = MOUSEEVENTF_RIGHTDOWN;
            up = MOUSEEVENTF_RIGHTUP;
            break;
        case MouseButton::Middle:
            down = MOUSEEVENTF_MIDDLEDOWN;
            up = MOUSEEVENTF_MIDDLEUP;
            break;
        case MouseButton::Back:
        case MouseButton::Forward:
            down = MOUSEEVENTF_XDOWN;
            up = MOUSEEVENTF_XUP;
            data = b == MouseButton::Back ? XBUTTON1 : XBUTTON2;
            break;
        default:
            down = MOUSEEVENTF_LEFTDOWN;
            up = MOUSEEVENTF_LEFTUP;
            break;
    }
}

void SendInputSink::move(int dx, int dy) {
    send_mouse(MOUSEEVENTF_MOVE, dx, dy);
}

void SendInputSink::move_to(int x, int y) {
    // Absolute coordinates are 0..65535 over the whole virtual screen.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 2);
    const int vh = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 2);
    send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, MulDiv(x - vx, 65535, vw - 1),
        MulDiv(y - vy, 65535, vh - 1));
}

void SendInputSink::wheel(int vertical, int horizontal) {
    if (vertical) {
        send_mouse(MOUSEEVENTF_WHEEL, 0, 0, (DWORD) vertical);
    }
    if (horizontal) {
        send_mouse(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD) horizontal);
    }
}

void SendInputSink::button(MouseButton b, bool down) {
    DWORD d, u, data;
    button_flags(b, d, u, data);
    send_mouse(down ? d : u, 0, 0, data);
}

void SendInputSink::click(MouseButton b, int count) {
    DWORD d, u, data;
    button_flags(b, d, u, data);
    INPUT in[4] = {};
    count = std::clamp(count, 1, 2);
    for (int i = 0; i < count * 2; i++) {
        in[i].type = INPUT_MOUSE;
        in[i].mi.mouseData = data;
        in[i].mi.dwFlags = (i % 2) ? u : d;
    }
    SendInput(count * 2, in, sizeof(INPUT));
}

bool SendInputSink::cursor_pos(int& x, int& y) {
    POINT p;
    if (!GetCursorPos(&p)) {
        return false;
    }
    x = p.x;
    y = p.y;
    return true;
}

bool SendInputSink::buttons_down() {
    // The async state covers the physical mouse, other programs' injected
    // input and our own. Checked once per 3dRudder frame, which is as often
    // as we move the cursor.
    for (int vk : { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 }) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            return true;
        }
    }
    return false;
}

ScreenRect SendInputSink::area(AbsoluteArea a) {
    ScreenRect r;
    if (a == AbsoluteArea::AllMonitors) {
        r.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        r.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        r.w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        r.x = mi.rcMonitor.left;
        r.y = mi.rcMonitor.top;
        r.w = mi.rcMonitor.right - mi.rcMonitor.left;
        r.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    r.w = std::max(r.w, 1);
    r.h = std::max(r.h, 1);
    return r;
}

// ---------------------------------------------------------------------------

MouseEngine::MouseEngine(MouseSink* sink) : sink_(sink) {
}

void MouseEngine::set_settings(const MouseSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
}

void MouseEngine::set_running(bool running) {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = running;
}

void MouseEngine::recenter() {
    std::lock_guard<std::mutex> lock(mutex_);
    recenter_requested_ = true;
}

void MouseEngine::clear_neutral() {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_neutral_requested_ = true;
}

std::shared_ptr<const MouseSnapshot> MouseEngine::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void MouseEngine::publish() {
    auto snap = std::make_shared<const MouseSnapshot>(current_);
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(snap);
}

void MouseEngine::release_all() {
    for (int b = 0; b < MOUSE_BUTTON_COUNT; b++) {
        if (held_ & mouse_button_mask((MouseButton) b)) {
            sink_->button((MouseButton) b, false);
        }
    }
    held_ = 0;
    current_.buttons = 0;
    memset(side_pressed_, 0, sizeof(side_pressed_));
}

void MouseEngine::stop_motion() {
    memset(move_rem_, 0, sizeof(move_rem_));
    memset(wheel_rem_, 0, sizeof(wheel_rem_));
    memset(current_.speed, 0, sizeof(current_.speed));
    memset(current_.scroll, 0, sizeof(current_.scroll));
    sent_target_ = false;
    dwell_anchored_ = dwell_armed_ = false;
    current_.dwell_progress = -1.0f;
    reset_hold();
    reset_freeze();
}

void MouseEngine::reset_hold() {
    hold_anchored_ = holding_ = false;
    current_.holding = false;
    current_.hold_progress = -1.0f;
}

void MouseEngine::reset_freeze() {
    frozen_ = dragging_ = lingering_ = false;
    current_.frozen = false;
}

// Freezing while clicking: while any mouse button is down, the positioned
// coordinates stay where the cursor was when it went down, so a click can't
// turn into a drag. It stays frozen for freeze_linger after the buttons are
// released, so the second click of a double click lands in the same place.
// If the position moves more than drag_radius away, it follows again: while
// a button is down that's a drag, which lasts until the buttons are released.
void MouseEngine::freeze_on_click(
    const MouseSettings& s, const bool (&has_pos)[2], const int (&cursor)[2], int (&target)[2], double t) {
    if (!s.freeze_on_click) {
        reset_freeze();
        return;
    }
    const bool down = sink_->buttons_down();
    if (down) {
        if (!frozen_ && !dragging_) {
            frozen_ = true;
            // Where we last put it (not re-sent), or where it is.
            for (int i = 0; i < 2; i++) {
                freeze_point_[i] = sent_target_ ? last_target_[i] : cursor[i];
            }
        }
        lingering_ = false;
    } else {
        dragging_ = false;
        if (frozen_ && !lingering_) {
            lingering_ = true;
            linger_until_ = t + s.freeze_linger;
        }
        if (frozen_ && t >= linger_until_) {
            frozen_ = lingering_ = false;
        }
    }
    if (frozen_) {
        int d2 = 0;
        for (int i = 0; i < 2; i++) {
            if (has_pos[i]) {
                d2 += (target[i] - freeze_point_[i]) * (target[i] - freeze_point_[i]);
            }
        }
        if (d2 > s.drag_radius * s.drag_radius) {
            frozen_ = lingering_ = false;
            dragging_ = down;
        } else {
            for (int i = 0; i < 2; i++) {
                if (has_pos[i]) {
                    target[i] = freeze_point_[i];
                }
            }
        }
    }
    current_.frozen = frozen_;
}

// Holding the position: once the target has stayed within hold_radius for
// hold_time, the positioned coordinates stay where they are until the target
// moves more than release_radius away. Small movements then don't move the
// cursor, so clicks don't turn into drags.
void MouseEngine::hold_position(const MouseSettings& s, const bool (&has_pos)[2], int (&target)[2], double t) {
    if (!s.hold) {
        reset_hold();
        return;
    }
    auto distance2 = [&](const int (&p)[2]) {
        int d2 = 0;
        for (int i = 0; i < 2; i++) {
            if (has_pos[i]) {
                d2 += (target[i] - p[i]) * (target[i] - p[i]);
            }
        }
        return d2;
    };
    if (holding_) {
        if (distance2(hold_point_) > s.release_radius * s.release_radius) {
            holding_ = false;
            hold_anchored_ = false;
        } else {
            for (int i = 0; i < 2; i++) {
                if (has_pos[i]) {
                    target[i] = hold_point_[i];
                }
            }
        }
    }
    if (!holding_) {
        if (!hold_anchored_ || distance2(hold_anchor_) > s.hold_radius * s.hold_radius) {
            hold_anchor_[0] = target[0];
            hold_anchor_[1] = target[1];
            hold_anchored_ = true;
            hold_since_ = t;
        } else if (t - hold_since_ >= s.hold_time) {
            // Where the cursor is now, so it doesn't jump.
            holding_ = true;
            for (int i = 0; i < 2; i++) {
                hold_point_[i] = sent_target_ ? last_target_[i] : target[i];
                if (has_pos[i]) {
                    target[i] = hold_point_[i];
                }
            }
        }
    }
    current_.holding = holding_;
    current_.hold_progress =
        holding_ ? 1.0f : std::clamp((float) ((t - hold_since_) / std::max(s.hold_time, 0.01f)), 0.0f, 1.0f);
}

// Rounds towards zero and keeps the rest for next time. The nudge keeps
// float error from turning 9.99999 into 9.
static int take_whole(float& acc, float unit = 1.0f) {
    const int n = (int) (acc / unit + (acc >= 0 ? 1e-4f : -1e-4f));
    acc -= n * unit;
    return n * (int) unit;
}

void MouseEngine::process(const RudderFrame* frame, double t) {
    MouseSettings s;
    bool running, recenter, clear;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = settings_;
        running = running_;
        recenter = recenter_requested_;
        clear = clear_neutral_requested_;
        recenter_requested_ = clear_neutral_requested_ = false;
    }

    auto idle = [&]() {
        stop_motion();
        release_all();
        for (auto& f : filters_) {
            f.reset();
        }
        memset(current_.levels, 0, sizeof(current_.levels));
        memset(current_.outputs, 0, sizeof(current_.outputs));
        publish();
    };

    if (!frame) {
        have_last_t_ = false;
        current_.in_use = false;
        memset(current_.relative, 0, sizeof(current_.relative));
        idle();
        return;
    }
    neutral_.update(*frame);
    if (recenter) {
        neutral_.recenter();
    }
    if (clear) {
        neutral_.clear();
    }
    current_.recentered = neutral_.recentered();
    // Not more than 0.1 s at a time, so a hiccup doesn't make the cursor jump.
    const float dt = have_last_t_ ? (float) std::clamp(t - last_t_, 0.0, 0.1) : 0.0f;
    have_last_t_ = true;
    last_t_ = t;

    current_.in_use = rudder_in_use(frame->status);
    if (!current_.in_use) {
        memset(current_.relative, 0, sizeof(current_.relative));
        idle();
        return;
    }

    float speed[2] = {};   // pixels per second, + = right / up
    float scroll[2] = {};  // notches per second, + = up / right
    float pos[2] = {};
    bool has_pos[2] = {};
    uint32_t want = 0;
    // Smoothing 0..1: still-cutoff from 10 Hz down to 0.2 Hz.
    const float min_cutoff = 10.0f * std::pow(0.02f, s.smoothing);
    for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
        const AxisMapping& m = s.axes[a];
        float v = neutral_.relative(*frame, a);
        if (s.smoothing > 0.0f) {
            // beta scaled so the axes react alike for the same share of their scale
            v = filters_[a].filter(v, t, min_cutoff, 2.0f / AXIS_LIMITS[a].display);
        } else {
            filters_[a].reset();
        }
        const float level = axis_level(m, v);
        const float out = axis_output(m, v);
        current_.relative[a] = v;
        current_.levels[a] = level;
        current_.outputs[a] = m.target == MouseTarget::None ? 0.0f : out;
        switch (m.target) {
            case MouseTarget::MoveX:
                speed[0] += out * m.move_speed;
                break;
            case MouseTarget::MoveY:
                speed[1] += out * m.move_speed;
                break;
            case MouseTarget::PositionX:
                pos[0] += out;
                has_pos[0] = true;
                break;
            case MouseTarget::PositionY:
                pos[1] += out;
                has_pos[1] = true;
                break;
            case MouseTarget::ScrollV:
                scroll[0] += out * m.scroll_speed;
                break;
            case MouseTarget::ScrollH:
                scroll[1] += out * m.scroll_speed;
                break;
            default:
                break;
        }
        side_pressed_[a][0] = button_pressed(side_pressed_[a][0], level, m.button_level);
        side_pressed_[a][1] = button_pressed(side_pressed_[a][1], -level, m.button_level);
        if (side_pressed_[a][0] && m.button_pos != NO_BUTTON) {
            want |= mouse_button_mask((MouseButton) m.button_pos);
        }
        if (side_pressed_[a][1] && m.button_neg != NO_BUTTON) {
            want |= mouse_button_mask((MouseButton) m.button_neg);
        }
    }
    memcpy(current_.speed, speed, sizeof(speed));
    memcpy(current_.scroll, scroll, sizeof(scroll));
    current_.positioning = has_pos[0] || has_pos[1];

    if (!running) {
        stop_motion();
        release_all();
        publish();
        return;
    }

    // Cursor. Screen y goes down, so + = up is subtracted.
    const float v[2] = { speed[0], -speed[1] };
    int d[2];
    for (int i = 0; i < 2; i++) {
        if (v[i] == 0.0f) {
            move_rem_[i] = 0.0f;  // start fresh next time
        }
        move_rem_[i] += v[i] * dt;
        d[i] = take_whole(move_rem_[i]);
    }
    if (current_.positioning) {
        const ScreenRect r = sink_->area(s.area);
        int cur[2] = { last_target_[0], last_target_[1] };
        sink_->cursor_pos(cur[0], cur[1]);
        int target[2];
        target[0] = has_pos[0] ? r.x + (int) std::lround((r.w - 1) * (0.5f + 0.5f * std::clamp(pos[0], -1.0f, 1.0f)))
                               : cur[0] + d[0];
        target[1] = has_pos[1] ? r.y + (int) std::lround((r.h - 1) * (0.5f - 0.5f * std::clamp(pos[1], -1.0f, 1.0f)))
                               : cur[1] + d[1];
        hold_position(s, has_pos, target, t);
        freeze_on_click(s, has_pos, cur, target, t);
        current_.target[0] = target[0];
        current_.target[1] = target[1];
        // Only when it changes, so the real mouse can still be used while
        // the feet are still.
        if (!sent_target_ || target[0] != last_target_[0] || target[1] != last_target_[1]) {
            sink_->move_to(target[0], target[1]);
            sent_target_ = true;
            last_target_[0] = target[0];
            last_target_[1] = target[1];
        }
    } else {
        sent_target_ = false;
        reset_hold();
        reset_freeze();
        if (d[0] || d[1]) {
            sink_->move(d[0], d[1]);
        }
    }

    // Wheel
    const float unit = s.smooth_scroll ? 1.0f : (float) WHEEL_DELTA;
    int w[2];
    for (int i = 0; i < 2; i++) {
        if (scroll[i] == 0.0f) {
            wheel_rem_[i] = 0.0f;
        }
        wheel_rem_[i] += scroll[i] * WHEEL_DELTA * dt;
        w[i] = take_whole(wheel_rem_[i], unit);
    }
    if (w[0] || w[1]) {
        sink_->wheel(w[0], w[1]);
    }

    // Buttons
    for (int b = 0; b < MOUSE_BUTTON_COUNT; b++) {
        const uint32_t mask = mouse_button_mask((MouseButton) b);
        if ((want & mask) != (held_ & mask)) {
            sink_->button((MouseButton) b, (want & mask) != 0);
            held_ ^= mask;
        }
    }
    current_.buttons = held_;

    // Dwell: after the cursor moves more than the radius, holding it within
    // the radius for the dwell time clicks once. Not while a button is held.
    int cx, cy;
    if (s.dwell == DwellClick::Off || held_ != 0 || !sink_->cursor_pos(cx, cy)) {
        dwell_anchored_ = dwell_armed_ = false;
        current_.dwell_progress = -1.0f;
    } else {
        if (!dwell_anchored_) {
            dwell_anchor_[0] = cx;
            dwell_anchor_[1] = cy;
            dwell_anchored_ = true;
            dwell_armed_ = false;
        }
        const int ddx = cx - dwell_anchor_[0], ddy = cy - dwell_anchor_[1];
        if (ddx * ddx + ddy * ddy > s.dwell_radius * s.dwell_radius) {
            dwell_anchor_[0] = cx;
            dwell_anchor_[1] = cy;
            dwell_armed_ = true;
            dwell_start_ = t;
        } else if (dwell_armed_ && t - dwell_start_ >= s.dwell_time) {
            switch (s.dwell) {
                case DwellClick::Double:
                    sink_->click(MouseButton::Left, 2);
                    break;
                case DwellClick::Right:
                    sink_->click(MouseButton::Right, 1);
                    break;
                default:
                    sink_->click(MouseButton::Left, 1);
                    break;
            }
            dwell_armed_ = false;
            current_.dwell_clicks++;
        }
        current_.dwell_progress =
            dwell_armed_ ? std::clamp((float) ((t - dwell_start_) / std::max(s.dwell_time, 0.05f)), 0.0f, 1.0f) : -1.0f;
    }

    publish();
}
