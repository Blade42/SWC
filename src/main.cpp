#include "core.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "resource.h"

namespace {

struct Failure {
    std::wstring message;
};

constexpr wchar_t kResidentMutexName[] = L"Local\\SimpleWallpaperChanger.Resident";
constexpr wchar_t kResidentWindowClass[] = L"SimpleWallpaperChangerResidentWindow";
constexpr wchar_t kSettingsWindowClass[] = L"SimpleWallpaperChangerSettingsWindow";
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kWallpaperChangeCompleteMessage = WM_APP + 2;
constexpr UINT kPlaylistRefreshMessage = WM_APP + 3;
constexpr UINT kTrayIconId = 1;
constexpr UINT kNextCommandId = 1001;
constexpr UINT kPreviousCommandId = 1002;
constexpr UINT kExitCommandId = 1003;
constexpr UINT kSettingsCommandId = 1004;
constexpr UINT_PTR kWorkerPollTimerId = 1;
constexpr int kSettingsTabsId = 2001;
constexpr int kChangeOnStartCheckboxId = 2002;
constexpr int kSourceDirectoryListId = 2003;
constexpr int kAddSourceDirectoryButtonId = 2004;
constexpr int kCloseSettingsButtonId = 2005;
constexpr int kRemoveSourceDirectoryButtonId = 2006;
constexpr int kSourceDirectoryHintId = 2007;
constexpr int kPlaylistListId = 2008;
constexpr int kPlaylistHintId = 2009;
constexpr int kMonitorPreviewPanelId = 2010;
constexpr UINT kSetDisplayCommandBase = 40000;
constexpr int kPlaylistThumbnailSize = 64;
constexpr std::size_t kPlaylistThumbnailCacheLimit = 512;
constexpr int kSettingsContentWidth = 968;
constexpr int kMonitorPreviewGap = 12;
constexpr wchar_t kMonitorPreviewPanelClass[] = L"SWCMonitorPreviewPanel";
constexpr wchar_t kPlaylistDragGhostClass[] = L"SWCPlaylistDragGhost";
constexpr int kPlaylistDragGhostWidth = 110;
constexpr int kPlaylistDragGhostHeight = 104;
std::mutex gSettingsMutex;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (value_) {
            value_->Release();
        }
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }

    T* Get() const {
        return value_;
    }

    T** Put() {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
        return &value_;
    }

    T* operator->() const {
        return value_;
    }

private:
    T* value_ = nullptr;
};

[[noreturn]] void Fail(const std::wstring& message) {
    throw Failure{message};
}

std::wstring HResultText(HRESULT result) {
    std::wostringstream output;
    output << L"0x" << std::hex << std::uppercase
        << static_cast<unsigned long>(result);
    return output.str();
}

void CheckHr(HRESULT result, const std::wstring& operation) {
    if (FAILED(result)) {
        Fail(operation + L" failed (" + HResultText(result) + L").");
    }
}

std::wstring Win32Message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring message = length == 0 ? L"Unknown Windows error." : buffer;
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

std::wstring FilesystemErrorText(const std::error_code& error) {
    return L"error " + std::to_wstring(error.value());
}

[[noreturn]] void FailWin32(const std::wstring& operation, DWORD error = GetLastError()) {
    Fail(operation + L": " + Win32Message(error));
}

HFONT CreateSettingsFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        FailWin32(L"Could not read the system interface font");
    }
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    if (!font) {
        FailWin32(L"Could not create the settings interface font");
    }
    return font;
}

std::filesystem::path ExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            FailWin32(L"Could not locate the executable");
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        if (buffer.size() >= 32768) {
            Fail(L"The executable path is too long.");
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

std::filesystem::path ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        Fail(L"The settings file is too large.");
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (length == 0) {
        Fail(L"The settings file is not valid UTF-8.");
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            length) == 0) {
        Fail(L"Could not decode the settings file.");
    }
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        Fail(L"The settings data is too large.");
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length == 0) {
        Fail(L"Could not encode settings as UTF-8.");
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            length,
            nullptr,
            nullptr) == 0) {
        Fail(L"Could not encode settings as UTF-8.");
    }
    return result;
}

swc::Settings ReadSettingsFile(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        Fail(L"Could not inspect settings.ini: " + FilesystemErrorText(error));
    }
    if (!exists) {
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail(L"Could not read settings.ini.");
    }

    const std::string bytes(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    if (file.bad()) {
        Fail(L"An error occurred while reading settings.ini.");
    }
    std::wstring content = Utf8ToWide(bytes);
    if (content.size() >= 1 && content[0] == 0xFEFF) {
        content.erase(0, 1);
    }
    std::wistringstream input(content);
    return swc::ParseSettings(input);
}

swc::Settings ReadSettings(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    return ReadSettingsFile(path);
}

std::filesystem::path CurrentDirectory() {
    std::error_code error;
    const auto path = std::filesystem::current_path(error);
    if (error) {
        Fail(L"Could not determine the current directory: " + FilesystemErrorText(error));
    }
    return path;
}

bool StartsWithWindowsText(const std::wstring& text, const std::wstring& prefix) {
    return text.size() >= prefix.size() &&
        CompareStringOrdinal(
            text.data(),
            static_cast<int>(prefix.size()),
            prefix.data(),
            static_cast<int>(prefix.size()),
            TRUE) == CSTR_EQUAL;
}

bool IsGeneratedWallpaperFile(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    if (CompareStringOrdinal(extension.c_str(), -1, L".png", -1, TRUE) != CSTR_EQUAL) {
        return false;
    }
    const auto stem = path.stem().wstring();
    constexpr wchar_t baseName[] = L"wallpaper-background";
    if (CompareStringOrdinal(
            stem.c_str(),
            -1,
            baseName,
            static_cast<int>(std::size(baseName) - 1),
            TRUE) == CSTR_EQUAL) {
        return true;
    }
    constexpr wchar_t displayPrefix[] = L"wallpaper-background-display-";
    if (StartsWithWindowsText(stem, displayPrefix)) {
        const auto hash = stem.substr(std::size(displayPrefix) - 1);
        return !hash.empty() && std::all_of(hash.begin(), hash.end(), [](wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') ||
                (ch >= L'A' && ch <= L'F') ||
                (ch >= L'a' && ch <= L'f');
        });
    }
    constexpr wchar_t numberedPrefix[] = L"wallpaper-background-";
    if (!StartsWithWindowsText(stem, numberedPrefix)) {
        return false;
    }
    const auto number = stem.substr(std::size(numberedPrefix) - 1);
    return !number.empty() && std::all_of(
        number.begin(),
        number.end(),
        [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

std::vector<std::filesystem::path> FindImages(
    const std::filesystem::path& sourceDirectory) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(sourceDirectory, error);
    if (error) {
        Fail(L"Could not read the source image directory: " + FilesystemErrorText(error));
    }

    std::vector<std::filesystem::path> images;
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            Fail(L"Could not enumerate the source image directory: " + FilesystemErrorText(error));
        }

        const auto& entry = *iterator;
        const bool regularFile = entry.is_regular_file(error);
        if (error) {
            Fail(L"Could not inspect a file in the source image directory: " + FilesystemErrorText(error));
        }
        if (!regularFile ||
            !swc::IsSupportedExtension(entry.path().extension().wstring()) ||
            IsGeneratedWallpaperFile(entry.path())) {
            continue;
        }
        images.push_back(entry.path());
    }
    if (error) {
        Fail(L"Could not finish enumerating the source image directory: " + FilesystemErrorText(error));
    }

    std::sort(images.begin(), images.end(), [](const auto& left, const auto& right) {
        const auto leftName = left.filename().native();
        const auto rightName = right.filename().native();
        const int logicalOrder = StrCmpLogicalW(leftName.c_str(), rightName.c_str());
        if (logicalOrder != 0) {
            return logicalOrder < 0;
        }
        return CompareStringOrdinal(
            leftName.c_str(),
            static_cast<int>(leftName.size()),
            rightName.c_str(),
            static_cast<int>(rightName.size()),
            FALSE) == CSTR_LESS_THAN;
    });
    return images;
}

struct SourceImage {
    std::filesystem::path path;
    std::size_t sourceIndex;
};

std::wstring SourceImageIdentity(const SourceImage& image) {
    return std::to_wstring(image.sourceIndex) + L"|" + image.path.wstring();
}

bool WindowsTextEquals(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

std::filesystem::path DedicatedDisplayWallpaperPath(
    const std::filesystem::path& directory,
    const std::wstring& devicePath) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t character : devicePath) {
        const auto normalized = static_cast<std::uint16_t>(std::towlower(character));
        hash ^= normalized & 0xFF;
        hash *= 1099511628211ULL;
        hash ^= normalized >> 8;
        hash *= 1099511628211ULL;
    }

    std::wostringstream name;
    name << L"wallpaper-background-display-" << std::hex << std::uppercase
        << hash << L".png";
    return directory / name.str();
}

std::vector<SourceImage> FindSourceImages(
    const std::vector<swc::SourceDirectory>& sourceDirectories,
    const std::filesystem::path& executableDirectory) {
    if (!swc::HasEnabledSourceDirectory(sourceDirectories)) {
        std::vector<SourceImage> images;
        for (auto& image : FindImages(CurrentDirectory())) {
            images.push_back({std::move(image), 0});
        }
        return images;
    }

    std::vector<SourceImage> images;
    std::vector<std::wstring> failures;
    bool sourceScanned = false;
    for (std::size_t index = 0; index < sourceDirectories.size(); ++index) {
        const auto& directory = sourceDirectories[index];
        if (!directory.enabled) {
            continue;
        }

        std::filesystem::path source(directory.path);
        if (source.is_relative()) {
            source = executableDirectory / source;
        }

        std::error_code error;
        if (!std::filesystem::is_directory(source, error) || error) {
            const std::wstring reason = error
                ? FilesystemErrorText(error)
                : L"folder does not exist";
            failures.push_back(directory.path + L": " + reason);
            continue;
        }
        const auto absolute = std::filesystem::absolute(source, error);
        if (error) {
            failures.push_back(directory.path + L": " + FilesystemErrorText(error));
            continue;
        }

        try {
            auto directoryImages = FindImages(absolute);
            sourceScanned = true;
            for (auto& image : directoryImages) {
                images.push_back({std::move(image), index});
            }
        } catch (const Failure& failure) {
            failures.push_back(directory.path + L": " + failure.message);
        }
    }

    if (!sourceScanned && !failures.empty()) {
        std::wstring message = L"Could not read any enabled source folder.";
        for (std::size_t index = 0; index < failures.size() && index < 8; ++index) {
            message += L"\r\n\r\n" + failures[index];
        }
        if (failures.size() > 8) {
            message += L"\r\n\r\nAdditional source folders could not be read.";
        }
        Fail(message);
    }
    return images;
}

struct Display {
    std::wstring devicePath;
    std::wstring friendlyName;
    LONG left;
    LONG top;
    UINT width;
    UINT height;
    bool primary;
};

struct MonitorPreview {
    Display display;
    std::wstring wallpaperPath;
    std::vector<BYTE> pixels;
    double wallpaperAspectRatio = 0.0;
    UINT pixelWidth = 0;
    UINT pixelHeight = 0;
    RECT frameRect{};
    RECT labelRect{};
    std::wstring statusText;
};

ComPtr<IDesktopWallpaper> CreateDesktopWallpaper() {
    ComPtr<IDesktopWallpaper> wallpaper;
    CheckHr(
        CoCreateInstance(
            CLSID_DesktopWallpaper,
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(wallpaper.Put())),
        L"Could not initialize per-monitor wallpaper support");
    return wallpaper;
}

std::wstring EdidMonitorProductName(const std::wstring& devicePath) {
    constexpr wchar_t interfacePrefix[] = L"DISPLAY#";
    std::size_t prefixOffset = std::wstring::npos;
    for (std::size_t offset = 0;
         offset + std::size(interfacePrefix) - 1 <= devicePath.size();
         ++offset) {
        if (CompareStringOrdinal(
                devicePath.data() + offset,
                static_cast<int>(std::size(interfacePrefix) - 1),
                interfacePrefix,
                static_cast<int>(std::size(interfacePrefix) - 1),
                TRUE) == CSTR_EQUAL) {
            prefixOffset = offset + std::size(interfacePrefix) - 1;
            break;
        }
    }
    if (prefixOffset == std::wstring::npos) {
        return {};
    }

    const auto modelEnd = devicePath.find(L'#', prefixOffset);
    if (modelEnd == std::wstring::npos || modelEnd == prefixOffset) {
        return {};
    }
    const auto instanceEnd = devicePath.find(L'#', modelEnd + 1);
    if (instanceEnd == std::wstring::npos || instanceEnd == modelEnd + 1) {
        return {};
    }
    const std::wstring modelId = devicePath.substr(prefixOffset, modelEnd - prefixOffset);
    const std::wstring instanceId =
        devicePath.substr(modelEnd + 1, instanceEnd - modelEnd - 1);
    if (modelId.find_first_of(L"\\/") != std::wstring::npos ||
        instanceId.find_first_of(L"\\/") != std::wstring::npos) {
        return {};
    }

    const std::wstring registryPath =
        L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + modelId +
        L"\\" + instanceId + L"\\Device Parameters";
    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            registryPath.c_str(),
            0,
            KEY_QUERY_VALUE,
            &rawKey) != ERROR_SUCCESS) {
        return {};
    }
    struct RegistryKey {
        HKEY value;
        ~RegistryKey() {
            RegCloseKey(value);
        }
    } key{rawKey};

    DWORD type = 0;
    DWORD byteCount = 0;
    if (RegQueryValueExW(
            key.value,
            L"EDID",
            nullptr,
            &type,
            nullptr,
            &byteCount) != ERROR_SUCCESS ||
        type != REG_BINARY ||
        byteCount < 128 ||
        byteCount > 4096) {
        return {};
    }
    std::vector<std::uint8_t> edid(byteCount);
    if (RegQueryValueExW(
            key.value,
            L"EDID",
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(edid.data()),
            &byteCount) != ERROR_SUCCESS ||
        type != REG_BINARY) {
        return {};
    }
    edid.resize(byteCount);
    return swc::ExtractEdidMonitorName(edid);
}

std::vector<Display> ConnectedDisplays(IDesktopWallpaper* wallpaper) {
    UINT count = 0;
    CheckHr(wallpaper->GetMonitorDevicePathCount(&count), L"Could not enumerate displays");
    if (count == 0) {
        Fail(L"Windows did not report any connected displays.");
    }

    std::vector<Display> displays;
    displays.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        LPWSTR rawDevicePath = nullptr;
        const HRESULT pathResult = wallpaper->GetMonitorDevicePathAt(index, &rawDevicePath);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> devicePath(rawDevicePath, CoTaskMemFree);
        CheckHr(pathResult, L"Could not read a display identifier");
        if (!devicePath) {
            Fail(L"Windows returned an empty display identifier.");
        }

        RECT rectangle{};
        const HRESULT rectangleResult =
            wallpaper->GetMonitorRECT(devicePath.get(), &rectangle);
        if (rectangleResult == S_FALSE) {
            continue;
        }
        CheckHr(rectangleResult, L"Could not read display dimensions");
        const auto width = static_cast<long long>(rectangle.right) - rectangle.left;
        const auto height = static_cast<long long>(rectangle.bottom) - rectangle.top;
        if (width <= 0 || height <= 0 ||
            width > std::numeric_limits<UINT>::max() ||
            height > std::numeric_limits<UINT>::max()) {
            Fail(L"Windows returned invalid display dimensions.");
        }

        displays.push_back({
            devicePath.get(),
            {},
            rectangle.left,
            rectangle.top,
            static_cast<UINT>(width),
            static_cast<UINT>(height),
            rectangle.left <= 0 && rectangle.right > 0 &&
                rectangle.top <= 0 && rectangle.bottom > 0});
    }

    if (std::none_of(displays.begin(), displays.end(), [](const Display& display) {
            return display.primary;
        })) {
        Fail(L"Could not identify the primary display.");
    }
    std::stable_sort(displays.begin(), displays.end(), [](const Display& left, const Display& right) {
        return left.primary && !right.primary;
    });
    for (std::size_t displayIndex = 0; displayIndex < displays.size(); ++displayIndex) {
        displays[displayIndex].friendlyName =
            swc::FormatDisplayName({}, displayIndex);
    }

    for (DWORD adapterIndex = 0;; ++adapterIndex) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0)) {
            break;
        }
        for (DWORD monitorIndex = 0;; ++monitorIndex) {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(
                    adapter.DeviceName,
                    monitorIndex,
                    &monitor,
                    EDD_GET_DEVICE_INTERFACE_NAME)) {
                break;
            }
            const auto display = std::find_if(
                displays.begin(),
                displays.end(),
                [&](const Display& item) {
                    return WindowsTextEquals(item.devicePath, monitor.DeviceID);
                });
            if (display != displays.end()) {
                std::wstring modelName = EdidMonitorProductName(display->devicePath);
                if (swc::IsGenericMonitorName(modelName) &&
                    !swc::IsGenericMonitorName(monitor.DeviceString)) {
                    modelName = monitor.DeviceString;
                }
                display->friendlyName =
                    swc::FormatDisplayName(modelName, static_cast<std::size_t>(
                        display - displays.begin()));
            }
        }
    }
    return displays;
}

ComPtr<IWICImagingFactory> CreateImagingFactory() {
    ComPtr<IWICImagingFactory> factory;
    auto factoryOutput = factory.Put();
    CheckHr(
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factoryOutput)),
        L"Could not initialize Windows Imaging Component");
    return factory;
}

swc::RgbColor EstimateImageBackgroundColor(
    IWICImagingFactory* factory,
    IWICFormatConverter* converter,
    UINT sourceWidth,
    UINT sourceHeight) {
    constexpr UINT sampleMaximum = 128;
    const auto sampleFit =
        swc::CalculateFitRect(sourceWidth, sourceHeight, sampleMaximum, sampleMaximum);
    const UINT sampleWidth = sampleFit.width;
    const UINT sampleHeight = sampleFit.height;
    const UINT sampleStride = sampleWidth * 4;
    const std::uint64_t sampleBytes =
        static_cast<std::uint64_t>(sampleStride) * sampleHeight;

    ComPtr<IWICBitmapScaler> sampleScaler;
    CheckHr(factory->CreateBitmapScaler(sampleScaler.Put()), L"Could not create color sampler");
    CheckHr(
        sampleScaler->Initialize(
            converter,
            sampleWidth,
            sampleHeight,
            WICBitmapInterpolationModeFant),
        L"Could not resize the image for color sampling");

    std::vector<BYTE> samplePixels(static_cast<std::size_t>(sampleBytes));
    CheckHr(
        sampleScaler->CopyPixels(
            nullptr,
            sampleStride,
            static_cast<UINT>(sampleBytes),
            samplePixels.data()),
        L"Could not read image pixels for color sampling");

    std::vector<swc::ColorSample> samples;
    samples.reserve(static_cast<std::size_t>(sampleWidth) * sampleHeight);
    for (std::size_t offset = 0; offset < samplePixels.size(); offset += 4) {
        const UINT alpha = samplePixels[offset + 3];
        if (alpha == 0) {
            samples.push_back({0, 0, 0, 0});
            continue;
        }

        const auto unpremultiply = [alpha](BYTE component) {
            return static_cast<std::uint8_t>(
                std::min(255U, (static_cast<UINT>(component) * 255U + alpha / 2) / alpha));
        };
        samples.push_back({
            unpremultiply(samplePixels[offset + 2]),
            unpremultiply(samplePixels[offset + 1]),
            unpremultiply(samplePixels[offset]),
            static_cast<std::uint8_t>(alpha)});
    }
    return swc::EstimateBackgroundColor(samples, sampleWidth, sampleHeight);
}

void CreateBackgroundPng(
    IWICImagingFactory* factory,
    const std::filesystem::path& source,
    UINT outputWidth,
    UINT outputHeight,
    const std::filesystem::path& destination) {
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHr(
        factory->CreateDecoderFromFilename(
            source.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.Put()),
        L"Could not decode " + source.filename().wstring());

    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(decoder->GetFrame(0, frame.Put()), L"Could not read the image frame");

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    CheckHr(frame->GetSize(&sourceWidth, &sourceHeight), L"Could not read image dimensions");
    if (sourceWidth == 0 || sourceHeight == 0) {
        Fail(L"The image has invalid dimensions.");
    }

    const auto fit = swc::CalculateFitRect(sourceWidth, sourceHeight, outputWidth, outputHeight);
    const UINT scaledWidth = fit.width;
    const UINT scaledHeight = fit.height;

    ComPtr<IWICFormatConverter> converter;
    CheckHr(factory->CreateFormatConverter(converter.Put()), L"Could not create image converter");
    CheckHr(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        L"Could not convert the source image");

    const auto background =
        EstimateImageBackgroundColor(factory, converter.Get(), sourceWidth, sourceHeight);

    ComPtr<IWICBitmapScaler> scaler;
    CheckHr(factory->CreateBitmapScaler(scaler.Put()), L"Could not create image scaler");
    CheckHr(
        scaler->Initialize(
            converter.Get(),
            scaledWidth,
            scaledHeight,
            WICBitmapInterpolationModeFant),
        L"Could not resize the source image");

    if (outputWidth > std::numeric_limits<UINT>::max() / 4 / outputHeight ||
        scaledWidth > std::numeric_limits<UINT>::max() / 4 / scaledHeight) {
        Fail(L"The image dimensions are too large to process.");
    }
    const std::uint64_t outputBytes =
        static_cast<std::uint64_t>(outputWidth) * outputHeight * 4;
    const std::uint64_t scaledBytes =
        static_cast<std::uint64_t>(scaledWidth) * scaledHeight * 4;

    std::vector<BYTE> canvas(static_cast<std::size_t>(outputBytes));
    for (std::size_t offset = 0; offset < canvas.size(); offset += 4) {
        canvas[offset] = background.blue;
        canvas[offset + 1] = background.green;
        canvas[offset + 2] = background.red;
        canvas[offset + 3] = 255;
    }

    const UINT scaledStride = scaledWidth * 4;
    std::vector<BYTE> scaledPixels(static_cast<std::size_t>(scaledBytes));
    CheckHr(
        scaler->CopyPixels(
            nullptr,
            scaledStride,
            static_cast<UINT>(scaledBytes),
            scaledPixels.data()),
        L"Could not read the resized image pixels");

    const UINT outputStride = outputWidth * 4;
    for (UINT row = 0; row < scaledHeight; ++row) {
        const std::size_t sourceOffset = static_cast<std::size_t>(row) * scaledStride;
        const std::size_t destinationOffset =
            static_cast<std::size_t>(fit.top + row) * outputStride +
            static_cast<std::size_t>(fit.left) * 4;
        for (UINT column = 0; column < scaledWidth; ++column) {
            const std::size_t pixel = sourceOffset + static_cast<std::size_t>(column) * 4;
            const std::size_t outputPixel =
                destinationOffset + static_cast<std::size_t>(column) * 4;
            canvas[outputPixel] = scaledPixels[pixel];
            canvas[outputPixel + 1] = scaledPixels[pixel + 1];
            canvas[outputPixel + 2] = scaledPixels[pixel + 2];
            canvas[outputPixel + 3] = 255;
        }
    }

    if (!DeleteFileW(destination.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            FailWin32(L"Could not replace the temporary background image", error);
        }
    }
    ComPtr<IWICStream> stream;
    CheckHr(factory->CreateStream(stream.Put()), L"Could not create the PNG output stream");
    CheckHr(
        stream->InitializeFromFilename(destination.c_str(), GENERIC_WRITE),
        L"Could not create the background PNG");

    ComPtr<IWICBitmapEncoder> encoder;
    CheckHr(
        factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()),
        L"Could not create the PNG encoder");
    CheckHr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), L"Could not initialize the PNG encoder");

    ComPtr<IWICBitmapFrameEncode> outputFrame;
    CheckHr(encoder->CreateNewFrame(outputFrame.Put(), nullptr), L"Could not create the PNG frame");
    CheckHr(outputFrame->Initialize(nullptr), L"Could not initialize the PNG frame");
    CheckHr(outputFrame->SetSize(outputWidth, outputHeight), L"Could not set PNG dimensions");
    WICPixelFormatGUID outputFormat = GUID_WICPixelFormat32bppBGRA;
    CheckHr(outputFrame->SetPixelFormat(&outputFormat), L"Could not set PNG pixel format");
    if (!IsEqualGUID(outputFormat, GUID_WICPixelFormat32bppBGRA)) {
        Fail(L"The PNG encoder does not support the required pixel format.");
    }
    CheckHr(
        outputFrame->WritePixels(
            outputHeight,
            outputStride,
            static_cast<UINT>(outputBytes),
            canvas.data()),
        L"Could not write the background PNG");
    CheckHr(outputFrame->Commit(), L"Could not finalize the PNG frame");
    CheckHr(encoder->Commit(), L"Could not finalize the background PNG");
}

std::vector<BYTE> CreatePlaylistThumbnail(
    IWICImagingFactory* factory,
    const std::filesystem::path& source) {
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHr(
        factory->CreateDecoderFromFilename(
            source.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.Put()),
        L"Could not decode " + source.filename().wstring());
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(decoder->GetFrame(0, frame.Put()), L"Could not read the image frame");
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    CheckHr(frame->GetSize(&sourceWidth, &sourceHeight), L"Could not read image dimensions");
    if (sourceWidth == 0 || sourceHeight == 0) {
        Fail(L"The image has invalid dimensions.");
    }

    ComPtr<IWICFormatConverter> converter;
    CheckHr(factory->CreateFormatConverter(converter.Put()), L"Could not create image converter");
    CheckHr(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        L"Could not convert the source image");

    const auto fit = swc::CalculateFitRect(
        sourceWidth,
        sourceHeight,
        kPlaylistThumbnailSize,
        kPlaylistThumbnailSize);
    ComPtr<IWICBitmapScaler> scaler;
    CheckHr(factory->CreateBitmapScaler(scaler.Put()), L"Could not create image thumbnail");
    CheckHr(
        scaler->Initialize(
            converter.Get(),
            fit.width,
            fit.height,
            WICBitmapInterpolationModeFant),
        L"Could not resize the image thumbnail");
    const UINT stride = fit.width * 4;
    std::vector<BYTE> pixels(static_cast<std::size_t>(stride) * fit.height);
    CheckHr(
        scaler->CopyPixels(
            nullptr,
            stride,
            static_cast<UINT>(pixels.size()),
            pixels.data()),
        L"Could not read the image thumbnail");

    std::vector<BYTE> thumbnailPixels(
        static_cast<std::size_t>(kPlaylistThumbnailSize) *
            kPlaylistThumbnailSize * 4);
    auto* destination = thumbnailPixels.data();
    const COLORREF background = GetSysColor(COLOR_WINDOW);
    const BYTE backgroundBlue = GetBValue(background);
    const BYTE backgroundGreen = GetGValue(background);
    const BYTE backgroundRed = GetRValue(background);
    for (UINT y = 0; y < kPlaylistThumbnailSize; ++y) {
        for (UINT x = 0; x < kPlaylistThumbnailSize; ++x) {
            const std::size_t outputOffset =
                (static_cast<std::size_t>(y) * kPlaylistThumbnailSize + x) * 4;
            destination[outputOffset] = backgroundBlue;
            destination[outputOffset + 1] = backgroundGreen;
            destination[outputOffset + 2] = backgroundRed;
            destination[outputOffset + 3] = 255;
        }
    }
    for (UINT y = 0; y < fit.height; ++y) {
        for (UINT x = 0; x < fit.width; ++x) {
            const std::size_t sourceOffset =
                (static_cast<std::size_t>(y) * fit.width + x) * 4;
            const std::size_t outputOffset =
                (static_cast<std::size_t>(fit.top + y) * kPlaylistThumbnailSize +
                    fit.left + x) * 4;
            const UINT alpha = pixels[sourceOffset + 3];
            const auto composite = [alpha](BYTE foreground, BYTE backgroundValue) {
                return static_cast<BYTE>(
                    (static_cast<UINT>(foreground) * alpha +
                        static_cast<UINT>(backgroundValue) * (255 - alpha) + 127) / 255);
            };
            destination[outputOffset] =
                composite(pixels[sourceOffset], backgroundBlue);
            destination[outputOffset + 1] =
                composite(pixels[sourceOffset + 1], backgroundGreen);
            destination[outputOffset + 2] =
                composite(pixels[sourceOffset + 2], backgroundRed);
            destination[outputOffset + 3] = 255;
        }
    }
    return thumbnailPixels;
}

double ReadWallpaperAspectRatio(
    IWICImagingFactory* factory,
    const std::filesystem::path& source) {
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHr(
        factory->CreateDecoderFromFilename(
            source.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.Put()),
        L"Could not decode the current display wallpaper");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(
        decoder->GetFrame(0, frame.Put()),
        L"Could not read the display wallpaper frame");
    UINT width = 0;
    UINT height = 0;
    CheckHr(frame->GetSize(&width, &height),
        L"Could not read the display wallpaper dimensions");
    if (width == 0 || height == 0) {
        Fail(L"The current display wallpaper has invalid dimensions.");
    }
    return static_cast<double>(width) / height;
}

std::vector<BYTE> CreateMonitorPreviewPixels(
    IWICImagingFactory* factory,
    const std::filesystem::path& source,
    UINT outputWidth,
    UINT outputHeight) {
    if (outputWidth == 0 || outputHeight == 0 ||
        outputWidth > std::numeric_limits<UINT>::max() / 4 / outputHeight) {
        Fail(L"The monitor preview dimensions are invalid.");
    }

    ComPtr<IWICBitmapDecoder> decoder;
    CheckHr(
        factory->CreateDecoderFromFilename(
            source.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.Put()),
        L"Could not decode the current display wallpaper");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(
        decoder->GetFrame(0, frame.Put()),
        L"Could not read the display wallpaper frame");
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    CheckHr(frame->GetSize(&sourceWidth, &sourceHeight),
        L"Could not read the display wallpaper dimensions");
    if (sourceWidth == 0 || sourceHeight == 0) {
        Fail(L"The current display wallpaper has invalid dimensions.");
    }

    ComPtr<IWICFormatConverter> converter;
    CheckHr(factory->CreateFormatConverter(converter.Put()),
        L"Could not create the display wallpaper converter");
    CheckHr(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        L"Could not convert the display wallpaper");

    ComPtr<IWICBitmapScaler> scaler;
    CheckHr(factory->CreateBitmapScaler(scaler.Put()),
        L"Could not create the display wallpaper preview");
    CheckHr(
        scaler->Initialize(
            converter.Get(),
            outputWidth,
            outputHeight,
            WICBitmapInterpolationModeFant),
        L"Could not resize the display wallpaper preview");

    const UINT scaledStride = outputWidth * 4;
    const std::size_t scaledBytes =
        static_cast<std::size_t>(scaledStride) * outputHeight;
    std::vector<BYTE> scaledPixels(scaledBytes);
    CheckHr(
        scaler->CopyPixels(
            nullptr,
            scaledStride,
            static_cast<UINT>(scaledBytes),
            scaledPixels.data()),
        L"Could not read the display wallpaper preview");

    std::vector<BYTE> pixels(
        static_cast<std::size_t>(outputWidth) * outputHeight * 4);
    const COLORREF background = GetSysColor(COLOR_WINDOW);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        pixels[offset] = GetBValue(background);
        pixels[offset + 1] = GetGValue(background);
        pixels[offset + 2] = GetRValue(background);
        pixels[offset + 3] = 255;
    }
    for (UINT y = 0; y < outputHeight; ++y) {
        for (UINT x = 0; x < outputWidth; ++x) {
            const std::size_t inputOffset =
                (static_cast<std::size_t>(y) * outputWidth + x) * 4;
            const std::size_t outputOffset =
                (static_cast<std::size_t>(y) * outputWidth + x) * 4;
            const UINT alpha = scaledPixels[inputOffset + 3];
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const UINT foreground = scaledPixels[inputOffset + channel];
                const UINT backgroundValue =
                    channel == 0 ? GetBValue(background)
                        : channel == 1 ? GetGValue(background)
                                       : GetRValue(background);
                pixels[outputOffset + channel] = static_cast<BYTE>(
                    (foreground * alpha + backgroundValue * (255 - alpha) + 127) / 255);
            }
            pixels[outputOffset + 3] = 255;
        }
    }
    return pixels;
}

HBITMAP CreatePlaylistThumbnailBitmap(const std::vector<BYTE>& pixels) {
    if (pixels.size() !=
        static_cast<std::size_t>(kPlaylistThumbnailSize) *
            kPlaylistThumbnailSize * 4) {
        Fail(L"The cached Playlist thumbnail has invalid dimensions.");
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = kPlaylistThumbnailSize;
    bitmapInfo.bmiHeader.biHeight = -kPlaylistThumbnailSize;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* rawBitmapPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        nullptr,
        &bitmapInfo,
        DIB_RGB_COLORS,
        &rawBitmapPixels,
        nullptr,
        0);
    if (!bitmap || !rawBitmapPixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        FailWin32(L"Could not create an image thumbnail bitmap");
    }
    std::copy(pixels.begin(), pixels.end(), static_cast<BYTE*>(rawBitmapPixels));
    return bitmap;
}

HBITMAP CreateInvalidImagePlaceholderBitmap() {
    std::vector<BYTE> pixels(
        static_cast<std::size_t>(kPlaylistThumbnailSize) *
            kPlaylistThumbnailSize * 4);
    const COLORREF background = GetSysColor(COLOR_WINDOW);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        pixels[offset] = GetBValue(background);
        pixels[offset + 1] = GetGValue(background);
        pixels[offset + 2] = GetRValue(background);
        pixels[offset + 3] = 255;
    }

    const auto setPixel = [&](int x, int y, COLORREF color, int thickness = 1) {
        for (int dy = -thickness / 2; dy <= thickness / 2; ++dy) {
            for (int dx = -thickness / 2; dx <= thickness / 2; ++dx) {
                const int pixelX = x + dx;
                const int pixelY = y + dy;
                if (pixelX < 0 || pixelY < 0 ||
                    pixelX >= kPlaylistThumbnailSize ||
                    pixelY >= kPlaylistThumbnailSize) {
                    continue;
                }
                const std::size_t offset =
                    (static_cast<std::size_t>(pixelY) * kPlaylistThumbnailSize +
                        pixelX) * 4;
                pixels[offset] = GetBValue(color);
                pixels[offset + 1] = GetGValue(color);
                pixels[offset + 2] = GetRValue(color);
                pixels[offset + 3] = 255;
            }
        }
    };
    const auto drawLine = [&](int x0, int y0, int x1, int y1,
                              COLORREF color, int thickness = 1) {
        const int deltaX = std::abs(x1 - x0);
        const int stepX = x0 < x1 ? 1 : -1;
        const int deltaY = -std::abs(y1 - y0);
        const int stepY = y0 < y1 ? 1 : -1;
        int error = deltaX + deltaY;
        for (;;) {
            setPixel(x0, y0, color, thickness);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int doubledError = 2 * error;
            if (doubledError >= deltaY) {
                error += deltaY;
                x0 += stepX;
            }
            if (doubledError <= deltaX) {
                error += deltaX;
                y0 += stepY;
            }
        }
    };

    const COLORREF outline = RGB(105, 112, 122);
    const COLORREF accent = RGB(203, 75, 75);
    drawLine(21, 19, 42, 19, outline, 2);
    drawLine(21, 19, 21, 44, outline, 2);
    drawLine(42, 19, 42, 44, outline, 2);
    drawLine(21, 44, 42, 44, outline, 2);
    drawLine(25, 39, 30, 33, outline, 2);
    drawLine(30, 33, 34, 37, outline, 2);
    drawLine(34, 37, 37, 34, outline, 2);
    drawLine(37, 34, 39, 36, outline, 2);
    drawLine(32, 27, 39, 34, accent, 2);
    drawLine(39, 27, 32, 34, accent, 2);

    return CreatePlaylistThumbnailBitmap(pixels);
}

void AddPlaylistImagePlaceholders(HIMAGELIST imageList) {
    HICON loadingIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!loadingIcon || ImageList_AddIcon(imageList, loadingIcon) == -1) {
        Fail(L"Could not create the Playlist loading placeholder.");
    }
    HBITMAP invalidBitmap = CreateInvalidImagePlaceholderBitmap();
    const int imageIndex = ImageList_Add(imageList, invalidBitmap, nullptr);
    DeleteObject(invalidBitmap);
    if (imageIndex != 1) {
        Fail(L"Could not create the invalid-image thumbnail.");
    }
}

void ApplyWallpaper(
    IDesktopWallpaper* wallpaper,
    const Display& display,
    const std::filesystem::path& image) {
    CheckHr(
        wallpaper->SetWallpaper(display.devicePath.c_str(), image.c_str()),
        L"Windows could not apply the wallpaper to a display");
}

void WriteSettingsFile(
    const std::filesystem::path& path,
    const swc::Settings& settings) {
    std::wstring contents =
        L"; Folder to read wallpaper images from. Relative paths use the executable folder.\r\n"
        L"; If all folders are disabled or none are listed, the working folder is used.\r\n"
        L"; Prefix a path with - to disable it. Entries are tried in list order.\r\n";
    for (const auto& directory : settings.sourceDirectories) {
        contents += L"source_directories=" + swc::SerializeSourceDirectory(directory) + L"\r\n";
    }
    contents +=
        L"; Last image identity; maintained automatically by the program.\r\n"
        L"last_image=" + settings.lastImage + L"\r\n" +
        L"; Current source image per display; maintained automatically by the program.\r\n";
    for (const auto& displayImage : settings.displayImages) {
        contents += L"display_image=" + displayImage.devicePath + L"|" +
            displayImage.imagePath + L"\r\n";
    }
    contents +=
        L"; Change wallpapers once when the program starts: true or false.\r\n";
    contents += L"change_on_start=" +
        std::wstring(settings.changeOnStart ? L"true" : L"false") + L"\r\n"
        L"; Example configuration:\r\n"
        L"; source_directories=C:\\Users\\Example\\Pictures\\Wallpapers\r\n"
        L"; source_directories=-D:\\SeasonalWallpapers\r\n"
        L"; last_image=0|C:\\Users\\Example\\Pictures\\Wallpapers\\wallpaper2.jpg\r\n"
        L"; display_image=\\\\?\\DISPLAY#ABC|C:\\Users\\Example\\Pictures\\Wallpapers\\wallpaper2.jpg\r\n"
        L"; change_on_start=false\r\n";
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    struct TemporarySettingsFile {
        const std::filesystem::path& path;
        ~TemporarySettingsFile() {
            DeleteFileW(path.c_str());
        }
    } temporaryFile{temporary};
    const std::string bytes = WideToUtf8(contents);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            Fail(L"Could not write the temporary settings file.");
        }
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            Fail(L"An error occurred while writing settings.ini.");
        }
    }

    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        FailWin32(L"Could not update settings.ini", error);
    }
}

void InitializeSettingsFile(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        Fail(L"Could not inspect settings.ini: " + FilesystemErrorText(error));
    }
    if (!exists) {
        WriteSettingsFile(path, swc::Settings{});
    }
}

void UpdateUserSettings(
    const std::filesystem::path& path,
    const swc::Settings& preferences) {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    auto settings = ReadSettingsFile(path);
    settings.sourceDirectories = preferences.sourceDirectories;
    settings.changeOnStart = preferences.changeOnStart;
    WriteSettingsFile(path, settings);
}

void UpdateWallpaperSettings(
    const std::filesystem::path& path,
    const std::wstring& lastImage,
    const std::vector<swc::DisplayImage>& displayImages,
    bool replaceDisplayImages) {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    auto settings = ReadSettingsFile(path);
    settings.lastImage = lastImage;
    if (replaceDisplayImages) {
        settings.displayImages = displayImages;
    } else {
        for (const auto& displayImage : displayImages) {
            const auto existing = std::find_if(
                settings.displayImages.begin(),
                settings.displayImages.end(),
                [&](const swc::DisplayImage& item) {
                    return CompareStringOrdinal(
                        item.devicePath.data(),
                        static_cast<int>(item.devicePath.size()),
                        displayImage.devicePath.data(),
                        static_cast<int>(displayImage.devicePath.size()),
                        TRUE) == CSTR_EQUAL;
                });
            if (existing == settings.displayImages.end()) {
                settings.displayImages.push_back(displayImage);
            } else {
                *existing = displayImage;
            }
        }
    }
    WriteSettingsFile(path, settings);
}

struct WallpaperRequest {
    bool previous = false;
    std::wstring firstImage;
    std::wstring displayDevicePath;
};

void Run(const WallpaperRequest& request) {
    const auto executableDirectory = ExecutableDirectory();
    const auto settingsPath = executableDirectory / L"settings.ini";
    const auto settings = ReadSettings(settingsPath);
    const auto images = FindSourceImages(settings.sourceDirectories, executableDirectory);
    if (images.empty()) {
        return;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CheckHr(comResult, L"Could not initialize Windows imaging");
    struct ComUninitializer {
        ~ComUninitializer() {
            CoUninitialize();
        }
    } comUninitializer;
    auto wallpaper = CreateDesktopWallpaper();
    const auto displays = ConnectedDisplays(wallpaper.Get());

    std::vector<std::wstring> names;
    names.reserve(images.size());
    for (const auto& image : images) {
        names.push_back(SourceImageIdentity(image));
    }

    std::wstring savedCursor = settings.lastImage;
    if (std::find(names.begin(), names.end(), savedCursor) == names.end()) {
        if (!settings.lastImage.empty()) {
            const auto legacyImage = std::find_if(
                images.begin(),
                images.end(),
                [&](const SourceImage& image) {
                    const auto fileName = image.path.filename().wstring();
                    return CompareStringOrdinal(
                        fileName.data(),
                        static_cast<int>(fileName.size()),
                        settings.lastImage.data(),
                        static_cast<int>(settings.lastImage.size()),
                        TRUE) == CSTR_EQUAL;
                });
            if (legacyImage != images.end()) {
                savedCursor = names[static_cast<std::size_t>(legacyImage - images.begin())];
            }
        }
    }

    auto factory = CreateImagingFactory();
    struct GeneratedWallpaper {
        Display display;
        std::filesystem::path temporaryPath;
        std::filesystem::path outputPath;
    };
    std::vector<GeneratedWallpaper> generated;
    generated.reserve(displays.size());
    std::vector<std::filesystem::path> temporaryPaths;
    temporaryPaths.reserve(displays.size());
    std::vector<std::filesystem::path> outputPaths;
    outputPaths.reserve(displays.size());
    for (std::size_t displayIndex = 0; displayIndex < displays.size(); ++displayIndex) {
        const std::wstring fileName = displayIndex == 0
            ? L"wallpaper-background.png"
            : L"wallpaper-background-" + std::to_wstring(displayIndex + 1) + L".png";
        const auto outputPath = executableDirectory / fileName;
        outputPaths.push_back(outputPath);
        temporaryPaths.emplace_back(outputPath.wstring() + L".tmp");
    }
    struct TemporaryFiles {
        std::vector<std::filesystem::path>& paths;
        ~TemporaryFiles() {
            for (const auto& path : paths) {
                DeleteFileW(path.c_str());
            }
        }
    } temporaryFiles{temporaryPaths};

    std::wstring lastImage = savedCursor;
    const auto selectCandidate = [&](
        std::size_t displayIndex,
        const std::wstring& cursor,
        bool startAtImage) {
        std::vector<std::wstring> failures;
        std::size_t selectedIndex = images.size();
        const auto order = startAtImage
            ? swc::CandidateAttemptOrderFrom(names, cursor)
            : request.previous
            ? swc::PreviousCandidateAttemptOrder(names, cursor)
            : swc::CandidateAttemptOrder(names, cursor);
        for (const std::size_t index : order) {
            try {
                CreateBackgroundPng(
                    factory.Get(),
                    images[index].path,
                    displays[displayIndex].width,
                    displays[displayIndex].height,
                    temporaryPaths[displayIndex]);
                selectedIndex = index;
                break;
            } catch (const Failure& failure) {
                failures.push_back(images[index].path.wstring() + L": " + failure.message);
            }
        }

        if (selectedIndex == images.size()) {
            std::wstring message = L"No usable image could be processed for display " +
                std::to_wstring(displayIndex + 1) + L".";
            for (std::size_t index = 0; index < failures.size() && index < 8; ++index) {
                message += L"\r\n\r\n" + failures[index];
            }
            if (failures.size() > 8) {
                message += L"\r\n\r\nAdditional images also failed.";
            }
            Fail(message);
        }

        return selectedIndex;
    };

    const bool hasSavedCursor =
        std::find(names.begin(), names.end(), lastImage) != names.end();
    if (request.displayDevicePath.empty() && request.previous && hasSavedCursor) {
        for (std::size_t skippedDisplay = 1; skippedDisplay < displays.size(); ++skippedDisplay) {
            const auto currentSequenceImage = selectCandidate(0, lastImage, false);
            lastImage = names[currentSequenceImage];
        }
    }

    std::wstring lastDisplayImage = lastImage;
    std::vector<swc::DisplayImage> displayImages;
    if (!request.displayDevicePath.empty()) {
        const auto display = std::find_if(
            displays.begin(),
            displays.end(),
            [&](const Display& item) {
                return CompareStringOrdinal(
                    item.devicePath.data(),
                    static_cast<int>(item.devicePath.size()),
                    request.displayDevicePath.data(),
                    static_cast<int>(request.displayDevicePath.size()),
                    TRUE) == CSTR_EQUAL;
            });
        if (display == displays.end()) {
            Fail(L"The selected display is no longer connected.");
        }
        if (std::find(names.begin(), names.end(), request.firstImage) == names.end()) {
            Fail(L"The selected playlist image is no longer available.");
        }

        const std::size_t displayIndex =
            static_cast<std::size_t>(display - displays.begin());
        outputPaths[displayIndex] = DedicatedDisplayWallpaperPath(
            executableDirectory, displays[displayIndex].devicePath);
        temporaryPaths[displayIndex] = outputPaths[displayIndex].wstring() + L".tmp";
        std::size_t selectedIndex =
            selectCandidate(displayIndex, request.firstImage, true);
        lastImage = names[selectedIndex];
        displayImages.push_back({
            displays[displayIndex].devicePath,
            images[selectedIndex].path.wstring()});
        generated.push_back({
            displays[displayIndex],
            temporaryPaths[displayIndex],
            outputPaths[displayIndex]});

        for (std::size_t followingDisplay = displayIndex + 1;
             followingDisplay < displays.size();
             ++followingDisplay) {
            selectedIndex = selectCandidate(followingDisplay, lastImage, false);
            lastImage = names[selectedIndex];
        }
    } else {
        const bool startAtImage = !request.firstImage.empty();
        if (startAtImage &&
            std::find(names.begin(), names.end(), request.firstImage) == names.end()) {
            Fail(L"The selected playlist image is no longer available.");
        }

        for (std::size_t offset = 0; offset < displays.size(); ++offset) {
            const std::size_t displayIndex = request.previous
                ? displays.size() - 1 - offset
                : offset;
            const std::size_t selectedIndex = selectCandidate(
                displayIndex,
                startAtImage && offset == 0 ? request.firstImage : lastImage,
                startAtImage && offset == 0);
            lastImage = names[selectedIndex];
            if (displayIndex == displays.size() - 1) {
                lastDisplayImage = lastImage;
            }
            displayImages.push_back({
                displays[displayIndex].devicePath,
                images[selectedIndex].path.wstring()});
            generated.push_back({
                displays[displayIndex],
                temporaryPaths[displayIndex],
                outputPaths[displayIndex]});
        }
    }

    for (const auto& item : generated) {
        if (!MoveFileExW(
                item.temporaryPath.c_str(),
                item.outputPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            FailWin32(L"Could not publish a generated wallpaper");
        }
    }

    for (const auto& item : generated) {
        ApplyWallpaper(wallpaper.Get(), item.display, item.outputPath);
    }
    UpdateWallpaperSettings(
        settingsPath,
        request.previous && request.displayDevicePath.empty() ? lastDisplayImage : lastImage,
        displayImages,
        request.displayDevicePath.empty());
}

void ShowError(const std::wstring& message, HWND owner = nullptr) {
    MessageBoxW(
        owner,
        message.c_str(),
        L"Simple Wallpaper Changer",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

struct PlaylistRow {
    SourceImage source;
    std::wstring sizeText;
    std::wstring statusText;
    std::wstring cacheKey;
};

struct PlaylistThumbnail {
    bool readable = false;
    std::vector<BYTE> pixels;
    std::wstring error;
};

struct PlaylistRefreshPacket {
    enum class Kind {
        rows,
        thumbnail,
        finished
    };

    Kind kind = Kind::finished;
    std::uint64_t generation = 0;
    std::vector<PlaylistRow> rows;
    std::size_t rowIndex = 0;
    std::wstring cacheKey;
    std::shared_ptr<const PlaylistThumbnail> thumbnail;
    swc::Settings settings;
    std::wstring error;
};

struct ResidentState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND settingsWindow = nullptr;
    HWND settingsTabs = nullptr;
    HWND monitorPreviewPanel = nullptr;
    HWND changeOnStartCheckbox = nullptr;
    HWND sourceDirectoryList = nullptr;
    HWND sourceDirectoryHint = nullptr;
    HWND playlistList = nullptr;
    HWND playlistHint = nullptr;
    HFONT settingsFont = nullptr;
    HIMAGELIST playlistImageList = nullptr;
    std::filesystem::path settingsPath;
    swc::Settings settings;
    std::vector<SourceImage> playlistSources;
    std::vector<std::wstring> playlistDisplayNames;
    std::vector<Display> playlistDisplays;
    std::vector<MonitorPreview> monitorPreviews;
    std::unordered_map<std::wstring, std::shared_ptr<const PlaylistThumbnail>>
        playlistThumbnailCache;
    std::deque<std::wstring> playlistThumbnailCacheOrder;
    std::mutex playlistPacketMutex;
    std::vector<PlaylistRefreshPacket> playlistPackets;
    std::shared_ptr<std::atomic_bool> playlistCancel;
    std::thread playlistThread;
    std::uint64_t playlistGeneration = 0;
    std::size_t playlistInvalidCount = 0;
    bool playlistRunning = false;
    bool playlistRefreshPending = false;
    std::wstring settingsWindowError;
    bool refreshingSourceList = false;
    int draggingSourceIndex = -1;
    int sourceDropIndex = -1;
    std::wstring draggingPlaylistImage;
    int draggingPlaylistRow = -1;
    HWND playlistDragGhost = nullptr;
    int monitorDropTarget = -1;
    NOTIFYICONDATAW icon{};
    bool iconAdded = false;
    bool changeRunning = false;
    bool shuttingDown = false;
    std::atomic_bool changeFinished = false;
    std::thread changeThread;
    std::mutex errorMutex;
    std::wstring changeError;
    std::wstring startupError;
};

void LayoutMonitorPreviews(ResidentState& state) {
    if (!state.monitorPreviewPanel) {
        return;
    }

    RECT client{};
    if (!GetClientRect(state.monitorPreviewPanel, &client) ||
        state.monitorPreviews.empty()) {
        return;
    }

    double minLeft = std::numeric_limits<double>::max();
    double minTop = std::numeric_limits<double>::max();
    double maxRight = std::numeric_limits<double>::lowest();
    double maxBottom = std::numeric_limits<double>::lowest();
    for (const auto& preview : state.monitorPreviews) {
        const double aspectRatio = preview.wallpaperAspectRatio > 0.0
            ? preview.wallpaperAspectRatio
            : static_cast<double>(preview.display.width) / preview.display.height;
        const double width = preview.display.height * aspectRatio;
        const double centerX =
            static_cast<double>(preview.display.left) + preview.display.width / 2.0;
        const double centerY =
            static_cast<double>(preview.display.top) + preview.display.height / 2.0;
        minLeft = std::min(minLeft, centerX - width / 2.0);
        minTop = std::min(minTop, centerY - preview.display.height / 2.0);
        maxRight = std::max(maxRight, centerX + width / 2.0);
        maxBottom = std::max(
            maxBottom, centerY + preview.display.height / 2.0);
    }

    const double desktopWidth = maxRight - minLeft;
    const double desktopHeight = maxBottom - minTop;
    const int availableWidth = std::max(1L, client.right - 28);
    const int availableHeight = std::max(1L, client.bottom - 42);
    const double scale = std::min(
        static_cast<double>(availableWidth) / desktopWidth,
        static_cast<double>(availableHeight) / desktopHeight);
    const int originX = 14 + static_cast<int>(std::lround(
        (availableWidth - desktopWidth * scale) / 2));
    const int originY = 8 + static_cast<int>(std::lround(
        (availableHeight - desktopHeight * scale) / 2));
    for (auto& preview : state.monitorPreviews) {
        const double aspectRatio = preview.wallpaperAspectRatio > 0.0
            ? preview.wallpaperAspectRatio
            : static_cast<double>(preview.display.width) / preview.display.height;
        const double visualWidth = preview.display.height * aspectRatio;
        const double centerX =
            static_cast<double>(preview.display.left) + preview.display.width / 2.0;
        const double centerY =
            static_cast<double>(preview.display.top) + preview.display.height / 2.0;
        const int width = std::max(
            1, static_cast<int>(std::lround(visualWidth * scale)));
        const int height = std::max(
            1, static_cast<int>(std::lround(preview.display.height * scale)));
        const int left = originX + static_cast<int>(std::lround(
            (centerX - visualWidth / 2.0 - minLeft) * scale));
        const int top = originY + static_cast<int>(std::lround(
            (centerY - preview.display.height / 2.0 - minTop) * scale));
        const int horizontalInset = std::min(kMonitorPreviewGap / 2, (width - 1) / 2);
        const int verticalInset = std::min(kMonitorPreviewGap / 2, (height - 1) / 2);
        preview.frameRect = {
            left + horizontalInset,
            top + verticalInset,
            left + width - horizontalInset,
            top + height - verticalInset};
        preview.labelRect = {
            left - 4,
            top + height + 4,
            left + width + 4,
            client.bottom - 2};
        preview.pixels.clear();
        const int innerWidth = static_cast<int>(std::max<LONG>(
            0, preview.frameRect.right - preview.frameRect.left - 8));
        const int innerHeight = static_cast<int>(std::max<LONG>(
            0, preview.frameRect.bottom - preview.frameRect.top - 8));
        if (innerWidth > 0 && innerHeight > 0) {
            const int fittedHeight = static_cast<int>(std::lround(
                innerWidth / aspectRatio));
            if (fittedHeight <= innerHeight) {
                preview.pixelWidth = static_cast<UINT>(innerWidth);
                preview.pixelHeight = static_cast<UINT>(
                    std::max(1, fittedHeight));
            } else {
                preview.pixelHeight = static_cast<UINT>(innerHeight);
                preview.pixelWidth = static_cast<UINT>(std::max(
                    1,
                    static_cast<int>(std::lround(innerHeight * aspectRatio))));
            }
        } else {
            preview.pixelWidth = 0;
            preview.pixelHeight = 0;
        }
    }
}

void RefreshMonitorPreviews(ResidentState& state) {
    auto wallpaper = CreateDesktopWallpaper();
    const auto displays = ConnectedDisplays(wallpaper.Get());
    state.monitorPreviews.clear();
    state.monitorPreviews.reserve(displays.size());
    for (const auto& display : displays) {
        MonitorPreview preview;
        preview.display = display;
        state.monitorPreviews.push_back(std::move(preview));
    }
    auto factory = CreateImagingFactory();
    for (auto& preview : state.monitorPreviews) {
        LPWSTR rawWallpaperPath = nullptr;
        const HRESULT result = wallpaper->GetWallpaper(
            preview.display.devicePath.c_str(), &rawWallpaperPath);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> wallpaperPath(
            rawWallpaperPath, CoTaskMemFree);
        if (FAILED(result)) {
            preview.statusText = L"Wallpaper unavailable";
            continue;
        }
        if (!wallpaperPath || !wallpaperPath.get()[0]) {
            preview.statusText = L"No wallpaper";
            continue;
        }
        preview.wallpaperPath = wallpaperPath.get();
        try {
            preview.wallpaperAspectRatio = ReadWallpaperAspectRatio(
                factory.Get(), preview.wallpaperPath);
        } catch (const Failure&) {
            preview.statusText = L"Preview unavailable";
        } catch (const std::exception&) {
            preview.statusText = L"Preview unavailable";
        }
    }
    LayoutMonitorPreviews(state);
    for (auto& preview : state.monitorPreviews) {
        if (preview.wallpaperPath.empty() || !preview.statusText.empty()) {
            continue;
        }
        if (preview.pixelWidth == 0 || preview.pixelHeight == 0) {
            preview.statusText = L"Preview too small";
            continue;
        }
        try {
            preview.pixels = CreateMonitorPreviewPixels(
                factory.Get(),
                preview.wallpaperPath,
                preview.pixelWidth,
                preview.pixelHeight);
        } catch (const Failure&) {
            preview.statusText = L"Preview unavailable";
        } catch (const std::exception&) {
            preview.statusText = L"Preview unavailable";
        }
    }
    if (state.monitorPreviewPanel) {
        InvalidateRect(state.monitorPreviewPanel, nullptr, TRUE);
    }
}

void RefreshMonitorPreviewsWithError(ResidentState& state, HWND owner) {
    try {
        RefreshMonitorPreviews(state);
    } catch (const Failure& failure) {
        ShowError(failure.message, owner);
    } catch (const std::exception&) {
        ShowError(L"Could not refresh the monitor previews.", owner);
    }
}

void DrawMonitorPreviews(const DRAWITEMSTRUCT& draw, ResidentState& state) {
    FillRect(draw.hDC, &draw.rcItem, GetSysColorBrush(COLOR_BTNFACE));
    HFONT previousFont = static_cast<HFONT>(SelectObject(draw.hDC, state.settingsFont));

    for (std::size_t index = 0; index < state.monitorPreviews.size(); ++index) {
        const auto& preview = state.monitorPreviews[index];
        RECT frame = preview.frameRect;
        FillRect(draw.hDC, &frame, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        RECT screen{
            frame.left + 4,
            frame.top + 4,
            frame.right - 4,
            frame.bottom - 4};
        if (!preview.pixels.empty() &&
            preview.pixelWidth > 0 && preview.pixelHeight > 0) {
            const int screenCenterX = (screen.left + screen.right) / 2;
            const int screenCenterY = (screen.top + screen.bottom) / 2;
            screen.left = screenCenterX -
                static_cast<int>(preview.pixelWidth) / 2;
            screen.top = screenCenterY -
                static_cast<int>(preview.pixelHeight) / 2;
            screen.right = screen.left +
                static_cast<int>(preview.pixelWidth);
            screen.bottom = screen.top +
                static_cast<int>(preview.pixelHeight);
        }
        if (screen.right > screen.left && screen.bottom > screen.top) {
            FillRect(draw.hDC, &screen, GetSysColorBrush(COLOR_WINDOW));
        }
        if (!preview.pixels.empty() &&
            preview.pixelWidth > 0 && preview.pixelHeight > 0 &&
            screen.right > screen.left && screen.bottom > screen.top) {
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(preview.pixelWidth);
            bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(preview.pixelHeight);
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(
                draw.hDC,
                screen.left,
                screen.top,
                screen.right - screen.left,
                screen.bottom - screen.top,
                0,
                0,
                preview.pixelWidth,
                preview.pixelHeight,
                preview.pixels.data(),
                &bitmapInfo,
                DIB_RGB_COLORS,
                SRCCOPY);
        } else if (!preview.statusText.empty()) {
            SetBkMode(draw.hDC, TRANSPARENT);
            DrawTextW(
                draw.hDC,
                preview.statusText.c_str(),
                -1,
                &screen,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        RECT label = preview.labelRect;
        SetBkMode(draw.hDC, TRANSPARENT);
        DrawTextW(
            draw.hDC,
            preview.display.friendlyName.c_str(),
            -1,
            &label,
            DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (static_cast<int>(index) == state.monitorDropTarget) {
            RECT target = preview.frameRect;
            InflateRect(&target, 3, 3);
            const COLORREF highlight = RGB(35, 220, 125);
            HBRUSH highlightBrush = CreateSolidBrush(highlight);
            for (int border = 0; border < 4; ++border) {
                FrameRect(draw.hDC, &target, highlightBrush);
                InflateRect(&target, -1, -1);
            }
            DeleteObject(highlightBrush);
        }
    }

    SelectObject(draw.hDC, previousFont);
}

int MonitorPreviewAt(const ResidentState& state, POINT point) {
    for (std::size_t index = 0; index < state.monitorPreviews.size(); ++index) {
        const RECT& frame = state.monitorPreviews[index].frameRect;
        if (point.x >= frame.left && point.x < frame.right &&
            point.y >= frame.top && point.y < frame.bottom) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

LRESULT CALLBACK PlaylistDragGhostProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    auto* state = reinterpret_cast<ResidentState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<ResidentState*>(creation->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(RGB(36, 43, 52));
            FillRect(dc, &client, background);
            DeleteObject(background);
            HPEN border = CreatePen(PS_SOLID, 2, RGB(225, 232, 240));
            HGDIOBJ previousPen = SelectObject(dc, border);
            HGDIOBJ previousBrush = SelectObject(
                dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, 1, 1, client.right - 1, client.bottom - 1);
            SelectObject(dc, previousBrush);
            SelectObject(dc, previousPen);
            DeleteObject(border);

            if (state->draggingPlaylistRow >= 0 &&
                state->playlistImageList &&
                static_cast<std::size_t>(state->draggingPlaylistRow) <
                    state->playlistSources.size()) {
                LVITEMW item{};
                item.mask = LVIF_IMAGE;
                item.iItem = state->draggingPlaylistRow;
                if (ListView_GetItem(state->playlistList, &item) &&
                    item.iImage >= 0) {
                    ImageList_Draw(
                        state->playlistImageList,
                        item.iImage,
                        dc,
                        23,
                        7,
                        ILD_TRANSPARENT);
                }

                const std::wstring name = state->playlistSources[
                    static_cast<std::size_t>(state->draggingPlaylistRow)]
                                              .path.filename().wstring();
                RECT label{7, 75, client.right - 7, client.bottom - 5};
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(
                    dc,
                    name.c_str(),
                    -1,
                    &label,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PositionPlaylistDragGhost(ResidentState& state) {
    if (!state.playlistDragGhost || !IsWindow(state.playlistDragGhost)) {
        return;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return;
    }
    SetWindowPos(
        state.playlistDragGhost,
        HWND_TOPMOST,
        cursor.x + 18,
        cursor.y + 18,
        kPlaylistDragGhostWidth,
        kPlaylistDragGhostHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void StartPlaylistDragGhost(ResidentState& state) {
    state.playlistDragGhost = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kPlaylistDragGhostClass,
        L"",
        WS_POPUP,
        0,
        0,
        kPlaylistDragGhostWidth,
        kPlaylistDragGhostHeight,
        state.settingsWindow,
        nullptr,
        state.instance,
        &state);
    if (!state.playlistDragGhost) {
        FailWin32(L"Could not create the Playlist drag preview");
    }
    if (!SetLayeredWindowAttributes(
            state.playlistDragGhost,
            0,
            238,
            LWA_ALPHA)) {
        const DWORD error = GetLastError();
        DestroyWindow(state.playlistDragGhost);
        state.playlistDragGhost = nullptr;
        FailWin32(L"Could not configure the Playlist drag preview", error);
    }
    PositionPlaylistDragGhost(state);
}

void StopPlaylistDragGhost(ResidentState& state) {
    if (state.playlistDragGhost) {
        DestroyWindow(state.playlistDragGhost);
        state.playlistDragGhost = nullptr;
    }
}

void SetPlaylistDragCursor(const ResidentState& state) {
    SetCursor(LoadCursorW(
        nullptr,
        state.monitorDropTarget >= 0 ? IDC_HAND : IDC_NO));
}

LRESULT CALLBACK PlaylistListDragSubclass(
    HWND list,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR) {
    auto* state = reinterpret_cast<ResidentState*>(
        GetWindowLongPtrW(GetParent(list), GWLP_USERDATA));
    if (message == WM_SETCURSOR && state &&
        !state->draggingPlaylistImage.empty()) {
        SetPlaylistDragCursor(*state);
        return TRUE;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(list, PlaylistListDragSubclass, subclassId);
    }
    return DefSubclassProc(list, message, wParam, lParam);
}

LRESULT CALLBACK MonitorPreviewPanelProc(
    HWND panel,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    auto* state = reinterpret_cast<ResidentState*>(
        GetWindowLongPtrW(GetParent(panel), GWLP_USERDATA));
    switch (message) {
    case WM_SETCURSOR:
        if (state && !state->draggingPlaylistImage.empty()) {
            SetPlaylistDragCursor(*state);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(panel, &paint);
            DRAWITEMSTRUCT draw{};
            draw.hDC = dc;
            GetClientRect(panel, &draw.rcItem);
            DrawMonitorPreviews(draw, *state);
            EndPaint(panel, &paint);
            return 0;
        }
        break;
    case WM_SIZE:
        if (state) {
            LayoutMonitorPreviews(*state);
            InvalidateRect(panel, nullptr, TRUE);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(panel, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(panel, message, wParam, lParam);
}

void RefreshSourceDirectoryList(ResidentState& state) {
    state.refreshingSourceList = true;
    ListView_DeleteAllItems(state.sourceDirectoryList);
    for (std::size_t index = 0; index < state.settings.sourceDirectories.size(); ++index) {
        const auto& directory = state.settings.sourceDirectories[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(directory.path.c_str());
        const int row = ListView_InsertItem(state.sourceDirectoryList, &item);
        if (row < 0) {
            state.refreshingSourceList = false;
            Fail(L"Could not display the source folder list.");
        }
        ListView_SetCheckState(state.sourceDirectoryList, row, directory.enabled);
    }
    state.refreshingSourceList = false;
}

void ScrollPlaylistToCurrentImage(const ResidentState& state) {
    const auto currentImage = std::find_if(
        state.playlistDisplayNames.begin(),
        state.playlistDisplayNames.end(),
        [](const std::wstring& names) { return !names.empty(); });
    if (currentImage == state.playlistDisplayNames.end()) {
        return;
    }

    const int row = static_cast<int>(
        currentImage - state.playlistDisplayNames.begin());
    if (!ListView_EnsureVisible(state.playlistList, row, FALSE)) {
        return;
    }

    RECT client{};
    RECT item{};
    if (!GetClientRect(state.playlistList, &client) ||
        !ListView_GetItemRect(state.playlistList, row, &item, LVIR_BOUNDS)) {
        return;
    }
    const int targetTop = client.top + (client.bottom - client.top) / 3;
    const int delta = targetTop - item.top;
    if (delta != 0) {
        ListView_Scroll(state.playlistList, 0, delta);
        ListView_EnsureVisible(state.playlistList, row, FALSE);
    }
}

void RefreshPlaylistDisplayAssignments(
    ResidentState& state,
    const swc::Settings& settings) {
    auto wallpaper = CreateDesktopWallpaper();
    state.playlistDisplays = ConnectedDisplays(wallpaper.Get());
    state.playlistDisplayNames.assign(state.playlistSources.size(), L"");

    for (std::size_t displayIndex = 0;
         displayIndex < state.playlistDisplays.size();
         ++displayIndex) {
        const auto& display = state.playlistDisplays[displayIndex];
        const std::wstring outputName = displayIndex == 0
            ? L"wallpaper-background.png"
            : L"wallpaper-background-" + std::to_wstring(displayIndex + 1) + L".png";
        const auto expectedOutput =
            (ExecutableDirectory() / outputName).wstring();
        const auto dedicatedOutput =
            DedicatedDisplayWallpaperPath(ExecutableDirectory(), display.devicePath).wstring();

        LPWSTR rawWallpaperPath = nullptr;
        CheckHr(
            wallpaper->GetWallpaper(display.devicePath.c_str(), &rawWallpaperPath),
            L"Could not read the current wallpaper for " + display.friendlyName);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> wallpaperPath(
            rawWallpaperPath, CoTaskMemFree);
        if (!wallpaperPath ||
            (!WindowsTextEquals(wallpaperPath.get(), expectedOutput) &&
                !WindowsTextEquals(wallpaperPath.get(), dedicatedOutput))) {
            continue;
        }

        const auto assignment = std::find_if(
            settings.displayImages.begin(),
            settings.displayImages.end(),
            [&](const swc::DisplayImage& item) {
                return WindowsTextEquals(item.devicePath, display.devicePath);
            });
        if (assignment == settings.displayImages.end()) {
            continue;
        }

        for (std::size_t imageIndex = 0;
             imageIndex < state.playlistSources.size();
             ++imageIndex) {
            if (WindowsTextEquals(
                    state.playlistSources[imageIndex].path.wstring(),
                    assignment->imagePath)) {
                if (!state.playlistDisplayNames[imageIndex].empty()) {
                    state.playlistDisplayNames[imageIndex] += L", ";
                }
                state.playlistDisplayNames[imageIndex] += display.friendlyName;
            }
        }
    }

    for (std::size_t row = 0; row < state.playlistDisplayNames.size(); ++row) {
        ListView_SetItemText(
            state.playlistList,
            static_cast<int>(row),
            3,
            const_cast<wchar_t*>(state.playlistDisplayNames[row].c_str()));
    }
    ScrollPlaylistToCurrentImage(state);
    InvalidateRect(state.playlistList, nullptr, TRUE);
}

std::wstring PlaylistThumbnailCacheKey(
    const std::filesystem::path& path,
    std::uintmax_t size,
    std::filesystem::file_time_type writeTime) {
    return path.wstring() + L"\n" + std::to_wstring(size) + L"\n" +
        std::to_wstring(writeTime.time_since_epoch().count());
}

void QueuePlaylistPacket(ResidentState& state, PlaylistRefreshPacket packet) {
    {
        std::lock_guard<std::mutex> lock(state.playlistPacketMutex);
        state.playlistPackets.push_back(std::move(packet));
    }
    PostMessageW(state.window, kPlaylistRefreshMessage, 0, 0);
}

void CancelPlaylistRefresh(ResidentState& state) {
    ++state.playlistGeneration;
    state.playlistRefreshPending = false;
    if (state.playlistCancel) {
        state.playlistCancel->store(true, std::memory_order_release);
    }
}

void StartPlaylistRefresh(ResidentState& state);

void FinishPlaylistRefresh(ResidentState& state) {
    if (state.playlistThread.joinable()) {
        state.playlistThread.join();
    }
    state.playlistRunning = false;
    state.playlistCancel.reset();
    if (state.playlistRefreshPending && state.settingsWindow &&
        TabCtrl_GetCurSel(state.settingsTabs) == 2) {
        state.playlistRefreshPending = false;
        StartPlaylistRefresh(state);
    }
}

void SetPlaylistHint(ResidentState& state, bool loading) {
    const std::wstring hintText = loading
        ? L"Loading playlist; thumbnails will appear as they are ready..."
        : state.playlistSources.empty()
        ? L"No supported images were found in the enabled source folders."
        : state.playlistInvalidCount == 0
        ? L"Double-click to start with an image, or right-click to set it on one display."
        : std::to_wstring(state.playlistInvalidCount) +
            L" image(s) can't be displayed; an error icon marks each one.";
    SetWindowTextW(state.playlistHint, hintText.c_str());
}

void StartPlaylistRefresh(ResidentState& state) {
    if (!state.settingsWindow || TabCtrl_GetCurSel(state.settingsTabs) != 2) {
        return;
    }
    if (state.playlistRunning) {
        ++state.playlistGeneration;
        state.playlistRefreshPending = true;
        if (state.playlistCancel) {
            state.playlistCancel->store(true, std::memory_order_release);
        }
        SetPlaylistHint(state, true);
        return;
    }
    if (state.playlistThread.joinable()) {
        state.playlistThread.join();
    }

    ++state.playlistGeneration;
    const std::uint64_t generation = state.playlistGeneration;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    state.playlistCancel = cancellation;
    state.playlistRunning = true;
    state.playlistRefreshPending = false;
    state.playlistSources.clear();
    state.playlistDisplayNames.clear();
    state.playlistInvalidCount = 0;
    ListView_DeleteAllItems(state.playlistList);
    ImageList_RemoveAll(state.playlistImageList);
    try {
        AddPlaylistImagePlaceholders(state.playlistImageList);
    } catch (...) {
        state.playlistRunning = false;
        state.playlistCancel.reset();
        throw;
    }
    SetPlaylistHint(state, true);

    const auto cache = state.playlistThumbnailCache;
    const auto settingsPath = state.settingsPath;
    const auto executableDirectory = ExecutableDirectory();
    try {
        state.playlistThread = std::thread(
            [&state, cancellation, generation, cache, settingsPath, executableDirectory]() {
                std::wstring failureMessage;
                try {
                    const auto settings = ReadSettings(settingsPath);
                    const auto sources =
                        FindSourceImages(settings.sourceDirectories, executableDirectory);
                    PlaylistRefreshPacket rowsPacket;
                    rowsPacket.kind = PlaylistRefreshPacket::Kind::rows;
                    rowsPacket.generation = generation;
                    rowsPacket.settings = settings;
                    rowsPacket.rows.reserve(sources.size());

                    for (const auto& source : sources) {
                        PlaylistRow row;
                        row.source = source;
                        std::error_code sizeError;
                        const auto fileSize =
                            std::filesystem::file_size(source.path, sizeError);
                        std::error_code timeError;
                        const auto writeTime =
                            std::filesystem::last_write_time(source.path, timeError);
                        if (!sizeError) {
                            row.sizeText = swc::FormatFileSize(fileSize);
                        } else {
                            row.sizeText = L"Unavailable";
                            row.statusText = L"Could not read file size";
                        }
                        if (!timeError && !sizeError) {
                            row.cacheKey = PlaylistThumbnailCacheKey(
                                source.path, fileSize, writeTime);
                        } else if (row.statusText.empty()) {
                            row.statusText = L"Could not read file metadata";
                        }
                        rowsPacket.rows.push_back(std::move(row));
                    }

                    std::vector<std::wstring> cacheKeys;
                    cacheKeys.reserve(rowsPacket.rows.size());
                    for (const auto& row : rowsPacket.rows) {
                        cacheKeys.push_back(row.cacheKey);
                    }
                    if (!cancellation->load(std::memory_order_acquire)) {
                        QueuePlaylistPacket(state, std::move(rowsPacket));
                    }

                    if (!cancellation->load(std::memory_order_acquire) && !sources.empty()) {
                        const HRESULT comResult =
                            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                        CheckHr(comResult, L"Could not initialize Playlist image loading");
                        struct ComUninitializer {
                            ~ComUninitializer() {
                                CoUninitialize();
                            }
                        } comUninitializer;
                        auto factory = CreateImagingFactory();
                        std::unordered_map<
                            std::wstring,
                            std::shared_ptr<const PlaylistThumbnail>> generatedThumbnails;

                        for (std::size_t rowIndex = 0;
                             rowIndex < sources.size();
                             ++rowIndex) {
                            if (cancellation->load(std::memory_order_acquire)) {
                                break;
                            }
                            const auto& source = sources[rowIndex];
                            PlaylistRefreshPacket thumbnailPacket;
                            thumbnailPacket.kind =
                                PlaylistRefreshPacket::Kind::thumbnail;
                            thumbnailPacket.generation = generation;
                            thumbnailPacket.rowIndex = rowIndex;
                            thumbnailPacket.cacheKey = cacheKeys[rowIndex];

                            const auto cached = cacheKeys[rowIndex].empty()
                                ? cache.end()
                                : cache.find(cacheKeys[rowIndex]);
                            if (cached != cache.end()) {
                                thumbnailPacket.thumbnail = cached->second;
                            } else if (!cacheKeys[rowIndex].empty() &&
                                generatedThumbnails.find(cacheKeys[rowIndex]) !=
                                    generatedThumbnails.end()) {
                                thumbnailPacket.thumbnail =
                                    generatedThumbnails.at(cacheKeys[rowIndex]);
                            } else {
                                auto thumbnail =
                                    std::make_shared<PlaylistThumbnail>();
                                try {
                                    thumbnail->pixels =
                                        CreatePlaylistThumbnail(factory.Get(), source.path);
                                    thumbnail->readable = true;
                                } catch (const Failure& error) {
                                    thumbnail->error = error.message;
                                }
                                thumbnailPacket.thumbnail = std::move(thumbnail);
                                if (!cacheKeys[rowIndex].empty()) {
                                    generatedThumbnails.emplace(
                                        cacheKeys[rowIndex],
                                        thumbnailPacket.thumbnail);
                                }
                            }
                            QueuePlaylistPacket(state, std::move(thumbnailPacket));
                        }
                    }
                } catch (const Failure& failure) {
                    failureMessage = failure.message;
                } catch (const std::exception&) {
                    failureMessage = L"Could not load the wallpaper Playlist.";
                } catch (...) {
                    failureMessage = L"Could not load the wallpaper Playlist.";
                }

                PlaylistRefreshPacket finishedPacket;
                finishedPacket.kind = PlaylistRefreshPacket::Kind::finished;
                finishedPacket.generation = generation;
                finishedPacket.error = std::move(failureMessage);
                QueuePlaylistPacket(state, std::move(finishedPacket));
            });
    } catch (const std::system_error& error) {
        state.playlistRunning = false;
        state.playlistCancel.reset();
        SetPlaylistHint(state, false);
        ShowError(
            L"Could not start Playlist loading: " +
                std::wstring(error.what(), error.what() +
                    std::char_traits<char>::length(error.what())),
            state.settingsWindow);
    } catch (const std::exception&) {
        state.playlistRunning = false;
        state.playlistCancel.reset();
        SetPlaylistHint(state, false);
        ShowError(L"Could not start Playlist loading.", state.settingsWindow);
    }
}

void ApplyPlaylistRows(ResidentState& state, PlaylistRefreshPacket& packet) {
    ListView_DeleteAllItems(state.playlistList);
    ImageList_RemoveAll(state.playlistImageList);
    AddPlaylistImagePlaceholders(state.playlistImageList);

    state.playlistSources.clear();
    state.playlistSources.reserve(packet.rows.size());
    state.playlistDisplayNames.assign(packet.rows.size(), L"");
    state.playlistInvalidCount = 0;
    std::unordered_map<std::wstring, bool> currentCacheKeys;
    for (const auto& row : packet.rows) {
        state.playlistSources.push_back(row.source);
        if (!row.cacheKey.empty()) {
            currentCacheKeys.emplace(row.cacheKey, true);
        }
    }
    for (auto cacheEntry = state.playlistThumbnailCache.begin();
         cacheEntry != state.playlistThumbnailCache.end();) {
        if (currentCacheKeys.find(cacheEntry->first) == currentCacheKeys.end()) {
            cacheEntry = state.playlistThumbnailCache.erase(cacheEntry);
        } else {
            ++cacheEntry;
        }
    }
    state.playlistThumbnailCacheOrder.erase(
        std::remove_if(
            state.playlistThumbnailCacheOrder.begin(),
            state.playlistThumbnailCacheOrder.end(),
            [&](const std::wstring& key) {
                return state.playlistThumbnailCache.find(key) ==
                    state.playlistThumbnailCache.end();
            }),
        state.playlistThumbnailCacheOrder.end());

    SendMessageW(state.playlistList, WM_SETREDRAW, FALSE, 0);
    for (std::size_t index = 0; index < packet.rows.size(); ++index) {
        const auto& rowData = packet.rows[index];
        const auto sourceFolder = rowData.source.path.parent_path();
        const std::wstring sourceDirectory = sourceFolder.filename().empty()
            ? sourceFolder.wstring()
            : sourceFolder.filename().wstring();
        const std::wstring fileName = rowData.source.path.filename().wstring();
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = static_cast<int>(index);
        item.iImage = 0;
        item.pszText = const_cast<wchar_t*>(sourceDirectory.c_str());
        const int row = ListView_InsertItem(state.playlistList, &item);
        if (row < 0) {
            SendMessageW(state.playlistList, WM_SETREDRAW, TRUE, 0);
            Fail(L"Could not display the wallpaper Playlist.");
        }
        ListView_SetItemText(
            state.playlistList, row, 1, const_cast<wchar_t*>(fileName.c_str()));
        ListView_SetItemText(
            state.playlistList,
            row,
            2,
            const_cast<wchar_t*>(rowData.sizeText.c_str()));
        ListView_SetItemText(
            state.playlistList,
            row,
            4,
            const_cast<wchar_t*>(rowData.statusText.c_str()));
    }
    SendMessageW(state.playlistList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.playlistList, nullptr, TRUE);
    RefreshPlaylistDisplayAssignments(state, packet.settings);
    SetPlaylistHint(state, true);
}

void ApplyPlaylistThumbnail(
    ResidentState& state,
    const PlaylistRefreshPacket& packet) {
    if (packet.rowIndex >= state.playlistSources.size() ||
        !packet.thumbnail) {
        return;
    }
    if (!packet.cacheKey.empty()) {
        state.playlistThumbnailCache[packet.cacheKey] = packet.thumbnail;
        state.playlistThumbnailCacheOrder.erase(
            std::remove(
                state.playlistThumbnailCacheOrder.begin(),
                state.playlistThumbnailCacheOrder.end(),
                packet.cacheKey),
            state.playlistThumbnailCacheOrder.end());
        state.playlistThumbnailCacheOrder.push_back(packet.cacheKey);
        while (state.playlistThumbnailCacheOrder.size() >
            kPlaylistThumbnailCacheLimit) {
            state.playlistThumbnailCache.erase(
                state.playlistThumbnailCacheOrder.front());
            state.playlistThumbnailCacheOrder.pop_front();
        }
    }
    if (packet.thumbnail->readable) {
        HBITMAP bitmap = CreatePlaylistThumbnailBitmap(packet.thumbnail->pixels);
        const int imageIndex = ImageList_Add(state.playlistImageList, bitmap, nullptr);
        DeleteObject(bitmap);
        if (imageIndex == -1) {
            Fail(L"Could not add a thumbnail to the Playlist.");
        }
        LVITEMW item{};
        item.mask = LVIF_IMAGE;
        item.iItem = static_cast<int>(packet.rowIndex);
        item.iImage = imageIndex;
        if (!ListView_SetItem(state.playlistList, &item)) {
            Fail(L"Could not update a Playlist thumbnail.");
        }
    } else {
        ListView_SetItemText(
            state.playlistList,
            static_cast<int>(packet.rowIndex),
            4,
            const_cast<wchar_t*>(L"Can't display"));
        LVITEMW item{};
        item.mask = LVIF_IMAGE;
        item.iItem = static_cast<int>(packet.rowIndex);
        item.iImage = 1;
        ListView_SetItem(state.playlistList, &item);
        ++state.playlistInvalidCount;
    }
    if (state.playlistDragGhost &&
        static_cast<int>(packet.rowIndex) == state.draggingPlaylistRow) {
        InvalidateRect(state.playlistDragGhost, nullptr, TRUE);
    }
    SetPlaylistHint(state, true);
}

void ProcessPlaylistPackets(ResidentState& state) {
    std::vector<PlaylistRefreshPacket> packets;
    {
        std::lock_guard<std::mutex> lock(state.playlistPacketMutex);
        packets.swap(state.playlistPackets);
    }

    for (auto& packet : packets) {
        if (packet.generation != state.playlistGeneration) {
            if (packet.kind == PlaylistRefreshPacket::Kind::finished &&
                state.playlistThread.joinable()) {
                state.playlistThread.join();
                state.playlistRunning = false;
                state.playlistCancel.reset();
                if (state.playlistRefreshPending && state.settingsWindow &&
                    TabCtrl_GetCurSel(state.settingsTabs) == 2) {
                    state.playlistRefreshPending = false;
                    StartPlaylistRefresh(state);
                }
            }
            continue;
        }

        try {
            if (packet.kind == PlaylistRefreshPacket::Kind::rows) {
                ApplyPlaylistRows(state, packet);
            } else if (packet.kind == PlaylistRefreshPacket::Kind::thumbnail &&
                state.settingsWindow && TabCtrl_GetCurSel(state.settingsTabs) == 2) {
                ApplyPlaylistThumbnail(state, packet);
            } else if (packet.kind == PlaylistRefreshPacket::Kind::finished) {
                FinishPlaylistRefresh(state);
                if (!state.playlistRunning) {
                    SetPlaylistHint(state, false);
                }
                if (!packet.error.empty() && state.settingsWindow) {
                    ShowError(packet.error, state.settingsWindow);
                }
            }
        } catch (const Failure& failure) {
            ShowError(failure.message, state.settingsWindow ? state.settingsWindow : state.window);
        } catch (const std::exception&) {
            ShowError(
                L"Could not update the Playlist display.",
                state.settingsWindow ? state.settingsWindow : state.window);
        }
    }
}

void SaveSourceDirectoryList(
    ResidentState& state,
    std::vector<swc::SourceDirectory> directories) {
    auto updated = state.settings;
    updated.sourceDirectories = std::move(directories);
    UpdateUserSettings(state.settingsPath, updated);
    state.settings = std::move(updated);
    RefreshSourceDirectoryList(state);
    CancelPlaylistRefresh(state);
}

HWND CreateSettingsControl(
    DWORD extendedStyle,
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    int controlId,
    HINSTANCE instance,
    HFONT font) {
    HWND control = CreateWindowExW(
        extendedStyle,
        className,
        text,
        style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        instance,
        nullptr);
    if (!control) {
        FailWin32(L"Could not create a settings control");
    }
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

void ShowSettingsPage(ResidentState& state) {
    const int page = TabCtrl_GetCurSel(state.settingsTabs);
    const bool startupPage = page == 0;
    const bool sourcesPage = page == 1;
    ShowWindow(state.changeOnStartCheckbox, startupPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.sourceDirectoryList, sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.sourceDirectoryHint, sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state.settingsWindow, kAddSourceDirectoryButtonId),
        sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state.settingsWindow, kRemoveSourceDirectoryButtonId),
        sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.playlistList, page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(state.playlistHint, page == 2 ? SW_SHOW : SW_HIDE);
}

void SaveSourceDirectoryListWithError(
    ResidentState& state,
    std::vector<swc::SourceDirectory> directories,
    HWND owner) {
    try {
        SaveSourceDirectoryList(state, std::move(directories));
    } catch (const Failure& failure) {
        RefreshSourceDirectoryList(state);
        ShowError(failure.message, owner);
    } catch (const std::exception&) {
        RefreshSourceDirectoryList(state);
        ShowError(L"Could not save the source folder list.", owner);
    }
}

LRESULT CALLBACK SourceListDropSubclass(
    HWND list,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR) {
    if (message == WM_DROPFILES) {
        SendMessageW(GetParent(list), WM_DROPFILES, wParam, lParam);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(list, SourceListDropSubclass, subclassId);
    }
    return DefSubclassProc(list, message, wParam, lParam);
}

void AddSourceDirectories(
    ResidentState& state,
    const std::vector<std::filesystem::path>& paths,
    HWND owner) {
    auto directories = state.settings.sourceDirectories;
    std::vector<std::wstring> failures;
    for (const auto& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_directory(path, error) || error) {
            failures.push_back(path.wstring() + L": not an accessible folder");
            continue;
        }
        const auto absolute = std::filesystem::absolute(path, error);
        if (error) {
            failures.push_back(path.wstring() + L": " + FilesystemErrorText(error));
            continue;
        }
        directories.push_back({absolute.wstring(), true});
    }

    if (directories.size() != state.settings.sourceDirectories.size()) {
        SaveSourceDirectoryListWithError(state, std::move(directories), owner);
    }
    if (!failures.empty()) {
        std::wstring message = L"Some dropped items could not be added as source folders.";
        for (std::size_t index = 0; index < failures.size() && index < 8; ++index) {
            message += L"\r\n\r\n" + failures[index];
        }
        if (failures.size() > 8) {
            message += L"\r\n\r\nAdditional dropped items were not folders.";
        }
        ShowError(message, owner);
    }
}

void ReorderSourceDirectory(
    ResidentState& state,
    int from,
    int insertionIndex,
    HWND owner) {
    auto directories = state.settings.sourceDirectories;
    if (from < 0 || static_cast<std::size_t>(from) >= directories.size()) {
        return;
    }
    insertionIndex = std::clamp(insertionIndex, 0, static_cast<int>(directories.size()));
    auto directory = std::move(directories[static_cast<std::size_t>(from)]);
    directories.erase(directories.begin() + from);
    if (from < insertionIndex) {
        --insertionIndex;
    }
    insertionIndex = std::clamp(insertionIndex, 0, static_cast<int>(directories.size()));
    directories.insert(directories.begin() + insertionIndex, std::move(directory));
    if (directories != state.settings.sourceDirectories) {
        SaveSourceDirectoryListWithError(state, std::move(directories), owner);
    }
}

std::wstring SelectSourceDirectory(HWND owner, const std::wstring& currentDirectory) {
    ComPtr<IFileOpenDialog> dialog;
    CheckHr(
        CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.Put())),
        L"Could not open the source folder selector");

    FILEOPENDIALOGOPTIONS options = 0;
    CheckHr(dialog->GetOptions(&options), L"Could not configure the source folder selector");
    CheckHr(
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST),
        L"Could not configure the source folder selector");
    CheckHr(dialog->SetTitle(L"Select wallpaper source folder"),
        L"Could not set the source folder selector title");

    ComPtr<IShellItem> initialFolder;
    const HRESULT initialFolderResult = SHCreateItemFromParsingName(
        currentDirectory.c_str(),
        nullptr,
        IID_PPV_ARGS(initialFolder.Put()));
    CheckHr(initialFolderResult, L"Could not locate the current source folder");
    CheckHr(dialog->SetFolder(initialFolder.Get()),
        L"Could not select the current source folder");

    const HRESULT showResult = dialog->Show(owner);
    if (showResult == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    CheckHr(showResult, L"Could not show the source folder selector");

    ComPtr<IShellItem> selectedFolder;
    CheckHr(dialog->GetResult(selectedFolder.Put()),
        L"Could not read the selected source folder");
    PWSTR rawPath = nullptr;
    CheckHr(selectedFolder->GetDisplayName(SIGDN_FILESYSPATH, &rawPath),
        L"Could not read the selected source folder path");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> selectedPath(rawPath, CoTaskMemFree);
    if (!selectedPath) {
        Fail(L"The selected source folder path is empty.");
    }
    return selectedPath.get();
}

void StartWallpaperChange(ResidentState& state, WallpaperRequest request = {});

void ShowPlaylistContextMenu(ResidentState& state, int row) {
    if (row < 0 ||
        static_cast<std::size_t>(row) >= state.playlistSources.size()) {
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        ShowError(L"Could not create the Playlist menu: " + Win32Message(GetLastError()),
            state.settingsWindow);
        return;
    }
    bool populated = true;
    for (std::size_t index = 0; index < state.playlistDisplays.size(); ++index) {
        const std::wstring label =
            L"Set as background on " + state.playlistDisplays[index].friendlyName;
        if (!AppendMenuW(
                menu,
                MF_STRING,
                kSetDisplayCommandBase + static_cast<UINT>(index),
                label.c_str())) {
            populated = false;
            break;
        }
    }
    if (!populated || state.playlistDisplays.empty()) {
        const DWORD error = GetLastError();
        DestroyMenu(menu);
        ShowError(
            state.playlistDisplays.empty()
                ? L"No connected displays are available."
                : L"Could not populate the Playlist menu: " + Win32Message(error),
            state.settingsWindow);
        return;
    }

    ListView_SetItemState(
        state.playlistList, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        const DWORD error = GetLastError();
        DestroyMenu(menu);
        ShowError(L"Could not locate the Playlist menu: " + Win32Message(error),
            state.settingsWindow);
        return;
    }
    SetForegroundWindow(state.settingsWindow);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        cursor.x,
        cursor.y,
        0,
        state.settingsWindow,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(state.settingsWindow, WM_NULL, 0, 0);
    if (command >= kSetDisplayCommandBase) {
        const std::size_t displayIndex = command - kSetDisplayCommandBase;
        if (displayIndex < state.playlistDisplays.size()) {
            StartWallpaperChange(
                state,
                WallpaperRequest{
                    false,
                    SourceImageIdentity(state.playlistSources[static_cast<std::size_t>(row)]),
                    state.playlistDisplays[displayIndex].devicePath});
        }
    }
}

LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ResidentState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<ResidentState*>(creation->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        state->settingsWindow = window;
        try {
            state->settingsFont = CreateSettingsFont();
            state->monitorPreviewPanel = CreateWindowExW(
                0,
                kMonitorPreviewPanelClass,
                L"",
                WS_CHILD | WS_VISIBLE,
                12,
                12,
                1000,
                125,
                window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kMonitorPreviewPanelId)),
                state->instance,
                nullptr);
            if (!state->monitorPreviewPanel) {
                FailWin32(L"Could not create the monitor preview area");
            }
            SendMessageW(
                state->monitorPreviewPanel,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(state->settingsFont),
                TRUE);
            state->settingsTabs = CreateSettingsControl(
                0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                12, 145, 1000, 570, window, kSettingsTabsId, state->instance,
                state->settingsFont);
            wchar_t startupTabText[] = L"Startup";
            wchar_t sourcesTabText[] = L"Sources";
            wchar_t playlistTabText[] = L"Playlist";
            TCITEMW tab{};
            tab.mask = TCIF_TEXT;
            tab.pszText = startupTabText;
            if (TabCtrl_InsertItem(state->settingsTabs, 0, &tab) == -1) {
                Fail(L"Could not add the Startup settings tab.");
            }
            tab.pszText = sourcesTabText;
            if (TabCtrl_InsertItem(state->settingsTabs, 1, &tab) == -1) {
                Fail(L"Could not add the Sources settings tab.");
            }
            tab.pszText = playlistTabText;
            if (TabCtrl_InsertItem(state->settingsTabs, 2, &tab) == -1) {
                Fail(L"Could not add the Playlist settings tab.");
            }
            TabCtrl_SetCurSel(state->settingsTabs, 0);

            state->changeOnStartCheckbox = CreateSettingsControl(
                0, L"BUTTON", L"Change wallpaper when the program starts",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                30, 187, 900, 28, window, kChangeOnStartCheckboxId, state->instance,
                state->settingsFont);
            SendMessageW(
                state->changeOnStartCheckbox,
                BM_SETCHECK,
                state->settings.changeOnStart ? BST_CHECKED : BST_UNCHECKED,
                0);

            state->sourceDirectoryList = CreateSettingsControl(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                28, 186, kSettingsContentWidth, 390, window, kSourceDirectoryListId,
                state->instance,
                state->settingsFont);
            if (!SetWindowSubclass(
                    state->sourceDirectoryList, SourceListDropSubclass, 1, 0)) {
                FailWin32(L"Could not enable folder drops on the source list");
            }
            DragAcceptFiles(state->sourceDirectoryList, TRUE);
            ListView_SetExtendedListViewStyle(
                state->sourceDirectoryList,
                LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                    LVS_EX_DOUBLEBUFFER);
            wchar_t sourceColumnText[] = L"Source folder (drag rows to reorder)";
            LVCOLUMNW sourceColumn{};
            sourceColumn.mask = LVCF_TEXT | LVCF_WIDTH;
            sourceColumn.pszText = sourceColumnText;
            sourceColumn.cx = kSettingsContentWidth - 24;
            if (ListView_InsertColumn(state->sourceDirectoryList, 0, &sourceColumn) == -1) {
                Fail(L"Could not create the source folder list column.");
            }
            CreateSettingsControl(
                0, L"BUTTON", L"Add folder...",
                WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                28, 590, 130, 32, window, kAddSourceDirectoryButtonId, state->instance,
                state->settingsFont);
            CreateSettingsControl(
                0, L"BUTTON", L"Remove",
                WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                172, 590, 110, 32, window, kRemoveSourceDirectoryButtonId, state->instance,
                state->settingsFont);
            state->sourceDirectoryHint = CreateSettingsControl(
                0, L"STATIC",
                L"Drop folders here to add them. Uncheck all folders to use the working folder.",
                WS_CHILD | SS_LEFT,
                28, 632, kSettingsContentWidth, 36, window, kSourceDirectoryHintId,
                state->instance,
                state->settingsFont);

            state->playlistList = CreateSettingsControl(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                28, 186, kSettingsContentWidth, 468, window, kPlaylistListId,
                state->instance,
                state->settingsFont);
            if (!SetWindowSubclass(
                    state->playlistList, PlaylistListDragSubclass, 1, 0)) {
                FailWin32(L"Could not enable Playlist drag feedback");
            }
            ListView_SetExtendedListViewStyle(
                state->playlistList,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            state->playlistImageList = ImageList_Create(
                kPlaylistThumbnailSize,
                kPlaylistThumbnailSize,
                ILC_COLOR32,
                16,
                16);
            if (!state->playlistImageList) {
                FailWin32(L"Could not create the playlist thumbnail list");
            }
            ListView_SetImageList(
                state->playlistList, state->playlistImageList, LVSIL_SMALL);
            const wchar_t* playlistColumnTitles[] = {
                L"Source folder", L"Name", L"Size", L"Displays", L"Status"};
            const int playlistColumnWidths[] = {270, 280, 100, 185, 105};
            for (int columnIndex = 0; columnIndex < 5; ++columnIndex) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(playlistColumnTitles[columnIndex]);
                column.cx = playlistColumnWidths[columnIndex];
                if (ListView_InsertColumn(
                        state->playlistList, columnIndex, &column) == -1) {
                    Fail(L"Could not create a Playlist column.");
                }
            }
            state->playlistHint = CreateSettingsControl(
                0, L"STATIC", L"",
                WS_CHILD | SS_LEFT,
                28, 662, kSettingsContentWidth, 28, window, kPlaylistHintId,
                state->instance,
                state->settingsFont);
            CreateSettingsControl(
                0, L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                914, 726, 82, 32, window, kCloseSettingsButtonId, state->instance,
                state->settingsFont);

            RefreshSourceDirectoryList(*state);
            DragAcceptFiles(window, TRUE);
            ShowSettingsPage(*state);
        } catch (const Failure& failure) {
            state->settingsWindowError = failure.message;
            return -1;
        } catch (const std::exception&) {
            state->settingsWindowError = L"Could not initialize the settings window.";
            return -1;
        }
        return 0;
    }
    case WM_NOTIFY:
        if (state) {
            const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
            if (notification->hwndFrom == state->settingsTabs &&
                notification->code == TCN_SELCHANGE) {
                ShowSettingsPage(*state);
                if (TabCtrl_GetCurSel(state->settingsTabs) == 2) {
                    try {
                        StartPlaylistRefresh(*state);
                    } catch (const Failure& failure) {
                        ShowError(failure.message, window);
                    } catch (const std::exception&) {
                        ShowError(L"Could not start Playlist loading.", window);
                    }
                } else {
                    CancelPlaylistRefresh(*state);
                }
                return 0;
            }
            if (notification->hwndFrom == state->sourceDirectoryList) {
                if (notification->code == LVN_ITEMCHANGED && !state->refreshingSourceList) {
                    const auto* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    const UINT oldImage =
                        (change->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                    const UINT newImage =
                        (change->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                    if ((change->uChanged & LVIF_STATE) != 0 &&
                        oldImage != newImage &&
                        change->iItem >= 0 &&
                        static_cast<std::size_t>(change->iItem) <
                            state->settings.sourceDirectories.size()) {
                        auto directories = state->settings.sourceDirectories;
                        directories[static_cast<std::size_t>(change->iItem)].enabled =
                            ListView_GetCheckState(state->sourceDirectoryList, change->iItem) != FALSE;
                        SaveSourceDirectoryListWithError(*state, std::move(directories), window);
                    }
                    return 0;
                }
                if (notification->code == LVN_BEGINDRAG) {
                    const auto* drag = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    state->draggingSourceIndex = drag->iItem;
                    state->sourceDropIndex = drag->iItem;
                    SetCapture(window);
                    return 0;
                }
            }
            if (notification->hwndFrom == state->playlistList) {
                if (notification->code == LVN_BEGINDRAG) {
                    const auto* drag = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    if (drag->iItem >= 0 &&
                        static_cast<std::size_t>(drag->iItem) <
                            state->playlistSources.size()) {
                        state->draggingPlaylistImage = SourceImageIdentity(
                            state->playlistSources[static_cast<std::size_t>(drag->iItem)]);
                        state->draggingPlaylistRow = drag->iItem;
                        state->monitorDropTarget = -1;
                        InvalidateRect(state->playlistList, nullptr, TRUE);
                        try {
                            StartPlaylistDragGhost(*state);
                        } catch (const Failure& failure) {
                            ShowError(failure.message, window);
                        } catch (const std::exception&) {
                            ShowError(L"Could not show the Playlist drag preview.", window);
                        }
                        SetCapture(window);
                        SetCursor(LoadCursorW(nullptr, IDC_NO));
                        InvalidateRect(state->monitorPreviewPanel, nullptr, TRUE);
                    }
                    return 0;
                }
                if (notification->code == NM_DBLCLK) {
                    const auto* activation =
                        reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                    if (activation->iItem >= 0 &&
                        static_cast<std::size_t>(activation->iItem) <
                            state->playlistSources.size()) {
                        StartWallpaperChange(
                            *state,
                            WallpaperRequest{
                                false,
                                SourceImageIdentity(
                                    state->playlistSources[
                                        static_cast<std::size_t>(activation->iItem)]),
                                {}});
                    }
                    return 0;
                }
                if (notification->code == NM_RCLICK) {
                    const auto* activation =
                        reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                    ShowPlaylistContextMenu(*state, activation->iItem);
                    return 0;
                }
                if (notification->code == NM_CUSTOMDRAW) {
                    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        const std::size_t row =
                            static_cast<std::size_t>(draw->nmcd.dwItemSpec);
                        if (static_cast<int>(row) == state->draggingPlaylistRow) {
                            draw->clrTextBk = RGB(255, 222, 125);
                            draw->clrText = RGB(35, 38, 42);
                            return CDRF_NEWFONT;
                        } else if (row < state->playlistDisplayNames.size() &&
                            !state->playlistDisplayNames[row].empty()) {
                            draw->clrTextBk = RGB(220, 238, 255);
                        }
                        return CDRF_DODEFAULT;
                    }
                }
            }
        }
        break;
    case WM_COMMAND:
        if (!state) {
            break;
        }
        switch (LOWORD(wParam)) {
        case kChangeOnStartCheckboxId:
            if (HIWORD(wParam) == BN_CLICKED) {
                auto updated = state->settings;
                updated.changeOnStart =
                    SendMessageW(state->changeOnStartCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                try {
                    UpdateUserSettings(state->settingsPath, updated);
                    state->settings = std::move(updated);
                } catch (const Failure& failure) {
                    SendMessageW(
                        state->changeOnStartCheckbox,
                        BM_SETCHECK,
                        state->settings.changeOnStart ? BST_CHECKED : BST_UNCHECKED,
                        0);
                    ShowError(failure.message, window);
                } catch (const std::exception&) {
                    SendMessageW(
                        state->changeOnStartCheckbox,
                        BM_SETCHECK,
                        state->settings.changeOnStart ? BST_CHECKED : BST_UNCHECKED,
                        0);
                    ShowError(L"Could not save the Startup setting.", window);
                }
                return 0;
            }
            break;
        case kAddSourceDirectoryButtonId:
            try {
                std::filesystem::path initialDirectory = CurrentDirectory();
                const int selectedRow = ListView_GetNextItem(
                    state->sourceDirectoryList, -1, LVNI_SELECTED);
                if (selectedRow >= 0 &&
                    static_cast<std::size_t>(selectedRow) <
                        state->settings.sourceDirectories.size()) {
                    const auto& selected =
                        state->settings.sourceDirectories[static_cast<std::size_t>(selectedRow)];
                    std::filesystem::path selectedPath(selected.path);
                    if (selectedPath.is_relative()) {
                        selectedPath = ExecutableDirectory() / selectedPath;
                    }
                    std::error_code error;
                    if (std::filesystem::is_directory(selectedPath, error) && !error) {
                        initialDirectory = std::move(selectedPath);
                    }
                }
                const auto selectedDirectory = SelectSourceDirectory(
                    window, initialDirectory.wstring());
                if (!selectedDirectory.empty()) {
                    AddSourceDirectories(
                        *state, {std::filesystem::path(selectedDirectory)}, window);
                }
            } catch (const Failure& failure) {
                ShowError(failure.message, window);
            } catch (const std::exception&) {
                ShowError(L"Could not add the source folder.", window);
            }
            return 0;
        case kRemoveSourceDirectoryButtonId: {
            const int selectedRow = ListView_GetNextItem(
                state->sourceDirectoryList, -1, LVNI_SELECTED);
            if (selectedRow >= 0 &&
                static_cast<std::size_t>(selectedRow) <
                    state->settings.sourceDirectories.size()) {
                auto directories = state->settings.sourceDirectories;
                directories.erase(directories.begin() + selectedRow);
                SaveSourceDirectoryListWithError(*state, std::move(directories), window);
            }
            return 0;
        }
        case kCloseSettingsButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                SendMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        default:
            break;
        }
        break;
    case WM_MOUSEMOVE:
            if (state && state->draggingSourceIndex >= 0) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(window, &point);
                ScreenToClient(state->sourceDirectoryList, &point);
                LVHITTESTINFO hit{};
                hit.pt = point;
                const int row = ListView_HitTest(state->sourceDirectoryList, &hit);
                if (row < 0) {
                    state->sourceDropIndex =
                        point.y < 0
                            ? 0
                            : static_cast<int>(state->settings.sourceDirectories.size());
                } else {
                    RECT rowRect{};
                    ListView_GetItemRect(state->sourceDirectoryList, row, &rowRect, LVIR_BOUNDS);
                    state->sourceDropIndex =
                        row + (point.y >= (rowRect.top + rowRect.bottom) / 2 ? 1 : 0);
                    ListView_SetItemState(
                        state->sourceDirectoryList, row, LVIS_SELECTED, LVIS_SELECTED);
                }
                return 0;
            }
            if (state && !state->draggingPlaylistImage.empty()) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(window, &point);
                ScreenToClient(state->monitorPreviewPanel, &point);
                const int target = MonitorPreviewAt(*state, point);
                if (target != state->monitorDropTarget) {
                    state->monitorDropTarget = target;
                    InvalidateRect(state->monitorPreviewPanel, nullptr, TRUE);
                }
                PositionPlaylistDragGhost(*state);
                SetPlaylistDragCursor(*state);
                return 0;
            }
            break;
    case WM_SETCURSOR:
            if (state && !state->draggingPlaylistImage.empty()) {
                SetPlaylistDragCursor(*state);
                PositionPlaylistDragGhost(*state);
                return TRUE;
            }
            break;
    case WM_LBUTTONUP:
            if (state && !state->draggingPlaylistImage.empty()) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(window, &point);
                ScreenToClient(state->monitorPreviewPanel, &point);
                const int target = MonitorPreviewAt(*state, point);
                std::wstring image = std::move(state->draggingPlaylistImage);
                state->draggingPlaylistImage.clear();
                state->draggingPlaylistRow = -1;
                state->monitorDropTarget = -1;
                StopPlaylistDragGhost(*state);
                InvalidateRect(state->playlistList, nullptr, TRUE);
                InvalidateRect(state->monitorPreviewPanel, nullptr, TRUE);
                ReleaseCapture();
                if (target >= 0 &&
                    static_cast<std::size_t>(target) < state->monitorPreviews.size()) {
                    StartWallpaperChange(
                        *state,
                        WallpaperRequest{
                            false,
                            std::move(image),
                            state->monitorPreviews[static_cast<std::size_t>(target)]
                                .display.devicePath});
                }
                return 0;
            }
            if (state && state->draggingSourceIndex >= 0) {
                ReleaseCapture();
                ReorderSourceDirectory(
                    *state,
                    state->draggingSourceIndex,
                    state->sourceDropIndex,
                    window);
                state->draggingSourceIndex = -1;
                state->sourceDropIndex = -1;
                return 0;
            }
            break;
    case WM_CAPTURECHANGED:
            if (state && !state->draggingPlaylistImage.empty()) {
                state->draggingPlaylistImage.clear();
                state->draggingPlaylistRow = -1;
                state->monitorDropTarget = -1;
                StopPlaylistDragGhost(*state);
                InvalidateRect(state->playlistList, nullptr, TRUE);
                InvalidateRect(state->monitorPreviewPanel, nullptr, TRUE);
                return 0;
            }
            break;
    case WM_DROPFILES:
            if (state) {
                HDROP drop = reinterpret_cast<HDROP>(wParam);
                try {
                    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                    std::vector<std::filesystem::path> paths;
                    paths.reserve(count);
                    for (UINT index = 0; index < count; ++index) {
                        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                        DragQueryFileW(drop, index, path.data(), length + 1);
                        path.resize(length);
                        paths.emplace_back(std::move(path));
                    }
                    DragFinish(drop);
                    drop = nullptr;
                    AddSourceDirectories(*state, paths, window);
                } catch (const Failure& failure) {
                    if (drop) {
                        DragFinish(drop);
                    }
                    ShowError(failure.message, window);
                } catch (const std::exception&) {
                    if (drop) {
                        DragFinish(drop);
                    }
                    ShowError(L"Could not add the dropped source folders.", window);
                }
                return 0;
            }
            break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (state && state->settingsWindow == window) {
            CancelPlaylistRefresh(*state);
            state->settingsWindow = nullptr;
            state->settingsTabs = nullptr;
            state->monitorPreviewPanel = nullptr;
            state->changeOnStartCheckbox = nullptr;
            state->sourceDirectoryList = nullptr;
            state->sourceDirectoryHint = nullptr;
            state->playlistList = nullptr;
            state->playlistHint = nullptr;
            state->monitorPreviews.clear();
            state->draggingPlaylistImage.clear();
            state->draggingPlaylistRow = -1;
            state->monitorDropTarget = -1;
            StopPlaylistDragGhost(*state);
            if (state->playlistImageList) {
                ImageList_Destroy(state->playlistImageList);
                state->playlistImageList = nullptr;
            }
            if (state->settingsFont) {
                DeleteObject(state->settingsFont);
                state->settingsFont = nullptr;
            }
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ShowSettingsWindow(ResidentState& state) {
    if (state.settingsWindow) {
        ShowWindow(state.settingsWindow, SW_SHOWNORMAL);
        SetForegroundWindow(state.settingsWindow);
        RefreshMonitorPreviewsWithError(state, state.settingsWindow);
        return;
    }

    state.settingsWindowError.clear();
    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kSettingsWindowClass,
        L"Settings - Simple Wallpaper Changer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1040,
        800,
        state.window,
        nullptr,
        state.instance,
        &state);
    if (!window) {
        ShowError(
            state.settingsWindowError.empty()
                ? L"Could not create the settings window: " + Win32Message(GetLastError())
                : state.settingsWindowError,
            state.window);
        return;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    RefreshMonitorPreviewsWithError(state, window);
}

bool AddTrayIcon(ResidentState& state) {
    state.icon = {};
    state.icon.cbSize = sizeof(state.icon);
    state.icon.hWnd = state.window;
    state.icon.uID = kTrayIconId;
    state.icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    state.icon.uCallbackMessage = kTrayCallbackMessage;
    state.icon.hIcon = LoadIconW(state.instance, MAKEINTRESOURCEW(IDI_TRAY_ICON));
    if (!state.icon.hIcon) {
        state.startupError = L"Could not load the tray icon resource.";
        return false;
    }
    lstrcpynW(
        state.icon.szTip,
        L"Simple Wallpaper Changer",
        static_cast<int>(std::size(state.icon.szTip)));
    if (!Shell_NotifyIconW(NIM_ADD, &state.icon)) {
        state.startupError = L"Could not create the tray icon: " + Win32Message(GetLastError());
        return false;
    }
    state.iconAdded = true;
    return true;
}

void RemoveTrayIcon(ResidentState& state) {
    if (state.iconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &state.icon);
        state.iconAdded = false;
    }
}

void CompleteWallpaperChange(ResidentState& state) {
    if (!state.changeRunning || !state.changeFinished.load(std::memory_order_acquire)) {
        return;
    }
    if (state.changeThread.joinable()) {
        state.changeThread.join();
    }
    state.changeRunning = false;

    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(state.errorMutex);
        error.swap(state.changeError);
    }
    if (!error.empty() && !state.shuttingDown) {
        ShowError(error, state.window);
    } else if (error.empty() && state.settingsWindow) {
        RefreshMonitorPreviewsWithError(state, state.settingsWindow);
        if (TabCtrl_GetCurSel(state.settingsTabs) == 2) {
            try {
                const auto settings = ReadSettings(state.settingsPath);
                RefreshPlaylistDisplayAssignments(state, settings);
            } catch (const Failure& failure) {
                ShowError(failure.message, state.settingsWindow);
            } catch (const std::exception&) {
                ShowError(L"Could not refresh the Playlist display highlights.",
                    state.settingsWindow);
            }
        }
    }
}

void StartWallpaperChange(ResidentState& state, WallpaperRequest request) {
    if (state.changeRunning) {
        ShowError(L"A wallpaper change is already in progress.", state.window);
        return;
    }

    state.changeRunning = true;
    state.changeFinished.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state.errorMutex);
        state.changeError.clear();
    }

    try {
        state.changeThread = std::thread([&state, request = std::move(request)]() {
            std::wstring error;
            try {
                Run(request);
            } catch (const Failure& failure) {
                error = failure.message;
            } catch (const std::exception&) {
                error = L"Wallpaper change failed due to an unexpected system error.";
            } catch (...) {
                error = L"Wallpaper change failed due to an unexpected error.";
            }

            {
                std::lock_guard<std::mutex> lock(state.errorMutex);
                state.changeError = std::move(error);
            }
            state.changeFinished.store(true, std::memory_order_release);
            PostMessageW(state.window, kWallpaperChangeCompleteMessage, 0, 0);
        });
    } catch (const std::system_error& error) {
        state.changeRunning = false;
        ShowError(
            L"Could not start the wallpaper change: " +
                std::wstring(error.what(), error.what() + std::char_traits<char>::length(error.what())),
            state.window);
    } catch (const std::exception&) {
        state.changeRunning = false;
        ShowError(L"Could not start the wallpaper change due to a system error.", state.window);
    }
}

void ShowTrayMenu(ResidentState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        ShowError(L"Could not create the tray menu: " + Win32Message(GetLastError()), state.window);
        return;
    }
    if (!AppendMenuW(menu, MF_STRING, kNextCommandId, L"Next") ||
        !AppendMenuW(menu, MF_STRING, kPreviousCommandId, L"Previous") ||
        !AppendMenuW(menu, MF_STRING, kSettingsCommandId, L"Settings...") ||
        !AppendMenuW(menu, MF_STRING, kExitCommandId, L"Exit")) {
        const DWORD error = GetLastError();
        DestroyMenu(menu);
        ShowError(L"Could not populate the tray menu: " + Win32Message(error), state.window);
        return;
    }

    SetForegroundWindow(state.window);
    const DWORD cursorPosition = GetMessagePos();
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        static_cast<short>(LOWORD(cursorPosition)),
        static_cast<short>(HIWORD(cursorPosition)),
        0,
        state.window,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(state.window, WM_NULL, 0, 0);
    if (command != 0 && !PostMessageW(
            state.window,
            WM_COMMAND,
            MAKEWPARAM(command, 0),
            0)) {
        ShowError(L"Could not dispatch the tray menu command.", state.window);
    }
}

void RequestResidentExit(ResidentState& state) {
    if (state.shuttingDown) {
        return;
    }
    state.shuttingDown = true;
    KillTimer(state.window, kWorkerPollTimerId);
    CancelPlaylistRefresh(state);
    if (state.playlistThread.joinable()) {
        state.playlistThread.join();
    }
    state.playlistRunning = false;
    {
        std::lock_guard<std::mutex> lock(state.playlistPacketMutex);
        state.playlistPackets.clear();
    }
    if (state.changeThread.joinable()) {
        state.changeThread.join();
    }
    state.changeRunning = false;
    if (state.settingsWindow) {
        DestroyWindow(state.settingsWindow);
    }
    RemoveTrayIcon(state);
    DestroyWindow(state.window);
}

LRESULT CALLBACK ResidentWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ResidentState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<ResidentState*>(creation->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE:
        if (!AddTrayIcon(*state)) {
            return -1;
        }
        if (!SetTimer(window, kWorkerPollTimerId, 250, nullptr)) {
            state->startupError = L"Could not initialize wallpaper task monitoring: " +
                Win32Message(GetLastError());
            RemoveTrayIcon(*state);
            return -1;
        }
        return 0;
    case kTrayCallbackMessage:
        if (wParam == kTrayIconId) {
            if (lParam == WM_LBUTTONUP || lParam == NIN_SELECT || lParam == NIN_KEYSELECT) {
                ShowSettingsWindow(*state);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(*state);
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kNextCommandId:
            StartWallpaperChange(*state);
            return 0;
        case kPreviousCommandId:
            StartWallpaperChange(*state, WallpaperRequest{true, {}, {}});
            return 0;
        case kSettingsCommandId:
            ShowSettingsWindow(*state);
            return 0;
        case kExitCommandId:
            RequestResidentExit(*state);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case kWallpaperChangeCompleteMessage:
        CompleteWallpaperChange(*state);
        return 0;
    case kPlaylistRefreshMessage:
        ProcessPlaylistPackets(*state);
        return 0;
    case WM_TIMER:
        if (wParam == kWorkerPollTimerId) {
            CompleteWallpaperChange(*state);
            ProcessPlaylistPackets(*state);
            return 0;
        }
        break;
    case WM_CLOSE:
        RequestResidentExit(*state);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kWorkerPollTimerId);
        RemoveTrayIcon(*state);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunResident(HINSTANCE instance) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CheckHr(comResult, L"Could not initialize the settings window");
    struct ComUninitializer {
        ~ComUninitializer() {
            CoUninitialize();
        }
    } comUninitializer;

    const auto settingsPath = ExecutableDirectory() / L"settings.ini";
    InitializeSettingsFile(settingsPath);
    const auto settings = ReadSettings(settingsPath);
    ResidentState state;
    state.instance = instance;
    state.settingsPath = settingsPath;
    state.settings = settings;

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
    if (!InitCommonControlsEx(&commonControls)) {
        FailWin32(L"Could not initialize the settings tabs");
    }

    WNDCLASSEXW settingsWindowClass{};
    settingsWindowClass.cbSize = sizeof(settingsWindowClass);
    settingsWindowClass.lpfnWndProc = SettingsWindowProc;
    settingsWindowClass.hInstance = instance;
    settingsWindowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_TRAY_ICON));
    settingsWindowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsWindowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    settingsWindowClass.lpszClassName = kSettingsWindowClass;
    if (!RegisterClassExW(&settingsWindowClass)) {
        FailWin32(L"Could not register the settings window");
    }

    WNDCLASSEXW monitorPreviewPanelClass{};
    monitorPreviewPanelClass.cbSize = sizeof(monitorPreviewPanelClass);
    monitorPreviewPanelClass.lpfnWndProc = MonitorPreviewPanelProc;
    monitorPreviewPanelClass.hInstance = instance;
    monitorPreviewPanelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    monitorPreviewPanelClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    monitorPreviewPanelClass.lpszClassName = kMonitorPreviewPanelClass;
    if (!RegisterClassExW(&monitorPreviewPanelClass)) {
        FailWin32(L"Could not register the monitor preview panel");
    }

    WNDCLASSEXW playlistDragGhostClass{};
    playlistDragGhostClass.cbSize = sizeof(playlistDragGhostClass);
    playlistDragGhostClass.lpfnWndProc = PlaylistDragGhostProc;
    playlistDragGhostClass.hInstance = instance;
    playlistDragGhostClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    playlistDragGhostClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    playlistDragGhostClass.lpszClassName = kPlaylistDragGhostClass;
    if (!RegisterClassExW(&playlistDragGhostClass)) {
        FailWin32(L"Could not register the Playlist drag preview");
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = ResidentWindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_TRAY_ICON));
    windowClass.lpszClassName = kResidentWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        FailWin32(L"Could not register the resident window");
    }

    HWND window = CreateWindowExW(
        0,
        kResidentWindowClass,
        L"Simple Wallpaper Changer",
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        &state);
    if (!window) {
        if (!state.startupError.empty()) {
            Fail(state.startupError);
        }
        FailWin32(L"Could not create the resident window");
    }

    if (settings.changeOnStart) {
        StartWallpaperChange(state);
    }

    MSG message{};
    for (;;) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1) {
            RequestResidentExit(state);
            FailWin32(L"Could not read a resident window message");
        }
        if (result == 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (state.changeThread.joinable()) {
        state.changeThread.join();
    }
    UnregisterClassW(kResidentWindowClass, instance);
    return static_cast<int>(message.wParam);
}

void LaunchResident() {
    const HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, kResidentMutexName);
    if (existing) {
        CloseHandle(existing);
        return;
    }
    const DWORD openError = GetLastError();
    if (openError != ERROR_FILE_NOT_FOUND) {
        FailWin32(L"Could not check whether the resident process is running", openError);
    }

    const auto executable = ExecutablePath();
    std::wstring commandLine = L"\"" + executable.native() + L"\" --resident";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            executable.parent_path().c_str(),
            &startup,
            &process)) {
        FailWin32(L"Could not start the resident process");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR commandLine, int) {
    if (!SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS)) {
        ShowError(L"Could not set the process to low priority: " + Win32Message(GetLastError()));
        return 1;
    }

    try {
        if (commandLine && std::string(commandLine) == "--resident") {
            SetLastError(ERROR_SUCCESS);
            HANDLE mutex = CreateMutexW(nullptr, FALSE, kResidentMutexName);
            if (!mutex) {
                FailWin32(L"Could not create the resident process guard");
            }
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(mutex);
                return 0;
            }
            const int result = RunResident(instance);
            CloseHandle(mutex);
            return result;
        }

        LaunchResident();
        return 0;
    } catch (const Failure& failure) {
        ShowError(failure.message);
        return 1;
    }
}
