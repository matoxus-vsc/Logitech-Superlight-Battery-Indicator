#include "tray_icon.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace slb {

namespace {

bool isPolishUi() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_POLISH;
}

uint32_t premulPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint32_t pr = (static_cast<uint32_t>(r) * a + 127) / 255;
    const uint32_t pg = (static_cast<uint32_t>(g) * a + 127) / 255;
    const uint32_t pb = (static_cast<uint32_t>(b) * a + 127) / 255;
    return (static_cast<uint32_t>(a) << 24) | (pr << 16) | (pg << 8) | pb;
}

}  // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

TrayIcon::IconTone TrayIcon::toneForSnapshot(
    const AppSnapshot& snapshot,
    const AppConfig& config) const {
    const auto& m = snapshot.primaryMouse;
    if (!m.present || m.percentage < 0) {
        return IconTone::Dimmed;
    }
    if (m.charging) {
        return IconTone::Charging;
    }
    if (!m.sleeping && m.percentage <= config.lowBatteryThreshold) {
        return IconTone::Low;
    }
    if (m.sleeping) {
        return IconTone::Dimmed;
    }
    return IconTone::Normal;
}

std::wstring TrayIcon::formatTooltip(const AppSnapshot& snapshot) const {
    const bool pl = isPolishUi();
    const auto& m = snapshot.primaryMouse;
    std::wstring name = m.deviceName.empty() ? L"Wireless Mouse" : m.deviceName;
    if (!m.present) {
        return name + (pl ? L": brak połączenia" : L": disconnected");
    }
    if (m.percentage < 0) {
        return name + (m.sleeping ? (pl ? L": uśpiona" : L": sleeping")
                                  : (pl ? L": odczyt..." : L": reading..."));
    }
    std::wstring tip = name + L": " + std::to_wstring(m.percentage) + L"%";
    if (m.charging) {
        tip += pl ? L" (Ładowanie)" : L" (Charging)";
    } else if (m.sleeping) {
        tip += pl ? L" (Uśpiona)" : L" (Sleeping)";
    }
    if (tip.size() >= 127) {
        tip.resize(124);
        tip += L"...";
    }
    return tip;
}

HICON TrayIcon::buildIcon(IconTone tone) const {
    const int size = std::max(16, GetSystemMetrics(SM_CXSMICON));
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(
        hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!colorBmp || !bits) {
        if (colorBmp) {
            DeleteObject(colorBmp);
        }
        return nullptr;
    }

    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, nullptr);
    if (!maskBmp) {
        DeleteObject(colorBmp);
        return nullptr;
    }

    uint8_t fgR = 0xE6, fgG = 0xE8, fgB = 0xEB;
    uint8_t accentR = 0x2E, accentG = 0xCC, accentB = 0x71;
    bool showAccentDot = false;
    switch (tone) {
        case IconTone::Charging:
            fgR = 0x2E; fgG = 0xCC; fgB = 0x71;
            accentR = 0x2E; accentG = 0xCC; accentB = 0x71;
            showAccentDot = true;
            break;
        case IconTone::Low:
            fgR = 0xE0; fgG = 0x5A; fgB = 0x4F;
            accentR = 0xE0; accentG = 0x5A; accentB = 0x4F;
            showAccentDot = true;
            break;
        case IconTone::Dimmed:
            fgR = 0x7E; fgG = 0x85; fgB = 0x90;
            break;
        case IconTone::Normal:
        default:
            break;
    }

    auto* px = static_cast<uint32_t*>(bits);
    const float s = static_cast<float>(size);
    const float half = s * 0.5f;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / s;
            const float fy = (static_cast<float>(y) + 0.5f) / s;

            // Rounded dark badge background.
            const float bx = std::max(0.0f, std::abs(fx - 0.5f) - 0.24f);
            const float by = std::max(0.0f, std::abs(fy - 0.5f) - 0.24f);
            const float badgeDist = std::hypot(bx, by) - 0.22f;

            uint32_t out = 0;
            if (badgeDist <= 0.0f) {
                const bool onBorder = badgeDist > -0.06f;
                out = onBorder ? premulPixel(0x28, 0x2D, 0x36, 255)
                               : premulPixel(0x0E, 0x10, 0x13, 255);

                // Centered Superlight mouse capsule silhouette.
                const float mx = (fx - 0.5f) / 0.21f;
                const float my = (fy - 0.5f) / 0.30f;
                const float capY = std::max(0.0f, std::abs(my) - 0.32f);
                const float mouseDist = std::hypot(mx, capY) - 0.68f;
                const float stroke = std::abs(mouseDist);

                // Scroll wheel slit near top center.
                const bool wheel = std::abs(fx - 0.5f) < (1.0f / half) * 0.55f &&
                                   fy >= 0.30f && fy <= 0.45f;

                if (stroke < 0.22f || wheel) {
                    out = premulPixel(fgR, fgG, fgB, 255);
                }

                if (showAccentDot) {
                    const float dx = fx - 0.77f;
                    const float dy = fy - 0.77f;
                    if (dx * dx + dy * dy <= 0.015f) {
                        out = premulPixel(accentR, accentG, accentB, 255);
                    }
                }
            }
            px[y * size + x] = out;
        }
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(maskBmp);
    DeleteObject(colorBmp);
    return icon;
}

bool TrayIcon::create(HWND hwnd, const AppSnapshot& snapshot, const AppConfig& config) {
    hwnd_ = hwnd;
    currentTone_ = toneForSnapshot(snapshot, config);
    currentTip_ = formatTooltip(snapshot);
    currentIcon_ = buildIcon(currentTone_);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = currentIcon_;
    wcsncpy_s(nid.szTip, currentTip_.c_str(), _TRUNCATE);

    added_ = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
    return added_;
}

void TrayIcon::destroy() {
    if (added_ && hwnd_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (currentIcon_) {
        DestroyIcon(currentIcon_);
        currentIcon_ = nullptr;
    }
}

void TrayIcon::recreate() {
    if (!hwnd_) {
        return;
    }
    added_ = false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = currentIcon_;
    wcsncpy_s(nid.szTip, currentTip_.c_str(), _TRUNCATE);
    added_ = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
}

void TrayIcon::update(const AppSnapshot& snapshot, const AppConfig& config) {
    if (!added_ || !hwnd_) {
        return;
    }

    const IconTone nextTone = toneForSnapshot(snapshot, config);
    const std::wstring nextTip = formatTooltip(snapshot);

    if (nextTone == currentTone_ && nextTip == currentTip_) {
        return;
    }

    HICON newIcon = nullptr;
    if (nextTone != currentTone_) {
        newIcon = buildIcon(nextTone);
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, nextTip.c_str(), _TRUNCATE);

    if (newIcon != nullptr) {
        nid.uFlags |= NIF_ICON;
        nid.hIcon = newIcon;
    }

    Shell_NotifyIconW(NIM_MODIFY, &nid);

    if (newIcon != nullptr) {
        if (currentIcon_) {
            DestroyIcon(currentIcon_);
        }
        currentIcon_ = newIcon;
        currentTone_ = nextTone;
    }
    currentTip_ = nextTip;
}

void TrayIcon::showLowBatteryNotification(const DeviceBatteryState& mouse) {
    if (!added_ || !hwnd_) {
        return;
    }
    const bool pl = isPolishUi();
    const std::wstring name = mouse.deviceName.empty() ? L"Wireless Mouse" : mouse.deviceName;
    const std::wstring title = pl ? L"Niski poziom baterii myszki" : L"Low mouse battery";
    const std::wstring body = name + L": " + std::to_wstring(mouse.percentage) + L"%";

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::showContextMenu(
    HWND hwnd,
    POINT screenPt,
    const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    const bool pl = isPolishUi();

    AppendMenuW(
        menu,
        MF_STRING | (state.dockToTaskbar ? MF_CHECKED : MF_UNCHECKED),
        IDM_DOCK_TASKBAR,
        pl ? L"Zadokuj na pasku zadań" : L"Dock to taskbar");
    AppendMenuW(
        menu,
        MF_STRING | (!state.dockToTaskbar ? MF_CHECKED : MF_UNCHECKED),
        IDM_FLOATING_MODE,
        pl ? L"Tryb pływający" : L"Floating mode");

    if (state.dockToTaskbar) {
        AppendMenuW(
            menu,
            MF_STRING | (state.adjustTaskbarX ? MF_CHECKED : MF_UNCHECKED),
            IDM_ADJUST_TASKBAR_X,
            pl ? L"Dopasuj pozycję X (przesuń)" : L"Adjust X position (drag)");
        AppendMenuW(
            menu,
            MF_STRING | (state.canResetTaskbarX ? MF_ENABLED : MF_GRAYED),
            IDM_RESET_TASKBAR_X,
            pl ? L"Resetuj pozycję na pasku" : L"Reset taskbar position");
    } else {
        AppendMenuW(
            menu,
            MF_STRING | (state.widgetVisible ? MF_CHECKED : MF_UNCHECKED),
            IDM_TOGGLE_VISIBLE,
            pl ? L"Pokaż / Ukryj widżet" : L"Show / Hide widget");
        AppendMenuW(
            menu,
            MF_STRING | (state.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED),
            IDM_ALWAYS_ON_TOP,
            pl ? L"Zawsze na wierzchu" : L"Always on top");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, pl ? L"Odśwież teraz" : L"Refresh now");
    AppendMenuW(
        menu,
        MF_STRING | (state.autostartEnabled ? MF_CHECKED : MF_UNCHECKED),
        IDM_AUTOSTART,
        pl ? L"Uruchamiaj z Windows" : L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, pl ? L"Zamknij" : L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(
        menu,
        TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_LEFTBUTTON,
        screenPt.x,
        screenPt.y,
        hwnd,
        nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

}  // namespace slb
