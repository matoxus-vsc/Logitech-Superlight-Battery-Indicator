#include "config.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <vector>

namespace slb {

namespace {

void skipWhitespace(std::string_view s, size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
}

bool parseJsonString(std::string_view s, size_t& pos, std::string& out) {
    skipWhitespace(s, pos);
    if (pos >= s.size() || s[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < s.size()) {
        const char ch = s[pos++];
        if (ch == '"') {
            return true;
        }
        if (ch == '\\') {
            if (pos >= s.size()) {
                return false;
            }
            const char esc = s[pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(esc); break;
            }
        } else {
            out.push_back(ch);
        }
    }
    return false;
}

bool parseJsonInt(std::string_view s, size_t& pos, int& out) {
    skipWhitespace(s, pos);
    if (pos >= s.size()) {
        return false;
    }
    const size_t start = pos;
    if (s[pos] == '-') {
        ++pos;
    }
    const size_t digitsStart = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    if (pos == digitsStart) {
        pos = start;
        return false;
    }
    int val = 0;
    const auto [ptr, ec] = std::from_chars(s.data() + start, s.data() + pos, val);
    if (ec != std::errc{} || ptr != s.data() + pos) {
        pos = start;
        return false;
    }
    out = val;
    return true;
}

bool parseJsonBool(std::string_view s, size_t& pos, bool& out) {
    skipWhitespace(s, pos);
    if (s.substr(pos).starts_with("true")) {
        pos += 4;
        out = true;
        return true;
    }
    if (s.substr(pos).starts_with("false")) {
        pos += 5;
        out = false;
        return true;
    }
    return false;
}

bool skipJsonValue(std::string_view s, size_t& pos) {
    skipWhitespace(s, pos);
    if (pos >= s.size()) {
        return false;
    }
    const char ch = s[pos];
    if (ch == '"') {
        std::string dummy;
        return parseJsonString(s, pos, dummy);
    }
    if (ch == '{' || ch == '[') {
        const char openCh = ch;
        const char closeCh = (ch == '{') ? '}' : ']';
        int depth = 0;
        bool inStr = false;
        while (pos < s.size()) {
            const char c = s[pos++];
            if (inStr) {
                if (c == '\\' && pos < s.size()) {
                    ++pos;
                } else if (c == '"') {
                    inStr = false;
                }
            } else {
                if (c == '"') {
                    inStr = true;
                } else if (c == openCh) {
                    ++depth;
                } else if (c == closeCh) {
                    --depth;
                    if (depth == 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }
    while (pos < s.size()) {
        const char c = s[pos];
        if (c == ',' || c == '}' || c == ']' || std::isspace(static_cast<unsigned char>(c))) {
            return true;
        }
        ++pos;
    }
    return true;
}

std::wstring getExecutableDirectory() {
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return L".";
        }
        if (len < buf.size() - 1) {
            std::filesystem::path exePath(std::wstring_view(buf.data(), len));
            return exePath.parent_path().wstring();
        }
        buf.resize(buf.size() * 2, L'\0');
    }
}

}  // namespace

std::filesystem::path getConfigPath() {
    return std::filesystem::path(getExecutableDirectory()) / L"config.json";
}

AppConfig clampConfig(AppConfig cfg) {
    cfg.pollIntervalSec = std::clamp(cfg.pollIntervalSec, 5, 3600);
    cfg.lowBatteryThreshold = std::clamp(cfg.lowBatteryThreshold, 1, 99);
    cfg.taskbarOffsetX = std::clamp(cfg.taskbarOffsetX, -2000, 1000);
    return cfg;
}

AppConfig parseConfigJson(std::string_view json) {
    AppConfig cfg;
    size_t pos = 0;
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != '{') {
        return clampConfig(cfg);
    }
    ++pos;

    std::string key;
    while (pos < json.size()) {
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') {
            break;
        }
        if (!parseJsonString(json, pos, key)) {
            break;
        }
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':') {
            break;
        }
        ++pos;

        if (key == "poll_interval_sec") {
            int v = cfg.pollIntervalSec;
            if (parseJsonInt(json, pos, v)) {
                cfg.pollIntervalSec = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "low_battery_threshold") {
            int v = cfg.lowBatteryThreshold;
            if (parseJsonInt(json, pos, v)) {
                cfg.lowBatteryThreshold = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "window_x") {
            int v = cfg.windowX;
            if (parseJsonInt(json, pos, v)) {
                cfg.windowX = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "window_y") {
            int v = cfg.windowY;
            if (parseJsonInt(json, pos, v)) {
                cfg.windowY = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "always_on_top") {
            bool v = cfg.alwaysOnTop;
            if (parseJsonBool(json, pos, v)) {
                cfg.alwaysOnTop = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "multi_device_mode") {
            bool v = cfg.multiDeviceMode;
            if (parseJsonBool(json, pos, v)) {
                cfg.multiDeviceMode = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "mock_mode") {
            bool v = cfg.mockMode;
            if (parseJsonBool(json, pos, v)) {
                cfg.mockMode = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "dock_to_taskbar") {
            bool v = cfg.dockToTaskbar;
            if (parseJsonBool(json, pos, v)) {
                cfg.dockToTaskbar = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "taskbar_offset_x") {
            int v = cfg.taskbarOffsetX;
            if (parseJsonInt(json, pos, v)) {
                cfg.taskbarOffsetX = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else if (key == "low_battery_notify") {
            bool v = cfg.lowBatteryNotify;
            if (parseJsonBool(json, pos, v)) {
                cfg.lowBatteryNotify = v;
            } else {
                skipJsonValue(json, pos);
            }
        } else {
            if (!skipJsonValue(json, pos)) {
                break;
            }
        }

        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < json.size() && json[pos] == '}') {
            break;
        }
    }

    return clampConfig(cfg);
}

std::string serializeConfigJson(const AppConfig& rawCfg) {
    const AppConfig cfg = clampConfig(rawCfg);
    std::ostringstream oss;
    oss << "{\n"
        << "  \"poll_interval_sec\": " << cfg.pollIntervalSec << ",\n"
        << "  \"low_battery_threshold\": " << cfg.lowBatteryThreshold << ",\n"
        << "  \"window_x\": " << cfg.windowX << ",\n"
        << "  \"window_y\": " << cfg.windowY << ",\n"
        << "  \"always_on_top\": " << (cfg.alwaysOnTop ? "true" : "false") << ",\n"
        << "  \"multi_device_mode\": " << (cfg.multiDeviceMode ? "true" : "false") << ",\n"
        << "  \"mock_mode\": " << (cfg.mockMode ? "true" : "false") << ",\n"
        << "  \"dock_to_taskbar\": " << (cfg.dockToTaskbar ? "true" : "false") << ",\n"
        << "  \"taskbar_offset_x\": " << cfg.taskbarOffsetX << ",\n"
        << "  \"low_battery_notify\": " << (cfg.lowBatteryNotify ? "true" : "false") << "\n"
        << "}\n";
    return oss.str();
}

AppConfig loadConfig(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return clampConfig(AppConfig{});
    }
    std::ostringstream buffer;
    buffer << ifs.rdbuf();
    return parseConfigJson(buffer.str());
}

bool saveConfig(const std::filesystem::path& path, const AppConfig& cfg) {
    std::filesystem::path tmpPath = path;
    tmpPath += L".tmp";

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }
        const std::string data = serializeConfigJson(cfg);
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        ofs.flush();
        if (!ofs.good()) {
            return false;
        }
    }

    if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

}  // namespace slb
