// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "mouse.h"

struct WindowRect {
    bool valid = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool maximized = false;

    bool operator==(const WindowRect& o) const {
        return valid == o.valid && x == o.x && y == o.y && w == o.w && h == o.h && maximized == o.maximized;
    }
};

// The global hotkey that starts and stops it. key is a Windows virtual-key
// code, 0 = no hotkey.
struct Hotkey {
    uint32_t key = 'M';
    bool ctrl = true;
    bool alt = true;
    bool shift = false;
    bool win = false;
};

struct Config {
    MouseSettings mouse;
    Hotkey hotkey;
    WindowRect window;

    Config();
};

// The defaults, also used by the reset buttons. Absolute = the same axes
// positioning the cursor instead of moving it.
void default_axes(AxisMapping (&axes)[RUDDER_AXIS_COUNT], bool absolute = false);

// %APPDATA%\3drudder-mouse\3drudder-mouse, created if needed.
std::wstring app_data_dir();

bool load_config(const std::wstring& path, Config& config);
bool save_config(const std::wstring& path, const Config& config);
