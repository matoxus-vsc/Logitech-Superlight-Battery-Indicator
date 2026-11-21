#include "renderer.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace slb {

namespace {

constexpr float kFloatingPillHeightDip = 38.0f;
constexpr float kFloatingCornerRadiusDip = 10.0f;
constexpr float kDockedCornerRadiusDip = 6.0f;
constexpr float kFloatingPaddingXDip = 13.0f;
constexpr float kDockedPaddingXDip = 9.0f;
constexpr float kIconWidthDip = 15.0f;
constexpr float kIconHeightDip = 17.0f;
constexpr float kIconTextGapDip = 6.5f;
constexpr float kBadgeGapDip = 4.5f;
constexpr float kBadgeWidthDip = 10.0f;

D2D1_COLOR_F colorFromHex(uint32_t rgb, float alpha = 1.0f) {
    const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    const float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>(rgb & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, alpha);
}

std::wstring_view formatPercentageText(const DeviceBatteryState& dev, wchar_t (&buf)[8]) {
    if (!dev.present || dev.percentage < 0) {
        buf[0] = L'-';
        buf[1] = L'-';
        buf[2] = L'%';
        buf[3] = L'\0';
        return std::wstring_view(buf, 3);
    }

    int pct = std::clamp(dev.percentage, 0, 100);
    size_t len = 0;
    if (pct == 100) {
        buf[len++] = L'1';
        buf[len++] = L'0';
        buf[len++] = L'0';
    } else if (pct >= 10) {
        buf[len++] = static_cast<wchar_t>(L'0' + (pct / 10));
        buf[len++] = static_cast<wchar_t>(L'0' + (pct % 10));
    } else {
        buf[len++] = static_cast<wchar_t>(L'0' + pct);
    }
    buf[len++] = L'%';
    buf[len] = L'\0';
    return std::wstring_view(buf, len);
}

bool hasStatusBadge(const DeviceBatteryState& dev, int lowThreshold) {
    if (!dev.present || dev.percentage < 0) {
        return false;
    }
    if (dev.charging) {
        return true;
    }
    return !dev.sleeping && dev.percentage <= lowThreshold;
}

class RenderContext {
public:
    ~RenderContext() { releaseDib(); }

    bool initFactories() {
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
            return false;
        }
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
            return false;
        }
        if (FAILED(dwriteFactory_->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                13.5f,
                L"pl-PL",
                textFormat_.GetAddressOf()))) {
            return false;
        }
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        const D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND,
            4.0f,
            D2D1_DASH_STYLE_SOLID,
            0.0f);
        return SUCCEEDED(d2dFactory_->CreateStrokeStyle(
            &strokeProps, nullptr, 0, roundStroke_.GetAddressOf()));
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> buildTextLayout(
        std::wstring_view txt,
        float layoutHeightDip,
        float& outWidthDip) const {
        outWidthDip = 26.0f;
        if (!dwriteFactory_ || !textFormat_) {
            return nullptr;
        }
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(
                txt.data(),
                static_cast<UINT32>(txt.size()),
                textFormat_.Get(),
                120.0f,
                layoutHeightDip,
                layout.GetAddressOf()))) {
            return nullptr;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
            outWidthDip = std::ceil(metrics.widthIncludingTrailingWhitespace);
        }
        return layout;
    }

    bool render(
        HWND hwnd,
        const AppSnapshot& snapshot,
        const AppConfig& config,
        const RenderOptions& options,
        UINT dpi,
        SIZE* outSizePx) {
        if (!initFactories()) {
            return false;
        }

        const float scale = static_cast<float>(dpi > 0 ? dpi : 96) / 96.0f;
        const int heightPx = (options.dockedToTaskbar && options.taskbarHeightPx > 0)
                                 ? std::max(24, options.taskbarHeightPx)
                                 : std::max(1, static_cast<int>(std::ceil(kFloatingPillHeightDip * scale)));
        const float heightDip = static_cast<float>(heightPx) / scale;

        wchar_t singleBuf[8]{};
        std::wstring_view singleText;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> singleLayout;
        float singleTextW = 26.0f;

        float widthDip = 0.0f;
        if (config.multiDeviceMode) {
            widthDip = options.dockedToTaskbar ? 316.0f : 336.0f;
        } else {
            const float padX = options.dockedToTaskbar ? kDockedPaddingXDip : kFloatingPaddingXDip;
            singleText = formatPercentageText(snapshot.primaryMouse, singleBuf);
            singleLayout = buildTextLayout(singleText, heightDip, singleTextW);
            const float effectiveTextW = std::max(26.0f, singleTextW);
            const bool badge = hasStatusBadge(snapshot.primaryMouse, config.lowBatteryThreshold);
            float w = padX * 2.0f + kIconWidthDip + kIconTextGapDip + effectiveTextW;
            if (badge) {
                w += kBadgeGapDip + kBadgeWidthDip;
            }
            widthDip = std::max(options.dockedToTaskbar ? 68.0f : 88.0f, std::ceil(w));
        }

        const int widthPx = std::max(1, static_cast<int>(std::ceil(widthDip * scale)));

        HDC screenDC = GetDC(nullptr);
        if (!screenDC) {
            return false;
        }

        if (!createDib(screenDC, widthPx, heightPx) ||
            !createTargetAndBrushes(options)) {
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        RECT bindRect = {0, 0, widthPx, heightPx};
        dcTarget_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        if (FAILED(dcTarget_->BindDC(memDC_, &bindRect))) {
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        const float drawW = static_cast<float>(widthPx) / scale;
        const float drawH = static_cast<float>(heightPx) / scale;

        dcTarget_->BeginDraw();
        dcTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        if (!options.dockedToTaskbar) {
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, drawW - 0.5f, drawH - 0.5f),
                kFloatingCornerRadiusDip,
                kFloatingCornerRadiusDip);
            dcTarget_->FillRoundedRectangle(pill, bgBrush_.Get());
            dcTarget_->DrawRoundedRectangle(pill, borderBrush_.Get(), 1.0f);
        } else {
            // Keep a minimal alpha surface (1/255) over the taskbar tile so drag & context-menu
            // hit-testing covers the full widget rectangle while letting the native Windows 11
            // taskbar show through cleanly behind the icon and text.
            const D2D1_ROUNDED_RECT tile = D2D1::RoundedRect(
                D2D1::RectF(1.0f, 4.0f, drawW - 1.0f, drawH - 4.0f),
                kDockedCornerRadiusDip,
                kDockedCornerRadiusDip);
            dcTarget_->FillRoundedRectangle(tile, bgBrush_.Get());
            if (options.showAdjustHighlight) {
                dcTarget_->DrawRoundedRectangle(tile, borderBrush_.Get(), 1.0f);
            }
        }

        const float padX = options.dockedToTaskbar ? kDockedPaddingXDip : kFloatingPaddingXDip;
        if (!config.multiDeviceMode) {
            drawSlot(
                snapshot.primaryMouse,
                DeviceCategory::Mouse,
                padX,
                drawW - padX * 2.0f,
                drawH,
                config.lowBatteryThreshold,
                singleLayout.Get(),
                singleTextW);
        } else {
            const std::array<std::pair<const DeviceBatteryState*, DeviceCategory>, 4> slots = {{
                {&snapshot.headset, DeviceCategory::Headset},
                {&snapshot.controller, DeviceCategory::Controller},
                {&snapshot.primaryMouse, DeviceCategory::Mouse},
                {&snapshot.keyboard, DeviceCategory::Keyboard},
            }};
            const float slotWidth = (drawW - 8.0f) / 4.0f;
            for (size_t i = 0; i < slots.size(); ++i) {
                const float sx = 4.0f + static_cast<float>(i) * slotWidth;
                drawSlot(
                    *slots[i].first,
                    slots[i].second,
                    sx,
                    slotWidth,
                    drawH,
                    config.lowBatteryThreshold,
                    nullptr,
                    0.0f);
                if (i + 1 < slots.size()) {
                    const float divX = sx + slotWidth;
                    dcTarget_->DrawLine(
                        D2D1::Point2F(divX, 10.0f),
                        D2D1::Point2F(divX, drawH - 10.0f),
                        dividerBrush_.Get(),
                        1.0f);
                }
            }
        }

        const HRESULT endHr = dcTarget_->EndDraw();
        if (FAILED(endHr)) {
            ReleaseDC(nullptr, screenDC);
            return false;
        }

        SIZE wndSize = {widthPx, heightPx};
        POINT ptSrc = {0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        const BOOL ulwOk = UpdateLayeredWindow(
            hwnd,
            screenDC,
            nullptr,
            &wndSize,
            memDC_,
            &ptSrc,
            0,
            &blend,
            ULW_ALPHA);

        ReleaseDC(nullptr, screenDC);
        if (outSizePx != nullptr) {
            *outSizePx = wndSize;
        }
        return ulwOk != FALSE;
    }

private:
    bool createDib(HDC screenDC, int widthPx, int heightPx) {
        memDC_ = CreateCompatibleDC(screenDC);
        if (!memDC_) {
            return false;
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = widthPx;
        bmi.bmiHeader.biHeight = -heightPx;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        dibBitmap_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dibBitmap_ || !bits) {
            releaseDib();
            return false;
        }
        oldBitmap_ = SelectObject(memDC_, dibBitmap_);
        return true;
    }

    void releaseDib() {
        if (memDC_) {
            if (oldBitmap_) {
                SelectObject(memDC_, oldBitmap_);
                oldBitmap_ = nullptr;
            }
            if (dibBitmap_) {
                DeleteObject(dibBitmap_);
                dibBitmap_ = nullptr;
            }
            DeleteDC(memDC_);
            memDC_ = nullptr;
        }
    }

    bool createTargetAndBrushes(const RenderOptions& options) {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE,
            D2D1_FEATURE_LEVEL_DEFAULT);

        if (FAILED(d2dFactory_->CreateDCRenderTarget(&props, dcTarget_.GetAddressOf()))) {
            return false;
        }
        dcTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dcTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        auto makeBrush = [&](uint32_t hex, float alpha, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& out) {
            return SUCCEEDED(dcTarget_->CreateSolidColorBrush(
                colorFromHex(hex, alpha), out.GetAddressOf()));
        };

        if (!options.dockedToTaskbar) {
            return makeBrush(0x0E1013, 0.96f, bgBrush_) &&
                   makeBrush(0x1E2229, 1.0f, borderBrush_) &&
                   makeBrush(0x1E2229, 1.0f, dividerBrush_) &&
                   makeBrush(0xB8BEC6, 1.0f, iconNormalBrush_) &&
                   makeBrush(0xE6E8EB, 1.0f, textNormalBrush_) &&
                   makeBrush(0x2ECC71, 1.0f, greenBrush_) &&
                   makeBrush(0xE05A4F, 1.0f, coralBrush_) &&
                   makeBrush(0x6D747E, 1.0f, dimBrush_) &&
                   makeBrush(0x5A606A, 1.0f, offlineBrush_);
        }

        if (options.lightTaskbarTheme) {
            const float bgAlpha = options.showAdjustHighlight ? 0.06f : 0.01f;
            return makeBrush(0x000000, bgAlpha, bgBrush_) &&
                   makeBrush(0x000000, 0.22f, borderBrush_) &&
                   makeBrush(0x000000, 0.14f, dividerBrush_) &&
                   makeBrush(0x3C424A, 1.0f, iconNormalBrush_) &&
                   makeBrush(0x1A1D21, 1.0f, textNormalBrush_) &&
                   makeBrush(0x169C4E, 1.0f, greenBrush_) &&
                   makeBrush(0xD14338, 1.0f, coralBrush_) &&
                   makeBrush(0x6E7580, 1.0f, dimBrush_) &&
                   makeBrush(0x828994, 1.0f, offlineBrush_);
        }

        const uint32_t bgHex = options.showAdjustHighlight ? 0xFFFFFFu : 0x0E1013u;
        const float bgAlpha = options.showAdjustHighlight ? 0.08f : 0.01f;
        return makeBrush(bgHex, bgAlpha, bgBrush_) &&
               makeBrush(0xFFFFFF, 0.24f, borderBrush_) &&
               makeBrush(0xFFFFFF, 0.14f, dividerBrush_) &&
               makeBrush(0xB8BEC6, 1.0f, iconNormalBrush_) &&
               makeBrush(0xE6E8EB, 1.0f, textNormalBrush_) &&
               makeBrush(0x2ECC71, 1.0f, greenBrush_) &&
               makeBrush(0xE05A4F, 1.0f, coralBrush_) &&
               makeBrush(0x7B828C, 1.0f, dimBrush_) &&
               makeBrush(0x636A74, 1.0f, offlineBrush_);
    }

    void drawMouseIcon(float x, float y, float w, float h, ID2D1SolidColorBrush* brush) {
        const float bodyW = 11.0f;
        const float bodyH = 16.0f;
        const float bx = x + (w - bodyW) * 0.5f;
        const float by = y + (h - bodyH) * 0.5f;

        const D2D1_ROUNDED_RECT body = D2D1::RoundedRect(
            D2D1::RectF(bx, by, bx + bodyW, by + bodyH), 5.5f, 5.5f);
        dcTarget_->DrawRoundedRectangle(body, brush, 1.5f, roundStroke_.Get());

        const float midX = bx + bodyW * 0.5f;
        dcTarget_->DrawLine(
            D2D1::Point2F(bx + 1.1f, by + 6.5f),
            D2D1::Point2F(bx + bodyW - 1.1f, by + 6.5f),
            brush,
            1.15f,
            roundStroke_.Get());
        dcTarget_->DrawLine(
            D2D1::Point2F(midX, by + 0.6f),
            D2D1::Point2F(midX, by + 2.2f),
            brush,
            1.2f,
            roundStroke_.Get());

        const D2D1_ROUNDED_RECT wheel = D2D1::RoundedRect(
            D2D1::RectF(midX - 1.1f, by + 2.3f, midX + 1.1f, by + 5.7f), 1.0f, 1.0f);
        dcTarget_->FillRoundedRectangle(wheel, brush);
    }

    void drawHeadsetIcon(float x, float y, float w, float h, ID2D1SolidColorBrush* brush) {
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arcGeo;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(arcGeo.GetAddressOf()))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(arcGeo->Open(sink.GetAddressOf()))) {
                sink->BeginFigure(D2D1::Point2F(cx - 5.5f, cy + 1.5f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(cx + 5.5f, cy + 1.5f),
                    D2D1::SizeF(5.5f, 6.2f),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                dcTarget_->DrawGeometry(arcGeo.Get(), brush, 1.5f, roundStroke_.Get());
            }
        }

        const D2D1_ROUNDED_RECT leftCup = D2D1::RoundedRect(
            D2D1::RectF(cx - 6.8f, cy + 0.5f, cx - 3.8f, cy + 6.0f), 1.2f, 1.2f);
        const D2D1_ROUNDED_RECT rightCup = D2D1::RoundedRect(
            D2D1::RectF(cx + 3.8f, cy + 0.5f, cx + 6.8f, cy + 6.0f), 1.2f, 1.2f);
        dcTarget_->DrawRoundedRectangle(leftCup, brush, 1.4f, roundStroke_.Get());
        dcTarget_->DrawRoundedRectangle(rightCup, brush, 1.4f, roundStroke_.Get());
    }

    void drawControllerIcon(float x, float y, float w, float h, ID2D1SolidColorBrush* brush) {
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;

        const D2D1_ROUNDED_RECT pad = D2D1::RoundedRect(
            D2D1::RectF(cx - 6.8f, cy - 4.2f, cx + 6.8f, cy + 4.4f), 3.6f, 3.6f);
        dcTarget_->DrawRoundedRectangle(pad, brush, 1.45f, roundStroke_.Get());

        dcTarget_->DrawLine(
            D2D1::Point2F(cx - 4.4f, cy), D2D1::Point2F(cx - 1.8f, cy), brush, 1.25f, roundStroke_.Get());
        dcTarget_->DrawLine(
            D2D1::Point2F(cx - 3.1f, cy - 1.3f), D2D1::Point2F(cx - 3.1f, cy + 1.3f), brush, 1.25f, roundStroke_.Get());
        dcTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 2.5f, cy + 0.6f), 0.9f, 0.9f), brush);
        dcTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 4.4f, cy - 0.8f), 0.9f, 0.9f), brush);
    }

    void drawKeyboardIcon(float x, float y, float w, float h, ID2D1SolidColorBrush* brush) {
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;

        const D2D1_ROUNDED_RECT frame = D2D1::RoundedRect(
            D2D1::RectF(cx - 7.0f, cy - 4.5f, cx + 7.0f, cy + 4.5f), 2.0f, 2.0f);
        dcTarget_->DrawRoundedRectangle(frame, brush, 1.4f, roundStroke_.Get());

        for (float kx : {-4.2f, -1.4f, 1.4f, 4.2f}) {
            dcTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + kx, cy - 1.5f), 0.75f, 0.75f), brush);
        }
        dcTarget_->DrawLine(
            D2D1::Point2F(cx - 3.4f, cy + 1.8f),
            D2D1::Point2F(cx + 3.4f, cy + 1.8f),
            brush,
            1.25f,
            roundStroke_.Get());
    }

    void drawLightningBolt(float x, float y, float w, float h, ID2D1SolidColorBrush* brush) {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(d2dFactory_->CreatePathGeometry(geo.GetAddressOf()))) {
            return;
        }
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(sink.GetAddressOf()))) {
            return;
        }

        const std::array<D2D1_POINT_2F, 6> pts = {{
            {x + w * 0.62f, y + h * 0.04f},
            {x + w * 0.14f, y + h * 0.54f},
            {x + w * 0.48f, y + h * 0.54f},
            {x + w * 0.36f, y + h * 0.96f},
            {x + w * 0.86f, y + h * 0.44f},
            {x + w * 0.52f, y + h * 0.44f},
        }};

        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();

        dcTarget_->FillGeometry(geo.Get(), brush);
    }

    void drawLowAlertIcon(float x, float y, float w, float h, ID2D1SolidColorBrush* brush) {
        const float cx = x + w * 0.5f;
        const float top = y + 1.0f;
        const float bot = y + h - 1.0f;

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> tri;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(tri.GetAddressOf()))) {
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(tri->Open(sink.GetAddressOf()))) {
                sink->BeginFigure(D2D1::Point2F(cx, top), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(x + w - 0.6f, bot));
                sink->AddLine(D2D1::Point2F(x + 0.6f, bot));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dcTarget_->DrawGeometry(tri.Get(), brush, 1.35f, roundStroke_.Get());
            }
        }

        dcTarget_->DrawLine(
            D2D1::Point2F(cx, top + 3.4f),
            D2D1::Point2F(cx, bot - 3.3f),
            brush,
            1.35f,
            roundStroke_.Get());
        dcTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, bot - 1.7f), 0.75f, 0.75f), brush);
    }

    void drawSlot(
        const DeviceBatteryState& dev,
        DeviceCategory fallbackCategory,
        float slotX,
        float slotWidth,
        float heightDip,
        int lowThreshold,
        IDWriteTextLayout* prebuiltLayout,
        float prebuiltTextW) {
        const bool isOffline = !dev.present;
        const bool isLow = dev.present && !dev.charging && !dev.sleeping &&
                           dev.percentage >= 0 && dev.percentage <= lowThreshold;
        const bool isCharging = dev.present && dev.charging;
        const bool isSleeping = dev.present && dev.sleeping && !isCharging;

        ID2D1SolidColorBrush* iconBrush = iconNormalBrush_.Get();
        ID2D1SolidColorBrush* textBrush = textNormalBrush_.Get();

        if (isOffline) {
            iconBrush = offlineBrush_.Get();
            textBrush = offlineBrush_.Get();
        } else if (isLow) {
            iconBrush = coralBrush_.Get();
            textBrush = coralBrush_.Get();
        } else if (isSleeping) {
            iconBrush = dimBrush_.Get();
            textBrush = dimBrush_.Get();
        }

        wchar_t pctBuf[8]{};
        std::wstring_view pctText;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> ownedLayout;
        IDWriteTextLayout* layoutPtr = prebuiltLayout;
        float textW = prebuiltTextW;

        if (!layoutPtr) {
            pctText = formatPercentageText(dev, pctBuf);
            ownedLayout = buildTextLayout(pctText, heightDip, textW);
            layoutPtr = ownedLayout.Get();
        }

        const bool showBadge = isCharging || isLow;
        const float contentW = kIconWidthDip + kIconTextGapDip + textW +
                               (showBadge ? (kBadgeGapDip + kBadgeWidthDip) : 0.0f);
        const float startX = slotX + std::max(0.0f, (slotWidth - contentW) * 0.5f);
        const float iconY = (heightDip - kIconHeightDip) * 0.5f;

        const DeviceCategory cat =
            (dev.category != DeviceCategory::Unknown) ? dev.category : fallbackCategory;
        switch (cat) {
            case DeviceCategory::Headset:
                drawHeadsetIcon(startX, iconY, kIconWidthDip, kIconHeightDip, iconBrush);
                break;
            case DeviceCategory::Controller:
                drawControllerIcon(startX, iconY, kIconWidthDip, kIconHeightDip, iconBrush);
                break;
            case DeviceCategory::Keyboard:
                drawKeyboardIcon(startX, iconY, kIconWidthDip, kIconHeightDip, iconBrush);
                break;
            case DeviceCategory::Mouse:
            case DeviceCategory::Unknown:
            default:
                drawMouseIcon(startX, iconY, kIconWidthDip, kIconHeightDip, iconBrush);
                break;
        }

        const float textX = startX + kIconWidthDip + kIconTextGapDip;
        if (layoutPtr) {
            dcTarget_->DrawTextLayout(D2D1::Point2F(textX, 0.0f), layoutPtr, textBrush);
        } else {
            if (pctText.empty()) {
                pctText = formatPercentageText(dev, pctBuf);
            }
            const D2D1_RECT_F textRect = D2D1::RectF(textX, 0.0f, textX + textW + 4.0f, heightDip - 0.5f);
            dcTarget_->DrawText(
                pctText.data(),
                static_cast<UINT32>(pctText.size()),
                textFormat_.Get(),
                textRect,
                textBrush);
        }

        if (showBadge) {
            const float badgeX = textX + textW + kBadgeGapDip;
            if (isCharging) {
                drawLightningBolt(badgeX, (heightDip - 13.0f) * 0.5f, 9.0f, 13.0f, greenBrush_.Get());
            } else if (isLow) {
                drawLowAlertIcon(badgeX, (heightDip - 11.0f) * 0.5f, 10.0f, 11.0f, coralBrush_.Get());
            }
        }
    }

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke_;

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dividerBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconNormalBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textNormalBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> greenBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> coralBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dimBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> offlineBrush_;

    HDC memDC_ = nullptr;
    HBITMAP dibBitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
};

}  // namespace

bool LayeredRenderer::renderToLayeredWindow(
    HWND hwnd,
    const AppSnapshot& snapshot,
    const AppConfig& config,
    const RenderOptions& options,
    UINT dpi,
    SIZE* outSizePx) const {
    bool ok = false;
    {
        RenderContext ctx;
        ok = ctx.render(hwnd, snapshot, config, options, dpi, outSizePx);
    }
    // Reclaim temporary Direct2D / WARP / DirectWrite initialization pages once pixels are
    // committed to DWM via UpdateLayeredWindow.
    SetProcessWorkingSetSize(
        GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    return ok;
}

}  // namespace slb
