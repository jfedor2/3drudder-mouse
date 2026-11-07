// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "config.h"
#include "mapping.h"
#include "mouse.h"
#include "rudder.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_device_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target_view = nullptr;
static bool g_swap_chain_occluded = false;
static UINT g_resize_width = 0;
static UINT g_resize_height = 0;
static float g_pending_dpi_scale = 0.0f;

static const ImVec4 COLOR_ERROR(1.0f, 0.42f, 0.42f, 1.0f);
static const ImVec4 COLOR_OK(0.45f, 0.9f, 0.55f, 1.0f);
static const ImVec4 COLOR_WARN(1.0f, 0.8f, 0.35f, 1.0f);

static const ImU32 COLOR_MARKER = IM_COL32(255, 200, 80, 255);
static const ImU32 COLOR_FILL = IM_COL32(60, 140, 220, 200);
static const ImU32 COLOR_BUTTON_MARK = IM_COL32(240, 120, 60, 255);

// The global hotkey (Ctrl+Alt+M unless changed) turns it on and off from anywhere.
static constexpr int HOTKEY_ID = 1;

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Application state, owned by the UI thread.
struct App {
    Config config;
    std::wstring config_path;
    bool running = false;
    bool hotkey_ok = false;
    bool capturing_hotkey = false;  // the next key combination becomes the hotkey
    std::string hotkey_hint;        // shown while capturing

    bool config_dirty = false;
    ULONGLONG config_dirty_since = 0;
    HWND main_hwnd = nullptr;

    SendInputSink sink;
    MouseEngine engine{ &sink };
    RudderReader reader;
    std::shared_ptr<const RudderStatus> status;
    std::shared_ptr<const MouseSnapshot> mouse;

    void mark_dirty() {
        if (!config_dirty) {
            config_dirty_since = GetTickCount64();
        }
        config_dirty = true;
    }

    // Call after any change to the mouse settings.
    void settings_changed() {
        engine.set_settings(config.mouse);
        mark_dirty();
    }

    bool in_use() const {
        return status && status->have_frame && mouse && mouse->in_use;
    }
};

static App g_app;

// ---------------------------------------------------------------------------
// D3D11 boilerplate (from the Dear ImGui example_win32_directx11)

static void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer != nullptr) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
        back_buffer->Release();
    }
}

static void cleanup_render_target() {
    if (g_render_target_view != nullptr) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
    }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    }
    if (FAILED(hr)) {
        return false;
    }
    create_render_target();
    return true;
}

static void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context) {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------

static void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

static bool g_track_placement = false;  // set once the saved placement has been applied

static void capture_window_placement() {
    if (!g_track_placement) {
        return;
    }
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(g_app.main_hwnd, &wp)) {
        return;
    }
    WindowRect w;
    w.valid = true;
    w.x = wp.rcNormalPosition.left;
    w.y = wp.rcNormalPosition.top;
    w.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    w.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    w.maximized = wp.showCmd == SW_SHOWMAXIMIZED ||
                  (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    if (!(w == g_app.config.window)) {
        g_app.config.window = w;
        g_app.mark_dirty();
    }
}

static void save_now() {
    save_config(g_app.config_path, g_app.config);
    g_app.config_dirty = false;
}

// ---------------------------------------------------------------------------
// Hotkey

static std::string key_name(UINT vk) {
    switch (vk) {
        case VK_PAUSE:
            return "Pause";
        case VK_SNAPSHOT:
            return "Print Screen";
    }
    LONG lparam = (LONG) (MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
    switch (vk) {
        // These share scan codes with the numeric keypad, the extended bit
        // tells them apart.
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            lparam |= 1 << 24;
            break;
    }
    wchar_t buf[64] = {};
    char out[64];
    if (lparam != 0 && GetKeyNameTextW(lparam, buf, 64) > 0) {
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
        return out;
    }
    snprintf(out, sizeof(out), "Key %02X", vk);
    return out;
}

static std::string hotkey_name(const Hotkey& h) {
    if (h.key == 0) {
        return "None";
    }
    std::string s;
    s += h.ctrl ? "Ctrl+" : "";
    s += h.alt ? "Alt+" : "";
    s += h.shift ? "Shift+" : "";
    s += h.win ? "Win+" : "";
    return s + key_name(h.key);
}

static void register_hotkey() {
    App& app = g_app;
    UnregisterHotKey(app.main_hwnd, HOTKEY_ID);
    const Hotkey& h = app.config.hotkey;
    app.hotkey_ok = false;
    if (h.key != 0 && !app.capturing_hotkey) {
        const UINT mods = MOD_NOREPEAT | (h.ctrl ? MOD_CONTROL : 0) | (h.alt ? MOD_ALT : 0) |
                          (h.shift ? MOD_SHIFT : 0) | (h.win ? MOD_WIN : 0);
        app.hotkey_ok = RegisterHotKey(app.main_hwnd, HOTKEY_ID, mods, h.key) != 0;
    }
}

static void start_hotkey_capture() {
    g_app.capturing_hotkey = true;
    g_app.hotkey_hint = "Press the new key combination, Esc to cancel";
    register_hotkey();  // off while capturing, so pressing the old one is seen
}

static void end_hotkey_capture() {
    g_app.capturing_hotkey = false;
    register_hotkey();
}

// A key went down while capturing.
static void capture_hotkey_key(UINT vk) {
    App& app = g_app;
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_LWIN: case VK_RWIN:
            return;  // wait for the key that goes with them
    }
    Hotkey h;
    h.key = vk;
    h.ctrl = GetKeyState(VK_CONTROL) < 0;
    h.alt = GetKeyState(VK_MENU) < 0;
    h.shift = GetKeyState(VK_SHIFT) < 0;
    h.win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    const bool modified = h.ctrl || h.alt || h.shift || h.win;
    if (vk == VK_ESCAPE && !modified) {
        end_hotkey_capture();
        return;
    }
    // A plain key would stop working everywhere else, so only keys that
    // aren't used for much are allowed without a modifier.
    const bool standalone = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL;
    if (!modified && !standalone) {
        app.hotkey_hint = "Use Ctrl, Alt, Shift or Win with that key (F-keys, Pause and Scroll Lock work alone)";
        return;
    }
    app.config.hotkey = h;
    app.mark_dirty();
    end_hotkey_capture();
}

// ---------------------------------------------------------------------------
// Window icon: a circle showing the state. The left half is the 3dRudder:
// red = not found, yellow = connected but not in use (feet not on it),
// green = in use. The right half is red when stopped, green when running.
// No 3dRudder at all makes the whole circle red.

constexpr uint32_t ICON_RED = 0xd94040, ICON_YELLOW = 0xe0b830, ICON_GREEN = 0x3cb85a;

static HICON make_status_icon(uint32_t left_rgb, uint32_t right_rgb, int size) {
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = size;
    bh.bV5Height = -size;  // top-down
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00ff0000;
    bh.bV5GreenMask = 0x0000ff00;
    bh.bV5BlueMask = 0x000000ff;
    bh.bV5AlphaMask = 0xff000000;
    uint32_t* pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*) &bh, DIB_RGB_COLORS, (void**) &pixels, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) {
        return nullptr;
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

    const float c = size / 2.0f, r = size / 2.0f - std::max(size / 32.0f, 0.5f);
    constexpr int SS = 4;  // supersampling, for smooth edges
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int inside = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    const float dx = x + (sx + 0.5f) / SS - c, dy = y + (sy + 0.5f) / SS - c;
                    inside += dx * dx + dy * dy <= r * r;
                }
            }
            const uint32_t rgb = x < size / 2 ? left_rgb : right_rgb;
            const uint32_t alpha = inside * 255 / (SS * SS);
            pixels[y * size + x] = (alpha << 24) | rgb;
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// Sets the window's icons if the state (or the DPI) changed.
static void update_icon(HWND hwnd) {
    static uint32_t current_left = 0, current_right = 0;
    static UINT current_dpi = 0;
    static HICON big_icon = nullptr, small_icon = nullptr;

    const RudderStatus* st = g_app.status.get();
    uint32_t left = ICON_RED, right = ICON_RED;
    if (st && st->connected) {
        left = st->have_frame && rudder_in_use(st->frame.status) ? ICON_GREEN : ICON_YELLOW;
        right = g_app.running ? ICON_GREEN : ICON_RED;
    }
    const UINT dpi = GetDpiForWindow(hwnd);
    if (left == current_left && right == current_right && dpi == current_dpi) {
        return;
    }
    current_left = left;
    current_right = right;
    current_dpi = dpi;
    HICON new_big = make_status_icon(left, right, GetSystemMetricsForDpi(SM_CXICON, dpi));
    HICON new_small = make_status_icon(left, right, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM) new_big);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM) new_small);
    if (big_icon) {
        DestroyIcon(big_icon);
    }
    if (small_icon) {
        DestroyIcon(small_icon);
    }
    big_icon = new_big;
    small_icon = new_small;
}

static void push_button_colors(ImVec4 base) {
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x * 1.2f, base.y * 1.2f, base.z * 1.2f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 1.4f, base.y * 1.4f, base.z * 1.4f, 1));
}

// "12.0°" or "0.50", depending on the axis.
static void format_value(int a, float v, char* buf, size_t size) {
    if (axis_in_degrees((RudderAxis) a)) {
        snprintf(buf, size, "%+.1f°", v);
    } else {
        snprintf(buf, size, "%+.2f", v);
    }
}

// ---------------------------------------------------------------------------
// Device view

// Top-down view: the feet turned by the rotation, the tilt as a marker.
static void draw_rudder_view(float size) {
    const RudderStatus* st = g_app.status.get();
    const MouseSnapshot* ms = g_app.mouse.get();
    const bool in_use = g_app.in_use();
    const float em = ImGui::GetFontSize();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size / 2;
    const ImVec2 c(p0.x + r, p0.y + r);

    dl->AddCircleFilled(c, r, IM_COL32(32, 34, 40, 255), 64);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 60), 64, 1.5f);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(255, 255, 255, 25));
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(255, 255, 255, 25));

    const float rotation = in_use ? ms->relative[(int) RudderAxis::Rotation] : 0.0f;
    const float tilt_x = in_use ? ms->relative[(int) RudderAxis::LeftRight] : 0.0f;
    const float tilt_y = in_use ? ms->relative[(int) RudderAxis::ForwardBack] : 0.0f;

    // Feet, rotated (clockwise = +, screen y is down).
    const float th = rotation * 3.14159265f / 180.0f;
    const float cs = std::cos(th), sn = std::sin(th);
    auto rot = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    // Opaque, so the toes and the soles don't add up where they overlap.
    const ImU32 foot_col = in_use ? IM_COL32(97, 102, 117, 255) : IM_COL32(56, 58, 66, 255);
    for (float side : { -1.0f, 1.0f }) {
        const float fx = side * r * 0.32f, fw = r * 0.16f, fh = r * 0.58f;
        const ImVec2 q[4] = { rot(fx - fw, -fh), rot(fx + fw, -fh), rot(fx + fw, fh), rot(fx - fw, fh) };
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], foot_col);
        // toes
        dl->AddCircleFilled(rot(fx, -fh), fw, foot_col, 16);
    }
    // Which way is forward.
    dl->AddTriangleFilled(rot(0, -r * 0.95f), rot(-em * 0.4f, -r * 0.95f + em * 0.6f),
        rot(em * 0.4f, -r * 0.95f + em * 0.6f), in_use ? COLOR_MARKER : IM_COL32(110, 110, 120, 120));

    if (in_use) {
        // Tilt, with the bar scales at the edge.
        const float sx = AXIS_LIMITS[(int) RudderAxis::LeftRight].display;
        const float sy = AXIS_LIMITS[(int) RudderAxis::ForwardBack].display;
        const ImVec2 dot(c.x + std::clamp(tilt_x / sx, -1.0f, 1.0f) * (r - em * 0.4f),
            c.y - std::clamp(tilt_y / sy, -1.0f, 1.0f) * (r - em * 0.4f));
        dl->AddLine(c, dot, IM_COL32(255, 200, 80, 120), 2.0f);
        dl->AddCircleFilled(dot, em * 0.35f, COLOR_MARKER);
    } else {
        const char* text = !st || !st->connected ? "Not connected"
                           : !st->have_frame     ? "No data"
                                                 : rudder_status_name(st->frame.status);
        const float wrap = size - em * 2;
        const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, wrap);
        const ImVec2 tp(c.x - ts.x / 2, c.y - ts.y / 2);
        dl->AddRectFilled(ImVec2(tp.x - em * 0.4f, tp.y - em * 0.2f), ImVec2(tp.x + ts.x + em * 0.4f, tp.y + ts.y + em * 0.2f),
            IM_COL32(0, 0, 0, 170), 4.0f);
        dl->AddText(nullptr, 0.0f, tp, ImGui::GetColorU32(COLOR_WARN), text, nullptr, wrap);
    }
}

// A vertical -1..1 bar for up / down.
static void draw_updown_bar(float height) {
    const MouseSnapshot* ms = g_app.mouse.get();
    const bool in_use = g_app.in_use();
    const float em = ImGui::GetFontSize();
    const float w = em * 1.2f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + height);
    ImGui::Dummy(ImVec2(w, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), 3.0f);
    const float cy = (p0.y + p1.y) / 2;
    dl->AddLine(ImVec2(p0.x, cy), ImVec2(p1.x, cy), IM_COL32(255, 255, 255, 50));
    if (in_use) {
        const float v = std::clamp(ms->relative[(int) RudderAxis::UpDown], -1.0f, 1.0f);
        const float y = cy - v * height / 2;
        dl->AddRectFilled(ImVec2(p0.x + 2, std::min(y, cy)), ImVec2(p1.x - 2, std::max(y, cy)), COLOR_FILL, 2.0f);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), COLOR_MARKER, 2.0f);
    }
    ImGui::SetItemTooltip("Up / down");
}

static void draw_sensors() {
    const RudderStatus* st = g_app.status.get();
    const bool have = st && st->have_frame;
    const float em = ImGui::GetFontSize();
    const float h = em * 3.0f, w = em * 1.4f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < RUDDER_SENSOR_COUNT; i++) {
        if (i > 0) {
            ImGui::SameLine(0, em * 0.4f);
        }
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + w, p0.y + h);
        ImGui::Dummy(ImVec2(w, h));
        dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), 3.0f);
        const int v = have ? st->frame.sensors[i] : 0;
        const float f = std::clamp(v / 8000.0f, 0.0f, 1.0f);
        if (f > 0) {
            dl->AddRectFilled(ImVec2(p0.x + 2, p1.y - 2 - f * (h - 4)), ImVec2(p1.x - 2, p1.y - 2), COLOR_FILL, 2.0f);
        }
        ImGui::SetItemTooltip("Force sensor %d: %d", i + 1, v);
    }
}

static void draw_device_panel() {
    App& app = g_app;
    const RudderStatus* st = app.status.get();
    const MouseSnapshot* ms = app.mouse.get();
    const float em = ImGui::GetFontSize();
    const bool in_use = app.in_use();

    const float bar_w = em * 1.2f + ImGui::GetStyle().ItemSpacing.x;
    const float size = std::clamp(ImGui::GetContentRegionAvail().x - bar_w, em * 10, em * 18);
    draw_rudder_view(size);
    ImGui::SameLine();
    draw_updown_bar(size);

    ImGui::Spacing();
    ImGui::BeginDisabled(!in_use);
    push_button_colors(ImVec4(0.18f, 0.36f, 0.58f, 1));
    if (ImGui::Button("Recenter", ImVec2(em * 7, ImGui::GetFrameHeight() * 1.5f))) {
        app.engine.recenter();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Take the current position as neutral (no output).\n"
                          "The 3dRudder zeroes itself when you put your feet on it,\n"
                          "this is for when it drifts or you want a different neutral.");
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ms && ms->recentered) {
        ImGui::TextUnformatted("Neutral: recentered.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            app.engine.clear_neutral();
        }
        ImGui::SetItemTooltip("Go back to the neutral the 3dRudder set when your feet went on.");
    } else {
        ImGui::TextUnformatted("Neutral: where your feet were when it started.");
    }
    ImGui::TextDisabled(in_use ? "Hold your feet where they should give no output and click."
                               : "Can be set while the 3dRudder is in use.");
    ImGui::EndGroup();

    ImGui::SeparatorText("Sensors");
    draw_sensors();
    // Right-aligned columns, the font isn't monospaced.
    const bool have = st && st->have_frame;
    const float col_w = ImGui::CalcTextSize("-00000").x + em * 0.6f;
    const float x0 = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Accelerometer").x + em * 0.5f;
    auto imu_row = [&](const char* label, const int16_t (&v)[3]) {
        ImGui::TextDisabled("%s", label);
        for (int i = 0; i < 3; i++) {
            char num[16];
            snprintf(num, sizeof(num), have ? "%d" : "--", v[i]);
            ImGui::SameLine(x0 + col_w * (i + 1) - ImGui::CalcTextSize(num).x);
            ImGui::TextDisabled("%s", num);
        }
    };
    const RudderFrame none;
    imu_row("Accelerometer", have ? st->frame.accel : none.accel);
    imu_row("Gyro", have ? st->frame.gyro : none.gyro);

    ImGui::SeparatorText("Device messages");
    if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_FrameStyle)) {
        if (st) {
            for (const std::string& line : st->log) {
                ImGui::TextUnformatted(line.c_str());
            }
            // Follow new lines unless scrolled up.
            static size_t last_count = 0;
            if (st->log.size() != last_count && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em) {
                ImGui::SetScrollHereY(1.0f);
            }
            last_count = st->log.size();
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Mapping

// Where a button level (0..1) is, in axis units.
static float button_point(const AxisMapping& m, float level) {
    return m.deadzone + level * (m.range - m.deadzone);
}

// The axis value on a scale, with the deadzone shaded, the full output points
// marked, button thresholds ticked and the output filled in from the center.
static void draw_axis_bar(int a, const AxisMapping& m, float value, bool valid, float width) {
    const float em = ImGui::GetFontSize();
    const float h = em * 1.25f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + h);
    ImGui::Dummy(ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scale = std::max(AXIS_LIMITS[a].display, m.range * 1.15f);
    const float cx = (p0.x + p1.x) / 2;
    // Invert flips which side is which, so the bar follows the output.
    const float dir = m.invert ? -1.0f : 1.0f;
    auto x_of = [&](float v) { return cx + std::clamp(v / scale, -1.0f, 1.0f) * width / 2; };
    const bool buttons = m.button_pos != NO_BUTTON || m.button_neg != NO_BUTTON;
    const bool used = m.target != MouseTarget::None || buttons;

    dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), 3.0f);
    if (used) {
        const float level = valid ? axis_level(m, value) : 0.0f;
        if (level != 0.0f) {
            const float edge = button_point(m, std::abs(level));
            const float sign = level < 0 ? -1.0f : 1.0f;
            const float x0 = x_of(sign * m.deadzone), x1 = x_of(sign * edge);
            dl->AddRectFilled(ImVec2(std::min(x0, x1), p0.y + 2), ImVec2(std::max(x0, x1), p1.y - 2), COLOR_FILL, 2.0f);
        }
        dl->AddRectFilled(ImVec2(x_of(-m.deadzone), p0.y), ImVec2(x_of(m.deadzone), p1.y), IM_COL32(255, 255, 255, 22));
        for (float s : { -1.0f, 1.0f }) {
            const float x = x_of(s * m.range);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 120), 1.5f);
        }
        const float bp = button_point(m, m.button_level);
        if (m.button_pos != NO_BUTTON) {
            const float x = x_of(bp);
            dl->AddTriangleFilled(ImVec2(x - em * 0.3f, p1.y), ImVec2(x + em * 0.3f, p1.y), ImVec2(x, p1.y - em * 0.4f),
                COLOR_BUTTON_MARK);
        }
        if (m.button_neg != NO_BUTTON) {
            const float x = x_of(-bp);
            dl->AddTriangleFilled(ImVec2(x - em * 0.3f, p1.y), ImVec2(x + em * 0.3f, p1.y), ImVec2(x, p1.y - em * 0.4f),
                COLOR_BUTTON_MARK);
        }
    }
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), IM_COL32(255, 255, 255, 50));
    if (valid) {
        const float x = x_of(value * dir);
        dl->AddTriangleFilled(ImVec2(x - em * 0.35f, p0.y), ImVec2(x + em * 0.35f, p0.y), ImVec2(x, p0.y + em * 0.45f),
            COLOR_MARKER);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), COLOR_MARKER, 2.0f);
    }
}

static bool button_combo(const char* id, int& button) {
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    if (ImGui::BeginCombo(id, button == NO_BUTTON ? "None" : MOUSE_BUTTON_NAMES[button])) {
        if (ImGui::Selectable("None", button == NO_BUTTON)) {
            button = NO_BUTTON;
            changed = true;
        }
        for (int b = 0; b < MOUSE_BUTTON_COUNT; b++) {
            if (ImGui::Selectable(MOUSE_BUTTON_NAMES[b], button == b)) {
                button = b;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void draw_axis(int a) {
    App& app = g_app;
    AxisMapping& m = app.config.mouse.axes[a];
    const AxisLimits& lim = AXIS_LIMITS[a];
    const float em = ImGui::GetFontSize();
    const MouseSnapshot* ms = app.mouse.get();
    const bool valid = app.in_use();
    const float value = valid ? ms->relative[a] : 0.0f;
    const bool degrees = axis_in_degrees((RudderAxis) a);
    bool changed = false;

    ImGui::PushID(a);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(RUDDER_AXIS_NAMES[a]);
    if (RUDDER_AXIS_DESCRIPTIONS[a][0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", RUDDER_AXIS_DESCRIPTIONS[a]);
    }
    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    char text[32];
    if (valid) {
        format_value(a, value, text, sizeof(text));
    } else {
        snprintf(text, sizeof(text), "--");
    }
    ImGui::SetCursorPosX(right - ImGui::CalcTextSize("+00.0°").x);
    ImGui::TextUnformatted(text);

    draw_axis_bar(a, m, value, valid, ImGui::GetContentRegionAvail().x);

    const float label_w = em * 5.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Output");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::BeginCombo("##target", MOUSE_TARGET_NAMES[(int) m.target])) {
        for (int t = 0; t < MOUSE_TARGET_COUNT; t++) {
            if (ImGui::Selectable(MOUSE_TARGET_NAMES[t], (int) m.target == t)) {
                m.target = (MouseTarget) t;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip(
        "Move: the axis sets how fast the cursor moves.\n"
        "Position: the axis sets where the cursor is; neutral is the middle of the area,\n"
        "full output on either side is its edge.\n"
        "Scroll: the axis sets how fast it scrolls.\n"
        "The + side (clockwise, toes down, tilt right, up) goes right / up.\n"
        "Several axes can drive the same output; they add up.");
    ImGui::SameLine(0, em);
    changed |= ImGui::Checkbox("Invert", &m.invert);
    ImGui::SetItemTooltip("Swap the + and - sides, for the output and the buttons.");
    ImGui::SameLine(0, em);
    ImGui::TextDisabled("output %+.2f", valid ? ms->outputs[a] : 0.0f);

    const float slider_w = ImGui::GetContentRegionAvail().x - label_w;
    char format[32];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sensitivity");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    snprintf(format, sizeof(format), degrees ? "full at %%.1f°" : "full at %%.2f");
    if (ImGui::SliderFloat("##range", &m.range, lim.range_min, lim.range_max, format)) {
        m.range = std::max(m.range, m.deadzone + lim.range_min / 2);
        changed = true;
    }
    ImGui::SetItemTooltip("How far from neutral for full output.\nLower = more sensitive.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Deadzone");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    snprintf(format, sizeof(format), degrees ? "%%.1f°" : "%%.2f");
    if (ImGui::SliderFloat("##deadzone", &m.deadzone, 0.0f, lim.deadzone_max, format)) {
        m.range = std::max(m.range, m.deadzone + lim.range_min / 2);
        changed = true;
    }
    ImGui::SetItemTooltip("How far from neutral with no output.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Curve");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    ImGui::BeginDisabled(m.target == MouseTarget::None);
    changed |= ImGui::SliderFloat("##curve", &m.curve, 1.0f, 3.0f, m.curve == 1.0f ? "linear" : "%.1f");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("1 = linear. Higher = finer control near neutral, the same full output.");
    if (target_is_move(m.target)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Speed");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(slider_w);
        changed |= ImGui::SliderFloat("##speed", &m.move_speed, 50.0f, 5000.0f, "%.0f pixels/s at full",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("How fast the cursor moves at full output.\n"
                              "Windows' pointer speed settings scale this too.");
    } else if (target_is_scroll(m.target)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Speed");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(slider_w);
        changed |= ImGui::SliderFloat("##scroll", &m.scroll_speed, 1.0f, 50.0f, "%.0f notches/s at full",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("How fast it scrolls at full output, in mouse wheel notches.");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Buttons");
    ImGui::SameLine(label_w);
    ImGui::TextUnformatted("+");
    ImGui::SameLine();
    changed |= button_combo("##bpos", m.button_pos);
    ImGui::SameLine(0, em * 0.7f);
    ImGui::TextUnformatted("-");
    ImGui::SameLine();
    changed |= button_combo("##bneg", m.button_neg);
    ImGui::SameLine(0, em * 0.7f);
    ImGui::BeginDisabled(m.button_pos == NO_BUTTON && m.button_neg == NO_BUTTON);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    changed |= ImGui::SliderFloat("##blevel", &m.button_level, 0.0f, 1.0f, "at %.2f");
    ImGui::SetItemTooltip("Mouse buttons held while the axis is this far between the deadzone (0) and full (1),\n"
                          "on the + or - side. Marked under the bar. Works with or without an output.");
    ImGui::EndDisabled();

    ImGui::PopID();
    if (changed) {
        app.settings_changed();
    }
}

static void draw_cursor_settings() {
    App& app = g_app;
    MouseSettings& s = app.config.mouse;
    const float em = ImGui::GetFontSize();
    const float label_w = em * 7.5f;
    bool changed = false;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Smoothing");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    changed |= ImGui::SliderFloat("##smoothing", &s.smoothing, 0.0f, 1.0f, s.smoothing == 0.0f ? "off" : "%.2f");
    ImGui::SetItemTooltip("Higher = steadier when holding still, but a little more lag.\n"
                          "Fast movements are smoothed less either way. Matters most for Position.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Position area");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::BeginCombo("##area", ABSOLUTE_AREA_NAMES[(int) s.area])) {
        for (int i = 0; i < ABSOLUTE_AREA_COUNT; i++) {
            if (ImGui::Selectable(ABSOLUTE_AREA_NAMES[i], (int) s.area == i)) {
                s.area = (AbsoluteArea) i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Where the Position outputs put the cursor: neutral is its middle,\n"
                          "full output its edges.");
    ImGui::SameLine(0, em * 1.5f);
    changed |= ImGui::Checkbox("Smooth scrolling", &s.smooth_scroll);
    ImGui::SetItemTooltip("Scroll in fractions of a wheel notch. Smoother in apps that support it,\n"
                          "but some apps only react to whole notches.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Freeze on click");
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("##freeze", &s.freeze_on_click);
    ImGui::SetItemTooltip("For the Position outputs: while any mouse button is down (the real mouse,\n"
                          "another program or the 3dRudder's own buttons), the cursor stays where it was,\n"
                          "so clicks don't turn into drags.");
    ImGui::BeginDisabled(!s.freeze_on_click);
    {
        ImGui::SameLine();
        const float fw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        ImGui::SetNextItemWidth(fw);
        changed |= ImGui::SliderFloat("##freeze_linger", &s.freeze_linger, 0.0f, 1.5f, "%.2f s after release");
        ImGui::SetItemTooltip("How long it stays frozen after the buttons are released, so the second click\n"
                              "of a double click lands in the same place. Windows' double-click time is %.2f s.",
            GetDoubleClickTime() / 1000.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw);
        changed |= ImGui::SliderInt("##drag_radius", &s.drag_radius, 5, 500, "drag beyond %d px");
        ImGui::SetItemTooltip("Moving the position further than this from where it froze lets the cursor\n"
                              "follow again: for dragging while a button is down, or to move on right away\n"
                              "after a click.");
    }
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hold position");
    ImGui::SameLine(label_w);
    changed |= ImGui::Checkbox("##hold", &s.hold);
    ImGui::SetItemTooltip("For the Position outputs: when the position stays within the \"still\" distance\n"
                          "for a moment, the cursor is held in place, so small movements (like the ones\n"
                          "while clicking) don't move it. It follows again after a movement larger than\n"
                          "the \"release\" distance. Lower values allow smaller corrections, higher values\n"
                          "hold the cursor more firmly.");
    ImGui::BeginDisabled(!s.hold);
    {
        ImGui::SameLine();
        const float hw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        ImGui::SetNextItemWidth(hw);
        changed |= ImGui::SliderFloat("##hold_time", &s.hold_time, 0.05f, 2.0f, "after %.2f s");
        ImGui::SetItemTooltip("How long the position has to stay still before it's held.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(hw);
        if (ImGui::SliderInt("##hold_radius", &s.hold_radius, 1, 100, "still within %d px")) {
            s.release_radius = std::max(s.release_radius, s.hold_radius);
            changed = true;
        }
        ImGui::SetItemTooltip("How far the position can wander and still count as still.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(hw);
        if (ImGui::SliderInt("##release_radius", &s.release_radius, 1, 300, "release beyond %d px")) {
            s.release_radius = std::max(s.release_radius, s.hold_radius);
            changed = true;
        }
        ImGui::SetItemTooltip("How far the position has to move from the held point to follow again.");
    }
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Dwell click");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::BeginCombo("##dwell", DWELL_CLICK_NAMES[(int) s.dwell])) {
        for (int i = 0; i < DWELL_CLICK_COUNT; i++) {
            if (ImGui::Selectable(DWELL_CLICK_NAMES[i], (int) s.dwell == i)) {
                s.dwell = (DwellClick) i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Click by holding the cursor still after moving it.\n"
                          "It clicks once, then waits for the cursor to move again.");
    ImGui::BeginDisabled(s.dwell == DwellClick::Off);
    ImGui::SameLine(0, em);
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::SetNextItemWidth(w);
    changed |= ImGui::SliderFloat("##dwell_time", &s.dwell_time, 0.2f, 3.0f, "after %.1f s");
    ImGui::SetItemTooltip("How long to hold still before it clicks.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w);
    changed |= ImGui::SliderInt("##dwell_radius", &s.dwell_radius, 2, 50, "within %d pixels");
    ImGui::SetItemTooltip("How far the cursor can wander and still count as holding still.");
    ImGui::EndDisabled();

    if (changed) {
        app.settings_changed();
    }
}

static void draw_output() {
    App& app = g_app;
    const MouseSnapshot* ms = app.mouse.get();
    const bool active = app.running && app.in_use();
    const float em = ImGui::GetFontSize();

    POINT p = {};
    GetCursorPos(&p);
    ImGui::TextUnformatted("Cursor");
    ImGui::SameLine(em * 7.5f);
    ImGui::Text("%d, %d", (int) p.x, (int) p.y);
    if (ms && ms->positioning && active) {
        ImGui::SameLine();
        if (ms->frozen) {
            ImGui::TextColored(COLOR_WARN, "(frozen while clicking at %d, %d)", ms->target[0], ms->target[1]);
        } else if (ms->holding) {
            ImGui::TextColored(COLOR_WARN, "(held at %d, %d)", ms->target[0], ms->target[1]);
        } else {
            ImGui::TextDisabled("(positioned at %d, %d)", ms->target[0], ms->target[1]);
        }
    }
    ImGui::TextUnformatted("Speed");
    ImGui::SameLine(em * 7.5f);
    ImGui::Text("%+.0f, %+.0f pixels/s", ms ? ms->speed[0] : 0.0f, ms ? ms->speed[1] : 0.0f);
    ImGui::TextUnformatted("Scroll");
    ImGui::SameLine(em * 7.5f);
    ImGui::Text("%+.1f up, %+.1f right notches/s", ms ? ms->scroll[0] : 0.0f, ms ? ms->scroll[1] : 0.0f);
    ImGui::TextUnformatted("Buttons");
    ImGui::SameLine(em * 7.5f);
    bool any = false;
    for (int b = 0; b < MOUSE_BUTTON_COUNT; b++) {
        if (ms && (ms->buttons & mouse_button_mask((MouseButton) b))) {
            if (any) {
                ImGui::SameLine();
            }
            push_button_colors(ImVec4(0.85f, 0.45f, 0.2f, 1));
            ImGui::SmallButton(MOUSE_BUTTON_NAMES[b]);
            ImGui::PopStyleColor(3);
            any = true;
        }
    }
    if (!any) {
        ImGui::TextDisabled("none");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hold");
    ImGui::SameLine(em * 7.5f);
    {
        const bool settling = ms && ms->hold_progress >= 0 && active && app.config.mouse.hold;
        const char* text = !settling ? "--" : ms->holding ? "held" : "following";
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
            settling && ms->holding ? ImVec4(0.9f, 0.7f, 0.2f, 1) : ImVec4(0.35f, 0.55f, 0.85f, 1));
        ImGui::ProgressBar(settling ? ms->hold_progress : 0.0f, ImVec2(em * 12, em * 1.1f), text);
        ImGui::PopStyleColor();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Dwell");
    ImGui::SameLine(em * 7.5f);
    const float progress = ms && ms->dwell_progress >= 0 ? ms->dwell_progress : 0.0f;
    char overlay[48];
    snprintf(overlay, sizeof(overlay), "%u clicks", ms ? ms->dwell_clicks : 0u);
    ImGui::ProgressBar(progress, ImVec2(em * 12, em * 1.1f), overlay);

    if (!app.running) {
        ImGui::TextDisabled("Stopped: the mouse is left alone.");
    } else if (!app.in_use()) {
        ImGui::TextDisabled("Running, waiting for the 3dRudder to be in use.");
    }
}

// ---------------------------------------------------------------------------

static void draw_top_bar() {
    App& app = g_app;
    const float em = ImGui::GetFontSize();
    const RudderStatus* st = app.status.get();

    const ImVec2 big(em * 9, ImGui::GetFrameHeight() * 1.7f);
    push_button_colors(app.running ? ImVec4(0.13f, 0.52f, 0.22f, 1) : ImVec4(0.45f, 0.17f, 0.17f, 1));
    if (ImGui::Button(app.running ? "RUNNING" : "STOPPED", big)) {
        app.running = !app.running;
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(app.running ? "Click to stop: the 3dRudder no longer moves the mouse."
                                      : "Click to let the 3dRudder control the mouse.");

    ImGui::SameLine(0, em * 1.5f);
    ImGui::BeginGroup();
    const float label_w = em * 6;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hotkey");
    ImGui::SameLine(label_w);
    const std::string name = hotkey_name(app.config.hotkey);
    if (app.capturing_hotkey) {
        push_button_colors(ImVec4(0.55f, 0.42f, 0.12f, 1));
        if (ImGui::Button("Press keys...", ImVec2(em * 9, 0))) {
            end_hotkey_capture();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextColored(COLOR_WARN, "%s", app.hotkey_hint.c_str());
    } else {
        if (ImGui::Button(name.c_str(), ImVec2(em * 9, 0))) {
            start_hotkey_capture();
        }
        ImGui::SetItemTooltip("Click, then press the key combination to use.");
        ImGui::SameLine();
        if (app.config.hotkey.key != 0) {
            if (ImGui::SmallButton("Clear")) {
                app.config.hotkey.key = 0;
                app.mark_dirty();
                register_hotkey();
            }
            ImGui::SetItemTooltip("No hotkey.");
            ImGui::SameLine();
        }
        if (app.config.hotkey.key == 0) {
            ImGui::TextDisabled("no hotkey: click to set one");
        } else if (app.hotkey_ok) {
            ImGui::TextUnformatted("starts and stops it from anywhere");
        } else {
            ImGui::TextColored(COLOR_WARN, "can't be used: taken by Windows or another program");
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("3dRudder");
    ImGui::SameLine(label_w);
    if (!st) {
        ImGui::TextDisabled("looking...");
    } else if (!st->connected) {
        ImGui::TextColored(COLOR_ERROR, "%s", st->error.c_str());
    } else {
        ImGui::TextUnformatted(st->device_name.c_str());
        ImGui::SameLine();
        if (!st->have_frame) {
            ImGui::TextColored(COLOR_WARN, "connected, no data");
        } else {
            const int status = st->frame.status;
            ImGui::TextColored(rudder_in_use(status) ? COLOR_OK : COLOR_WARN, "%s", rudder_status_name(status));
            ImGui::SameLine();
            ImGui::TextDisabled("(status %d, %.0f reports/s)", status, st->rate);
        }
    }
    ImGui::EndGroup();
}

static void draw_ui() {
    App& app = g_app;
    Config& cfg = app.config;
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_top_bar();
    ImGui::Spacing();

    const float left_w = std::max(em * 20, ImGui::GetContentRegionAvail().x * 0.34f);

    if (ImGui::BeginChild("left", ImVec2(left_w, 0))) {
        ImGui::SeparatorText("3dRudder");
        draw_device_panel();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("right", ImVec2(0, 0))) {
        ImGui::SeparatorText("Output");
        draw_output();
        ImGui::SeparatorText("Mouse");
        draw_cursor_settings();
        ImGui::SeparatorText("Axes");
        if (ImGui::SmallButton("Reset to relative defaults")) {
            default_axes(cfg.mouse.axes, false);
            app.settings_changed();
        }
        ImGui::SetItemTooltip("Rotation moves the cursor left / right, forward / back moves it up / down (inverted).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset to absolute defaults")) {
            default_axes(cfg.mouse.axes, true);
            app.settings_changed();
        }
        ImGui::SetItemTooltip("Rotation positions the cursor left / right, forward / back up / down (inverted).\n"
                              "Linear, no deadzones.");
        ImGui::Spacing();
        for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
            if (a > 0) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }
            draw_axis(a);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------

static LRESULT WINAPI main_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // While capturing a hotkey, keys go to that and not to the UI.
    if (g_app.capturing_hotkey) {
        switch (msg) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                capture_hotkey_key((UINT) wparam);
                return 0;
            case WM_KEYUP:
            case WM_SYSKEYUP:
            case WM_CHAR:
            case WM_SYSCHAR:
                return 0;
            case WM_KILLFOCUS:
                end_hotkey_capture();
                break;
        }
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
        case WM_HOTKEY:
            if (wparam == HOTKEY_ID) {
                g_app.running = !g_app.running;
            }
            return 0;
        case WM_SIZE:
            capture_window_placement();
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            g_resize_width = (UINT) LOWORD(lparam);
            g_resize_height = (UINT) HIWORD(lparam);
            return 0;
        case WM_MOVE:
            capture_window_placement();
            return 0;
        case WM_CLOSE:
            capture_window_placement();
            break;
        case WM_GETMINMAXINFO: {
            float s = GetDpiForWindow(hwnd) / 96.0f;
            auto* mmi = (MINMAXINFO*) lparam;
            mmi->ptMinTrackSize.x = (LONG) (960 * s);
            mmi->ptMinTrackSize.y = (LONG) (700 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_pending_dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT* r = (const RECT*) lparam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {  // disable ALT application menu
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    // Also makes the cursor positions physical pixels on every monitor.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    App& app = g_app;
    app.config_path = app_data_dir() + L"\\settings.json";
    load_config(app.config_path, app.config);
    if (wcsstr(GetCommandLineW(), L"--start") != nullptr) {
        app.running = true;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RudderMouse";
    RegisterClassExW(&wc);

    // Restore the window where it was last time, but only if its title bar
    // would still be reachable (monitors may have changed since).
    const WindowRect& saved = app.config.window;
    bool restore = false;
    if (saved.valid) {
        RECT title = { saved.x, saved.y, saved.x + saved.w, saved.y + GetSystemMetrics(SM_CYCAPTION) };
        if (HMONITOR mon = MonitorFromRect(&title, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi = { sizeof(mi) };
            RECT visible;
            restore = GetMonitorInfoW(mon, &mi) && IntersectRect(&visible, &title, &mi.rcWork) &&
                      visible.right - visible.left >= 100;
        }
    }

    const float primary_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = restore
        ? CreateWindowW(wc.lpszClassName, L"3dRudder Mouse", WS_OVERLAPPEDWINDOW,
              saved.x, saved.y, saved.w, saved.h, nullptr, nullptr, instance, nullptr)
        : CreateWindowW(wc.lpszClassName, L"3dRudder Mouse", WS_OVERLAPPEDWINDOW,
              (int) (80 * primary_scale), (int) (80 * primary_scale), (int) (1180 * primary_scale),
              (int) (900 * primary_scale), nullptr, nullptr, instance, nullptr);
    app.main_hwnd = hwnd;
    update_icon(hwnd);
    const float scale = GetDpiForWindow(hwnd) / 96.0f;

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"3dRudder Mouse", MB_ICONERROR);
        return 1;
    }

    register_hotkey();
    app.settings_changed();
    app.config_dirty = false;
    app.reader.start([](const RudderFrame* frame) { g_app.engine.process(frame, now_seconds()); });

    if (restore) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = { saved.x, saved.y, saved.x + saved.w, saved.y + saved.h };
        wp.showCmd = show_cmd;
        if (saved.maximized && (show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOW || show_cmd == SW_SHOWDEFAULT)) {
            wp.showCmd = SW_SHOWMAXIMIZED;
        }
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, show_cmd);
    }
    UpdateWindow(hwnd);
    g_track_placement = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    apply_style(scale);
    // Latin plus the degree sign.
    static const ImWchar glyph_ranges[] = { 0x0020, 0x00FF, 0 };
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr, glyph_ranges);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_device_context);

    bool done = false;
    while (!done) {
        const bool minimized = IsIconic(hwnd) != 0;
        if (minimized || g_swap_chain_occluded) {
            // Nothing to draw: sleep until something happens.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        app.engine.set_running(app.running);
        if (app.config_dirty && GetTickCount64() - app.config_dirty_since >= 1000) {
            save_now();
        }

        // Also while minimized, the taskbar shows the icon.
        app.status = app.reader.status();
        app.mouse = app.engine.snapshot();
        update_icon(hwnd);

        if (minimized) {
            continue;
        }
        if (g_swap_chain_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            continue;
        }
        g_swap_chain_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }
        if (g_pending_dpi_scale > 0) {
            apply_style(g_pending_dpi_scale);
            g_pending_dpi_scale = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();

        const float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_device_context->OMSetRenderTargets(1, &g_render_target_view, nullptr);
        g_device_context->ClearRenderTargetView(g_render_target_view, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swap_chain->Present(1, 0);
        g_swap_chain_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Once the reader thread is gone, let go of any buttons still held.
    app.running = false;
    app.engine.set_running(false);
    app.reader.stop();
    app.engine.process(nullptr, now_seconds());
    if (app.hotkey_ok) {
        UnregisterHotKey(hwnd, HOTKEY_ID);
        app.hotkey_ok = false;
    }
    if (app.config_dirty) {
        save_now();
    }

    app.status.reset();
    app.mouse.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
