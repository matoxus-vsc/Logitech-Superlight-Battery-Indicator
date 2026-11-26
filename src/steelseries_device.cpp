#include "steelseries_device.hpp"

#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>

namespace slb {

namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : handle_(h) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

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

bool sendSteelSeriesCommand(
    HANDLE handle,
    const SteelSeriesInterfaceInfo& iface,
    SteelSeriesBatteryKind kind,
    uint8_t command,
    HANDLE writeEvent) {
    constexpr size_t kStackSize = 65;
    std::array<uint8_t, kStackSize> stackBuf{};
    std::vector<uint8_t> heapBuf;

    auto fillPayload = [&](uint8_t* ptr, size_t len) {
        std::fill_n(ptr, len, uint8_t{0});
        if (len >= 2) {
            ptr[0] = 0x00;  // Report ID
            ptr[1] = command;
            if (kind == SteelSeriesBatteryKind::Rival3Or650 && len >= 3) {
                ptr[2] = 0x01;
            }
        }
    };

    if (iface.outputReportLen >= 2) {
        const size_t outLen = iface.outputReportLen;
        uint8_t* outPtr = nullptr;
        if (outLen <= kStackSize) {
            outPtr = stackBuf.data();
        } else {
            heapBuf.resize(outLen);
            outPtr = heapBuf.data();
        }
        fillPayload(outPtr, outLen);

        ResetEvent(writeEvent);
        OVERLAPPED writeOv{};
        writeOv.hEvent = writeEvent;
        DWORD bytesWritten = 0;
        BOOL writeOk = WriteFile(
            handle, outPtr, static_cast<DWORD>(outLen), &bytesWritten, &writeOv);
        if (writeOk) {
            return true;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(writeEvent, 200) == WAIT_OBJECT_0 &&
                GetOverlappedResult(handle, &writeOv, &bytesWritten, FALSE)) {
                return true;
            }
            CancelIoEx(handle, &writeOv);
            GetOverlappedResult(handle, &writeOv, &bytesWritten, TRUE);
        }
    }

    if (iface.featureReportLen >= 2) {
        const size_t featLen = iface.featureReportLen;
        uint8_t* featPtr = nullptr;
        if (featLen <= kStackSize) {
            featPtr = stackBuf.data();
        } else {
            heapBuf.resize(featLen);
            featPtr = heapBuf.data();
        }
        fillPayload(featPtr, featLen);
        if (HidD_SetFeature(handle, featPtr, static_cast<ULONG>(featLen))) {
            return true;
        }
    }

    return false;
}

std::optional<BatteryReading> decodeSteelSeriesPacket(
    std::span<const uint8_t> pkt,
    SteelSeriesBatteryKind kind,
    uint8_t command) {
    switch (kind) {
        case SteelSeriesBatteryKind::AeroxPrime:
            if (!isMatchingSteelSeriesCommandResponse(pkt, command)) {
                return std::nullopt;
            }
            return decodeSteelSeriesAeroxPrime(pkt);

        case SteelSeriesBatteryKind::DirectAerox:
            if (!isMatchingSteelSeriesCommandResponse(pkt, command)) {
                return std::nullopt;
            }
            return decodeSteelSeriesDirectAerox(pkt);

        case SteelSeriesBatteryKind::Rival3Or650:
            return decodeSteelSeriesRival3Or650(pkt);
    }
    return std::nullopt;
}

}  // namespace

std::vector<SteelSeriesInterfaceInfo> enumerateSteelSeriesVendorInterfaces() {
    std::vector<SteelSeriesInterfaceInfo> results;

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
        if (!HidD_GetAttributes(queryHandle.get(), &attrs) || attrs.VendorID != kSteelSeriesVid) {
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

        // Only open vendor-defined HID collections (>= 0xFF00); never touch OS mouse polling
        // collections (0x01/0x02) so high-rate gaming mice experience zero interference.
        if (caps.UsagePage < 0xFF00) {
            continue;
        }
        if (caps.InputReportByteLength < 2 ||
            (caps.OutputReportByteLength < 2 && caps.FeatureReportByteLength < 2)) {
            continue;
        }

        SteelSeriesInterfaceInfo info;
        info.devicePath = path;
        info.productId = attrs.ProductID;
        info.usagePage = caps.UsagePage;
        info.usage = caps.Usage;
        info.interfaceNumber = parseWindowsHidInterfaceNumber(path);
        info.inputReportLen = caps.InputReportByteLength;
        info.outputReportLen = caps.OutputReportByteLength;
        info.featureReportLen = caps.FeatureReportByteLength;

        wchar_t prodBuf[128]{};
        if (HidD_GetProductString(queryHandle.get(), prodBuf, sizeof(prodBuf))) {
            info.productString = prodBuf;
        }

        results.push_back(std::move(info));
    }

    // Prioritize wired charging profiles ahead of wireless dongles so plugging in the USB cable
    // takes precedence immediately.
    std::stable_sort(results.begin(), results.end(), [](const SteelSeriesInterfaceInfo& a, const SteelSeriesInterfaceInfo& b) {
        const auto pa = findSteelSeriesWirelessProfile(a.productId);
        const auto pb = findSteelSeriesWirelessProfile(b.productId);
        const int rankA = pa.has_value() ? (pa->isWired ? 2 : 1) : 0;
        const int rankB = pb.has_value() ? (pb->isWired ? 2 : 1) : 0;
        if (rankA != rankB) {
            return rankA > rankB;
        }
        return a.outputReportLen > b.outputReportLen;
    });

    return results;
}

SteelSeriesHidClient::SteelSeriesHidClient() {
    ensureEvents();
}

SteelSeriesHidClient::~SteelSeriesHidClient() {
    if (writeEvent_) {
        CloseHandle(static_cast<HANDLE>(writeEvent_));
        writeEvent_ = nullptr;
    }
    if (readEvent_) {
        CloseHandle(static_cast<HANDLE>(readEvent_));
        readEvent_ = nullptr;
    }
}

void SteelSeriesHidClient::ensureEvents() {
    if (!writeEvent_) {
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (!readEvent_) {
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
}

std::optional<BatteryReading> SteelSeriesHidClient::queryInterface(
    void* rawHandle,
    const SteelSeriesInterfaceInfo& iface,
    SteelSeriesBatteryKind kind,
    uint8_t command) {
    HANDLE handle = static_cast<HANDLE>(rawHandle);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return std::nullopt;
    }

    ensureEvents();
    HANDLE writeEv = static_cast<HANDLE>(writeEvent_);
    HANDLE readEv = static_cast<HANDLE>(readEvent_);
    if (!writeEv || !readEv) {
        return std::nullopt;
    }

    constexpr size_t kStackSize = 65;
    std::array<uint8_t, kStackSize> inStack{};
    std::vector<uint8_t> inHeap;
    const size_t readBufSize = std::max<size_t>(iface.inputReportLen, 8);
    uint8_t* inPtr = nullptr;
    if (readBufSize <= kStackSize) {
        inPtr = inStack.data();
    } else {
        inHeap.resize(readBufSize);
        inPtr = inHeap.data();
    }

    for (int writeAttempt = 0; writeAttempt < 2; ++writeAttempt) {
        HidD_FlushQueue(handle);
        if (!sendSteelSeriesCommand(handle, iface, kind, command, writeEv)) {
            continue;
        }

        const ULONGLONG deadline = GetTickCount64() + 450;
        for (int pktIdx = 0; pktIdx < 64; ++pktIdx) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                break;
            }
            const DWORD remainingMs = std::min<DWORD>(static_cast<DWORD>(deadline - now), 120);

            ResetEvent(readEv);
            OVERLAPPED readOv{};
            readOv.hEvent = readEv;
            std::fill_n(inPtr, readBufSize, uint8_t{0});
            DWORD bytesRead = 0;

            BOOL readOk = ReadFile(
                handle, inPtr, static_cast<DWORD>(readBufSize), &bytesRead, &readOv);
            if (!readOk) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    break;
                }
                const DWORD waitRes = WaitForSingleObject(readEv, remainingMs);
                if (waitRes != WAIT_OBJECT_0) {
                    CancelIoEx(handle, &readOv);
                    GetOverlappedResult(handle, &readOv, &bytesRead, TRUE);
                    continue;
                }
                if (!GetOverlappedResult(handle, &readOv, &bytesRead, FALSE)) {
                    break;
                }
            }

            if (bytesRead >= 2) {
                const auto decoded = decodeSteelSeriesPacket(
                    std::span<const uint8_t>(inPtr, bytesRead), kind, command);
                if (decoded.has_value()) {
                    return decoded;
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<BatteryReading> SteelSeriesHidClient::queryGenericFallback(
    void* handle,
    const SteelSeriesInterfaceInfo& iface,
    SteelSeriesBatteryKind& outKind,
    uint8_t& outCommand) {
    struct ProbeAttempt {
        SteelSeriesBatteryKind kind;
        uint8_t command;
    };
    constexpr std::array<ProbeAttempt, 5> kAttempts = {{
        {SteelSeriesBatteryKind::DirectAerox, 0xD2},
        {SteelSeriesBatteryKind::DirectAerox, 0x92},
        {SteelSeriesBatteryKind::AeroxPrime, 0xD2},
        {SteelSeriesBatteryKind::AeroxPrime, 0x92},
        {SteelSeriesBatteryKind::Rival3Or650, 0xAA},
    }};

    for (const auto& att : kAttempts) {
        auto reading = queryInterface(handle, iface, att.kind, att.command);
        if (reading.has_value()) {
            outKind = att.kind;
            outCommand = att.command;
            return reading;
        }
    }
    return std::nullopt;
}

DeviceBatteryState SteelSeriesHidClient::poll(bool topologyChanged) {
    auto makeState = [&](const CachedDevice& dev, const BatteryReading& r) {
        DeviceBatteryState st;
        st.present = true;
        st.sleeping = false;
        st.percentage = r.percentage;
        st.charging = r.charging || dev.isWired;
        st.full = r.full;
        st.connection = dev.isWired ? ConnectionType::UsbWired : ConnectionType::LightspeedWireless;
        st.category = DeviceCategory::Mouse;
        st.deviceName = dev.deviceName;
        return st;
    };

    // Fast steady-state path: poll cached interface directly without SetupDi enumeration.
    if (!topologyChanged && cachedDevice_.has_value()) {
        UniqueHandle h = openHidReadWrite(cachedDevice_->iface.devicePath);
        if (h.valid()) {
            const auto reading = queryInterface(
                h.get(), cachedDevice_->iface, cachedDevice_->kind, cachedDevice_->command);
            if (reading.has_value()) {
                DeviceBatteryState st = makeState(*cachedDevice_, *reading);
                lastKnownState_ = st;
                lastKnownPid_ = cachedDevice_->iface.productId;
                return st;
            }
        }
        cachedDevice_.reset();
    }

    const auto interfaces = enumerateSteelSeriesVendorInterfaces();
    if (interfaces.empty()) {
        cachedDevice_.reset();
        lastKnownState_.reset();
        lastKnownPid_ = 0;
        DeviceBatteryState empty;
        empty.present = false;
        empty.category = DeviceCategory::Mouse;
        return empty;
    }

    std::vector<uint16_t> foundWirelessPids;
    std::wstring sleepingName;

    // Pass 1: Known SteelSeries wireless/wired mouse profiles.
    for (const auto& iface : interfaces) {
        const auto prof = findSteelSeriesWirelessProfile(iface.productId);
        if (!prof.has_value()) {
            continue;
        }
        if (iface.interfaceNumber >= 0 && iface.interfaceNumber != static_cast<int>(prof->endpoint)) {
            continue;
        }

        if (std::find(foundWirelessPids.begin(), foundWirelessPids.end(), iface.productId) ==
            foundWirelessPids.end()) {
            foundWirelessPids.push_back(iface.productId);
        }
        if (sleepingName.empty()) {
            sleepingName = prof->name;
        }

        UniqueHandle h = openHidReadWrite(iface.devicePath);
        if (!h.valid()) {
            continue;
        }

        const auto reading = queryInterface(h.get(), iface, prof->kind, prof->command);
        if (reading.has_value()) {
            CachedDevice dev;
            dev.iface = iface;
            dev.deviceName = prof->name;
            dev.kind = prof->kind;
            dev.command = prof->command;
            dev.isWired = prof->isWired;
            cachedDevice_ = dev;

            DeviceBatteryState st = makeState(dev, *reading);
            lastKnownState_ = st;
            lastKnownPid_ = iface.productId;
            return st;
        }
    }

    // Pass 2: Generic fallback for unlisted SteelSeries wireless mice (never non-mouse devices).
    if (foundWirelessPids.empty()) {
        for (const auto& iface : interfaces) {
            if (isKnownNonWirelessSteelSeriesPid(iface.productId) ||
                isNonMouseSteelSeriesName(iface.productString)) {
                continue;
            }
            UniqueHandle h = openHidReadWrite(iface.devicePath);
            if (!h.valid()) {
                continue;
            }
            SteelSeriesBatteryKind kind = SteelSeriesBatteryKind::Rival3Or650;
            uint8_t cmd = 0xAA;
            const auto reading = queryGenericFallback(h.get(), iface, kind, cmd);
            if (reading.has_value()) {
                CachedDevice dev;
                dev.iface = iface;
                dev.deviceName = iface.productString.empty()
                                     ? L"SteelSeries Wireless Mouse"
                                     : iface.productString;
                dev.kind = kind;
                dev.command = cmd;
                dev.isWired = false;
                cachedDevice_ = dev;

                DeviceBatteryState st = makeState(dev, *reading);
                lastKnownState_ = st;
                lastKnownPid_ = iface.productId;
                return st;
            }
        }
    }

    // Deep-sleep resilience: preserve last known battery percentage while the receiver remains attached.
    if (!foundWirelessPids.empty()) {
        const bool sameReceiver =
            std::find(foundWirelessPids.begin(), foundWirelessPids.end(), lastKnownPid_) !=
            foundWirelessPids.end();
        if (sameReceiver && lastKnownState_.has_value()) {
            DeviceBatteryState sleeping = *lastKnownState_;
            sleeping.present = true;
            sleeping.sleeping = true;
            sleeping.charging = false;
            return sleeping;
        }

        DeviceBatteryState stub;
        stub.present = true;
        stub.sleeping = true;
        stub.percentage = -1;
        stub.connection = ConnectionType::LightspeedWireless;
        stub.category = DeviceCategory::Mouse;
        stub.deviceName = sleepingName.empty() ? L"SteelSeries Wireless Mouse" : sleepingName;
        return stub;
    }

    lastKnownState_.reset();
    lastKnownPid_ = 0;
    DeviceBatteryState offline;
    offline.present = false;
    offline.category = DeviceCategory::Mouse;
    return offline;
}

void runSteelSeriesHardwareProbe(std::wostream& out) {
    out << L"\n=== SteelSeries Wireless HID Hardware Probe ===\n";
    const auto interfaces = enumerateSteelSeriesVendorInterfaces();
    if (interfaces.empty()) {
        out << L"No SteelSeries vendor HID interfaces (VID=0x1038, UsagePage>=0xFF00) found.\n";
        return;
    }

    out << L"Found " << interfaces.size() << L" SteelSeries vendor HID interface(s):\n";
    SteelSeriesHidClient probeClient;

    for (size_t i = 0; i < interfaces.size(); ++i) {
        const auto& iface = interfaces[i];
        out << L"\n[" << i << L"] PID=0x" << std::hex << std::setw(4) << std::setfill(L'0')
            << iface.productId << L" UsagePage=0x" << std::setw(4) << iface.usagePage
            << L" Usage=0x" << std::setw(4) << iface.usage << std::dec
            << L" Iface=" << iface.interfaceNumber
            << L" In=" << iface.inputReportLen
            << L" Out=" << iface.outputReportLen
            << L" Feat=" << iface.featureReportLen << L"\n";
        if (!iface.productString.empty()) {
            out << L"    Product: " << iface.productString << L"\n";
        }
        out << L"    Path:    " << iface.devicePath << L"\n";

        const auto prof = findSteelSeriesWirelessProfile(iface.productId);
        if (prof.has_value()) {
            out << L"    Profile: " << prof->name << L" (expected iface="
                << static_cast<int>(prof->endpoint) << L", cmd=0x"
                << std::hex << std::setw(2) << std::setfill(L'0')
                << static_cast<int>(prof->command) << std::dec << L")\n";
        }

        UniqueHandle h = openHidReadWrite(iface.devicePath);
        if (!h.valid()) {
            out << L"    Open failed (GetLastError=" << GetLastError() << L")\n";
            continue;
        }

        if (prof.has_value()) {
            if (iface.interfaceNumber >= 0 && iface.interfaceNumber != static_cast<int>(prof->endpoint)) {
                out << L"    Skipped (interface mismatch)\n";
                continue;
            }
            const auto r = probeClient.queryInterface(h.get(), iface, prof->kind, prof->command);
            if (r.has_value()) {
                out << L"    Battery: " << r->percentage << L"% charging="
                    << (r->charging ? L"true" : L"false") << L"\n";
            } else {
                out << L"    Battery: ASLEEP / no reply\n";
            }
        } else if (!isKnownNonWirelessSteelSeriesPid(iface.productId) &&
                   !isNonMouseSteelSeriesName(iface.productString)) {
            SteelSeriesBatteryKind kind = SteelSeriesBatteryKind::Rival3Or650;
            uint8_t cmd = 0xAA;
            const auto r = probeClient.queryGenericFallback(h.get(), iface, kind, cmd);
            if (r.has_value()) {
                out << L"    Generic Battery (cmd=0x" << std::hex << static_cast<int>(cmd)
                    << std::dec << L"): " << r->percentage << L"% charging="
                    << (r->charging ? L"true" : L"false") << L"\n";
            } else {
                out << L"    Generic fallback: no reply\n";
            }
        }
    }
}

}  // namespace slb
