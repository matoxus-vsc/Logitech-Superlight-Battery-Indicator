#include "hid_device.hpp"

#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <array>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <string_view>
#include <utility>

namespace slb {

namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : handle_(h) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

struct DevInfoSetDeleter {
    void operator()(HDEVINFO set) const {
        if (set != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
};

std::wstring toLower(std::wstring s) {
    for (wchar_t& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return s;
}

std::wstring extractParentKey(const std::wstring& devicePath) {
    std::wstring lower = toLower(devicePath);
    const size_t colPos = lower.find(L"&col");
    if (colPos != std::wstring::npos) {
        return lower.substr(0, colPos);
    }
    const size_t hashPos = lower.find_last_of(L'#');
    if (hashPos != std::wstring::npos) {
        return lower.substr(0, hashPos);
    }
    return lower;
}

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

bool containsIgnoreCase(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    return toLower(std::wstring(haystack)).find(toLower(std::wstring(needle))) !=
           std::wstring::npos;
}

bool isSuperlightPreferredName(std::wstring_view name) {
    return containsIgnoreCase(name, L"superlight") ||
           containsIgnoreCase(name, L"superstrike") ||
           containsIgnoreCase(name, L"pro x");
}

enum class TransactStatus {
    Ok,
    Hidpp10Error,
    Hidpp20Error,
    Timeout,
    IoError
};

struct TransactResult {
    TransactStatus status = TransactStatus::IoError;
    uint8_t errorCode = 0;
    std::array<uint8_t, kLongReportLen> response{};
    size_t responseLen = 0;
};

TransactResult transactHidpp(
    HANDLE handle,
    const HidInterfaceInfo& iface,
    uint8_t devIdx,
    uint8_t featIdx,
    uint8_t fn,
    uint8_t swId,
    HANDLE writeEvent,
    HANDLE readEvent,
    uint8_t p0 = 0,
    uint8_t p1 = 0,
    uint8_t p2 = 0,
    DWORD readTimeoutMs = 450) {
    TransactResult result;
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return result;
    }

    UniqueHandle localWriteEvent;
    UniqueHandle localReadEvent;
    if (!writeEvent) {
        localWriteEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        writeEvent = localWriteEvent.get();
    }
    if (!readEvent) {
        localReadEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        readEvent = localReadEvent.get();
    }
    if (!writeEvent || !readEvent) {
        return result;
    }

    // Drain stale input reports so unread mouse events do not clutter this query.
    HidD_FlushQueue(handle);

    constexpr size_t kStackBufSize = 64;
    std::array<uint8_t, kStackBufSize> outStack{};
    std::vector<uint8_t> outHeap;
    uint8_t* outPtr = nullptr;
    size_t outSize = 0;

    if (iface.supportsLong && iface.outputReportLen >= kLongReportLen) {
        outSize = iface.outputReportLen;
        if (outSize <= kStackBufSize) {
            outStack.fill(0);
            outPtr = outStack.data();
        } else {
            outHeap.assign(outSize, 0);
            outPtr = outHeap.data();
        }
        const auto pkt = buildLongRequest(devIdx, featIdx, fn, swId, p0, p1, p2);
        std::copy(pkt.begin(), pkt.end(), outPtr);
    } else if (iface.supportsShort && iface.outputReportLen >= kShortReportLen) {
        outSize = iface.outputReportLen;
        if (outSize <= kStackBufSize) {
            outStack.fill(0);
            outPtr = outStack.data();
        } else {
            outHeap.assign(outSize, 0);
            outPtr = outHeap.data();
        }
        const auto pkt = buildShortRequest(devIdx, featIdx, fn, swId, p0, p1, p2);
        std::copy(pkt.begin(), pkt.end(), outPtr);
    } else {
        return result;
    }

    ResetEvent(writeEvent);
    OVERLAPPED writeOv{};
    writeOv.hEvent = writeEvent;
    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(
        handle, outPtr, static_cast<DWORD>(outSize), &bytesWritten, &writeOv);
    if (!writeOk) {
        const DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            return result;
        }
        const DWORD waitRes = WaitForSingleObject(writeEvent, 250);
        if (waitRes != WAIT_OBJECT_0) {
            CancelIoEx(handle, &writeOv);
            GetOverlappedResult(handle, &writeOv, &bytesWritten, TRUE);
            result.status = TransactStatus::Timeout;
            return result;
        }
        if (!GetOverlappedResult(handle, &writeOv, &bytesWritten, FALSE)) {
            return result;
        }
    }

    const size_t readBufSize = std::max<size_t>(iface.inputReportLen, kLongReportLen);
    std::array<uint8_t, kStackBufSize> inStack{};
    std::vector<uint8_t> inHeap;
    uint8_t* inPtr = nullptr;
    if (readBufSize <= kStackBufSize) {
        inPtr = inStack.data();
    } else {
        inHeap.assign(readBufSize, 0);
        inPtr = inHeap.data();
    }

    const ULONGLONG deadline = GetTickCount64() + readTimeoutMs;

    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            result.status = TransactStatus::Timeout;
            return result;
        }
        const DWORD remainingMs = static_cast<DWORD>(deadline - now);

        ResetEvent(readEvent);
        OVERLAPPED readOv{};
        readOv.hEvent = readEvent;
        std::fill_n(inPtr, readBufSize, uint8_t{0});
        DWORD bytesRead = 0;

        BOOL readOk = ReadFile(
            handle, inPtr, static_cast<DWORD>(readBufSize), &bytesRead, &readOv);
        if (!readOk) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                result.status = TransactStatus::IoError;
                return result;
            }
            const DWORD waitRes = WaitForSingleObject(readEvent, remainingMs);
            if (waitRes != WAIT_OBJECT_0) {
                CancelIoEx(handle, &readOv);
                GetOverlappedResult(handle, &readOv, &bytesRead, TRUE);
                result.status = TransactStatus::Timeout;
                return result;
            }
            if (!GetOverlappedResult(handle, &readOv, &bytesRead, FALSE)) {
                result.status = TransactStatus::IoError;
                return result;
            }
        }

        if (bytesRead >= kShortReportLen) {
            uint8_t errCode = 0;
            const auto kind = classifyResponse(
                std::span<const uint8_t>(inPtr, bytesRead),
                devIdx,
                featIdx,
                fn,
                swId,
                errCode);

            if (kind == HidppResponseKind::Success) {
                result.status = TransactStatus::Ok;
                result.responseLen = std::min<size_t>(bytesRead, result.response.size());
                std::copy_n(inPtr, result.responseLen, result.response.begin());
                return result;
            }
            if (kind == HidppResponseKind::Hidpp10Error) {
                result.status = TransactStatus::Hidpp10Error;
                result.errorCode = errCode;
                return result;
            }
            if (kind == HidppResponseKind::Hidpp20Error) {
                result.status = TransactStatus::Hidpp20Error;
                result.errorCode = errCode;
                return result;
            }
        }
    }
}

bool isTransactAsleep(const TransactResult& r) {
    if (r.status == TransactStatus::Timeout) {
        return true;
    }
    if (r.status == TransactStatus::Hidpp10Error) {
        return isDeviceAsleepError(HidppResponseKind::Hidpp10Error, r.errorCode);
    }
    if (r.status == TransactStatus::Hidpp20Error) {
        return isDeviceAsleepError(HidppResponseKind::Hidpp20Error, r.errorCode);
    }
    return false;
}

UniqueHandle openHidReadWrite(const std::wstring& path) {
    return UniqueHandle(CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr));
}

bool queryFeatureIndexDirect(
    HANDLE handle,
    const HidInterfaceInfo& iface,
    uint8_t devIdx,
    uint16_t featureId,
    uint8_t swId,
    HANDLE writeEvent,
    HANDLE readEvent,
    uint8_t& outIndex) {
    const auto r = transactHidpp(
        handle,
        iface,
        devIdx,
        0x00,
        0x00,
        swId,
        writeEvent,
        readEvent,
        static_cast<uint8_t>((featureId >> 8) & 0xFF),
        static_cast<uint8_t>(featureId & 0xFF),
        0x00);
    if (r.status != TransactStatus::Ok || r.responseLen < 7) {
        return false;
    }
    outIndex = r.response[4];
    return outIndex != 0;
}

bool queryDeviceNameAndType(
    HANDLE handle,
    const HidInterfaceInfo& iface,
    uint8_t devIdx,
    uint8_t nameFeatureIdx,
    uint8_t& swIdCounter,
    HANDLE writeEvent,
    HANDLE readEvent,
    std::wstring& outName,
    DeviceCategory& outCategory) {
    auto nextId = [&]() -> uint8_t {
        swIdCounter = static_cast<uint8_t>((swIdCounter < 0x0F) ? (swIdCounter + 1) : 0x09);
        return swIdCounter;
    };

    const auto typeRes = transactHidpp(
        handle, iface, devIdx, nameFeatureIdx, 0x02, nextId(), writeEvent, readEvent);
    if (typeRes.status == TransactStatus::Ok && typeRes.responseLen >= 5) {
        outCategory = mapHidppDeviceType(typeRes.response[4]);
    }

    const auto lenRes = transactHidpp(
        handle, iface, devIdx, nameFeatureIdx, 0x00, nextId(), writeEvent, readEvent);
    if (lenRes.status != TransactStatus::Ok || lenRes.responseLen < 5) {
        return false;
    }
    const size_t totalLen = std::min<size_t>(lenRes.response[4], 64);
    if (totalLen == 0) {
        return false;
    }

    std::string utf8Name;
    utf8Name.reserve(totalLen);
    for (size_t offset = 0; offset < totalLen;) {
        const auto chunkRes = transactHidpp(
            handle,
            iface,
            devIdx,
            nameFeatureIdx,
            0x01,
            nextId(),
            writeEvent,
            readEvent,
            static_cast<uint8_t>(offset));
        if (chunkRes.status != TransactStatus::Ok || chunkRes.responseLen < 5) {
            break;
        }
        const size_t payloadAvail = chunkRes.responseLen - 4;
        const size_t toCopy = std::min(payloadAvail, totalLen - offset);
        if (toCopy == 0) {
            break;
        }
        for (size_t i = 0; i < toCopy; ++i) {
            const char c = static_cast<char>(chunkRes.response[4 + i]);
            if (c == '\0') {
                offset = totalLen;
                break;
            }
            utf8Name.push_back(c);
        }
        offset += toCopy;
    }

    if (!utf8Name.empty()) {
        outName = utf8ToWide(utf8Name);
    }
    return !outName.empty();
}

std::optional<BatteryReading> queryBatteryReading(
    HANDLE handle,
    const HidInterfaceInfo& iface,
    uint8_t devIdx,
    uint16_t featureId,
    uint8_t featureIndex,
    uint8_t swId,
    HANDLE writeEvent,
    HANDLE readEvent,
    bool& outAsleepOrTimeout) {
    outAsleepOrTimeout = false;
    const uint8_t fn = (featureId == kFeatureUnifiedBattery) ? 0x01 : 0x00;

    const auto res = transactHidpp(
        handle, iface, devIdx, featureIndex, fn, swId, writeEvent, readEvent);
    if (res.status != TransactStatus::Ok) {
        outAsleepOrTimeout = isTransactAsleep(res);
        return std::nullopt;
    }

    const std::span<const uint8_t> span(res.response.data(), res.responseLen);
    if (featureId == kFeatureUnifiedBattery) {
        return parseUnifiedBatteryStatus(span);
    }
    if (featureId == kFeatureBatteryLevelStatus) {
        return parseBatteryLevelStatus(span);
    }
    if (featureId == kFeatureBatteryVoltage) {
        return parseBatteryVoltage(span);
    }
    return std::nullopt;
}

}  // namespace

std::vector<HidInterfaceInfo> enumerateLogitechHidppInterfaces() {
    std::vector<HidInterfaceInfo> results;

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    std::unique_ptr<void, DevInfoSetDeleter> devInfo(SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!devInfo || devInfo.get() == INVALID_HANDLE_VALUE) {
        return results;
    }

    SP_DEVICE_INTERFACE_DATA ifaceData{};
    ifaceData.cbSize = sizeof(ifaceData);
    std::vector<uint8_t> detailBuf;

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(devInfo.get(), nullptr, &hidGuid, index, &ifaceData);
         ++index) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(
            devInfo.get(), &ifaceData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) {
            continue;
        }

        if (detailBuf.size() < reqSize) {
            detailBuf.resize(reqSize);
        }
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(
                devInfo.get(), &ifaceData, detail, reqSize, nullptr, nullptr)) {
            continue;
        }

        const std::wstring path(detail->DevicePath);
        // Query-only open (access=0) inspects attributes/caps without locking OS collections.
        UniqueHandle queryHandle(CreateFileW(
            path.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr));
        if (!queryHandle.valid()) {
            continue;
        }

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        if (!HidD_GetAttributes(queryHandle.get(), &attrs) || attrs.VendorID != kLogitechVid) {
            continue;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!HidD_GetPreparsedData(queryHandle.get(), &preparsed) || !preparsed) {
            continue;
        }

        HIDP_CAPS caps{};
        const NTSTATUS capsStatus = HidP_GetCaps(preparsed, &caps);
        HidD_FreePreparsedData(preparsed);
        if (capsStatus != HIDP_STATUS_SUCCESS) {
            continue;
        }

        if (caps.UsagePage != kUsagePageHidpp && caps.UsagePage != kUsagePageHidppAlt) {
            continue;
        }

        HidInterfaceInfo info;
        info.devicePath = path;
        info.parentKey = extractParentKey(path);
        info.productId = attrs.ProductID;
        info.usagePage = caps.UsagePage;
        info.usage = caps.Usage;
        info.inputReportLen = caps.InputReportByteLength;
        info.outputReportLen = caps.OutputReportByteLength;
        info.supportsLong = (caps.OutputReportByteLength >= kLongReportLen &&
                             caps.InputReportByteLength >= kLongReportLen);
        info.supportsShort = (caps.OutputReportByteLength >= kShortReportLen &&
                              caps.InputReportByteLength >= kShortReportLen);

        wchar_t prodBuf[128]{};
        if (HidD_GetProductString(queryHandle.get(), prodBuf, sizeof(prodBuf))) {
            info.productString = prodBuf;
        }

        if (info.supportsLong || info.supportsShort) {
            results.push_back(std::move(info));
        }
    }

    // Prefer long HID++ 2.0 (0x11) collections ahead of short (0x10) collections.
    std::stable_sort(results.begin(), results.end(), [](const HidInterfaceInfo& a, const HidInterfaceInfo& b) {
        if (a.supportsLong != b.supportsLong) {
            return a.supportsLong > b.supportsLong;
        }
        return a.usage > b.usage;
    });

    return results;
}

SuperlightHidClient::SuperlightHidClient() {
    ensureEvents();
}

SuperlightHidClient::~SuperlightHidClient() {
    if (writeEvent_) {
        CloseHandle(static_cast<HANDLE>(writeEvent_));
        writeEvent_ = nullptr;
    }
    if (readEvent_) {
        CloseHandle(static_cast<HANDLE>(readEvent_));
        readEvent_ = nullptr;
    }
}

void SuperlightHidClient::ensureEvents() {
    if (!writeEvent_) {
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (!readEvent_) {
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
}

uint8_t SuperlightHidClient::nextSwId() {
    swId_ = static_cast<uint8_t>((swId_ < 0x0F) ? (swId_ + 1) : 0x09);
    return swId_;
}

std::optional<SuperlightHidClient::CachedDevice> SuperlightHidClient::discoverOnOpenHandle(
    void* rawHandle,
    const HidInterfaceInfo& iface,
    uint8_t devIdx,
    bool isReceiver) {
    HANDLE handle = static_cast<HANDLE>(rawHandle);
    ensureEvents();
    HANDLE writeEv = static_cast<HANDLE>(writeEvent_);
    HANDLE readEv = static_cast<HANDLE>(readEvent_);

    CachedDevice dev;
    dev.iface = iface;
    dev.isReceiver = isReceiver;
    dev.deviceIndex = devIdx;
    dev.connection = isReceiver ? ConnectionType::LightspeedWireless : ConnectionType::UsbWired;
    dev.deviceName = iface.productString.empty() ? L"Logitech Wireless Mouse" : iface.productString;

    uint8_t nameFeatIdx = 0;
    if (queryFeatureIndexDirect(
            handle, iface, devIdx, kFeatureDeviceNameType, nextSwId(), writeEv, readEv, nameFeatIdx)) {
        queryDeviceNameAndType(
            handle, iface, devIdx, nameFeatIdx, swId_, writeEv, readEv, dev.deviceName, dev.category);
    }
    if (dev.category == DeviceCategory::Unknown) {
        if (containsIgnoreCase(dev.deviceName, L"mouse") ||
            isSuperlightPreferredName(dev.deviceName)) {
            dev.category = DeviceCategory::Mouse;
        }
    }

    const std::array<uint16_t, 3> batteryFeatures = {
        kFeatureUnifiedBattery,
        kFeatureBatteryLevelStatus,
        kFeatureBatteryVoltage,
    };

    for (uint16_t fid : batteryFeatures) {
        uint8_t fidx = 0;
        if (queryFeatureIndexDirect(
                handle, iface, devIdx, fid, nextSwId(), writeEv, readEv, fidx)) {
            dev.batteryFeatureId = fid;
            dev.batteryFeatureIndex = fidx;
            break;
        }
    }

    // Fallback via IFeatureSet (0x0001) enumeration if direct IRoot lookups did not match.
    if (dev.batteryFeatureIndex == 0) {
        uint8_t featSetIdx = 0;
        if (queryFeatureIndexDirect(
                handle, iface, devIdx, kFeatureIFeatureSet, nextSwId(), writeEv, readEv, featSetIdx)) {
            const auto countRes = transactHidpp(
                handle, iface, devIdx, featSetIdx, 0x00, nextSwId(), writeEv, readEv);
            if (countRes.status == TransactStatus::Ok && countRes.responseLen >= 5) {
                const uint8_t count = std::min<uint8_t>(countRes.response[4], 64);
                for (uint8_t idx = 1; idx <= count; ++idx) {
                    const auto fRes = transactHidpp(
                        handle, iface, devIdx, featSetIdx, 0x01, nextSwId(), writeEv, readEv, idx);
                    if (fRes.status == TransactStatus::Ok && fRes.responseLen >= 6) {
                        const uint16_t fid = static_cast<uint16_t>(
                            (static_cast<uint16_t>(fRes.response[4]) << 8) | fRes.response[5]);
                        if (fid == kFeatureUnifiedBattery ||
                            (fid == kFeatureBatteryLevelStatus && dev.batteryFeatureId != kFeatureUnifiedBattery) ||
                            (fid == kFeatureBatteryVoltage && dev.batteryFeatureId == 0)) {
                            dev.batteryFeatureId = fid;
                            dev.batteryFeatureIndex = idx;
                            if (fid == kFeatureUnifiedBattery) {
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (dev.batteryFeatureIndex == 0) {
        return std::nullopt;
    }

    bool asleep = false;
    const auto reading = queryBatteryReading(
        handle,
        iface,
        devIdx,
        dev.batteryFeatureId,
        dev.batteryFeatureIndex,
        nextSwId(),
        writeEv,
        readEv,
        asleep);

    dev.lastState.present = true;
    dev.lastState.connection = dev.connection;
    dev.lastState.category = dev.category;
    dev.lastState.deviceName = dev.deviceName;
    dev.lastState.featureUsed = dev.batteryFeatureId;

    if (reading.has_value()) {
        dev.lastState.sleeping = false;
        dev.lastState.percentage = reading->percentage;
        dev.lastState.charging = reading->charging || (dev.connection == ConnectionType::UsbWired);
        dev.lastState.full = reading->full;
        dev.lastState.voltageMv = reading->voltageMv;
    } else if (asleep) {
        dev.lastState.sleeping = true;
    }

    return dev;
}

std::optional<SuperlightHidClient::CachedDevice> SuperlightHidClient::probeDirectUsbDevice(
    const HidInterfaceInfo& iface,
    bool& outIsReceiverHint) {
    outIsReceiverHint = false;
    UniqueHandle handle = openHidReadWrite(iface.devicePath);
    if (!handle.valid()) {
        return std::nullopt;
    }

    ensureEvents();
    HANDLE writeEv = static_cast<HANDLE>(writeEvent_);
    HANDLE readEv = static_cast<HANDLE>(readEvent_);

    const auto pingFF = transactHidpp(
        handle.get(), iface, kDevIdxDirectUsb, 0x00, 0x01, nextSwId(), writeEv, readEv, 0, 0, 0xAA, 120);
    if (pingFF.status == TransactStatus::Ok) {
        return discoverOnOpenHandle(handle.get(), iface, kDevIdxDirectUsb, false);
    }
    if (pingFF.status == TransactStatus::Hidpp10Error) {
        // Wireless receivers reject devIdx 0xFF immediately with a HID++ 1.0 0x8F error.
        outIsReceiverHint = true;
        return std::nullopt;
    }

    const auto ping00 = transactHidpp(
        handle.get(), iface, kDevIdxDirectUsbAlt, 0x00, 0x01, nextSwId(), writeEv, readEv, 0, 0, 0xAA, 100);
    if (ping00.status == TransactStatus::Ok) {
        return discoverOnOpenHandle(handle.get(), iface, kDevIdxDirectUsbAlt, false);
    }

    return std::nullopt;
}

std::vector<SuperlightHidClient::CachedDevice> SuperlightHidClient::probeReceiverSlots(
    const HidInterfaceInfo& iface,
    bool multiDeviceMode) {
    std::vector<CachedDevice> found;
    UniqueHandle handle = openHidReadWrite(iface.devicePath);
    if (!handle.valid()) {
        return found;
    }

    ensureEvents();
    HANDLE writeEv = static_cast<HANDLE>(writeEvent_);
    HANDLE readEv = static_cast<HANDLE>(readEvent_);

    bool slot1Asleep = false;
    bool foundActiveMouse = false;

    for (uint8_t slot = kDevIdxReceiverSlot1; slot <= kDevIdxMaxReceiverSlot; ++slot) {
        const auto ping = transactHidpp(
            handle.get(), iface, slot, 0x00, 0x01, nextSwId(), writeEv, readEv, 0, 0, 0xAA, 150);
        if (ping.status != TransactStatus::Ok) {
            if (slot == kDevIdxReceiverSlot1 && isTransactAsleep(ping)) {
                slot1Asleep = true;
            }
            continue;
        }

        auto dev = discoverOnOpenHandle(handle.get(), iface, slot, true);
        if (!dev.has_value()) {
            continue;
        }

        const bool isMouse = (dev->category == DeviceCategory::Mouse);
        if (isMouse) {
            foundActiveMouse = true;
        }
        const bool preferredSuperlight = isMouse && isSuperlightPreferredName(dev->deviceName);
        found.push_back(std::move(*dev));

        if (!multiDeviceMode && (preferredSuperlight || (isMouse && slot == kDevIdxReceiverSlot1))) {
            break;
        }
    }

    if (!foundActiveMouse && slot1Asleep) {
        CachedDevice stub;
        stub.iface = iface;
        stub.isReceiver = true;
        stub.deviceIndex = kDevIdxReceiverSlot1;
        stub.category = DeviceCategory::Mouse;
        stub.connection = ConnectionType::LightspeedWireless;
        stub.deviceName = iface.productString.empty() ? L"Logitech Wireless Mouse" : iface.productString;
        stub.lastState.present = true;
        stub.lastState.sleeping = true;
        stub.lastState.percentage = -1;
        stub.lastState.connection = stub.connection;
        stub.lastState.category = stub.category;
        stub.lastState.deviceName = stub.deviceName;
        found.push_back(std::move(stub));
    }

    return found;
}

bool SuperlightHidClient::refreshCachedDevice(CachedDevice& dev) {
    UniqueHandle handle = openHidReadWrite(dev.iface.devicePath);
    if (!handle.valid()) {
        return false;
    }

    ensureEvents();
    HANDLE writeEv = static_cast<HANDLE>(writeEvent_);
    HANDLE readEv = static_cast<HANDLE>(readEvent_);

    // Complete feature discovery if this device was initially cached while deep-sleeping.
    if (dev.batteryFeatureIndex == 0) {
        const auto ping = transactHidpp(
            handle.get(), dev.iface, dev.deviceIndex, 0x00, 0x01, nextSwId(), writeEv, readEv, 0, 0, 0xAA, 150);
        if (ping.status != TransactStatus::Ok) {
            return isTransactAsleep(ping);
        }
        auto discovered = discoverOnOpenHandle(handle.get(), dev.iface, dev.deviceIndex, dev.isReceiver);
        if (!discovered.has_value()) {
            return true;
        }
        const int prevPct = dev.lastState.percentage;
        dev = std::move(*discovered);
        if (dev.lastState.sleeping && dev.lastState.percentage < 0 && prevPct >= 0) {
            dev.lastState.percentage = prevPct;
        }
        return true;
    }

    bool asleep = false;
    const auto reading = queryBatteryReading(
        handle.get(),
        dev.iface,
        dev.deviceIndex,
        dev.batteryFeatureId,
        dev.batteryFeatureIndex,
        nextSwId(),
        writeEv,
        readEv,
        asleep);

    if (reading.has_value()) {
        dev.lastState.present = true;
        dev.lastState.sleeping = false;
        dev.lastState.percentage = reading->percentage;
        dev.lastState.charging = reading->charging || (dev.connection == ConnectionType::UsbWired);
        dev.lastState.full = reading->full;
        dev.lastState.voltageMv = reading->voltageMv;
        dev.lastState.featureUsed = dev.batteryFeatureId;
        dev.lastState.connection = dev.connection;
        dev.lastState.deviceName = dev.deviceName;
        return true;
    }

    if (asleep) {
        // Keep last known percentage cached/dimmed when sleeping.
        dev.lastState.present = true;
        dev.lastState.sleeping = true;
        dev.lastState.charging = false;
        return true;
    }

    return false;
}

void SuperlightHidClient::discoverAllDevices(
    const std::vector<HidInterfaceInfo>& interfaces,
    bool multiDeviceMode) {
    std::set<std::wstring> activeParentKeys;

    auto assignSlot = [&](const CachedDevice& dev) {
        if (dev.category == DeviceCategory::Mouse || dev.category == DeviceCategory::Unknown) {
            if (!primaryMouse_.has_value()) {
                primaryMouse_ = dev;
                return;
            }
            // Prefer an active wired or Superlight mouse over a sleeping/generic fallback.
            const bool curWired = (primaryMouse_->connection == ConnectionType::UsbWired &&
                                   !primaryMouse_->lastState.sleeping);
            const bool newWired = (dev.connection == ConnectionType::UsbWired &&
                                   !dev.lastState.sleeping);
            const bool curSuperlight = isSuperlightPreferredName(primaryMouse_->deviceName);
            const bool newSuperlight = isSuperlightPreferredName(dev.deviceName);
            if ((!curWired && newWired) ||
                (primaryMouse_->lastState.sleeping && !dev.lastState.sleeping) ||
                (!curSuperlight && newSuperlight && !dev.lastState.sleeping)) {
                primaryMouse_ = dev;
            }
        } else if (multiDeviceMode) {
            if (dev.category == DeviceCategory::Headset && !cachedHeadset_.has_value()) {
                cachedHeadset_ = dev;
            } else if (dev.category == DeviceCategory::Controller && !cachedController_.has_value()) {
                cachedController_ = dev;
            } else if (dev.category == DeviceCategory::Keyboard && !cachedKeyboard_.has_value()) {
                cachedKeyboard_ = dev;
            }
        }
    };

    // Pass 1: Dynamically probe direct wired USB devices first (0xFF / 0x00).
    for (const auto& iface : interfaces) {
        if (activeParentKeys.contains(iface.parentKey)) {
            continue;
        }
        bool isReceiverHint = false;
        auto wired = probeDirectUsbDevice(iface, isReceiverHint);
        if (wired.has_value()) {
            activeParentKeys.insert(iface.parentKey);
            knownNonWiredParentKeys_.erase(iface.parentKey);
            assignSlot(*wired);
        } else if (isReceiverHint) {
            knownNonWiredParentKeys_.insert(iface.parentKey);
        }
    }

    if (!multiDeviceMode && primaryMouse_.has_value() &&
        primaryMouse_->connection == ConnectionType::UsbWired &&
        !primaryMouse_->lastState.sleeping) {
        return;
    }

    // Pass 2: Probe wireless receiver slots (0x01..0x06) on interfaces that did not answer as direct USB.
    for (const auto& iface : interfaces) {
        if (activeParentKeys.contains(iface.parentKey)) {
            continue;
        }
        auto devs = probeReceiverSlots(iface, multiDeviceMode);
        if (!devs.empty()) {
            activeParentKeys.insert(iface.parentKey);
            knownNonWiredParentKeys_.insert(iface.parentKey);
            for (const auto& d : devs) {
                assignSlot(d);
            }
        }
    }
}

AppSnapshot SuperlightHidClient::poll(bool multiDeviceMode, bool topologyChanged) {
    bool refreshedPrimaryFast = false;

    // Steady-state zero-SetupDi fast path: open cached device path directly and read 1 packet.
    if (!topologyChanged && primaryMouse_.has_value() && !primaryMouse_->lastState.sleeping &&
        (!multiDeviceMode ||
         ((!cachedHeadset_.has_value() || !cachedHeadset_->lastState.sleeping) &&
          (!cachedController_.has_value() || !cachedController_->lastState.sleeping) &&
          (!cachedKeyboard_.has_value() || !cachedKeyboard_->lastState.sleeping)))) {
        if (refreshCachedDevice(*primaryMouse_)) {
            refreshedPrimaryFast = true;
            if (!primaryMouse_->lastState.sleeping) {
                if (multiDeviceMode) {
                    if (cachedHeadset_.has_value() && !refreshCachedDevice(*cachedHeadset_)) {
                        cachedHeadset_.reset();
                    }
                    if (cachedController_.has_value() && !refreshCachedDevice(*cachedController_)) {
                        cachedController_.reset();
                    }
                    if (cachedKeyboard_.has_value() && !refreshCachedDevice(*cachedKeyboard_)) {
                        cachedKeyboard_.reset();
                    }
                } else {
                    cachedHeadset_.reset();
                    cachedController_.reset();
                    cachedKeyboard_.reset();
                }

                if (primaryMouse_->lastState.percentage >= 0) {
                    lastKnownMouseState_ = primaryMouse_->lastState;
                }

                AppSnapshot fastSnap;
                fastSnap.primaryMouse = primaryMouse_->lastState;
                if (multiDeviceMode) {
                    fastSnap.headset = cachedHeadset_.has_value()
                                           ? cachedHeadset_->lastState
                                           : DeviceBatteryState{.category = DeviceCategory::Headset};
                    fastSnap.controller = cachedController_.has_value()
                                              ? cachedController_->lastState
                                              : DeviceBatteryState{.category = DeviceCategory::Controller};
                    fastSnap.keyboard = cachedKeyboard_.has_value()
                                            ? cachedKeyboard_->lastState
                                            : DeviceBatteryState{.category = DeviceCategory::Keyboard};
                }
                return fastSnap;
            }
        } else {
            primaryMouse_.reset();
        }
    }

    const auto interfaces = enumerateLogitechHidppInterfaces();

    if (interfaces.empty()) {
        primaryMouse_.reset();
        cachedHeadset_.reset();
        cachedController_.reset();
        cachedKeyboard_.reset();
        knownNonWiredParentKeys_.clear();
        lastKnownMouseState_ = DeviceBatteryState{};

        AppSnapshot emptySnap;
        emptySnap.primaryMouse.present = false;
        emptySnap.primaryMouse.category = DeviceCategory::Mouse;
        if (multiDeviceMode) {
            emptySnap.headset.category = DeviceCategory::Headset;
            emptySnap.controller.category = DeviceCategory::Controller;
            emptySnap.keyboard.category = DeviceCategory::Keyboard;
        }
        return emptySnap;
    }

    // Prune parent keys that are no longer attached so a re-plugged USB cable is probed cleanly.
    std::set<std::wstring> currentParents;
    for (const auto& iface : interfaces) {
        currentParents.insert(iface.parentKey);
    }
    for (auto it = knownNonWiredParentKeys_.begin(); it != knownNonWiredParentKeys_.end();) {
        if (!currentParents.contains(*it)) {
            it = knownNonWiredParentKeys_.erase(it);
        } else {
            ++it;
        }
    }

    auto findIface = [&](const std::wstring& path) -> const HidInterfaceInfo* {
        for (const auto& iface : interfaces) {
            if (iface.devicePath == path) {
                return &iface;
            }
        }
        return nullptr;
    };

    // Check newly attached interfaces for direct USB wired charging when tracking wireless mouse.
    if (primaryMouse_.has_value() &&
        primaryMouse_->connection == ConnectionType::LightspeedWireless) {
        for (const auto& iface : interfaces) {
            if (knownNonWiredParentKeys_.contains(iface.parentKey)) {
                continue;
            }
            bool isReceiverHint = false;
            auto wired = probeDirectUsbDevice(iface, isReceiverHint);
            if (wired.has_value() && !wired->lastState.sleeping) {
                primaryMouse_ = std::move(wired);
                refreshedPrimaryFast = true;
                break;
            }
            knownNonWiredParentKeys_.insert(iface.parentKey);
        }
    }

    if (primaryMouse_.has_value() && !refreshedPrimaryFast) {
        const HidInterfaceInfo* iface = findIface(primaryMouse_->iface.devicePath);
        if (iface != nullptr) {
            primaryMouse_->iface = *iface;
            if (!refreshCachedDevice(*primaryMouse_)) {
                primaryMouse_.reset();
            }
        } else {
            primaryMouse_.reset();
        }
    }

    auto refreshExtra = [&](std::optional<CachedDevice>& slot) {
        if (!multiDeviceMode) {
            slot.reset();
            return;
        }
        if (slot.has_value()) {
            const HidInterfaceInfo* iface = findIface(slot->iface.devicePath);
            if (iface != nullptr) {
                slot->iface = *iface;
                if (!refreshCachedDevice(*slot)) {
                    slot.reset();
                }
            } else {
                slot.reset();
            }
        }
    };
    refreshExtra(cachedHeadset_);
    refreshExtra(cachedController_);
    refreshExtra(cachedKeyboard_);

    if (!primaryMouse_.has_value() ||
        (multiDeviceMode && (!cachedHeadset_.has_value() || !cachedController_.has_value() ||
                             !cachedKeyboard_.has_value()))) {
        discoverAllDevices(interfaces, multiDeviceMode);
    }

    if (primaryMouse_.has_value()) {
        if (primaryMouse_->lastState.percentage >= 0) {
            lastKnownMouseState_ = primaryMouse_->lastState;
        } else if (primaryMouse_->lastState.sleeping && lastKnownMouseState_.percentage >= 0) {
            primaryMouse_->lastState.percentage = lastKnownMouseState_.percentage;
            if (!lastKnownMouseState_.deviceName.empty()) {
                primaryMouse_->lastState.deviceName = lastKnownMouseState_.deviceName;
            }
        }
    }

    AppSnapshot snap;
    if (primaryMouse_.has_value()) {
        snap.primaryMouse = primaryMouse_->lastState;
    } else {
        snap.primaryMouse.present = false;
        snap.primaryMouse.category = DeviceCategory::Mouse;
    }

    if (multiDeviceMode) {
        snap.headset = cachedHeadset_.has_value()
                           ? cachedHeadset_->lastState
                           : DeviceBatteryState{.category = DeviceCategory::Headset};
        snap.controller = cachedController_.has_value()
                              ? cachedController_->lastState
                              : DeviceBatteryState{.category = DeviceCategory::Controller};
        snap.keyboard = cachedKeyboard_.has_value()
                            ? cachedKeyboard_->lastState
                            : DeviceBatteryState{.category = DeviceCategory::Keyboard};
    }

    return snap;
}

void runHardwareProbe(std::wostream& out) {
    out << L"=== Logitech HID++ Hardware Probe ===\n";
    const auto interfaces = enumerateLogitechHidppInterfaces();
    if (interfaces.empty()) {
        out << L"No Logitech HID++ interfaces (VID=0x046D, UsagePage 0xFF00/0xFF43) found.\n";
        return;
    }

    out << L"Found " << interfaces.size() << L" Logitech HID++ interface(s):\n";
    uint8_t swId = 0x09;
    auto nextId = [&]() -> uint8_t {
        swId = static_cast<uint8_t>((swId < 0x0F) ? (swId + 1) : 0x09);
        return swId;
    };

    UniqueHandle writeEv(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle readEv(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    for (size_t i = 0; i < interfaces.size(); ++i) {
        const auto& iface = interfaces[i];
        out << L"\n[" << i << L"] PID=0x" << std::hex << std::setw(4) << std::setfill(L'0')
            << iface.productId << L" UsagePage=0x" << std::setw(4) << iface.usagePage
            << L" Usage=0x" << std::setw(4) << iface.usage << std::dec
            << L" InLen=" << iface.inputReportLen << L" OutLen=" << iface.outputReportLen
            << L" Long=" << (iface.supportsLong ? L"yes" : L"no")
            << L" Short=" << (iface.supportsShort ? L"yes" : L"no") << L"\n";
        if (!iface.productString.empty()) {
            out << L"    Product: " << iface.productString << L"\n";
        }
        out << L"    Path:    " << iface.devicePath << L"\n";

        UniqueHandle h = openHidReadWrite(iface.devicePath);
        if (!h.valid()) {
            out << L"    Open failed (GetLastError=" << GetLastError() << L")\n";
            continue;
        }

        auto printDeviceDetails = [&](uint8_t devIdx, const TransactResult& ping, const wchar_t* modeTag) {
            out << L"    devIdx=0x" << std::hex << std::setw(2) << std::setfill(L'0')
                << static_cast<int>(devIdx) << std::dec << L" (" << modeTag << L"): HID++ "
                << static_cast<int>(ping.response[4]) << L"."
                << static_cast<int>(ping.response[5]) << L"\n";

            uint8_t nameFeatIdx = 0;
            std::wstring name;
            DeviceCategory cat = DeviceCategory::Unknown;
            if (queryFeatureIndexDirect(
                    h.get(), iface, devIdx, kFeatureDeviceNameType, nextId(), writeEv.get(), readEv.get(), nameFeatIdx)) {
                queryDeviceNameAndType(
                    h.get(), iface, devIdx, nameFeatIdx, swId, writeEv.get(), readEv.get(), name, cat);
            }
            if (!name.empty()) {
                const wchar_t* catLabel = L"Unknown";
                switch (cat) {
                    case DeviceCategory::Mouse: catLabel = L"Mouse"; break;
                    case DeviceCategory::Headset: catLabel = L"Headset"; break;
                    case DeviceCategory::Controller: catLabel = L"Controller"; break;
                    case DeviceCategory::Keyboard: catLabel = L"Keyboard"; break;
                    case DeviceCategory::Unknown:
                    default: break;
                }
                out << L"      Name: " << name << L" (Category=" << catLabel << L")\n";
            }

            uint8_t featSetIdx = 0;
            if (queryFeatureIndexDirect(
                    h.get(), iface, devIdx, kFeatureIFeatureSet, nextId(), writeEv.get(), readEv.get(), featSetIdx)) {
                const auto cntRes = transactHidpp(
                    h.get(), iface, devIdx, featSetIdx, 0x00, nextId(), writeEv.get(), readEv.get());
                if (cntRes.status == TransactStatus::Ok && cntRes.responseLen >= 5) {
                    out << L"      IFeatureSet count: " << static_cast<int>(cntRes.response[4]) << L"\n";
                }
            }

            for (uint16_t fid : {kFeatureUnifiedBattery, kFeatureBatteryLevelStatus, kFeatureBatteryVoltage}) {
                uint8_t fidx = 0;
                if (queryFeatureIndexDirect(
                        h.get(), iface, devIdx, fid, nextId(), writeEv.get(), readEv.get(), fidx)) {
                    bool asleep = false;
                    const auto reading = queryBatteryReading(
                        h.get(), iface, devIdx, fid, fidx, nextId(), writeEv.get(), readEv.get(), asleep);
                    out << L"      Feature 0x" << std::hex << std::setw(4) << std::setfill(L'0')
                        << fid << std::dec << L" (idx=" << static_cast<int>(fidx) << L"): ";
                    if (reading.has_value()) {
                        out << reading->percentage << L"% charging="
                            << (reading->charging ? L"true" : L"false")
                            << L" full=" << (reading->full ? L"true" : L"false");
                        if (reading->voltageMv > 0) {
                            out << L" voltage=" << reading->voltageMv << L"mV";
                        }
                        out << L"\n";
                    } else {
                        out << L"read failed (asleep=" << (asleep ? L"true" : L"false") << L")\n";
                    }
                }
            }
        };

        // 1. Dynamic direct USB wired probe (0xFF / 0x00)
        const auto pingFF = transactHidpp(
            h.get(), iface, kDevIdxDirectUsb, 0x00, 0x01, nextId(), writeEv.get(), readEv.get(), 0, 0, 0xAA, 120);
        if (pingFF.status == TransactStatus::Ok) {
            printDeviceDetails(kDevIdxDirectUsb, pingFF, L"USB Wired");
            continue;
        }
        if (pingFF.status != TransactStatus::Hidpp10Error) {
            const auto ping00 = transactHidpp(
                h.get(), iface, kDevIdxDirectUsbAlt, 0x00, 0x01, nextId(), writeEv.get(), readEv.get(), 0, 0, 0xAA, 100);
            if (ping00.status == TransactStatus::Ok) {
                printDeviceDetails(kDevIdxDirectUsbAlt, ping00, L"USB Wired");
                continue;
            }
        }

        // 2. Dynamic receiver slot probe (0x01..0x06)
        for (uint8_t devIdx = kDevIdxReceiverSlot1; devIdx <= kDevIdxMaxReceiverSlot; ++devIdx) {
            const auto ping = transactHidpp(
                h.get(), iface, devIdx, 0x00, 0x01, nextId(), writeEv.get(), readEv.get(), 0, 0, 0xAA, 150);
            if (ping.status == TransactStatus::Ok) {
                printDeviceDetails(devIdx, ping, L"Receiver");
            } else if (devIdx <= 0x02) {
                out << L"    devIdx=0x" << std::hex << std::setw(2) << std::setfill(L'0')
                    << static_cast<int>(devIdx) << std::dec << L": "
                    << (isTransactAsleep(ping) ? L"ASLEEP / unreachable" : L"no response")
                    << L" (status=" << static_cast<int>(ping.status)
                    << L", err=0x" << std::hex << static_cast<int>(ping.errorCode) << std::dec << L")\n";
            }
        }
    }
}

}  // namespace slb
