// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "config.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

void default_axes(AxisMapping (&axes)[RUDDER_AXIS_COUNT], bool absolute) {
    // Like the HID Remapper "3dRudder: mouse" example: rotation moves the
    // cursor left / right, tilting forward / back moves it up / down, with
    // forward / back inverted (toes down = lower on the screen).
    // Absolute: the same axes position it, linear with no deadzone.
    for (AxisMapping& m : axes) {
        m = AxisMapping();
    }
    AxisMapping& rot = axes[(int) RudderAxis::Rotation];
    rot.target = absolute ? MouseTarget::PositionX : MouseTarget::MoveX;
    rot.range = 15.0f;
    rot.deadzone = absolute ? 0.0f : 2.0f;
    rot.curve = absolute ? 1.0f : 1.5f;
    AxisMapping& fb = axes[(int) RudderAxis::ForwardBack];
    fb.target = absolute ? MouseTarget::PositionY : MouseTarget::MoveY;
    fb.invert = true;
    fb.range = 12.0f;
    fb.deadzone = absolute ? 0.0f : 1.5f;
    fb.curve = absolute ? 1.0f : 1.5f;
    axes[(int) RudderAxis::LeftRight].range = 10.0f;
    axes[(int) RudderAxis::UpDown].range = 0.8f;
    axes[(int) RudderAxis::UpDown].deadzone = 0.1f;
}

Config::Config() {
    default_axes(mouse.axes);
}

// The same layout SDL_GetPrefPath() gives the other HID Forwarder tools.
std::wstring app_data_dir() {
    static std::wstring dir = []() {
        PWSTR appdata = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appdata))) {
            CoTaskMemFree(appdata);
            return std::wstring(L".");
        }
        std::wstring path = appdata;
        CoTaskMemFree(appdata);
        for (const wchar_t* part : { L"\\3drudder-mouse", L"\\3drudder-mouse" }) {
            path += part;
            CreateDirectoryW(path.c_str(), nullptr);
        }
        return path;
    }();
    return dir;
}

template <typename E, int N>
static bool enum_from_name(const std::string& name, const char* const (&names)[N], E& out) {
    for (int i = 0; i < N; i++) {
        if (name == names[i]) {
            out = (E) i;
            return true;
        }
    }
    return false;
}

static int button_from_name(const std::string& name) {
    int b = NO_BUTTON;
    enum_from_name(name, MOUSE_BUTTON_NAMES, b);
    return b;
}

static std::string button_name(int b) {
    return b >= 0 && b < MOUSE_BUTTON_COUNT ? MOUSE_BUTTON_NAMES[b] : "";
}

bool load_config(const std::wstring& path, Config& config) {
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    try {
        json j = json::parse(f);
        MouseSettings& s = config.mouse;
        if (j.contains("axes") && j["axes"].is_object()) {
            for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
                const json& ja = j["axes"].value(RUDDER_AXIS_KEYS[a], json::object());
                const AxisLimits& lim = AXIS_LIMITS[a];
                AxisMapping& m = s.axes[a];
                enum_from_name(ja.value("output", ""), MOUSE_TARGET_NAMES, m.target);
                m.invert = ja.value("invert", m.invert);
                m.range = std::clamp(ja.value("range", m.range), lim.range_min, lim.range_max);
                m.deadzone = std::clamp(ja.value("deadzone", m.deadzone), 0.0f, lim.deadzone_max);
                m.curve = std::clamp(ja.value("curve", m.curve), 1.0f, 3.0f);
                m.move_speed = std::clamp(ja.value("move_speed", m.move_speed), 50.0f, 5000.0f);
                m.scroll_speed = std::clamp(ja.value("scroll_speed", m.scroll_speed), 1.0f, 50.0f);
                m.button_pos = button_from_name(ja.value("button_plus", ""));
                m.button_neg = button_from_name(ja.value("button_minus", ""));
                m.button_level = std::clamp(ja.value("button_level", m.button_level), 0.0f, 1.0f);
            }
        }
        s.smoothing = std::clamp(j.value("smoothing", s.smoothing), 0.0f, 1.0f);
        enum_from_name(j.value("absolute_area", ""), ABSOLUTE_AREA_NAMES, s.area);
        s.smooth_scroll = j.value("smooth_scroll", s.smooth_scroll);
        s.hold = j.value("hold", s.hold);
        s.hold_time = std::clamp(j.value("hold_time", s.hold_time), 0.05f, 2.0f);
        s.hold_radius = std::clamp(j.value("hold_radius", s.hold_radius), 1, 100);
        s.release_radius = std::clamp(j.value("release_radius", s.release_radius), s.hold_radius, 300);
        s.freeze_on_click = j.value("freeze_on_click", s.freeze_on_click);
        s.drag_radius = std::clamp(j.value("drag_radius", s.drag_radius), 5, 500);
        s.freeze_linger = std::clamp(j.value("freeze_linger", s.freeze_linger), 0.0f, 1.5f);
        enum_from_name(j.value("dwell", ""), DWELL_CLICK_NAMES, s.dwell);
        s.dwell_time = std::clamp(j.value("dwell_time", s.dwell_time), 0.2f, 3.0f);
        s.dwell_radius = std::clamp(j.value("dwell_radius", s.dwell_radius), 2, 50);
        if (j.contains("hotkey") && j["hotkey"].is_object()) {
            const json& jh = j["hotkey"];
            Hotkey& h = config.hotkey;
            h.key = std::min(jh.value("key", h.key), 0xffu);
            h.ctrl = jh.value("ctrl", h.ctrl);
            h.alt = jh.value("alt", h.alt);
            h.shift = jh.value("shift", h.shift);
            h.win = jh.value("win", h.win);
        }
        if (j.contains("window") && j["window"].is_object()) {
            const json& jw = j["window"];
            config.window.x = jw.value("x", 0);
            config.window.y = jw.value("y", 0);
            config.window.w = jw.value("w", 0);
            config.window.h = jw.value("h", 0);
            config.window.maximized = jw.value("maximized", false);
            config.window.valid = config.window.w > 0 && config.window.h > 0;
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool save_config(const std::wstring& path, const Config& config) {
    const MouseSettings& s = config.mouse;
    json j;
    json axes = json::object();
    for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
        const AxisMapping& m = s.axes[a];
        axes[RUDDER_AXIS_KEYS[a]] = {
            { "output", MOUSE_TARGET_NAMES[(int) m.target] },
            { "invert", m.invert },
            { "range", m.range },
            { "deadzone", m.deadzone },
            { "curve", m.curve },
            { "move_speed", m.move_speed },
            { "scroll_speed", m.scroll_speed },
            { "button_plus", button_name(m.button_pos) },
            { "button_minus", button_name(m.button_neg) },
            { "button_level", m.button_level },
        };
    }
    j["axes"] = axes;
    j["smoothing"] = s.smoothing;
    j["absolute_area"] = ABSOLUTE_AREA_NAMES[(int) s.area];
    j["smooth_scroll"] = s.smooth_scroll;
    j["hold"] = s.hold;
    j["hold_time"] = s.hold_time;
    j["hold_radius"] = s.hold_radius;
    j["release_radius"] = s.release_radius;
    j["freeze_on_click"] = s.freeze_on_click;
    j["drag_radius"] = s.drag_radius;
    j["freeze_linger"] = s.freeze_linger;
    j["dwell"] = DWELL_CLICK_NAMES[(int) s.dwell];
    j["dwell_time"] = s.dwell_time;
    j["dwell_radius"] = s.dwell_radius;
    const Hotkey& h = config.hotkey;
    j["hotkey"] = {
        { "key", h.key },
        { "ctrl", h.ctrl },
        { "alt", h.alt },
        { "shift", h.shift },
        { "win", h.win },
    };
    if (config.window.valid) {
        j["window"] = {
            { "x", config.window.x },
            { "y", config.window.y },
            { "w", config.window.w },
            { "h", config.window.h },
            { "maximized", config.window.maximized },
        };
    }

    std::wstring tmp_path = path + L".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f) {
            return false;
        }
        f << j.dump(2);
        if (!f) {
            return false;
        }
    }
    return MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
