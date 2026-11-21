#pragma once

#include "app_state.hpp"
#include "config.hpp"

#include <windows.h>

namespace slb {

struct RenderOptions {
    bool dockedToTaskbar = false;
    int taskbarHeightPx = 0;
    bool lightTaskbarTheme = false;
    bool showAdjustHighlight = false;
};

class LayeredRenderer {
public:
    LayeredRenderer() = default;
    ~LayeredRenderer() = default;

    LayeredRenderer(const LayeredRenderer&) = delete;
    LayeredRenderer& operator=(const LayeredRenderer&) = delete;

    bool initialize() const { return true; }

    bool renderToLayeredWindow(
        HWND hwnd,
        const AppSnapshot& snapshot,
        const AppConfig& config,
        const RenderOptions& options,
        UINT dpi,
        SIZE* outSizePx = nullptr) const;
};

}  // namespace slb
