// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// Command line tool for checking the protocol decoding and the mouse engine.
// Nothing here moves the real mouse: the engine talks to a fake one.
//
//   rm-test             run the self-tests
//   rm-test dump        print what the connected 3dRudder would do
//   rm-test replay FILE the same for a recording (lines of "seconds hex hex ...")

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "mapping.h"
#include "mouse.h"
#include "rudder.h"

static int g_failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            g_failures++;                                              \
        }                                                              \
    } while (0)

static bool close_to(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) <= eps;
}

static std::vector<uint8_t> from_hex(const std::string& s) {
    std::vector<uint8_t> out;
    std::istringstream in(s);
    std::string byte;
    while (in >> byte) {
        out.push_back((uint8_t) strtoul(byte.c_str(), nullptr, 16));
    }
    return out;
}

// Keeps a cursor position and counts what it's asked to do.
class FakeSink : public MouseSink {
public:
    int x = 500, y = 400;
    int moves = 0, move_tos = 0, wheel_v = 0, wheel_h = 0, wheel_calls = 0;
    bool wheel_whole_notches = true;
    uint32_t held = 0;
    int clicks[MOUSE_BUTTON_COUNT] = {};
    ScreenRect rect{ 0, 0, 1001, 801 };

    void move(int dx, int dy) override {
        x += dx;
        y += dy;
        moves++;
    }
    void move_to(int nx, int ny) override {
        x = nx;
        y = ny;
        move_tos++;
    }
    void wheel(int v, int h) override {
        wheel_v += v;
        wheel_h += h;
        wheel_calls++;
        wheel_whole_notches &= v % WHEEL_DELTA == 0 && h % WHEEL_DELTA == 0;
    }
    void button(MouseButton b, bool down) override {
        if (down) {
            held |= mouse_button_mask(b);
        } else {
            held &= ~mouse_button_mask(b);
        }
    }
    void click(MouseButton b, int count) override { clicks[(int) b] += count; }
    bool cursor_pos(int& cx, int& cy) override {
        cx = x;
        cy = y;
        return true;
    }
    ScreenRect area(AbsoluteArea) override { return rect; }
    bool down = false;  // a mouse button is down, from wherever
    bool buttons_down() override { return down; }
};

static RudderFrame in_use_frame(float rotation = 0, float fb = 0, float lr = 0, float ud = 0) {
    RudderFrame f;
    f.status = 6;
    f.axes[0] = rotation;
    f.axes[1] = fb;
    f.axes[2] = lr;
    f.axes[3] = ud;
    return f;
}

// Simple settings: only rotation, linear, no deadzone, full at 10°.
static MouseSettings rotation_only(MouseTarget target) {
    MouseSettings s;
    for (AxisMapping& m : s.axes) {
        m = AxisMapping();
    }
    AxisMapping& m = s.axes[(int) RudderAxis::Rotation];
    m.target = target;
    m.range = 10.0f;
    m.deadzone = 0.0f;
    m.move_speed = 600.0f;
    m.scroll_speed = 10.0f;
    s.smoothing = 0.0f;
    s.hold = false;
    s.freeze_on_click = false;
    return s;
}

// Feeds the same frame n times at 60 Hz, starting at t.
static double feed(MouseEngine& e, const RudderFrame& f, int n, double t) {
    for (int i = 0; i < n; i++) {
        e.process(&f, t);
        t += 1.0 / 60;
    }
    return t;
}

static void run_tests() {
    // Recorded while rotating clockwise.
    const std::vector<uint8_t> report = from_hex(
        "01 00 f5 b6 06 00 a0 77 b5 41 1a 70 0d c0 59 07 2e c0 91 65 ac 3d b6 a8 24 43 5c 00 9f 04 ff 0a "
        "f3 07 bf 0c 6e 01 e8 f7 10 fd 50 c0 d4 ff dc ff e2 ff 7f 00 6d 00 7f 01 00 00 00 00 00 00 00 00");
    RudderFrame rf;
    CHECK(parse_rudder_frame(report.data(), report.size(), rf));
    CHECK(rf.status == 6);
    CHECK(close_to(rf.axes[(int) RudderAxis::Rotation], 22.68f));
    CHECK(close_to(rf.axes[(int) RudderAxis::ForwardBack], -2.21f));

    // Relative movement: full output = 600 px/s = 10 px per frame at 60 Hz.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        e.set_settings(rotation_only(MouseTarget::MoveX));
        e.set_running(true);
        double t = feed(e, in_use_frame(20), 61, 0.0);  // the first frame has no time step
        CHECK(sink.x == 500 + 600);
        CHECK(sink.y == 400);
        // Half way at 5°: 300 px/s.
        feed(e, in_use_frame(5), 60, t);
        CHECK(sink.x == 1100 + 300);
    }
    // Forward = up on screen.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        MouseSettings s = rotation_only(MouseTarget::None);
        s.axes[(int) RudderAxis::ForwardBack] = s.axes[(int) RudderAxis::Rotation];
        s.axes[(int) RudderAxis::ForwardBack].target = MouseTarget::MoveY;
        e.set_settings(s);
        e.set_running(true);
        feed(e, in_use_frame(0, 20), 61, 0.0);
        CHECK(sink.y == 400 - 600);
    }
    // Stopped, or not in use: nothing.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        e.set_settings(rotation_only(MouseTarget::MoveX));
        feed(e, in_use_frame(20), 30, 0.0);
        CHECK(sink.moves == 0);
        e.set_running(true);
        RudderFrame idle = in_use_frame(20);
        idle.status = 3;
        feed(e, idle, 30, 1.0);
        CHECK(sink.moves == 0);
    }
    // Absolute: neutral = middle of the area, full = its edge. Only sent on change.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        e.set_settings(rotation_only(MouseTarget::PositionX));
        e.set_running(true);
        feed(e, in_use_frame(0), 10, 0.0);
        CHECK(sink.x == 500 && sink.y == 400);
        CHECK(sink.move_tos == 1);
        // The real mouse moves it; with the feet still, it's left alone.
        sink.x = 123;
        feed(e, in_use_frame(0), 10, 1.0);
        CHECK(sink.x == 123);
        feed(e, in_use_frame(10), 1, 2.0);
        CHECK(sink.x == 1000);
        feed(e, in_use_frame(-30), 1, 3.0);
        CHECK(sink.x == 0);
        CHECK(sink.y == 400);  // Y is untouched
    }
    // Holding the position: 1° = 50 px here. Once still, small movements
    // don't move the cursor; a big one releases it.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        MouseSettings s = rotation_only(MouseTarget::PositionX);
        s.hold = true;
        s.hold_time = 0.3f;
        s.hold_radius = 12;
        s.release_radius = 40;
        e.set_settings(s);
        e.set_running(true);
        double t = feed(e, in_use_frame(0), 30, 0.0);
        CHECK(sink.x == 500);
        CHECK(e.snapshot()->holding);
        for (int i = 0; i < 30; i++) {
            t = feed(e, in_use_frame(i % 2 ? 0.2f : -0.2f), 1, t);  // +/- 10 px
        }
        CHECK(sink.x == 500);
        t = feed(e, in_use_frame(0.6f), 10, t);  // 30 px: still held
        CHECK(sink.x == 500);
        CHECK(e.snapshot()->holding);
        t = feed(e, in_use_frame(1.0f), 1, t);  // 50 px: released
        CHECK(sink.x == 550);
        CHECK(!e.snapshot()->holding);
        // Follows small movements until it's still again.
        t = feed(e, in_use_frame(1.2f), 1, t);
        CHECK(sink.x == 560);
        t = feed(e, in_use_frame(1.2f), 30, t);
        CHECK(e.snapshot()->holding);
        feed(e, in_use_frame(1.4f), 1, t);
        CHECK(sink.x == 560);

        // Without it, small movements go through.
        FakeSink sink2;
        MouseEngine e2(&sink2);
        e2.set_settings(rotation_only(MouseTarget::PositionX));
        e2.set_running(true);
        t = feed(e2, in_use_frame(0), 30, 0.0);
        feed(e2, in_use_frame(0.2f), 1, t);
        CHECK(sink2.x == 510);
    }
    // Freezing while a button is down: 1° = 50 px here. A large movement
    // unfreezes it for dragging until the buttons are up.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        MouseSettings s = rotation_only(MouseTarget::PositionX);
        s.freeze_on_click = true;
        s.drag_radius = 60;
        s.freeze_linger = 0.0f;
        e.set_settings(s);
        e.set_running(true);
        double t = feed(e, in_use_frame(0), 2, 0.0);
        CHECK(sink.x == 500);
        sink.down = true;
        t = feed(e, in_use_frame(0.4f), 2, t);  // 20 px
        CHECK(sink.x == 500);
        CHECK(e.snapshot()->frozen);
        t = feed(e, in_use_frame(1.0f), 2, t);  // 50 px
        CHECK(sink.x == 500);
        t = feed(e, in_use_frame(1.4f), 1, t);  // 70 px: drag
        CHECK(sink.x == 570);
        CHECK(!e.snapshot()->frozen);
        t = feed(e, in_use_frame(1.2f), 1, t);  // keeps following while down
        CHECK(sink.x == 560);
        sink.down = false;
        t = feed(e, in_use_frame(1.2f), 1, t);
        sink.down = true;
        t = feed(e, in_use_frame(1.6f), 1, t);  // a new click freezes again
        CHECK(sink.x == 560);
        CHECK(e.snapshot()->frozen);
        sink.down = false;
        feed(e, in_use_frame(1.6f), 1, t);  // released: follows
        CHECK(sink.x == 580);
        CHECK(!e.snapshot()->frozen);

        // Off: the cursor follows while the button is down.
        FakeSink sink2;
        MouseEngine e2(&sink2);
        e2.set_settings(rotation_only(MouseTarget::PositionX));
        e2.set_running(true);
        t = feed(e2, in_use_frame(0), 2, 0.0);
        sink2.down = true;
        feed(e2, in_use_frame(0.4f), 1, t);
        CHECK(sink2.x == 520);
    }
    // The freeze lingers after the release, so a double click lands in one
    // place. A large movement ends it early.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        MouseSettings s = rotation_only(MouseTarget::PositionX);
        s.freeze_on_click = true;
        s.drag_radius = 60;
        s.freeze_linger = 0.4f;
        e.set_settings(s);
        e.set_running(true);
        double t = feed(e, in_use_frame(0), 2, 0.0);
        sink.down = true;
        t = feed(e, in_use_frame(0.2f), 3, t);
        sink.down = false;
        t = feed(e, in_use_frame(0.4f), 6, t);  // 0.1 s after release, 20 px
        CHECK(sink.x == 500);
        CHECK(e.snapshot()->frozen);
        sink.down = true;  // second click
        t = feed(e, in_use_frame(0.4f), 3, t);
        CHECK(sink.x == 500);
        sink.down = false;
        t = feed(e, in_use_frame(0.4f), 12, t);  // 0.2 s
        CHECK(sink.x == 500);
        t = feed(e, in_use_frame(0.4f), 18, t);  // 0.5 s: over
        CHECK(sink.x == 520);
        CHECK(!e.snapshot()->frozen);

        sink.down = true;
        t = feed(e, in_use_frame(0.4f), 2, t);
        sink.down = false;
        t = feed(e, in_use_frame(1.8f), 1, t);  // 70 px from the freeze: follows right away
        CHECK(sink.x == 590);
        CHECK(!e.snapshot()->frozen);
    }
    // Scrolling: 10 notches/s, whole notches unless smooth.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        e.set_settings(rotation_only(MouseTarget::ScrollV));
        e.set_running(true);
        feed(e, in_use_frame(20), 61, 0.0);
        CHECK(sink.wheel_v == 10 * WHEEL_DELTA);
        CHECK(sink.wheel_whole_notches);
        CHECK(sink.wheel_calls == 10);

        FakeSink sink2;
        MouseEngine e2(&sink2);
        MouseSettings s = rotation_only(MouseTarget::ScrollV);
        s.smooth_scroll = true;
        e2.set_settings(s);
        e2.set_running(true);
        feed(e2, in_use_frame(-20), 61, 0.0);
        CHECK(sink2.wheel_v == -10 * WHEEL_DELTA);
        CHECK(sink2.wheel_calls == 60);
    }
    // Buttons are held past the level and released on the way back, when
    // stopped, and when the data stops.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        MouseSettings s = rotation_only(MouseTarget::None);
        s.axes[(int) RudderAxis::UpDown].button_pos = (int) MouseButton::Left;
        s.axes[(int) RudderAxis::UpDown].button_neg = (int) MouseButton::Right;
        s.axes[(int) RudderAxis::UpDown].deadzone = 0.1f;
        s.axes[(int) RudderAxis::UpDown].range = 0.5f;
        e.set_settings(s);
        e.set_running(true);
        feed(e, in_use_frame(0, 0, 0, 0.5f), 2, 0.0);
        CHECK(sink.held == mouse_button_mask(MouseButton::Left));
        feed(e, in_use_frame(0, 0, 0, 0.0f), 2, 1.0);
        CHECK(sink.held == 0);
        feed(e, in_use_frame(0, 0, 0, -0.5f), 2, 2.0);
        CHECK(sink.held == mouse_button_mask(MouseButton::Right));
        e.process(nullptr, 3.0);
        CHECK(sink.held == 0);
        feed(e, in_use_frame(0, 0, 0, -0.5f), 2, 4.0);
        CHECK(sink.held == mouse_button_mask(MouseButton::Right));
        e.set_running(false);
        feed(e, in_use_frame(0, 0, 0, -0.5f), 1, 5.0);
        CHECK(sink.held == 0);
    }
    // Dwell: clicks once after moving and holding still, then waits for
    // movement again.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        MouseSettings s = rotation_only(MouseTarget::MoveX);
        s.dwell = DwellClick::Left;
        s.dwell_time = 0.5f;
        s.dwell_radius = 5;
        e.set_settings(s);
        e.set_running(true);
        double t = feed(e, in_use_frame(0), 60, 0.0);
        CHECK(sink.clicks[(int) MouseButton::Left] == 0);  // never moved
        t = feed(e, in_use_frame(20), 10, t);
        t = feed(e, in_use_frame(0), 20, t);  // 0.33 s still
        CHECK(sink.clicks[(int) MouseButton::Left] == 0);
        t = feed(e, in_use_frame(0), 20, t);  // 0.67 s
        CHECK(sink.clicks[(int) MouseButton::Left] == 1);
        t = feed(e, in_use_frame(0), 120, t);
        CHECK(sink.clicks[(int) MouseButton::Left] == 1);
        t = feed(e, in_use_frame(20), 10, t);
        feed(e, in_use_frame(0), 60, t);
        CHECK(sink.clicks[(int) MouseButton::Left] == 2);
    }
    // Recentering.
    {
        FakeSink sink;
        MouseEngine e(&sink);
        e.set_settings(rotation_only(MouseTarget::MoveX));
        e.set_running(true);
        double t = feed(e, in_use_frame(20), 2, 0.0);
        const int x = sink.x;
        e.recenter();
        feed(e, in_use_frame(20), 30, t);
        CHECK(sink.x == x);
        CHECK(e.snapshot()->recentered);
    }
}

static void print_snapshot(const RudderFrame& f, const MouseSnapshot& m, const FakeSink& sink) {
    printf("st=%d rot=%7.2f fb=%7.2f lr=%7.2f ud=%5.2f  speed=(%+5.0f,%+5.0f) scroll=(%+5.1f,%+5.1f) "
           "cursor=(%d,%d) buttons=%x dwell clicks=%u\n",
        f.status, f.axes[0], f.axes[1], f.axes[2], f.axes[3], m.speed[0], m.speed[1], m.scroll[0], m.scroll[1],
        sink.x, sink.y, m.buttons, m.dwell_clicks);
}

static Config load_settings() {
    Config c;
    load_config(app_data_dir() + L"\\settings.json", c);
    return c;
}

static int dump() {
    FakeSink sink;
    MouseEngine engine(&sink);
    engine.set_settings(load_settings().mouse);
    engine.set_running(true);
    RudderReader reader;
    reader.start([&](const RudderFrame* f) { engine.process(f, GetTickCount64() / 1000.0); });
    size_t log_seen = 0;
    for (;;) {
        Sleep(200);
        auto st = reader.status();
        auto ms = engine.snapshot();
        if (!st) {
            continue;
        }
        if (st->log.size() < log_seen) {
            log_seen = 0;
        }
        for (size_t i = log_seen; i < st->log.size(); i++) {
            printf("  [device] %s\n", st->log[i].c_str());
        }
        log_seen = st->log.size();
        if (!st->connected) {
            printf("%s\n", st->error.c_str());
        } else if (!st->have_frame || !ms) {
            printf("%s: no data\n", st->device_name.c_str());
        } else {
            print_snapshot(st->frame, *ms, sink);
        }
        fflush(stdout);
    }
}

static int replay(const char* path) {
    std::ifstream in(path);
    if (!in) {
        printf("can't open %s\n", path);
        return 1;
    }
    FakeSink sink;
    MouseEngine engine(&sink);
    engine.set_settings(load_settings().mouse);
    engine.set_running(true);
    std::string line;
    double last_print = -1;
    while (std::getline(in, line)) {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            continue;
        }
        const double t = atof(line.substr(0, sp).c_str());
        const std::vector<uint8_t> r = from_hex(line.substr(sp + 1));
        RudderFrame f;
        std::string text;
        if (parse_rudder_frame(r.data(), r.size(), f)) {
            engine.process(&f, t);
            if (t - last_print >= 0.25) {
                printf("%7.2f ", t);
                print_snapshot(f, *engine.snapshot(), sink);
                last_print = t;
            }
        } else if (parse_rudder_text(r.data(), r.size(), text) && !text.empty()) {
            printf("%7.2f [device] %s\n", t, text.c_str());
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        return dump();
    }
    if (argc >= 3 && strcmp(argv[1], "replay") == 0) {
        return replay(argv[2]);
    }
    run_tests();
    printf(g_failures ? "%d check(s) failed\n" : "all tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
