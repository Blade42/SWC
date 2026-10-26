#include "core.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
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
constexpr UINT kTrayIconId = 1;
constexpr UINT kNextCommandId = 1001;
constexpr UINT kPreviousCommandId = 1002;
constexpr UINT kExitCommandId = 1003;
constexpr UINT kSettingsCommandId = 1004;
constexpr UINT_PTR kWorkerPollTimerId = 1;
constexpr int kSettingsTabsId = 2001;
constexpr int kChangeOnStartCheckboxId = 2002;
constexpr int kSourceDirectoryEditId = 2003;
constexpr int kBrowseSourceDirectoryButtonId = 2004;
constexpr int kCloseSettingsButtonId = 2005;
constexpr int kSourceDirectoryLabelId = 2006;
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

std::filesystem::path ResolveSourceDirectory(
    const std::wstring& configuredDirectory,
    const std::filesystem::path& executableDirectory) {
    if (configuredDirectory.empty()) {
        return CurrentDirectory();
    }

    std::filesystem::path source(configuredDirectory);
    if (source.is_relative()) {
        source = executableDirectory / source;
    }

    std::error_code error;
    if (std::filesystem::is_directory(source, error) && !error) {
        const auto absolute = std::filesystem::absolute(source, error);
        if (!error) {
            return absolute;
        }
    }
    return CurrentDirectory();
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

struct Display {
    std::wstring devicePath;
    UINT width;
    UINT height;
    bool primary;
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
    const std::wstring contents =
        L"source_directory=" + settings.sourceDirectory + L"\r\n" +
        L"last_image=" + settings.lastImage + L"\r\n" +
        L"change_on_start=" + (settings.changeOnStart ? L"true" : L"false") + L"\r\n";
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

void UpdateUserSettings(
    const std::filesystem::path& path,
    const swc::Settings& preferences) {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    auto settings = ReadSettingsFile(path);
    settings.sourceDirectory = preferences.sourceDirectory;
    settings.changeOnStart = preferences.changeOnStart;
    WriteSettingsFile(path, settings);
}

void UpdateLastImageSettings(
    const std::filesystem::path& path,
    const std::wstring& lastImage) {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    auto settings = ReadSettingsFile(path);
    settings.lastImage = lastImage;
    WriteSettingsFile(path, settings);
}

void Run(bool previous = false) {
    const auto executableDirectory = ExecutableDirectory();
    const auto settingsPath = executableDirectory / L"settings.ini";
    const auto settings = ReadSettings(settingsPath);
    const auto sourceDirectory =
        ResolveSourceDirectory(settings.sourceDirectory, executableDirectory);
    const auto images = FindImages(sourceDirectory);
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
        names.push_back(image.filename().wstring());
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

    std::wstring lastImage = settings.lastImage;
    const auto selectCandidate = [&](std::size_t displayIndex, const std::wstring& cursor) {
        std::vector<std::wstring> failures;
        std::size_t selectedIndex = images.size();
        const auto order = previous
            ? swc::PreviousCandidateAttemptOrder(names, cursor)
            : swc::CandidateAttemptOrder(names, cursor);
        for (const std::size_t index : order) {
            try {
                CreateBackgroundPng(
                    factory.Get(),
                    images[index],
                    displays[displayIndex].width,
                    displays[displayIndex].height,
                    temporaryPaths[displayIndex]);
                selectedIndex = index;
                break;
            } catch (const Failure& failure) {
                failures.push_back(names[index] + L": " + failure.message);
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
    if (previous && hasSavedCursor) {
        for (std::size_t skippedDisplay = 1; skippedDisplay < displays.size(); ++skippedDisplay) {
            const auto currentSequenceImage = selectCandidate(0, lastImage);
            lastImage = names[currentSequenceImage];
        }
    }

    std::wstring lastDisplayImage = lastImage;
    for (std::size_t offset = 0; offset < displays.size(); ++offset) {
        const std::size_t displayIndex = previous
            ? displays.size() - 1 - offset
            : offset;
        const std::size_t selectedIndex = selectCandidate(displayIndex, lastImage);
        lastImage = names[selectedIndex];
        if (displayIndex == displays.size() - 1) {
            lastDisplayImage = lastImage;
        }
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
    UpdateLastImageSettings(settingsPath, previous ? lastDisplayImage : lastImage);
}

void ShowError(const std::wstring& message, HWND owner = nullptr) {
    MessageBoxW(
        owner,
        message.c_str(),
        L"Simple Wallpaper Changer",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

struct ResidentState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND settingsWindow = nullptr;
    HWND settingsTabs = nullptr;
    HWND changeOnStartCheckbox = nullptr;
    HWND sourceDirectoryLabel = nullptr;
    HWND sourceDirectoryEdit = nullptr;
    HFONT settingsFont = nullptr;
    std::filesystem::path settingsPath;
    swc::Settings settings;
    std::wstring settingsWindowError;
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
    const bool startupPage = TabCtrl_GetCurSel(state.settingsTabs) == 0;
    ShowWindow(state.changeOnStartCheckbox, startupPage ? SW_SHOW : SW_HIDE);
    ShowWindow(state.sourceDirectoryLabel, startupPage ? SW_HIDE : SW_SHOW);
    ShowWindow(state.sourceDirectoryEdit, startupPage ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(state.settingsWindow, kBrowseSourceDirectoryButtonId),
        startupPage ? SW_HIDE : SW_SHOW);
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
            state->settingsTabs = CreateSettingsControl(
                0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                12, 12, 460, 220, window, kSettingsTabsId, state->instance,
                state->settingsFont);
            wchar_t startupTabText[] = L"Startup";
            wchar_t sourcesTabText[] = L"Sources";
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
            TabCtrl_SetCurSel(state->settingsTabs, 0);

            state->changeOnStartCheckbox = CreateSettingsControl(
                0, L"BUTTON", L"Change wallpaper when the program starts",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                30, 54, 420, 24, window, kChangeOnStartCheckboxId, state->instance,
                state->settingsFont);
            SendMessageW(
                state->changeOnStartCheckbox,
                BM_SETCHECK,
                state->settings.changeOnStart ? BST_CHECKED : BST_UNCHECKED,
                0);

            state->sourceDirectoryLabel = CreateSettingsControl(
                0, L"STATIC", L"Current source folder:",
                WS_CHILD | SS_LEFT,
                30, 52, 420, 20, window, kSourceDirectoryLabelId, state->instance,
                state->settingsFont);
            state->sourceDirectoryEdit = CreateSettingsControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                30, 76, 350, 24, window, kSourceDirectoryEditId, state->instance,
                state->settingsFont);
            CreateSettingsControl(
                0, L"BUTTON", L"Browse...",
                WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                388, 76, 82, 24, window, kBrowseSourceDirectoryButtonId, state->instance,
                state->settingsFont);
            CreateSettingsControl(
                0, L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                388, 250, 82, 28, window, kCloseSettingsButtonId, state->instance,
                state->settingsFont);

            const auto sourceDirectory = ResolveSourceDirectory(
                state->settings.sourceDirectory,
                ExecutableDirectory());
            SetWindowTextW(state->sourceDirectoryEdit, sourceDirectory.c_str());
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
        if (state && reinterpret_cast<const NMHDR*>(lParam)->hwndFrom == state->settingsTabs &&
            reinterpret_cast<const NMHDR*>(lParam)->code == TCN_SELCHANGE) {
            ShowSettingsPage(*state);
            return 0;
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
        case kBrowseSourceDirectoryButtonId:
            try {
                const int currentDirectoryLength =
                    GetWindowTextLengthW(state->sourceDirectoryEdit);
                std::wstring currentDirectory(
                    static_cast<std::size_t>(currentDirectoryLength) + 1, L'\0');
                GetWindowTextW(
                    state->sourceDirectoryEdit,
                    currentDirectory.data(),
                    currentDirectoryLength + 1);
                currentDirectory.resize(static_cast<std::size_t>(currentDirectoryLength));
                const auto selectedDirectory = SelectSourceDirectory(
                    window, currentDirectory);
                if (!selectedDirectory.empty()) {
                    auto updated = state->settings;
                    updated.sourceDirectory = selectedDirectory;
                    UpdateUserSettings(state->settingsPath, updated);
                    state->settings = std::move(updated);
                    SetWindowTextW(state->sourceDirectoryEdit, selectedDirectory.c_str());
                }
            } catch (const Failure& failure) {
                ShowError(failure.message, window);
            } catch (const std::exception&) {
                ShowError(L"Could not save the Sources setting.", window);
            }
            return 0;
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
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (state && state->settingsWindow == window) {
            state->settingsWindow = nullptr;
            state->settingsTabs = nullptr;
            state->changeOnStartCheckbox = nullptr;
            state->sourceDirectoryLabel = nullptr;
            state->sourceDirectoryEdit = nullptr;
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
        500,
        330,
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
    }
}

void StartWallpaperChange(ResidentState& state, bool previous = false) {
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
        state.changeThread = std::thread([&state, previous]() {
            std::wstring error;
            try {
                Run(previous);
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
            StartWallpaperChange(*state, true);
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
    case WM_TIMER:
        if (wParam == kWorkerPollTimerId) {
            CompleteWallpaperChange(*state);
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
    const auto settings = ReadSettings(settingsPath);
    ResidentState state;
    state.instance = instance;
    state.settingsPath = settingsPath;
    state.settings = settings;

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_TAB_CLASSES;
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
