#include "core.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
constexpr int kExitSettingsButtonId = 2011;
constexpr int kOpenWindowOnStartCheckboxId = 2012;
constexpr int kStartWithWindowsCheckboxId = 2034;
constexpr int kBackgroundFillColorModeRadioId = 2013;
constexpr int kBackgroundFillColorButtonId = 2014;
constexpr int kBackgroundFillModeLabelId = 2015;
constexpr int kBackgroundFillColorLabelId = 2016;
constexpr int kBackgroundAdaptiveModeRadioId = 2017;
constexpr int kImageResizeToRadioId = 2018;
constexpr int kImageAutoFitRadioId = 2019;
constexpr int kImageResizeToPercentEditId = 2020;
constexpr int kImageResizeToPercentLabelId = 2021;
constexpr int kImageSmallGroupId = 2022;
constexpr int kImageSmallCenteredRadioId = 2023;
constexpr int kImageSmallTileRadioId = 2024;
constexpr int kImageSmallFitRadioId = 2025;
constexpr int kImageMaxEnlargeLabelId = 2026;
constexpr int kImageMaxEnlargePercentEditId = 2027;
constexpr int kImageMaxEnlargePercentLabelId = 2028;
constexpr int kImageLargeGroupId = 2029;
constexpr int kImageLargeCenteredRadioId = 2030;
constexpr int kImageLargeFillRadioId = 2031;
constexpr int kImageLargeFitRadioId = 2032;
constexpr int kImageResizeToPercentUnitLabelId = 2033;
constexpr int kSameImageOnDisplaysRadioId = 2035;
constexpr int kSeparateImagesOnDisplaysRadioId = 2036;
constexpr int kMultiMonitorSettingsGroupId = 2037;
constexpr int kImagePlacementGroupId = 2038;
constexpr int kCountdownEnabledCheckboxId = 2040;
constexpr int kCountdownHoursEditId = 2041;
constexpr int kCountdownMinutesEditId = 2042;
constexpr int kCountdownSecondsEditId = 2043;
constexpr int kCountdownPreviousButtonId = 2044;
constexpr int kCountdownStartStopButtonId = 2045;
constexpr int kCountdownPauseResumeButtonId = 2046;
constexpr int kCountdownRestartButtonId = 2047;
constexpr int kCountdownNextButtonId = 2048;
constexpr int kCountdownDurationGroupId = 2049;
constexpr int kCountdownDisplayId = 2051;
constexpr int kCountdownHoursLabelId = 2052;
constexpr int kCountdownMinutesLabelId = 2053;
constexpr int kCountdownSecondsLabelId = 2054;
constexpr UINT kSetDisplayCommandBase = 40000;
constexpr int kPlaylistThumbnailSize = 64;
constexpr std::size_t kPlaylistThumbnailCacheLimit = 512;
constexpr int kSettingsContentWidth = 968;
constexpr int kMonitorPreviewGap = 12;
constexpr int kSettingsMinimumClientWidth = 800;
constexpr int kSettingsMinimumClientHeight = 640;
constexpr DWORD kSettingsWindowStyle =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN |
    WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
constexpr DWORD kSettingsWindowExtendedStyle =
    WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
constexpr wchar_t kMonitorPreviewPanelClass[] = L"SWCMonitorPreviewPanel";
constexpr wchar_t kPlaylistDragGhostClass[] = L"SWCPlaylistDragGhost";
constexpr int kPlaylistDragGhostWidth = 110;
constexpr int kPlaylistDragGhostHeight = 104;
constexpr int kSourceDragGhostWidth = 220;
constexpr int kSourceDragGhostHeight = 64;
std::mutex gSettingsMutex;
COLORREF gCustomBackgroundColors[16]{};

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

void SetStartWithWindows(bool enabled) {
    constexpr wchar_t runKeyPath[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t valueName[] = L"Simple Wallpaper Changer";
    HKEY key = nullptr;

    if (enabled) {
        DWORD disposition = 0;
        const LSTATUS openResult = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            runKeyPath,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &key,
            &disposition);
        if (openResult != ERROR_SUCCESS) {
            FailWin32(L"Could not open the current-user startup registry key", openResult);
        }

        struct RegistryKeyCloser {
            HKEY value;
            ~RegistryKeyCloser() {
                if (value) {
                    RegCloseKey(value);
                }
            }
        } closer{key};

        const std::wstring command = L"\"" + ExecutablePath().wstring() + L"\"";
        const std::uint64_t byteCount =
            (static_cast<std::uint64_t>(command.size()) + 1) * sizeof(wchar_t);
        if (byteCount > std::numeric_limits<DWORD>::max()) {
            Fail(L"The executable path is too long to register for Windows startup.");
        }
        const LSTATUS setResult = RegSetValueExW(
            key,
            valueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>(byteCount));
        if (setResult != ERROR_SUCCESS) {
            FailWin32(L"Could not register the program to start with Windows", setResult);
        }
        return;
    }

    const LSTATUS openResult = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        runKeyPath,
        0,
        KEY_SET_VALUE,
        &key);
    if (openResult == ERROR_FILE_NOT_FOUND) {
        return;
    }
    if (openResult != ERROR_SUCCESS) {
        FailWin32(L"Could not open the current-user startup registry key", openResult);
    }

    struct RegistryKeyCloser {
        HKEY value;
        ~RegistryKeyCloser() {
            if (value) {
                RegCloseKey(value);
            }
        }
    } closer{key};

    const LSTATUS deleteResult = RegDeleteValueW(key, valueName);
    if (deleteResult != ERROR_SUCCESS && deleteResult != ERROR_FILE_NOT_FOUND) {
        FailWin32(L"Could not remove the program from Windows startup", deleteResult);
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
    auto stem = path.stem().wstring();
    const auto reapplyMarker = stem.rfind(L"-reapply-");
    if (reapplyMarker != std::wstring::npos) {
        const auto refreshId = stem.substr(reapplyMarker + 9);
        if (refreshId.empty() || !std::all_of(
                refreshId.begin(),
                refreshId.end(),
                [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) {
            return false;
        }
        stem.resize(reapplyMarker);
    }

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

bool IsReappliedWallpaperVariant(
    const std::filesystem::path& candidate,
    const std::filesystem::path& basePath) {
    if (!WindowsTextEquals(
            candidate.parent_path().wstring(),
            basePath.parent_path().wstring()) ||
        !WindowsTextEquals(
            candidate.extension().wstring(),
            basePath.extension().wstring())) {
        return false;
    }
    const auto candidateStem = candidate.stem().wstring();
    const auto baseStem = basePath.stem().wstring();
    const std::wstring prefix = baseStem + L"-reapply-";
    if (!StartsWithWindowsText(candidateStem, prefix)) {
        return false;
    }
    const auto refreshId = candidateStem.substr(prefix.size());
    return !refreshId.empty() && std::all_of(
        refreshId.begin(),
        refreshId.end(),
        [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
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
    UINT pixelBufferWidth = 0;
    UINT pixelBufferHeight = 0;
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
    const std::filesystem::path& destination,
    const swc::Settings& settings) {
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

    const auto placement = swc::CalculateImagePlacement(
        sourceWidth,
        sourceHeight,
        outputWidth,
        outputHeight,
        settings);
    const UINT scaledWidth = placement.width;
    const UINT scaledHeight = placement.height;
    if (scaledWidth == 0 || scaledHeight == 0 ||
        scaledWidth > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
        scaledHeight > static_cast<UINT>(std::numeric_limits<LONG>::max())) {
        Fail(L"The selected image sizing produces dimensions that are too large.");
    }

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

    const auto background = settings.backgroundFillMode ==
            swc::BackgroundFillMode::adaptiveToImage
        ? EstimateImageBackgroundColor(factory, converter.Get(), sourceWidth, sourceHeight)
        : settings.backgroundFillColor;

    ComPtr<IWICBitmapScaler> scaler;
    CheckHr(factory->CreateBitmapScaler(scaler.Put()), L"Could not create image scaler");
    CheckHr(
        scaler->Initialize(
            converter.Get(),
            scaledWidth,
            scaledHeight,
            WICBitmapInterpolationModeFant),
        L"Could not resize the source image");

    if (outputWidth > std::numeric_limits<UINT>::max() / 4 / outputHeight) {
        Fail(L"The image dimensions are too large to process.");
    }
    const std::uint64_t outputBytes =
        static_cast<std::uint64_t>(outputWidth) * outputHeight * 4;
    std::vector<BYTE> canvas(static_cast<std::size_t>(outputBytes));
    for (std::size_t offset = 0; offset < canvas.size(); offset += 4) {
        canvas[offset] = background.blue;
        canvas[offset + 1] = background.green;
        canvas[offset + 2] = background.red;
        canvas[offset + 3] = 255;
    }

    const UINT outputStride = outputWidth * 4;
    std::vector<BYTE> tilePixels;
    UINT tileStride = 0;
    if (placement.tileColumns > 1 || placement.tileRows > 1) {
        const std::uint64_t tileBytes =
            static_cast<std::uint64_t>(scaledWidth) * scaledHeight * 4;
        if (tileBytes > std::numeric_limits<UINT>::max()) {
            Fail(L"The tile image is too large to process.");
        }
        tileStride = scaledWidth * 4;
        tilePixels.resize(static_cast<std::size_t>(tileBytes));
        CheckHr(
            scaler->CopyPixels(
                nullptr,
                tileStride,
                static_cast<UINT>(tileBytes),
                tilePixels.data()),
            L"Could not read the tiled image pixels");
    }

    const auto drawTile = [&](std::int64_t left, std::int64_t top) {
        const std::int64_t clippedLeft = std::max<std::int64_t>(0, left);
        const std::int64_t clippedTop = std::max<std::int64_t>(0, top);
        const std::int64_t clippedRight = std::min<std::int64_t>(
            outputWidth, left + scaledWidth);
        const std::int64_t clippedBottom = std::min<std::int64_t>(
            outputHeight, top + scaledHeight);
        if (clippedLeft >= clippedRight || clippedTop >= clippedBottom) {
            return;
        }

        const UINT copyWidth = static_cast<UINT>(clippedRight - clippedLeft);
        const UINT copyHeight = static_cast<UINT>(clippedBottom - clippedTop);
        const UINT copyStride = copyWidth * 4;
        if (!tilePixels.empty()) {
            for (UINT row = 0; row < copyHeight; ++row) {
                const std::size_t sourceOffset =
                    static_cast<std::size_t>(clippedTop + row - top) * tileStride +
                    static_cast<std::size_t>(clippedLeft - left) * 4;
                const std::size_t destinationOffset =
                    static_cast<std::size_t>(clippedTop + row) * outputStride +
                    static_cast<std::size_t>(clippedLeft) * 4;
                std::copy_n(
                    tilePixels.data() + sourceOffset,
                    copyStride,
                    canvas.data() + destinationOffset);
                for (UINT column = 0; column < copyWidth; ++column) {
                    canvas[destinationOffset + static_cast<std::size_t>(column) * 4 + 3] =
                        255;
                }
            }
            return;
        }

        const std::uint64_t copyBytes =
            static_cast<std::uint64_t>(copyStride) * copyHeight;
        if (copyBytes > std::numeric_limits<UINT>::max()) {
            Fail(L"The selected image area is too large to process.");
        }
        std::vector<BYTE> scaledPixels(static_cast<std::size_t>(copyBytes));
        WICRect sourceRect{
            static_cast<INT>(clippedLeft - left),
            static_cast<INT>(clippedTop - top),
            static_cast<INT>(copyWidth),
            static_cast<INT>(copyHeight)};
        CheckHr(
            scaler->CopyPixels(
                &sourceRect,
                copyStride,
                static_cast<UINT>(copyBytes),
                scaledPixels.data()),
            L"Could not read the resized image pixels");

        for (UINT row = 0; row < copyHeight; ++row) {
            const std::size_t sourceOffset = static_cast<std::size_t>(row) * copyStride;
            const std::size_t destinationOffset =
                static_cast<std::size_t>(clippedTop + row) * outputStride +
                static_cast<std::size_t>(clippedLeft) * 4;
            for (UINT column = 0; column < copyWidth; ++column) {
                const std::size_t pixel =
                    sourceOffset + static_cast<std::size_t>(column) * 4;
                const std::size_t outputPixel =
                    destinationOffset + static_cast<std::size_t>(column) * 4;
                canvas[outputPixel] = scaledPixels[pixel];
                canvas[outputPixel + 1] = scaledPixels[pixel + 1];
                canvas[outputPixel + 2] = scaledPixels[pixel + 2];
                canvas[outputPixel + 3] = 255;
            }
        }
    };

    for (std::uint32_t row = 0; row < placement.tileRows; ++row) {
        for (std::uint32_t column = 0; column < placement.tileColumns; ++column) {
            drawTile(
                placement.left + static_cast<std::int64_t>(column) * scaledWidth,
                placement.top + static_cast<std::int64_t>(row) * scaledHeight);
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

void RemoveUnusedGeneratedWallpapers(
    const std::filesystem::path& directory,
    IDesktopWallpaper* wallpaper,
    const std::vector<Display>& displays) {
    for (std::size_t displayIndex = 0; displayIndex < displays.size(); ++displayIndex) {
        const auto& display = displays[displayIndex];
        LPWSTR rawWallpaperPath = nullptr;
        CheckHr(
            wallpaper->GetWallpaper(display.devicePath.c_str(), &rawWallpaperPath),
            L"Could not inspect the current wallpaper for " + display.friendlyName);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> wallpaperPath(
            rawWallpaperPath, CoTaskMemFree);
        const std::wstring baseName = displayIndex == 0
            ? L"wallpaper-background"
            : L"wallpaper-background-" + std::to_wstring(displayIndex + 1);
        const auto basePath = directory / (baseName + L".png");

        std::error_code error;
        std::filesystem::directory_iterator iterator(directory, error);
        if (error) {
            Fail(L"Could not inspect generated wallpapers: " + FilesystemErrorText(error));
        }
        const std::filesystem::directory_iterator end;
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                Fail(L"Could not enumerate generated wallpapers: " + FilesystemErrorText(error));
            }

            const auto& entry = *iterator;
            const bool regularFile = entry.is_regular_file(error);
            if (error) {
                Fail(L"Could not inspect a generated wallpaper: " + FilesystemErrorText(error));
            }
            if (!regularFile || !IsReappliedWallpaperVariant(entry.path(), basePath)) {
                continue;
            }
            if (wallpaperPath &&
                WindowsTextEquals(entry.path().wstring(), wallpaperPath.get())) {
                continue;
            }
            if (!DeleteFileW(entry.path().c_str())) {
                const DWORD deleteError = GetLastError();
                if (deleteError != ERROR_FILE_NOT_FOUND &&
                    deleteError != ERROR_PATH_NOT_FOUND) {
                    FailWin32(
                        L"Could not remove an unused generated wallpaper",
                        deleteError);
                }
            }
        }
        if (error) {
            Fail(L"Could not finish enumerating generated wallpapers: " +
                FilesystemErrorText(error));
        }
    }
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
        L"; Open the Settings window when the program starts: true or false.\r\n";
    contents += L"open_window_on_start=" +
        std::wstring(settings.openWindowOnStart ? L"true" : L"false") + L"\r\n"
        L"; Start the program automatically with the current Windows user: true or false.\r\n";
    contents += L"start_with_windows=" +
        std::wstring(settings.startWithWindows ? L"true" : L"false") + L"\r\n"
        L"; Settings window position and size (left,top,width,height); maintained automatically.\r\n";
    if (settings.hasWindowBounds) {
        contents += L"settings_window_bounds=" +
            std::to_wstring(settings.windowBounds.x) + L"," +
            std::to_wstring(settings.windowBounds.y) + L"," +
            std::to_wstring(settings.windowBounds.width) + L"," +
            std::to_wstring(settings.windowBounds.height) + L"\r\n";
    }
    contents +=
        L"; Background fill mode: fill_color or adaptive.\r\n"
        L"background_fill=" +
        std::wstring(
            settings.backgroundFillMode == swc::BackgroundFillMode::fillColor
                ? L"fill_color"
                : L"adaptive") + L"\r\n";
    std::wostringstream backgroundColor;
    backgroundColor << std::uppercase << std::hex << std::setfill(L'0')
        << std::setw(2) << static_cast<unsigned int>(settings.backgroundFillColor.red)
        << std::setw(2) << static_cast<unsigned int>(settings.backgroundFillColor.green)
        << std::setw(2) << static_cast<unsigned int>(settings.backgroundFillColor.blue);
    contents += L"; Fill color in RGB hex; used when background_fill=fill_color.\r\n"
        L"background_color=" + backgroundColor.str() + L"\r\n"
        L"; Image sizing: resize_to or autofit.\r\n"
        L"image_sizing=" +
        std::wstring(
            settings.imageSizingMode == swc::ImageSizingMode::resizeTo
                ? L"resize_to"
                : L"autofit") + L"\r\n"
        L"; Resize-to target percentage: 1 through 1000.\r\n"
        L"resize_to_percent=" + std::to_wstring(settings.resizeToPercent) + L"\r\n"
        L"; Autofit behavior for images that fit inside the display: centered, tile, or fit.\r\n"
        L"autofit_small=" +
        std::wstring(
            settings.smallImageMode == swc::SmallImageMode::centered
                ? L"centered"
                : settings.smallImageMode == swc::SmallImageMode::tile
                ? L"tile"
                : L"fit") + L"\r\n"
        L"; Maximum enlargement percentage for small-image Fit: 100 through 1000.\r\n"
        L"autofit_max_enlarge_percent=" +
        std::to_wstring(settings.maxSmallImageEnlargePercent) + L"\r\n"
        L"; Autofit behavior for images exceeding the display: centered, fill, or fit.\r\n"
        L"autofit_large=" +
        std::wstring(
            settings.largeImageMode == swc::LargeImageMode::centered
                ? L"centered"
                : settings.largeImageMode == swc::LargeImageMode::fill
                ? L"fill"
                : L"fit") + L"\r\n"
        L"; Multi-monitor source selection: same_image or separate_images.\r\n"
        L"multi_monitor_image_mode=" +
        std::wstring(
            settings.multiMonitorImageMode == swc::MultiMonitorImageMode::sameImage
                ? L"same_image"
                : L"separate_images") + L"\r\n"
        L"; Timed wallpaper changes: enabled, hours, minutes, seconds.\r\n"
        L"countdown_enabled=" +
        std::wstring(settings.countdownEnabled ? L"true" : L"false") + L"\r\n"
        L"countdown_hours=" + std::to_wstring(settings.countdownHours) + L"\r\n"
        L"countdown_minutes=" + std::to_wstring(settings.countdownMinutes) + L"\r\n"
        L"countdown_seconds=" + std::to_wstring(settings.countdownSeconds) + L"\r\n"
        L"; Example configuration:\r\n"
        L"; source_directories=C:\\Users\\Example\\Pictures\\Wallpapers\r\n"
        L"; source_directories=-D:\\SeasonalWallpapers\r\n"
        L"; last_image=0|C:\\Users\\Example\\Pictures\\Wallpapers\\wallpaper2.jpg\r\n"
        L"; display_image=\\\\?\\DISPLAY#ABC|C:\\Users\\Example\\Pictures\\Wallpapers\\wallpaper2.jpg\r\n"
        L"; change_on_start=false\r\n"
        L"; open_window_on_start=true\r\n";
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
    settings.openWindowOnStart = preferences.openWindowOnStart;
    settings.startWithWindows = preferences.startWithWindows;
    settings.backgroundFillMode = preferences.backgroundFillMode;
    settings.backgroundFillColor = preferences.backgroundFillColor;
    settings.imageSizingMode = preferences.imageSizingMode;
    settings.resizeToPercent = preferences.resizeToPercent;
    settings.smallImageMode = preferences.smallImageMode;
    settings.maxSmallImageEnlargePercent = preferences.maxSmallImageEnlargePercent;
    settings.largeImageMode = preferences.largeImageMode;
    settings.multiMonitorImageMode = preferences.multiMonitorImageMode;
    settings.countdownEnabled = preferences.countdownEnabled;
    settings.countdownHours = preferences.countdownHours;
    settings.countdownMinutes = preferences.countdownMinutes;
    settings.countdownSeconds = preferences.countdownSeconds;
    WriteSettingsFile(path, settings);
}

void UpdateSettingsWindowBounds(
    const std::filesystem::path& path,
    const swc::WindowBounds& bounds) {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    auto settings = ReadSettingsFile(path);
    settings.windowBounds = bounds;
    settings.hasWindowBounds = true;
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
    bool reapplyCurrent = false;
    bool reapplyMultiMonitorMode = false;
};

void Run(const WallpaperRequest& request) {
    const auto executableDirectory = ExecutableDirectory();
    const auto settingsPath = executableDirectory / L"settings.ini";
    const auto settings = ReadSettings(settingsPath);
    const auto images = request.reapplyCurrent && !request.reapplyMultiMonitorMode
        ? std::vector<SourceImage>{}
        : FindSourceImages(settings.sourceDirectories, executableDirectory);
    if (images.empty() && !request.reapplyCurrent) {
        return;
    }
    if (images.empty() && request.reapplyMultiMonitorMode) {
        Fail(L"Could not reapply the multi-monitor setting because no source images are available.");
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

    if (request.reapplyCurrent && !request.reapplyMultiMonitorMode) {
        const std::wstring refreshId = std::to_wstring(GetTickCount64());
        for (std::size_t displayIndex = 0; displayIndex < displays.size(); ++displayIndex) {
            const auto current = std::find_if(
                settings.displayImages.begin(),
                settings.displayImages.end(),
                [&](const swc::DisplayImage& item) {
                    return CompareStringOrdinal(
                        item.devicePath.data(),
                        static_cast<int>(item.devicePath.size()),
                        displays[displayIndex].devicePath.data(),
                        static_cast<int>(displays[displayIndex].devicePath.size()),
                        TRUE) == CSTR_EQUAL;
                });
            if (current == settings.displayImages.end()) {
                continue;
            }
            const auto& basePath = outputPaths[displayIndex];
            outputPaths[displayIndex] = basePath.parent_path() /
                (basePath.stem().wstring() + L"-reapply-" + refreshId +
                    basePath.extension().wstring());
            temporaryPaths[displayIndex] = outputPaths[displayIndex].wstring() + L".tmp";
            const std::filesystem::path image(current->imagePath);
            CreateBackgroundPng(
                factory.Get(),
                image,
                displays[displayIndex].width,
                displays[displayIndex].height,
                temporaryPaths[displayIndex],
                settings);
            generated.push_back({
                displays[displayIndex],
                temporaryPaths[displayIndex],
                outputPaths[displayIndex]});
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
        RemoveUnusedGeneratedWallpapers(executableDirectory, wallpaper.Get(), displays);
        return;
    }

    if (request.reapplyCurrent && request.reapplyMultiMonitorMode) {
        const auto findImageIndex = [&](const std::wstring& imagePath) {
            for (std::size_t index = 0; index < images.size(); ++index) {
                const std::wstring path = images[index].path.wstring();
                if (CompareStringOrdinal(
                        path.data(),
                        static_cast<int>(path.size()),
                        imagePath.data(),
                        static_cast<int>(imagePath.size()),
                        TRUE) == CSTR_EQUAL) {
                    return index;
                }
            }
            return images.size();
        };
        const auto currentDisplayImage = [&](std::size_t displayIndex) {
            const auto current = std::find_if(
                settings.displayImages.begin(),
                settings.displayImages.end(),
                [&](const swc::DisplayImage& item) {
                    return CompareStringOrdinal(
                        item.devicePath.data(),
                        static_cast<int>(item.devicePath.size()),
                        displays[displayIndex].devicePath.data(),
                        static_cast<int>(displays[displayIndex].devicePath.size()),
                        TRUE) == CSTR_EQUAL;
                });
            return current == settings.displayImages.end()
                ? images.size()
                : findImageIndex(current->imagePath);
        };

        const std::wstring refreshId = std::to_wstring(GetTickCount64());
        for (std::size_t displayIndex = 0; displayIndex < displays.size(); ++displayIndex) {
            const auto& basePath = outputPaths[displayIndex];
            outputPaths[displayIndex] = basePath.parent_path() /
                (basePath.stem().wstring() + L"-reapply-" + refreshId +
                    basePath.extension().wstring());
            temporaryPaths[displayIndex] = outputPaths[displayIndex].wstring() + L".tmp";
        }

        std::vector<std::size_t> selectedImages(displays.size(), images.size());
        if (settings.multiMonitorImageMode == swc::MultiMonitorImageMode::sameImage) {
            std::size_t currentImage = displays.empty()
                ? images.size()
                : currentDisplayImage(0);
            if (currentImage == images.size()) {
                const auto cursor = std::find(names.begin(), names.end(), savedCursor);
                currentImage = cursor == names.end()
                    ? 0
                    : static_cast<std::size_t>(cursor - names.begin());
            }

            std::wstring lastFailure;
            bool rendered = false;
            for (const std::size_t candidate :
                 swc::CandidateAttemptOrderFrom(names, names[currentImage])) {
                try {
                    for (std::size_t displayIndex = 0;
                         displayIndex < displays.size();
                         ++displayIndex) {
                        CreateBackgroundPng(
                            factory.Get(),
                            images[candidate].path,
                            displays[displayIndex].width,
                            displays[displayIndex].height,
                            temporaryPaths[displayIndex],
                            settings);
                    }
                    std::fill(
                        selectedImages.begin(), selectedImages.end(), candidate);
                    rendered = true;
                    break;
                } catch (const Failure& failure) {
                    lastFailure = failure.message;
                    for (const auto& path : temporaryPaths) {
                        DeleteFileW(path.c_str());
                    }
                }
            }
            if (!rendered && !displays.empty()) {
                Fail(lastFailure.empty()
                    ? L"No usable image could be processed for all displays."
                    : L"No usable image could be processed for all displays.\r\n\r\n" +
                        lastFailure);
            }
        } else {
            bool hasCompleteAssignments = !displays.empty();
            bool hasDifferentImages = false;
            for (std::size_t displayIndex = 0;
                 displayIndex < displays.size();
                 ++displayIndex) {
                selectedImages[displayIndex] = currentDisplayImage(displayIndex);
                if (selectedImages[displayIndex] == images.size()) {
                    hasCompleteAssignments = false;
                    break;
                }
                if (displayIndex != 0 &&
                    selectedImages[displayIndex] != selectedImages[0]) {
                    hasDifferentImages = true;
                }
            }

            if (!hasCompleteAssignments || !hasDifferentImages) {
                std::size_t cursorIndex = 0;
                const auto cursor = std::find(names.begin(), names.end(), savedCursor);
                if (cursor != names.end()) {
                    cursorIndex = static_cast<std::size_t>(cursor - names.begin());
                }
                for (std::size_t offset = displays.size(); offset > 0; --offset) {
                    selectedImages[offset - 1] = cursorIndex;
                    if (offset > 1) {
                        const auto previous =
                            swc::PreviousCandidateAttemptOrder(names, names[cursorIndex]);
                        cursorIndex = previous.front();
                    }
                }
            }

            for (std::size_t displayIndex = 0;
                 displayIndex < displays.size();
                 ++displayIndex) {
                const std::size_t desiredImage = selectedImages[displayIndex];
                std::wstring lastFailure;
                bool rendered = false;
                for (const std::size_t candidate : swc::CandidateAttemptOrderFrom(
                         names, names[desiredImage])) {
                    try {
                        CreateBackgroundPng(
                            factory.Get(),
                            images[candidate].path,
                            displays[displayIndex].width,
                            displays[displayIndex].height,
                            temporaryPaths[displayIndex],
                            settings);
                        selectedImages[displayIndex] = candidate;
                        rendered = true;
                        break;
                    } catch (const Failure& failure) {
                        lastFailure = failure.message;
                        DeleteFileW(temporaryPaths[displayIndex].c_str());
                    }
                }
                if (!rendered) {
                    Fail(lastFailure.empty()
                        ? L"No usable image could be processed for display " +
                            std::to_wstring(displayIndex + 1) + L"."
                        : L"No usable image could be processed for display " +
                            std::to_wstring(displayIndex + 1) + L".\r\n\r\n" +
                            lastFailure);
                }
            }
        }

        for (std::size_t displayIndex = 0;
             displayIndex < displays.size();
             ++displayIndex) {
            if (!MoveFileExW(
                    temporaryPaths[displayIndex].c_str(),
                    outputPaths[displayIndex].c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                FailWin32(L"Could not publish a generated wallpaper");
            }
            generated.push_back({
                displays[displayIndex],
                temporaryPaths[displayIndex],
                outputPaths[displayIndex]});
        }
        for (const auto& item : generated) {
            ApplyWallpaper(wallpaper.Get(), item.display, item.outputPath);
        }
        std::vector<swc::DisplayImage> displayImages;
        displayImages.reserve(displays.size());
        for (std::size_t displayIndex = 0;
             displayIndex < displays.size();
             ++displayIndex) {
            displayImages.push_back({
                displays[displayIndex].devicePath,
                images[selectedImages[displayIndex]].path.wstring()});
        }
        UpdateWallpaperSettings(
            settingsPath, settings.lastImage, displayImages, true);
        RemoveUnusedGeneratedWallpapers(executableDirectory, wallpaper.Get(), displays);
        return;
    }

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
                    temporaryPaths[displayIndex],
                    settings);
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
    if (request.displayDevicePath.empty() && request.previous && hasSavedCursor &&
        settings.multiMonitorImageMode == swc::MultiMonitorImageMode::separateImages) {
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

        if (settings.multiMonitorImageMode == swc::MultiMonitorImageMode::sameImage) {
            if (!displays.empty()) {
                const std::size_t selectedIndex = selectCandidate(
                    0,
                    startAtImage ? request.firstImage : lastImage,
                    startAtImage);
                lastImage = names[selectedIndex];
                lastDisplayImage = lastImage;
                for (std::size_t displayIndex = 0;
                     displayIndex < displays.size();
                     ++displayIndex) {
                    if (displayIndex != 0) {
                        CreateBackgroundPng(
                            factory.Get(),
                            images[selectedIndex].path,
                            displays[displayIndex].width,
                            displays[displayIndex].height,
                            temporaryPaths[displayIndex],
                            settings);
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
        } else {
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
    RemoveUnusedGeneratedWallpapers(executableDirectory, wallpaper.Get(), displays);
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
    HWND countdownDisplay = nullptr;
    HWND countdownPreviousButton = nullptr;
    HWND countdownStartStopButton = nullptr;
    HWND countdownPauseResumeButton = nullptr;
    HWND countdownRestartButton = nullptr;
    HWND countdownNextButton = nullptr;
    HWND countdownEnabledCheckbox = nullptr;
    HWND countdownDurationGroup = nullptr;
    HWND countdownHoursEdit = nullptr;
    HWND countdownMinutesEdit = nullptr;
    HWND countdownSecondsEdit = nullptr;
    HWND countdownHoursLabel = nullptr;
    HWND countdownMinutesLabel = nullptr;
    HWND countdownSecondsLabel = nullptr;
    HWND changeOnStartCheckbox = nullptr;
    HWND openWindowOnStartCheckbox = nullptr;
    HWND startWithWindowsCheckbox = nullptr;
    HWND backgroundFillModeLabel = nullptr;
    HWND backgroundFillColorModeRadio = nullptr;
    HWND backgroundAdaptiveModeRadio = nullptr;
    HWND backgroundFillColorLabel = nullptr;
    HWND backgroundFillColorButton = nullptr;
    HWND multiMonitorSettingsGroup = nullptr;
    HWND imagePlacementGroup = nullptr;
    HWND imageResizeToRadio = nullptr;
    HWND imageAutoFitRadio = nullptr;
    HWND imageResizeToPercentEdit = nullptr;
    HWND imageResizeToPercentLabel = nullptr;
    HWND imageResizeToPercentUnitLabel = nullptr;
    HWND imageSmallGroup = nullptr;
    HWND imageSmallCenteredRadio = nullptr;
    HWND imageSmallTileRadio = nullptr;
    HWND imageSmallFitRadio = nullptr;
    HWND imageMaxEnlargeLabel = nullptr;
    HWND imageMaxEnlargePercentEdit = nullptr;
    HWND imageMaxEnlargePercentLabel = nullptr;
    HWND imageLargeGroup = nullptr;
    HWND imageLargeCenteredRadio = nullptr;
    HWND imageLargeFillRadio = nullptr;
    HWND imageLargeFitRadio = nullptr;
    HWND sameImageOnDisplaysRadio = nullptr;
    HWND separateImagesOnDisplaysRadio = nullptr;
    RECT settingsWindowNormalRect{};
    bool hasSettingsWindowNormalRect = false;
    HWND sourceDirectoryList = nullptr;
    HWND sourceDirectoryHint = nullptr;
    HWND playlistList = nullptr;
    HWND playlistHint = nullptr;
    HFONT settingsFont = nullptr;
    HFONT countdownFont = nullptr;
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
    bool backgroundReapplyPending = false;
    bool multiMonitorModeReapplyPending = false;
    bool countdownPaused = false;
    bool timerNextPending = false;
    std::uint32_t countdownRemainingSeconds = 600;
    std::uint32_t lastDisplayedCountdown = std::numeric_limits<std::uint32_t>::max();
    std::chrono::steady_clock::time_point countdownDeadline{};
    bool shuttingDown = false;
    std::atomic_bool changeFinished = false;
    std::thread changeThread;
    std::mutex errorMutex;
    std::wstring changeError;
    std::wstring startupError;
};

std::wstring FormatCountdown(std::uint32_t seconds) {
    const std::uint32_t hours = seconds / 3600;
    const std::uint32_t minutes = (seconds / 60) % 60;
    const std::uint32_t remainder = seconds % 60;
    std::wostringstream formatted;
    formatted << std::setfill(L'0') << std::setw(2) << hours << L":"
        << std::setw(2) << minutes << L":" << std::setw(2) << remainder;
    return formatted.str();
}

std::uint32_t CurrentCountdownSeconds(const ResidentState& state) {
    if (!state.settings.countdownEnabled || state.countdownPaused) {
        return state.countdownRemainingSeconds;
    }

    const auto remaining = state.countdownDeadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    return static_cast<std::uint32_t>((milliseconds + 999) / 1000);
}

void SyncCountdownControls(ResidentState& state) {
    if (state.countdownEnabledCheckbox) {
        SendMessageW(
            state.countdownEnabledCheckbox,
            BM_SETCHECK,
            state.settings.countdownEnabled ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
    if (state.countdownHoursEdit) {
        SetWindowTextW(
            state.countdownHoursEdit,
            std::to_wstring(state.settings.countdownHours).c_str());
    }
    if (state.countdownMinutesEdit) {
        SetWindowTextW(
            state.countdownMinutesEdit,
            std::to_wstring(state.settings.countdownMinutes).c_str());
    }
    if (state.countdownSecondsEdit) {
        SetWindowTextW(
            state.countdownSecondsEdit,
            std::to_wstring(state.settings.countdownSeconds).c_str());
    }
    if (state.countdownStartStopButton) {
        SetWindowTextW(
            state.countdownStartStopButton,
            state.settings.countdownEnabled
                ? L"Stop countdown"
                : L"Start countdown");
        InvalidateRect(state.countdownStartStopButton, nullptr, TRUE);
    }
    if (state.countdownPauseResumeButton) {
        SetWindowTextW(
            state.countdownPauseResumeButton,
            state.countdownPaused
                ? L"Resume countdown"
                : L"Pause countdown");
        EnableWindow(
            state.countdownPauseResumeButton,
            state.settings.countdownEnabled);
        InvalidateRect(state.countdownPauseResumeButton, nullptr, TRUE);
    }
    if (state.countdownRestartButton) {
        EnableWindow(state.countdownRestartButton, state.settings.countdownEnabled);
        InvalidateRect(state.countdownRestartButton, nullptr, TRUE);
    }
    state.lastDisplayedCountdown = std::numeric_limits<std::uint32_t>::max();
}

void RefreshCountdownDisplay(ResidentState& state) {
    if (!state.countdownDisplay) {
        return;
    }
    const std::uint32_t remaining = CurrentCountdownSeconds(state);
    if (remaining != state.lastDisplayedCountdown) {
        SetWindowTextW(state.countdownDisplay, FormatCountdown(remaining).c_str());
        state.lastDisplayedCountdown = remaining;
    }
}

void DrawCountdownButton(const DRAWITEMSTRUCT& draw, const ResidentState& state) {
    UINT frameState = DFCS_BUTTONPUSH;
    if ((draw.itemState & ODS_SELECTED) != 0) {
        frameState |= DFCS_PUSHED;
    }
    if ((draw.itemState & ODS_DISABLED) != 0) {
        frameState |= DFCS_INACTIVE;
    }
    DrawFrameControl(draw.hDC, const_cast<RECT*>(&draw.rcItem), DFC_BUTTON, frameState);

    const int centerX = (draw.rcItem.left + draw.rcItem.right) / 2;
    const int centerY = (draw.rcItem.top + draw.rcItem.bottom) / 2;
    const int offset = (draw.itemState & ODS_SELECTED) != 0 ? 1 : 0;
    const int colorIndex = (draw.itemState & ODS_DISABLED) != 0
        ? COLOR_GRAYTEXT
        : COLOR_BTNTEXT;
    HPEN pen = CreatePen(PS_SOLID, 2, GetSysColor(colorIndex));
    HGDIOBJ previousPen = SelectObject(draw.hDC, pen);
    HGDIOBJ previousBrush = SelectObject(
        draw.hDC,
        GetSysColorBrush(colorIndex == COLOR_GRAYTEXT ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    const auto triangle = [&](POINT first, POINT second, POINT third) {
        POINT points[] = {first, second, third};
        Polygon(draw.hDC, points, static_cast<int>(std::size(points)));
    };

    switch (GetDlgCtrlID(draw.hwndItem)) {
    case kCountdownPreviousButtonId:
        MoveToEx(draw.hDC, centerX - 7 + offset, centerY - 8 + offset, nullptr);
        LineTo(draw.hDC, centerX - 7 + offset, centerY + 8 + offset);
        triangle(
            {centerX - 5 + offset, centerY + offset},
            {centerX + 6 + offset, centerY - 8 + offset},
            {centerX + 6 + offset, centerY + 8 + offset});
        break;
    case kCountdownNextButtonId:
        triangle(
            {centerX + 5 + offset, centerY + offset},
            {centerX - 6 + offset, centerY - 8 + offset},
            {centerX - 6 + offset, centerY + 8 + offset});
        MoveToEx(draw.hDC, centerX + 7 + offset, centerY - 8 + offset, nullptr);
        LineTo(draw.hDC, centerX + 7 + offset, centerY + 8 + offset);
        break;
    case kCountdownStartStopButtonId:
        if (state.settings.countdownEnabled) {
            Rectangle(
                draw.hDC,
                centerX - 6 + offset,
                centerY - 6 + offset,
                centerX + 7 + offset,
                centerY + 7 + offset);
        } else {
            triangle(
                {centerX + 7 + offset, centerY + offset},
                {centerX - 6 + offset, centerY - 8 + offset},
                {centerX - 6 + offset, centerY + 8 + offset});
        }
        break;
    case kCountdownPauseResumeButtonId:
        if (state.countdownPaused) {
            triangle(
                {centerX + 7 + offset, centerY + offset},
                {centerX - 6 + offset, centerY - 8 + offset},
                {centerX - 6 + offset, centerY + 8 + offset});
        } else {
            Rectangle(
                draw.hDC,
                centerX - 7 + offset,
                centerY - 7 + offset,
                centerX - 2 + offset,
                centerY + 8 + offset);
            Rectangle(
                draw.hDC,
                centerX + 2 + offset,
                centerY - 7 + offset,
                centerX + 7 + offset,
                centerY + 8 + offset);
        }
        break;
    case kCountdownRestartButtonId:
    {
        POINT rewindArc[] = {
            {centerX - 4 + offset, centerY - 6 + offset},
            {centerX - 1 + offset, centerY - 8 + offset},
            {centerX + 3 + offset, centerY - 7 + offset},
            {centerX + 6 + offset, centerY - 4 + offset},
            {centerX + 8 + offset, centerY + offset},
            {centerX + 6 + offset, centerY + 4 + offset},
            {centerX + 3 + offset, centerY + 6 + offset},
            {centerX + offset, centerY + 7 + offset},
            {centerX - 3 + offset, centerY + 6 + offset},
            {centerX - 6 + offset, centerY + 4 + offset}};
        HPEN rewindPen = CreatePen(PS_SOLID, 3, GetSysColor(colorIndex));
        HGDIOBJ previousRewindPen = SelectObject(draw.hDC, rewindPen);
        Polyline(draw.hDC, rewindArc, static_cast<int>(std::size(rewindArc)));
        triangle(
            {centerX - 9 + offset, centerY - 6 + offset},
            {centerX - 3 + offset, centerY - 9 + offset},
            {centerX - 3 + offset, centerY - 3 + offset});
        SelectObject(draw.hDC, previousRewindPen);
        DeleteObject(rewindPen);
        break;
    }
    default:
        break;
    }

    SelectObject(draw.hDC, previousBrush);
    SelectObject(draw.hDC, previousPen);
    DeleteObject(pen);
    if ((draw.itemState & ODS_FOCUS) != 0) {
        RECT focusRect = draw.rcItem;
        InflateRect(&focusRect, -3, -3);
        DrawFocusRect(draw.hDC, &focusRect);
    }
}

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
            preview.pixelBufferWidth = preview.pixelWidth;
            preview.pixelBufferHeight = preview.pixelHeight;
        } catch (const Failure&) {
            preview.pixels.clear();
            preview.pixelBufferWidth = 0;
            preview.pixelBufferHeight = 0;
            preview.statusText = L"Preview unavailable";
        } catch (const std::exception&) {
            preview.pixels.clear();
            preview.pixelBufferWidth = 0;
            preview.pixelBufferHeight = 0;
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
            preview.pixelWidth > 0 && preview.pixelHeight > 0 &&
            preview.pixelBufferWidth > 0 && preview.pixelBufferHeight > 0) {
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
            preview.pixelBufferWidth > 0 && preview.pixelBufferHeight > 0 &&
            screen.right > screen.left && screen.bottom > screen.top) {
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth =
                static_cast<LONG>(preview.pixelBufferWidth);
            bitmapInfo.bmiHeader.biHeight =
                -static_cast<LONG>(preview.pixelBufferHeight);
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
                preview.pixelBufferWidth,
                preview.pixelBufferHeight,
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

            if (state->draggingSourceIndex >= 0 &&
                static_cast<std::size_t>(state->draggingSourceIndex) <
                    state->settings.sourceDirectories.size()) {
                RECT folder{12, 19, 34, 39};
                HBRUSH folderBrush = CreateSolidBrush(RGB(245, 196, 66));
                HPEN folderPen = CreatePen(PS_SOLID, 1, RGB(149, 104, 20));
                HGDIOBJ previousBrush = SelectObject(dc, folderBrush);
                HGDIOBJ previousPen = SelectObject(dc, folderPen);
                POINT folderShape[] = {
                    {folder.left, folder.top + 4},
                    {folder.left + 7, folder.top + 4},
                    {folder.left + 9, folder.top},
                    {folder.right, folder.top},
                    {folder.right, folder.bottom},
                    {folder.left, folder.bottom}};
                Polygon(dc, folderShape, static_cast<int>(std::size(folderShape)));
                SelectObject(dc, previousPen);
                SelectObject(dc, previousBrush);
                DeleteObject(folderPen);
                DeleteObject(folderBrush);

                const std::filesystem::path sourcePath(
                    state->settings.sourceDirectories[
                        static_cast<std::size_t>(state->draggingSourceIndex)].path);
                const std::wstring name = sourcePath.filename().empty()
                    ? sourcePath.wstring()
                    : sourcePath.filename().wstring();
                RECT label{42, 4, client.right - 8, client.bottom - 4};
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(
                    dc,
                    name.c_str(),
                    -1,
                    &label,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else if (state->draggingPlaylistRow >= 0 &&
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

void PositionDragGhost(ResidentState& state) {
    if (!state.playlistDragGhost || !IsWindow(state.playlistDragGhost)) {
        return;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return;
    }
    const bool draggingSource = state.draggingSourceIndex >= 0;
    const int width = draggingSource
        ? kSourceDragGhostWidth
        : kPlaylistDragGhostWidth;
    const int height = draggingSource
        ? kSourceDragGhostHeight
        : kPlaylistDragGhostHeight;
    SetWindowPos(
        state.playlistDragGhost,
        HWND_TOPMOST,
        cursor.x + 18,
        cursor.y + 18,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void StartDragGhost(ResidentState& state) {
    const bool draggingSource = state.draggingSourceIndex >= 0;
    const int width = draggingSource
        ? kSourceDragGhostWidth
        : kPlaylistDragGhostWidth;
    const int height = draggingSource
        ? kSourceDragGhostHeight
        : kPlaylistDragGhostHeight;
    state.playlistDragGhost = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kPlaylistDragGhostClass,
        L"",
        WS_POPUP,
        0,
        0,
        width,
        height,
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
    PositionDragGhost(state);
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
                !IsReappliedWallpaperVariant(wallpaperPath.get(), expectedOutput) &&
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

void LayoutSettingsWindow(ResidentState& state, int clientWidth, int clientHeight) {
    if (!state.settingsTabs) {
        return;
    }

    const auto moveControl = [](HWND control, int x, int y, int width, int height) {
        if (control) {
            SetWindowPos(
                control,
                nullptr,
                x,
                y,
                std::max(1, width),
                std::max(1, height),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    const int contentWidth = std::max(1, clientWidth - 24);
    const int footerY = std::max(0, clientHeight - 44);
    const int tabsY = 145;
    moveControl(state.countdownDisplay, 12, 40, 220, 42);
    const int countdownButtonWidth = 30;
    const int countdownButtonGap = 4;
    const int countdownButtonsWidth =
        5 * countdownButtonWidth + 4 * countdownButtonGap;
    const int countdownButtonsX = 12 + (220 - countdownButtonsWidth) / 2;
    moveControl(state.countdownPreviousButton, countdownButtonsX, 86,
        countdownButtonWidth, 30);
    moveControl(state.countdownStartStopButton,
        countdownButtonsX + countdownButtonWidth + countdownButtonGap,
        86, countdownButtonWidth, 30);
    moveControl(state.countdownPauseResumeButton,
        countdownButtonsX + 2 * (countdownButtonWidth + countdownButtonGap),
        86, countdownButtonWidth, 30);
    moveControl(state.countdownRestartButton,
        countdownButtonsX + 3 * (countdownButtonWidth + countdownButtonGap),
        86, countdownButtonWidth, 30);
    moveControl(state.countdownNextButton,
        countdownButtonsX + 4 * (countdownButtonWidth + countdownButtonGap),
        86, countdownButtonWidth, 30);
    moveControl(state.monitorPreviewPanel, 242, 12, contentWidth - 230, 125);
    moveControl(
        state.settingsTabs,
        12,
        tabsY,
        contentWidth,
        std::max(1, footerY - 12 - tabsY));
    moveControl(
        state.sourceDirectoryList,
        28,
        186,
        clientWidth - 56,
        clientHeight - 171 - 14 - 186);
    moveControl(
        GetDlgItem(state.settingsWindow, kAddSourceDirectoryButtonId),
        28,
        clientHeight - 171,
        130,
        32);
    moveControl(
        GetDlgItem(state.settingsWindow, kRemoveSourceDirectoryButtonId),
        172,
        clientHeight - 171,
        110,
        32);
    moveControl(
        state.sourceDirectoryHint,
        28,
        clientHeight - 129,
        clientWidth - 56,
        36);
    moveControl(
        state.playlistList,
        28,
        186,
        clientWidth - 56,
        clientHeight - 295);
    moveControl(
        state.playlistHint,
        28,
        clientHeight - 99,
        clientWidth - 56,
        28);
    moveControl(state.backgroundFillModeLabel, 30, 190, clientWidth - 60, 28);
    moveControl(state.backgroundFillColorModeRadio, 30, 222, 500, 28);
    moveControl(state.backgroundFillColorLabel, 58, 260, 122, 28);
    moveControl(state.backgroundFillColorButton, 180, 253, 180, 42);
    moveControl(state.backgroundAdaptiveModeRadio, 30, 306, 600, 28);
    moveControl(state.multiMonitorSettingsGroup, 20, 180, clientWidth - 40, 82);
    moveControl(state.sameImageOnDisplaysRadio, 40, 204, 340, 28);
    moveControl(state.separateImagesOnDisplaysRadio, 390, 204, clientWidth - 420, 28);
    moveControl(state.imagePlacementGroup, 20, 268, clientWidth - 40, 306);
    moveControl(state.imageResizeToRadio, 40, 288, clientWidth - 80, 28);
    moveControl(state.imageResizeToPercentLabel, 68, 321, 168, 26);
    moveControl(state.imageResizeToPercentEdit, 240, 316, 64, 27);
    moveControl(state.imageResizeToPercentUnitLabel, 310, 321, 24, 26);
    moveControl(state.imageAutoFitRadio, 40, 349, clientWidth - 80, 28);
    const int imageGroupWidth = std::max(300, (clientWidth - 80) / 2);
    const int imageLargeGroupX = clientWidth - 30 - imageGroupWidth;
    moveControl(state.imageSmallGroup, 30, 380, imageGroupWidth, 176);
    moveControl(state.imageSmallCenteredRadio, 50, 403, imageGroupWidth - 40, 28);
    moveControl(state.imageSmallTileRadio, 50, 433, imageGroupWidth - 40, 28);
    moveControl(state.imageSmallFitRadio, 50, 463, imageGroupWidth - 40, 28);
    moveControl(state.imageMaxEnlargeLabel, 72, 497, 170, 26);
    moveControl(state.imageMaxEnlargePercentEdit, 247, 492, 64, 27);
    moveControl(state.imageMaxEnlargePercentLabel, 315, 497, 32, 26);
    moveControl(
        state.imageLargeGroup, imageLargeGroupX, 380, imageGroupWidth, 176);
    moveControl(
        state.imageLargeCenteredRadio,
        imageLargeGroupX + 20, 403, imageGroupWidth - 40, 28);
    moveControl(
        state.imageLargeFillRadio,
        imageLargeGroupX + 20, 433, imageGroupWidth - 40, 28);
    moveControl(
        state.imageLargeFitRadio,
        imageLargeGroupX + 20, 463, imageGroupWidth - 40, 28);
    moveControl(state.countdownEnabledCheckbox, 30, 190, clientWidth - 60, 28);
    moveControl(state.countdownDurationGroup, 20, 232, clientWidth - 40, 104);
    moveControl(state.countdownHoursLabel, 50, 260, 120, 24);
    moveControl(state.countdownHoursEdit, 50, 286, 120, 30);
    moveControl(state.countdownMinutesLabel, 250, 260, 120, 24);
    moveControl(state.countdownMinutesEdit, 250, 286, 120, 30);
    moveControl(state.countdownSecondsLabel, 450, 260, 120, 24);
    moveControl(state.countdownSecondsEdit, 450, 286, 120, 30);
    moveControl(
        GetDlgItem(state.settingsWindow, kExitSettingsButtonId),
        clientWidth - 28 - 82 - 10 - 82,
        footerY,
        82,
        32);
    moveControl(
        GetDlgItem(state.settingsWindow, kCloseSettingsButtonId),
        clientWidth - 28 - 82,
        footerY,
        82,
        32);

    if (state.sourceDirectoryList) {
        ListView_SetColumnWidth(
            state.sourceDirectoryList, 0, std::max(100, clientWidth - 80));
    }
    if (state.playlistList) {
        ListView_SetColumnWidth(
            state.playlistList,
            0,
            std::max(270, clientWidth - 56 - 670 - 24));
    }
}

void RecordNormalSettingsWindowRect(ResidentState& state, HWND window) {
    if (!IsIconic(window) && !IsZoomed(window)) {
        RECT bounds{};
        if (GetWindowRect(window, &bounds)) {
            state.settingsWindowNormalRect = bounds;
            state.hasSettingsWindowNormalRect = true;
        }
    }
}

void SaveSettingsWindowBounds(ResidentState& state, HWND window) {
    RecordNormalSettingsWindowRect(state, window);
    if (!state.hasSettingsWindowNormalRect) {
        return;
    }

    const RECT& rect = state.settingsWindowNormalRect;
    const swc::WindowBounds bounds{
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top};
    UpdateSettingsWindowBounds(state.settingsPath, bounds);
    state.settings.windowBounds = bounds;
    state.settings.hasWindowBounds = true;
}

void SaveSettingsWindowBoundsWithError(ResidentState& state, HWND window) {
    try {
        SaveSettingsWindowBounds(state, window);
    } catch (const Failure& failure) {
        ShowError(failure.message, window);
    } catch (const std::exception&) {
        ShowError(L"Could not save the Settings window position and size.", window);
    }
}

bool IsSettingsWindowBoundsUsable(const swc::WindowBounds& bounds) {
    if (bounds.width < kSettingsMinimumClientWidth ||
        bounds.height < kSettingsMinimumClientHeight) {
        return false;
    }

    const long long right = static_cast<long long>(bounds.x) + bounds.width;
    const long long bottom = static_cast<long long>(bounds.y) + bounds.height;
    if (right > std::numeric_limits<LONG>::max() ||
        bottom > std::numeric_limits<LONG>::max() ||
        right < std::numeric_limits<LONG>::min() ||
        bottom < std::numeric_limits<LONG>::min()) {
        return false;
    }
    const RECT rect{
        bounds.x,
        bounds.y,
        static_cast<LONG>(right),
        static_cast<LONG>(bottom)};
    const HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        return false;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    const LONG visibleWidth = std::max<LONG>(
        0,
        std::min(rect.right, info.rcWork.right) -
            std::max(rect.left, info.rcWork.left));
    const LONG visibleHeight = std::max<LONG>(
        0,
        std::min(rect.bottom, info.rcWork.bottom) -
            std::max(rect.top, info.rcWork.top));
    return visibleWidth >= 100 && visibleHeight >= 100;
}

void SyncImageSizingControls(ResidentState& state) {
    const bool resizeTo =
        state.settings.imageSizingMode == swc::ImageSizingMode::resizeTo;
    const bool autoFit = !resizeTo;
    const bool smallFit =
        state.settings.smallImageMode == swc::SmallImageMode::fit;
    SendMessageW(
        state.imageResizeToRadio,
        BM_SETCHECK,
        resizeTo ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.imageAutoFitRadio,
        BM_SETCHECK,
        autoFit ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.imageSmallCenteredRadio,
        BM_SETCHECK,
        state.settings.smallImageMode == swc::SmallImageMode::centered
            ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.imageSmallTileRadio,
        BM_SETCHECK,
        state.settings.smallImageMode == swc::SmallImageMode::tile
            ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.imageSmallFitRadio,
        BM_SETCHECK,
        smallFit ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.imageLargeCenteredRadio,
        BM_SETCHECK,
        state.settings.largeImageMode == swc::LargeImageMode::centered
            ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.imageLargeFillRadio,
        BM_SETCHECK,
        state.settings.largeImageMode == swc::LargeImageMode::fill
            ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.imageLargeFitRadio,
        BM_SETCHECK,
        state.settings.largeImageMode == swc::LargeImageMode::fit
            ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.sameImageOnDisplaysRadio,
        BM_SETCHECK,
        state.settings.multiMonitorImageMode ==
                swc::MultiMonitorImageMode::sameImage
            ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        state.separateImagesOnDisplaysRadio,
        BM_SETCHECK,
        state.settings.multiMonitorImageMode ==
                swc::MultiMonitorImageMode::separateImages
            ? BST_CHECKED : BST_UNCHECKED,
        0);

    EnableWindow(state.imageResizeToPercentEdit, resizeTo);
    EnableWindow(state.imageResizeToPercentLabel, resizeTo);
    EnableWindow(state.imageResizeToPercentUnitLabel, resizeTo);
    EnableWindow(state.imageSmallGroup, autoFit);
    EnableWindow(state.imageSmallCenteredRadio, autoFit);
    EnableWindow(state.imageSmallTileRadio, autoFit);
    EnableWindow(state.imageSmallFitRadio, autoFit);
    EnableWindow(state.imageMaxEnlargeLabel, autoFit && smallFit);
    EnableWindow(state.imageMaxEnlargePercentEdit, autoFit && smallFit);
    EnableWindow(state.imageMaxEnlargePercentLabel, autoFit && smallFit);
    EnableWindow(state.imageLargeGroup, autoFit);
    EnableWindow(state.imageLargeCenteredRadio, autoFit);
    EnableWindow(state.imageLargeFillRadio, autoFit);
    EnableWindow(state.imageLargeFitRadio, autoFit);
    if (state.imageResizeToPercentEdit) {
        SetWindowTextW(
            state.imageResizeToPercentEdit,
            std::to_wstring(state.settings.resizeToPercent).c_str());
    }
    if (state.imageMaxEnlargePercentEdit) {
        SetWindowTextW(
            state.imageMaxEnlargePercentEdit,
            std::to_wstring(state.settings.maxSmallImageEnlargePercent).c_str());
    }
}

void ShowSettingsPage(ResidentState& state) {
    const int page = TabCtrl_GetCurSel(state.settingsTabs);
    const bool startupPage = page == 0;
    const bool sourcesPage = page == 1;
    const bool backgroundPage = page == 3;
    const bool countdownPage = page == 5;
    ShowWindow(state.changeOnStartCheckbox, startupPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.openWindowOnStartCheckbox, startupPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.startWithWindowsCheckbox, startupPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.sourceDirectoryList, sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.sourceDirectoryHint, sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state.settingsWindow, kAddSourceDirectoryButtonId),
        sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state.settingsWindow, kRemoveSourceDirectoryButtonId),
        sourcesPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.playlistList, page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(state.playlistHint, page == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(state.backgroundFillModeLabel, backgroundPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.backgroundFillColorModeRadio, backgroundPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.backgroundAdaptiveModeRadio, backgroundPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.backgroundFillColorLabel, backgroundPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.backgroundFillColorButton, backgroundPage ? SW_SHOW : SW_HIDE);
    if (state.backgroundFillColorButton) {
        const BOOL fillColorMode =
            state.settings.backgroundFillMode == swc::BackgroundFillMode::fillColor;
        EnableWindow(state.backgroundFillColorLabel, fillColorMode);
        EnableWindow(state.backgroundFillColorButton, fillColorMode);
    }
    const bool imagePage = page == 4;
    ShowWindow(state.multiMonitorSettingsGroup, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imagePlacementGroup, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.sameImageOnDisplaysRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.separateImagesOnDisplaysRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageResizeToRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageAutoFitRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageResizeToPercentEdit, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageResizeToPercentLabel, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageResizeToPercentUnitLabel, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageSmallGroup, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageSmallCenteredRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageSmallTileRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageSmallFitRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageMaxEnlargeLabel, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageMaxEnlargePercentEdit, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageMaxEnlargePercentLabel, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageLargeGroup, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageLargeCenteredRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageLargeFillRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.imageLargeFitRadio, imagePage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownEnabledCheckbox, countdownPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownDurationGroup, countdownPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownHoursLabel, countdownPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownMinutesLabel, countdownPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownSecondsLabel, countdownPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownHoursEdit, countdownPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownMinutesEdit, countdownPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.countdownSecondsEdit, countdownPage ? SW_SHOW : SW_HIDE);
    SyncImageSizingControls(state);
    SyncCountdownControls(state);
    RefreshCountdownDisplay(state);
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

int SourceDirectoryInsertionIndex(
    const ResidentState& state,
    POINT point) {
    RECT client{};
    if (!GetClientRect(state.sourceDirectoryList, &client) ||
        point.x < client.left || point.x >= client.right ||
        point.y < client.top || point.y >= client.bottom) {
        return -1;
    }

    const int count = ListView_GetItemCount(state.sourceDirectoryList);
    if (count == 0) {
        return 0;
    }

    for (int row = 0; row < count; ++row) {
        RECT rowRect{};
        if (!ListView_GetItemRect(
                state.sourceDirectoryList, row, &rowRect, LVIR_BOUNDS)) {
            return -1;
        }
        if (point.y < rowRect.top) {
            return row;
        }
        if (point.y < rowRect.bottom) {
            return row + (point.y >= (rowRect.top + rowRect.bottom) / 2 ? 1 : 0);
        }
    }
    return count;
}

void DrawSourceDirectoryInsertionMark(
    HDC dc,
    HWND list,
    int insertionIndex) {
    const int count = ListView_GetItemCount(list);
    if (insertionIndex < 0 || insertionIndex > count) {
        return;
    }

    int y = 0;
    if (insertionIndex < count) {
        RECT rowRect{};
        if (!ListView_GetItemRect(list, insertionIndex, &rowRect, LVIR_BOUNDS)) {
            return;
        }
        y = rowRect.top;
    } else if (count > 0) {
        RECT rowRect{};
        if (!ListView_GetItemRect(list, count - 1, &rowRect, LVIR_BOUNDS)) {
            return;
        }
        y = rowRect.bottom;
    } else {
        return;
    }

    RECT client{};
    if (!GetClientRect(list, &client)) {
        return;
    }
    const int markerY = std::clamp(
        y,
        static_cast<int>(client.top),
        static_cast<int>(client.bottom - 1));
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
    HGDIOBJ previousPen = SelectObject(dc, pen);
    MoveToEx(dc, client.left + 2, markerY, nullptr);
    LineTo(dc, client.right - 2, markerY);
    SelectObject(dc, previousPen);
    DeleteObject(pen);

    POINT arrow[] = {
        {client.left + 1, markerY - 4},
        {client.left + 6, markerY},
        {client.left + 1, markerY + 4}};
    HBRUSH brush = CreateSolidBrush(RGB(0, 120, 215));
    HGDIOBJ previousBrush = SelectObject(dc, brush);
    previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, arrow, static_cast<int>(std::size(arrow)));
    SelectObject(dc, previousPen);
    SelectObject(dc, previousBrush);
    DeleteObject(brush);
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

void ResetCountdown(ResidentState& state) {
    state.countdownRemainingSeconds = swc::CountdownDurationSeconds(state.settings);
    state.countdownPaused = false;
    if (state.settings.countdownEnabled) {
        state.countdownDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(state.countdownRemainingSeconds);
    }
    SyncCountdownControls(state);
    RefreshCountdownDisplay(state);
    if (state.countdownPauseResumeButton) {
        InvalidateRect(state.countdownPauseResumeButton, nullptr, TRUE);
    }
}

void StartOrStopCountdown(
    ResidentState& state,
    bool enabled,
    HWND owner) {
    auto updated = state.settings;
    updated.countdownEnabled = enabled;
    try {
        UpdateUserSettings(state.settingsPath, updated);
        state.settings = std::move(updated);
        if (!enabled) {
            state.timerNextPending = false;
        }
        ResetCountdown(state);
    } catch (const Failure& failure) {
        SyncCountdownControls(state);
        ShowError(failure.message, owner);
    } catch (const std::exception&) {
        SyncCountdownControls(state);
        ShowError(L"Could not save the Countdown settings.", owner);
    }
}

void ToggleCountdownPause(ResidentState& state) {
    if (!state.settings.countdownEnabled) {
        return;
    }
    if (!state.countdownPaused) {
        const std::uint32_t remaining = CurrentCountdownSeconds(state);
        if (remaining == 0) {
            ResetCountdown(state);
            if (state.changeRunning) {
                state.timerNextPending = true;
            } else {
                StartWallpaperChange(state);
            }
            return;
        }
        state.countdownRemainingSeconds = remaining;
        state.countdownPaused = true;
    } else {
        state.countdownPaused = false;
        state.countdownDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(state.countdownRemainingSeconds);
    }
    SyncCountdownControls(state);
    RefreshCountdownDisplay(state);
    if (state.countdownPauseResumeButton) {
        InvalidateRect(state.countdownPauseResumeButton, nullptr, TRUE);
    }
}

std::wstring ReadEditText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
    const int copied = GetWindowTextW(
        edit, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(std::max(0, copied)));
    return text;
}

bool IsCountdownDurationEditId(int controlId) {
    return controlId == kCountdownHoursEditId ||
        controlId == kCountdownMinutesEditId ||
        controlId == kCountdownSecondsEditId;
}

void SaveCountdownDuration(
    ResidentState& state,
    HWND owner,
    bool force = false) {
    const HWND focused = GetFocus();
    if (!force && focused &&
        IsCountdownDurationEditId(GetDlgCtrlID(focused))) {
        return;
    }

    const std::wstring hours = ReadEditText(state.countdownHoursEdit);
    const std::wstring minutes = ReadEditText(state.countdownMinutesEdit);
    const std::wstring seconds = ReadEditText(state.countdownSecondsEdit);
    std::uint32_t totalSeconds = 0;
    if (!swc::ParseCountdownDuration(hours, minutes, seconds, totalSeconds)) {
        ShowError(
            L"Countdown time must be greater than zero. Hours must be 0-99; "
            L"minutes and seconds must be 0-59.",
            owner);
        SyncCountdownControls(state);
        RefreshCountdownDisplay(state);
        return;
    }

    auto updated = state.settings;
    swc::ParsePercent(hours, 0, 99, updated.countdownHours);
    swc::ParsePercent(minutes, 0, 59, updated.countdownMinutes);
    swc::ParsePercent(seconds, 0, 59, updated.countdownSeconds);
    try {
        UpdateUserSettings(state.settingsPath, updated);
        state.settings = std::move(updated);
        state.countdownRemainingSeconds = totalSeconds;
        if (state.settings.countdownEnabled && !state.countdownPaused) {
            state.countdownDeadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(totalSeconds);
        }
        SyncCountdownControls(state);
        RefreshCountdownDisplay(state);
    } catch (const Failure& failure) {
        SyncCountdownControls(state);
        RefreshCountdownDisplay(state);
        ShowError(failure.message, owner);
    } catch (const std::exception&) {
        SyncCountdownControls(state);
        RefreshCountdownDisplay(state);
        ShowError(L"Could not save the Countdown duration.", owner);
    }
}

void RequestNextOrPrevious(
    ResidentState& state,
    bool previous,
    bool fromCountdown = false) {
    if (state.settings.countdownEnabled) {
        ResetCountdown(state);
    }
    if (state.changeRunning) {
        if (fromCountdown) {
            state.timerNextPending = true;
        } else {
            ShowError(L"A wallpaper change is already in progress.", state.window);
        }
        return;
    }
    StartWallpaperChange(
        state, previous ? WallpaperRequest{true, {}, {}} : WallpaperRequest{});
}

void ProcessCountdownTick(ResidentState& state) {
    if (!state.settings.countdownEnabled || state.countdownPaused) {
        RefreshCountdownDisplay(state);
        return;
    }
    if (CurrentCountdownSeconds(state) > 0) {
        RefreshCountdownDisplay(state);
        return;
    }

    ResetCountdown(state);
    if (state.changeRunning) {
        state.timerNextPending = true;
        return;
    }
    StartWallpaperChange(state);
}

void RequestBackgroundReapply(
    ResidentState& state,
    bool reapplyMultiMonitorMode = false) {
    if (state.changeRunning) {
        state.backgroundReapplyPending = true;
        state.multiMonitorModeReapplyPending =
            state.multiMonitorModeReapplyPending || reapplyMultiMonitorMode;
        return;
    }
    StartWallpaperChange(
        state,
        WallpaperRequest{false, {}, {}, true, reapplyMultiMonitorMode});
}

void SaveBackgroundSettings(
    ResidentState& state,
    swc::Settings updated,
    HWND owner) {
    try {
        UpdateUserSettings(state.settingsPath, updated);
        state.settings = std::move(updated);
        const bool fillColorMode =
            state.settings.backgroundFillMode == swc::BackgroundFillMode::fillColor;
        SendMessageW(
            state.backgroundFillColorModeRadio,
            BM_SETCHECK,
            fillColorMode ? BST_CHECKED : BST_UNCHECKED,
            0);
        SendMessageW(
            state.backgroundAdaptiveModeRadio,
            BM_SETCHECK,
            fillColorMode ? BST_UNCHECKED : BST_CHECKED,
            0);
        EnableWindow(state.backgroundFillColorLabel, fillColorMode);
        EnableWindow(state.backgroundFillColorButton, fillColorMode);
        InvalidateRect(state.backgroundFillColorButton, nullptr, TRUE);
        RequestBackgroundReapply(state);
    } catch (const Failure& failure) {
        const bool fillColorMode =
            state.settings.backgroundFillMode == swc::BackgroundFillMode::fillColor;
        SendMessageW(
            state.backgroundFillColorModeRadio,
            BM_SETCHECK,
            fillColorMode ? BST_CHECKED : BST_UNCHECKED,
            0);
        SendMessageW(
            state.backgroundAdaptiveModeRadio,
            BM_SETCHECK,
            fillColorMode ? BST_UNCHECKED : BST_CHECKED,
            0);
        EnableWindow(state.backgroundFillColorLabel, fillColorMode);
        EnableWindow(state.backgroundFillColorButton, fillColorMode);
        InvalidateRect(state.backgroundFillColorButton, nullptr, TRUE);
        ShowError(failure.message, owner);
    } catch (const std::exception&) {
        const bool fillColorMode =
            state.settings.backgroundFillMode == swc::BackgroundFillMode::fillColor;
        SendMessageW(
            state.backgroundFillColorModeRadio,
            BM_SETCHECK,
            fillColorMode ? BST_CHECKED : BST_UNCHECKED,
            0);
        SendMessageW(
            state.backgroundAdaptiveModeRadio,
            BM_SETCHECK,
            fillColorMode ? BST_UNCHECKED : BST_CHECKED,
            0);
        EnableWindow(state.backgroundFillColorLabel, fillColorMode);
        EnableWindow(state.backgroundFillColorButton, fillColorMode);
        InvalidateRect(state.backgroundFillColorButton, nullptr, TRUE);
        ShowError(L"Could not save the Background settings.", owner);
    }
}

void SaveImageSettings(
    ResidentState& state,
    swc::Settings updated,
    HWND owner) {
    try {
        const bool multiMonitorModeChanged =
            updated.multiMonitorImageMode != state.settings.multiMonitorImageMode;
        UpdateUserSettings(state.settingsPath, updated);
        state.settings = std::move(updated);
        SyncImageSizingControls(state);
        RequestBackgroundReapply(state, multiMonitorModeChanged);
    } catch (const Failure& failure) {
        SyncImageSizingControls(state);
        ShowError(failure.message, owner);
    } catch (const std::exception&) {
        SyncImageSizingControls(state);
        ShowError(L"Could not save the Image settings.", owner);
    }
}

void SaveImagePercentSetting(
    ResidentState& state,
    bool maximumEnlarge,
    HWND owner) {
    const HWND edit = maximumEnlarge
        ? state.imageMaxEnlargePercentEdit
        : state.imageResizeToPercentEdit;
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
    const int copied = GetWindowTextW(
        edit,
        text.data(),
        static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(std::max(0, copied)));
    std::uint32_t percent = 0;
    const std::uint32_t minimum = maximumEnlarge ? 100 : 1;
    if (!swc::ParsePercent(text, minimum, 1000, percent)) {
        ShowError(
            maximumEnlarge
                ? L"Maximum enlargement must be between 100% and 1000%."
                : L"Resize-to target must be between 1% and 1000%.",
            owner);
        SyncImageSizingControls(state);
        return;
    }

    auto updated = state.settings;
    if (maximumEnlarge) {
        updated.maxSmallImageEnlargePercent = percent;
    } else {
        updated.resizeToPercent = percent;
    }
    SaveImageSettings(state, std::move(updated), owner);
}

void ChooseBackgroundFillColor(ResidentState& state, HWND owner) {
    const COLORREF initialColor = RGB(
        state.settings.backgroundFillColor.red,
        state.settings.backgroundFillColor.green,
        state.settings.backgroundFillColor.blue);
    CHOOSECOLORW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.rgbResult = initialColor;
    dialog.lpCustColors = gCustomBackgroundColors;
    dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&dialog)) {
        const DWORD error = CommDlgExtendedError();
        if (error != 0) {
            ShowError(
                L"Could not open the color picker: " + Win32Message(error),
                owner);
        }
        return;
    }

    auto updated = state.settings;
    updated.backgroundFillColor = {
        GetRValue(dialog.rgbResult),
        GetGValue(dialog.rgbResult),
        GetBValue(dialog.rgbResult)};
    SaveBackgroundSettings(state, std::move(updated), owner);
}

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
        LOGFONTW countdownFontDescription{};
        countdownFontDescription.lfHeight = -34;
        countdownFontDescription.lfWeight = FW_BOLD;
        lstrcpynW(
            countdownFontDescription.lfFaceName,
            L"Segoe UI",
            LF_FACESIZE);
        state->countdownFont = CreateFontIndirectW(&countdownFontDescription);
        if (!state->countdownFont) {
            FailWin32(L"Could not create the countdown display font");
        }
        state->countdownDisplay = CreateSettingsControl(
            0, L"STATIC", L"00:10:00",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            12, 40, 220, 42, window, kCountdownDisplayId,
            state->instance, state->settingsFont);
        SendMessageW(
            state->countdownDisplay,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(state->countdownFont),
            TRUE);
        state->countdownPreviousButton = CreateSettingsControl(
            0, L"BUTTON", L"Previous image",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            39, 86, 30, 30, window, kCountdownPreviousButtonId,
            state->instance, state->settingsFont);
        state->countdownStartStopButton = CreateSettingsControl(
            0, L"BUTTON", L"Start countdown",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            73, 86, 30, 30, window, kCountdownStartStopButtonId,
            state->instance, state->settingsFont);
        state->countdownPauseResumeButton = CreateSettingsControl(
            0, L"BUTTON", L"Pause countdown",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            107, 86, 30, 30, window, kCountdownPauseResumeButtonId,
            state->instance, state->settingsFont);
        state->countdownRestartButton = CreateSettingsControl(
            0, L"BUTTON", L"Restart countdown",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            141, 86, 30, 30, window, kCountdownRestartButtonId,
            state->instance, state->settingsFont);
        state->countdownNextButton = CreateSettingsControl(
            0, L"BUTTON", L"Next image",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            175, 86, 30, 30, window, kCountdownNextButtonId,
            state->instance, state->settingsFont);
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
            wchar_t backgroundTabText[] = L"Background";
            wchar_t imageTabText[] = L"Image";
            wchar_t countdownTabText[] = L"Countdown";
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
            tab.pszText = backgroundTabText;
            if (TabCtrl_InsertItem(state->settingsTabs, 3, &tab) == -1) {
                Fail(L"Could not add the Background settings tab.");
            }
            tab.pszText = imageTabText;
            if (TabCtrl_InsertItem(state->settingsTabs, 4, &tab) == -1) {
                Fail(L"Could not add the Image settings tab.");
            }
            tab.pszText = countdownTabText;
            if (TabCtrl_InsertItem(state->settingsTabs, 5, &tab) == -1) {
                Fail(L"Could not add the Countdown settings tab.");
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
            state->openWindowOnStartCheckbox = CreateSettingsControl(
                0, L"BUTTON", L"Open window on startup",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                30, 219, 900, 28, window, kOpenWindowOnStartCheckboxId, state->instance,
                state->settingsFont);
            SendMessageW(
                state->openWindowOnStartCheckbox,
                BM_SETCHECK,
                state->settings.openWindowOnStart ? BST_CHECKED : BST_UNCHECKED,
                0);
            state->startWithWindowsCheckbox = CreateSettingsControl(
                0, L"BUTTON", L"Start with Windows",
                WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
                30, 251, 900, 28, window, kStartWithWindowsCheckboxId,
                state->instance, state->settingsFont);
            SendMessageW(
                state->startWithWindowsCheckbox,
                BM_SETCHECK,
                state->settings.startWithWindows ? BST_CHECKED : BST_UNCHECKED,
                0);

            state->backgroundFillModeLabel = CreateSettingsControl(
                0, L"STATIC", L"How to fill areas not covered by the image:",
                WS_CHILD | SS_LEFT,
                30, 190, kSettingsContentWidth - 60, 28, window,
                kBackgroundFillModeLabelId, state->instance, state->settingsFont);
            state->backgroundFillColorModeRadio = CreateSettingsControl(
                0, L"BUTTON", L"Fill color",
                WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                30, 222, 500, 28, window, kBackgroundFillColorModeRadioId,
                state->instance, state->settingsFont);
            const bool fillColorMode =
                state->settings.backgroundFillMode == swc::BackgroundFillMode::fillColor;
            state->backgroundFillColorLabel = CreateSettingsControl(
                0, L"STATIC", L"Background color:",
                WS_CHILD | SS_LEFT,
                58, 260, 122, 28, window, kBackgroundFillColorLabelId,
                state->instance, state->settingsFont);
            state->backgroundFillColorButton = CreateSettingsControl(
                0, L"BUTTON", L"",
                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                180, 253, 180, 42, window, kBackgroundFillColorButtonId,
                state->instance, state->settingsFont);
            state->backgroundAdaptiveModeRadio = CreateSettingsControl(
                0, L"BUTTON", L"Adaptive to the displayed image",
                WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
                30, 306, 600, 28, window, kBackgroundAdaptiveModeRadioId,
                state->instance, state->settingsFont);
            SendMessageW(
                state->backgroundFillColorModeRadio,
                BM_SETCHECK,
                fillColorMode ? BST_CHECKED : BST_UNCHECKED,
                0);
            SendMessageW(
                state->backgroundAdaptiveModeRadio,
                BM_SETCHECK,
                fillColorMode ? BST_UNCHECKED : BST_CHECKED,
                0);
            EnableWindow(state->backgroundFillColorLabel, fillColorMode);
            EnableWindow(state->backgroundFillColorButton, fillColorMode);

            state->multiMonitorSettingsGroup = CreateSettingsControl(
                0, L"BUTTON", L"Multi-monitor settings",
                WS_CHILD | BS_GROUPBOX,
                20, 180, kSettingsContentWidth - 40, 82, window,
                kMultiMonitorSettingsGroupId, state->instance, state->settingsFont);
            state->imagePlacementGroup = CreateSettingsControl(
                0, L"BUTTON", L"Image placement",
                WS_CHILD | BS_GROUPBOX,
                20, 268, kSettingsContentWidth - 40, 306, window,
                kImagePlacementGroupId, state->instance, state->settingsFont);
            state->sameImageOnDisplaysRadio = CreateSettingsControl(
                0, L"BUTTON", L"Use the same image on all displays",
                WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                40, 204, 340, 28, window, kSameImageOnDisplaysRadioId,
                state->instance, state->settingsFont);
            state->separateImagesOnDisplaysRadio = CreateSettingsControl(
                0, L"BUTTON", L"Use separate images on all displays",
                WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
                390, 204, 548, 28, window, kSeparateImagesOnDisplaysRadioId,
                state->instance, state->settingsFont);
            state->imageResizeToRadio = CreateSettingsControl(
                0, L"BUTTON", L"Resize to...",
                WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                40, 288, 450, 28, window, kImageResizeToRadioId,
                state->instance, state->settingsFont);
            state->imageResizeToPercentLabel = CreateSettingsControl(
                0, L"STATIC", L"Target on closer dimension:",
                WS_CHILD | SS_LEFT,
                68, 321, 168, 26, window, kImageResizeToPercentLabelId,
                state->instance, state->settingsFont);
            state->imageResizeToPercentEdit = CreateSettingsControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                240, 316, 64, 27, window, kImageResizeToPercentEditId,
                state->instance, state->settingsFont);
            SendMessageW(state->imageResizeToPercentEdit, EM_LIMITTEXT, 4, 0);
            SetWindowTextW(
                state->imageResizeToPercentEdit,
                std::to_wstring(state->settings.resizeToPercent).c_str());
            state->imageResizeToPercentUnitLabel = CreateSettingsControl(
                0, L"STATIC", L"%",
                WS_CHILD | SS_LEFT,
                310, 321, 24, 26, window, kImageResizeToPercentUnitLabelId,
                state->instance, state->settingsFont);
            state->imageAutoFitRadio = CreateSettingsControl(
                0, L"BUTTON", L"Autofit...",
                WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
                40, 349, 450, 28, window, kImageAutoFitRadioId,
                state->instance, state->settingsFont);
            state->imageSmallGroup = CreateSettingsControl(
                0, L"BUTTON", L"If the image is too small",
                WS_CHILD | BS_GROUPBOX,
                30, 380, 444, 176, window, kImageSmallGroupId,
                state->instance, state->settingsFont);
            state->imageLargeGroup = CreateSettingsControl(
                0, L"BUTTON", L"If the image is too large",
                WS_CHILD | BS_GROUPBOX,
                490, 380, 444, 176, window, kImageLargeGroupId,
                state->instance, state->settingsFont);
            state->imageSmallCenteredRadio = CreateSettingsControl(
                0, L"BUTTON", L"Centered - display without enlarging",
                WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                50, 403, 390, 28, window, kImageSmallCenteredRadioId,
                state->instance, state->settingsFont);
            state->imageSmallTileRadio = CreateSettingsControl(
                0, L"BUTTON", L"Tile",
                WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
                50, 433, 390, 28, window, kImageSmallTileRadioId,
                state->instance, state->settingsFont);
            state->imageSmallFitRadio = CreateSettingsControl(
                0, L"BUTTON", L"Fit - enlarge to touch the frame",
                WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
                50, 463, 390, 28, window, kImageSmallFitRadioId,
                state->instance, state->settingsFont);
            state->imageMaxEnlargeLabel = CreateSettingsControl(
                0, L"STATIC", L"Maximum enlargement:",
                WS_CHILD | SS_LEFT,
                72, 497, 170, 26, window, kImageMaxEnlargeLabelId,
                state->instance, state->settingsFont);
            state->imageMaxEnlargePercentEdit = CreateSettingsControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                247, 492, 64, 27, window, kImageMaxEnlargePercentEditId,
                state->instance, state->settingsFont);
            SendMessageW(state->imageMaxEnlargePercentEdit, EM_LIMITTEXT, 4, 0);
            SetWindowTextW(
                state->imageMaxEnlargePercentEdit,
                std::to_wstring(
                    state->settings.maxSmallImageEnlargePercent).c_str());
            state->imageMaxEnlargePercentLabel = CreateSettingsControl(
                0, L"STATIC", L"%",
                WS_CHILD | SS_LEFT,
                315, 497, 32, 26, window, kImageMaxEnlargePercentLabelId,
                state->instance, state->settingsFont);
            state->imageLargeCenteredRadio = CreateSettingsControl(
                0, L"BUTTON", L"Centered - crop equally from each side",
                WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                510, 403, 390, 28, window, kImageLargeCenteredRadioId,
                state->instance, state->settingsFont);
            state->imageLargeFillRadio = CreateSettingsControl(
                0, L"BUTTON", L"Fill - crop the longer dimension",
                WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
                510, 433, 390, 28, window, kImageLargeFillRadioId,
                state->instance, state->settingsFont);
            state->imageLargeFitRadio = CreateSettingsControl(
                0, L"BUTTON", L"Fit - show the entire image",
                WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
                510, 463, 390, 28, window, kImageLargeFitRadioId,
                state->instance, state->settingsFont);
            SyncImageSizingControls(*state);

            state->countdownEnabledCheckbox = CreateSettingsControl(
                0, L"BUTTON", L"Enabled",
                WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
                30, 190, 900, 28, window, kCountdownEnabledCheckboxId,
                state->instance, state->settingsFont);
            state->countdownDurationGroup = CreateSettingsControl(
                0, L"BUTTON", L"Countdown time",
                WS_CHILD | BS_GROUPBOX,
                20, 232, kSettingsContentWidth - 40, 104, window,
                kCountdownDurationGroupId, state->instance, state->settingsFont);
            state->countdownHoursLabel = CreateSettingsControl(
                0, L"STATIC", L"Hours (0-99)",
                WS_CHILD | SS_LEFT,
                50, 260, 120, 24, window, kCountdownHoursLabelId,
                state->instance, state->settingsFont);
            state->countdownHoursEdit = CreateSettingsControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                50, 286, 120, 30, window, kCountdownHoursEditId,
                state->instance, state->settingsFont);
            SendMessageW(state->countdownHoursEdit, EM_LIMITTEXT, 2, 0);
            state->countdownMinutesLabel = CreateSettingsControl(
                0, L"STATIC", L"Minutes (0-59)",
                WS_CHILD | SS_LEFT,
                250, 260, 120, 24, window, kCountdownMinutesLabelId,
                state->instance, state->settingsFont);
            state->countdownMinutesEdit = CreateSettingsControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                250, 286, 120, 30, window, kCountdownMinutesEditId,
                state->instance, state->settingsFont);
            SendMessageW(state->countdownMinutesEdit, EM_LIMITTEXT, 2, 0);
            state->countdownSecondsLabel = CreateSettingsControl(
                0, L"STATIC", L"Seconds (0-59)",
                WS_CHILD | SS_LEFT,
                450, 260, 120, 24, window, kCountdownSecondsLabelId,
                state->instance, state->settingsFont);
            state->countdownSecondsEdit = CreateSettingsControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                450, 286, 120, 30, window, kCountdownSecondsEditId,
                state->instance, state->settingsFont);
            SendMessageW(state->countdownSecondsEdit, EM_LIMITTEXT, 2, 0);
            SyncCountdownControls(*state);

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
            CreateSettingsControl(
                0, L"BUTTON", L"Exit",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                822, 726, 82, 32, window, kExitSettingsButtonId, state->instance,
                state->settingsFont);

            RefreshSourceDirectoryList(*state);
            DragAcceptFiles(window, TRUE);
            ShowSettingsPage(*state);
            RECT client{};
            if (GetClientRect(window, &client)) {
                LayoutSettingsWindow(
                    *state, client.right - client.left, client.bottom - client.top);
            }
            RecordNormalSettingsWindowRect(*state, window);
        } catch (const Failure& failure) {
            state->settingsWindowError = failure.message;
            return -1;
        } catch (const std::exception&) {
            state->settingsWindowError = L"Could not initialize the settings window.";
            return -1;
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT minimum{
            0, 0, kSettingsMinimumClientWidth, kSettingsMinimumClientHeight};
        if (AdjustWindowRectEx(
                &minimum,
                kSettingsWindowStyle,
                FALSE,
                kSettingsWindowExtendedStyle)) {
            limits->ptMinTrackSize.x = minimum.right - minimum.left;
            limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
        } else {
            ShowError(
                L"Could not determine the Settings window minimum size.",
                window);
            limits->ptMinTrackSize.x = kSettingsMinimumClientWidth + 100;
            limits->ptMinTrackSize.y = kSettingsMinimumClientHeight + 100;
        }
        return 0;
    }
    case WM_SIZE:
        if (state && wParam != SIZE_MINIMIZED) {
            LayoutSettingsWindow(
                *state, static_cast<int>(LOWORD(lParam)),
                static_cast<int>(HIWORD(lParam)));
            RecordNormalSettingsWindowRect(*state, window);
            RedrawWindow(
                window,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
        return 0;
    case WM_DRAWITEM:
        if (state && (
                static_cast<int>(wParam) == kCountdownPreviousButtonId ||
                static_cast<int>(wParam) == kCountdownStartStopButtonId ||
                static_cast<int>(wParam) == kCountdownPauseResumeButtonId ||
                static_cast<int>(wParam) == kCountdownRestartButtonId ||
                static_cast<int>(wParam) == kCountdownNextButtonId)) {
            DrawCountdownButton(
                *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), *state);
            return TRUE;
        }
        if (state && static_cast<int>(wParam) == kBackgroundFillColorButtonId) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            const COLORREF color = RGB(
                state->settings.backgroundFillColor.red,
                state->settings.backgroundFillColor.green,
                state->settings.backgroundFillColor.blue);
            const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
            const COLORREF buttonFace = GetSysColor(COLOR_BTNFACE);
            const COLORREF swatchColor = disabled
                ? RGB(
                    (GetRValue(color) + GetRValue(buttonFace)) / 2,
                    (GetGValue(color) + GetGValue(buttonFace)) / 2,
                    (GetBValue(color) + GetBValue(buttonFace)) / 2)
                : color;
            const COLORREF previousBrushColor =
                SetDCBrushColor(draw->hDC, swatchColor);
            FillRect(
                draw->hDC,
                &draw->rcItem,
                static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            SetDCBrushColor(draw->hDC, previousBrushColor);
            FrameRect(
                draw->hDC,
                &draw->rcItem,
                GetSysColorBrush(disabled ? COLOR_3DSHADOW : COLOR_WINDOWTEXT));
            std::wostringstream colorLabel;
            colorLabel << L"#" << std::uppercase << std::hex << std::setfill(L'0')
                << std::setw(2) << static_cast<unsigned int>(state->settings.backgroundFillColor.red)
                << std::setw(2) << static_cast<unsigned int>(state->settings.backgroundFillColor.green)
                << std::setw(2) << static_cast<unsigned int>(state->settings.backgroundFillColor.blue);
            const int brightness =
                299 * GetRValue(swatchColor) +
                587 * GetGValue(swatchColor) +
                114 * GetBValue(swatchColor);
            const COLORREF previousTextColor = SetTextColor(
                draw->hDC,
                disabled
                    ? GetSysColor(COLOR_GRAYTEXT)
                    : brightness >= 128000 ? RGB(0, 0, 0) : RGB(255, 255, 255));
            const int previousBackgroundMode = SetBkMode(draw->hDC, TRANSPARENT);
            RECT textRect = draw->rcItem;
            DrawTextW(
                draw->hDC,
                colorLabel.str().c_str(),
                -1,
                &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetBkMode(draw->hDC, previousBackgroundMode);
            SetTextColor(draw->hDC, previousTextColor);
            if (!disabled && (draw->itemState & ODS_FOCUS) != 0) {
                RECT focusRect = draw->rcItem;
                InflateRect(&focusRect, -3, -3);
                DrawFocusRect(draw->hDC, &focusRect);
            }
            return TRUE;
        }
        break;
    case WM_MOVE:
        if (state) {
            RecordNormalSettingsWindowRect(*state, window);
        }
        break;
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
                    if (drag->iItem < 0 ||
                        static_cast<std::size_t>(drag->iItem) >=
                            state->settings.sourceDirectories.size()) {
                        return 0;
                    }
                    state->draggingSourceIndex = drag->iItem;
                    state->sourceDropIndex = -1;
                    InvalidateRect(state->sourceDirectoryList, nullptr, TRUE);
                    try {
                        StartDragGhost(*state);
                    } catch (const Failure& failure) {
                        ShowError(failure.message, window);
                    } catch (const std::exception&) {
                        ShowError(L"Could not show the source-folder drag preview.", window);
                    }
                    SetCapture(window);
                    SetCursor(LoadCursorW(nullptr, IDC_NO));
                    return 0;
                }
                if (notification->code == NM_CUSTOMDRAW) {
                    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
                    }
                    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT &&
                        static_cast<int>(draw->nmcd.dwItemSpec) ==
                            state->draggingSourceIndex) {
                        draw->clrTextBk = RGB(255, 222, 125);
                        draw->clrText = RGB(35, 38, 42);
                        return CDRF_NEWFONT;
                    }
                    if (draw->nmcd.dwDrawStage == CDDS_POSTPAINT &&
                        state->draggingSourceIndex >= 0) {
                        DrawSourceDirectoryInsertionMark(
                            draw->nmcd.hdc,
                            state->sourceDirectoryList,
                            state->sourceDropIndex);
                    }
                    return CDRF_DODEFAULT;
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
                            StartDragGhost(*state);
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
        case kCountdownEnabledCheckboxId:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled =
                    SendMessageW(
                        state->countdownEnabledCheckbox,
                        BM_GETCHECK,
                        0,
                        0) == BST_CHECKED;
                StartOrStopCountdown(*state, enabled, window);
                return 0;
            }
            break;
        case kCountdownHoursEditId:
        case kCountdownMinutesEditId:
        case kCountdownSecondsEditId:
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                SaveCountdownDuration(*state, window);
                return 0;
            }
            break;
        case kCountdownPreviousButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                RequestNextOrPrevious(*state, true);
                return 0;
            }
            break;
        case kCountdownNextButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                RequestNextOrPrevious(*state, false);
                return 0;
            }
            break;
        case kCountdownStartStopButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                StartOrStopCountdown(
                    *state, !state->settings.countdownEnabled, window);
                return 0;
            }
            break;
        case kCountdownPauseResumeButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                ToggleCountdownPause(*state);
                return 0;
            }
            break;
        case kCountdownRestartButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (state->settings.countdownEnabled) {
                    ResetCountdown(*state);
                }
                return 0;
            }
            break;
        case kImageResizeToRadioId:
        case kImageAutoFitRadioId:
        case kSameImageOnDisplaysRadioId:
        case kSeparateImagesOnDisplaysRadioId:
        case kImageSmallCenteredRadioId:
        case kImageSmallTileRadioId:
        case kImageSmallFitRadioId:
        case kImageLargeCenteredRadioId:
        case kImageLargeFillRadioId:
        case kImageLargeFitRadioId:
            if (HIWORD(wParam) == BN_CLICKED) {
                auto updated = state->settings;
                switch (LOWORD(wParam)) {
                case kSameImageOnDisplaysRadioId:
                    updated.multiMonitorImageMode =
                        swc::MultiMonitorImageMode::sameImage;
                    break;
                case kSeparateImagesOnDisplaysRadioId:
                    updated.multiMonitorImageMode =
                        swc::MultiMonitorImageMode::separateImages;
                    break;
                case kImageResizeToRadioId:
                    updated.imageSizingMode = swc::ImageSizingMode::resizeTo;
                    break;
                case kImageAutoFitRadioId:
                    updated.imageSizingMode = swc::ImageSizingMode::autoFit;
                    break;
                case kImageSmallCenteredRadioId:
                    updated.smallImageMode = swc::SmallImageMode::centered;
                    break;
                case kImageSmallTileRadioId:
                    updated.smallImageMode = swc::SmallImageMode::tile;
                    break;
                case kImageSmallFitRadioId:
                    updated.smallImageMode = swc::SmallImageMode::fit;
                    break;
                case kImageLargeCenteredRadioId:
                    updated.largeImageMode = swc::LargeImageMode::centered;
                    break;
                case kImageLargeFillRadioId:
                    updated.largeImageMode = swc::LargeImageMode::fill;
                    break;
                case kImageLargeFitRadioId:
                    updated.largeImageMode = swc::LargeImageMode::fit;
                    break;
                }
                SaveImageSettings(*state, std::move(updated), window);
                return 0;
            }
            break;
        case kImageResizeToPercentEditId:
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                SaveImagePercentSetting(*state, false, window);
                return 0;
            }
            break;
        case kImageMaxEnlargePercentEditId:
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                SaveImagePercentSetting(*state, true, window);
                return 0;
            }
            break;
        case kBackgroundFillColorModeRadioId:
        case kBackgroundAdaptiveModeRadioId:
            if (HIWORD(wParam) == BN_CLICKED) {
                auto updated = state->settings;
                updated.backgroundFillMode = LOWORD(wParam) ==
                    kBackgroundFillColorModeRadioId
                    ? swc::BackgroundFillMode::fillColor
                    : swc::BackgroundFillMode::adaptiveToImage;
                SaveBackgroundSettings(*state, std::move(updated), window);
                return 0;
            }
            break;
        case kBackgroundFillColorButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                ChooseBackgroundFillColor(*state, window);
                return 0;
            }
            break;
        case kChangeOnStartCheckboxId:
        case kOpenWindowOnStartCheckboxId:
        case kStartWithWindowsCheckboxId:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool openWindowSetting =
                    LOWORD(wParam) == kOpenWindowOnStartCheckboxId;
                const bool startWithWindowsSetting =
                    LOWORD(wParam) == kStartWithWindowsCheckboxId;
                const HWND checkbox = startWithWindowsSetting
                    ? state->startWithWindowsCheckbox
                    : openWindowSetting
                    ? state->openWindowOnStartCheckbox
                    : state->changeOnStartCheckbox;
                auto updated = state->settings;
                const bool checked =
                    SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (startWithWindowsSetting) {
                    updated.startWithWindows = checked;
                } else if (openWindowSetting) {
                    updated.openWindowOnStart = checked;
                } else {
                    updated.changeOnStart = checked;
                }
                bool registryChanged = false;
                try {
                    if (startWithWindowsSetting) {
                        SetStartWithWindows(checked);
                        registryChanged = true;
                    }
                    UpdateUserSettings(state->settingsPath, updated);
                    state->settings = std::move(updated);
                } catch (const Failure& failure) {
                    std::wstring error = failure.message;
                    if (registryChanged) {
                        try {
                            SetStartWithWindows(state->settings.startWithWindows);
                        } catch (const Failure& rollbackFailure) {
                            error += L"\r\n\r\nCould not restore the previous Windows startup "
                                L"entry: " + rollbackFailure.message;
                        } catch (const std::exception&) {
                            error += L"\r\n\r\nCould not restore the previous Windows startup "
                                L"entry due to an unexpected error.";
                        }
                    }
                    SendMessageW(
                        checkbox,
                        BM_SETCHECK,
                        (startWithWindowsSetting
                            ? state->settings.startWithWindows
                            : openWindowSetting
                            ? state->settings.openWindowOnStart
                            : state->settings.changeOnStart)
                            ? BST_CHECKED
                            : BST_UNCHECKED,
                        0);
                    ShowError(error, window);
                } catch (const std::exception&) {
                    std::wstring error = L"Could not save the Startup setting.";
                    if (registryChanged) {
                        try {
                            SetStartWithWindows(state->settings.startWithWindows);
                        } catch (const Failure& rollbackFailure) {
                            error += L"\r\n\r\nCould not restore the previous Windows startup "
                                L"entry: " + rollbackFailure.message;
                        } catch (const std::exception&) {
                            error += L"\r\n\r\nCould not restore the previous Windows startup "
                                L"entry due to an unexpected error.";
                        }
                    }
                    SendMessageW(
                        checkbox,
                        BM_SETCHECK,
                        (startWithWindowsSetting
                            ? state->settings.startWithWindows
                            : openWindowSetting
                            ? state->settings.openWindowOnStart
                            : state->settings.changeOnStart)
                            ? BST_CHECKED
                            : BST_UNCHECKED,
                        0);
                    ShowError(error, window);
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
        case kExitSettingsButtonId:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (!PostMessageW(
                        state->window,
                        WM_COMMAND,
                        MAKEWPARAM(kExitCommandId, 0),
                        0)) {
                    ShowError(L"Could not dispatch the Exit command.", window);
                }
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
                const int target = SourceDirectoryInsertionIndex(*state, point);
                if (target != state->sourceDropIndex) {
                    state->sourceDropIndex = target;
                    InvalidateRect(state->sourceDirectoryList, nullptr, TRUE);
                }
                PositionDragGhost(*state);
                SetCursor(LoadCursorW(
                    nullptr, target >= 0 ? IDC_SIZEALL : IDC_NO));
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
                PositionDragGhost(*state);
                SetPlaylistDragCursor(*state);
                return 0;
            }
            break;
    case WM_SETCURSOR:
            if (state && state->draggingSourceIndex >= 0) {
                SetCursor(LoadCursorW(
                    nullptr,
                    state->sourceDropIndex >= 0 ? IDC_SIZEALL : IDC_NO));
                PositionDragGhost(*state);
                return TRUE;
            }
            if (state && !state->draggingPlaylistImage.empty()) {
                SetPlaylistDragCursor(*state);
                PositionDragGhost(*state);
                return TRUE;
            }
            break;
    case WM_LBUTTONUP:
            if (state && state->draggingSourceIndex >= 0) {
                const int from = state->draggingSourceIndex;
                const int insertionIndex = state->sourceDropIndex;
                state->draggingSourceIndex = -1;
                state->sourceDropIndex = -1;
                StopPlaylistDragGhost(*state);
                InvalidateRect(state->sourceDirectoryList, nullptr, TRUE);
                ReleaseCapture();
                if (insertionIndex >= 0) {
                    ReorderSourceDirectory(
                        *state, from, insertionIndex, window);
                }
                return 0;
            }
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
            break;
    case WM_CAPTURECHANGED:
            if (state && state->draggingSourceIndex >= 0) {
                state->draggingSourceIndex = -1;
                state->sourceDropIndex = -1;
                StopPlaylistDragGhost(*state);
                InvalidateRect(state->sourceDirectoryList, nullptr, TRUE);
                return 0;
            }
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
        if (state) {
            SaveCountdownDuration(*state, window, true);
            SaveSettingsWindowBoundsWithError(*state, window);
        }
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (state && state->settingsWindow == window) {
            CancelPlaylistRefresh(*state);
            state->settingsWindow = nullptr;
            state->settingsTabs = nullptr;
            state->monitorPreviewPanel = nullptr;
            state->countdownDisplay = nullptr;
            state->countdownPreviousButton = nullptr;
            state->countdownStartStopButton = nullptr;
            state->countdownPauseResumeButton = nullptr;
            state->countdownRestartButton = nullptr;
            state->countdownNextButton = nullptr;
            state->countdownEnabledCheckbox = nullptr;
            state->countdownDurationGroup = nullptr;
            state->countdownHoursEdit = nullptr;
            state->countdownMinutesEdit = nullptr;
            state->countdownSecondsEdit = nullptr;
            state->countdownHoursLabel = nullptr;
            state->countdownMinutesLabel = nullptr;
            state->countdownSecondsLabel = nullptr;
            state->changeOnStartCheckbox = nullptr;
            state->openWindowOnStartCheckbox = nullptr;
            state->startWithWindowsCheckbox = nullptr;
            state->backgroundFillModeLabel = nullptr;
            state->backgroundFillColorModeRadio = nullptr;
            state->backgroundAdaptiveModeRadio = nullptr;
            state->backgroundFillColorLabel = nullptr;
            state->backgroundFillColorButton = nullptr;
            state->multiMonitorSettingsGroup = nullptr;
            state->imagePlacementGroup = nullptr;
            state->sameImageOnDisplaysRadio = nullptr;
            state->separateImagesOnDisplaysRadio = nullptr;
            state->imageResizeToRadio = nullptr;
            state->imageAutoFitRadio = nullptr;
            state->imageResizeToPercentEdit = nullptr;
            state->imageResizeToPercentLabel = nullptr;
            state->imageResizeToPercentUnitLabel = nullptr;
            state->imageSmallGroup = nullptr;
            state->imageSmallCenteredRadio = nullptr;
            state->imageSmallTileRadio = nullptr;
            state->imageSmallFitRadio = nullptr;
            state->imageMaxEnlargeLabel = nullptr;
            state->imageMaxEnlargePercentEdit = nullptr;
            state->imageMaxEnlargePercentLabel = nullptr;
            state->imageLargeGroup = nullptr;
            state->imageLargeCenteredRadio = nullptr;
            state->imageLargeFillRadio = nullptr;
            state->imageLargeFitRadio = nullptr;
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
            if (state->countdownFont) {
                DeleteObject(state->countdownFont);
                state->countdownFont = nullptr;
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
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int width = 1040;
    int height = 800;
    if (state.settings.hasWindowBounds &&
        IsSettingsWindowBoundsUsable(state.settings.windowBounds)) {
        x = state.settings.windowBounds.x;
        y = state.settings.windowBounds.y;
        width = state.settings.windowBounds.width;
        height = state.settings.windowBounds.height;
    }
    HWND window = CreateWindowExW(
        kSettingsWindowExtendedStyle,
        kSettingsWindowClass,
        L"Settings - Simple Wallpaper Changer",
        kSettingsWindowStyle,
        x,
        y,
        width,
        height,
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
    if (state.timerNextPending && !state.shuttingDown) {
        state.timerNextPending = false;
        StartWallpaperChange(state);
    } else if (state.backgroundReapplyPending && !state.shuttingDown) {
        state.backgroundReapplyPending = false;
        const bool reapplyMultiMonitorMode =
            state.multiMonitorModeReapplyPending;
        state.multiMonitorModeReapplyPending = false;
        StartWallpaperChange(
            state,
            WallpaperRequest{
                false, {}, {}, true, reapplyMultiMonitorMode});
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
        SaveCountdownDuration(state, state.settingsWindow, true);
        SaveSettingsWindowBoundsWithError(state, state.settingsWindow);
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
            RequestNextOrPrevious(*state, false);
            return 0;
        case kPreviousCommandId:
            RequestNextOrPrevious(*state, true);
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
            ProcessCountdownTick(*state);
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
    state.countdownRemainingSeconds =
        swc::CountdownDurationSeconds(state.settings);
    if (state.settings.countdownEnabled) {
        state.countdownDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(state.countdownRemainingSeconds);
    }

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
    if (settings.openWindowOnStart) {
        ShowSettingsWindow(state);
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
        if (state.settingsWindow &&
            IsDialogMessageW(state.settingsWindow, &message)) {
            continue;
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
