#include "core.h"

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct Failure {
    std::wstring message;
};

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

std::filesystem::path ExecutableDirectory() {
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
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768) {
            Fail(L"The executable path is too long.");
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
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

swc::Settings ReadSettings(const std::filesystem::path& path) {
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

bool SameWindowsPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring leftText = left.lexically_normal().native();
    const std::wstring rightText = right.lexically_normal().native();
    return CompareStringOrdinal(
        leftText.c_str(),
        static_cast<int>(leftText.size()),
        rightText.c_str(),
        static_cast<int>(rightText.size()),
        TRUE) == CSTR_EQUAL;
}

std::vector<std::filesystem::path> FindImages(
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& generatedImage) {
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
            SameWindowsPath(entry.path(), generatedImage)) {
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

std::pair<UINT, UINT> PrimaryDisplaySize() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) ||
        mode.dmPelsWidth == 0 || mode.dmPelsHeight == 0) {
        Fail(L"Could not determine the primary display resolution.");
    }
    return {mode.dmPelsWidth, mode.dmPelsHeight};
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

    std::vector<BYTE> canvas(static_cast<std::size_t>(outputBytes), 0);
    for (std::size_t offset = 3; offset < canvas.size(); offset += 4) {
        canvas[offset] = 255;
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

void ApplyWallpaper(const std::filesystem::path& image) {
    std::wstring filename = image.native();
    if (!SystemParametersInfoW(
            SPI_SETDESKWALLPAPER,
            0,
            filename.data(),
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        FailWin32(L"Windows could not apply the wallpaper");
    }
}

void WriteSettings(
    const std::filesystem::path& path,
    const swc::Settings& settings,
    const std::wstring& selectedImage) {
    const std::wstring contents =
        L"source_directory=" + settings.sourceDirectory + L"\r\n" +
        L"last_image=" + selectedImage + L"\r\n";
    const std::filesystem::path temporary = path.wstring() + L".tmp";
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
        DeleteFileW(temporary.c_str());
        FailWin32(L"Could not update settings.ini", error);
    }
}

void Run() {
    const auto executableDirectory = ExecutableDirectory();
    const auto settingsPath = executableDirectory / L"settings.ini";
    const auto settings = ReadSettings(settingsPath);
    const auto sourceDirectory =
        ResolveSourceDirectory(settings.sourceDirectory, executableDirectory);
    const auto outputPath = executableDirectory / L"wallpaper-background.png";
    const auto images = FindImages(sourceDirectory, outputPath);
    if (images.empty()) {
        return;
    }

    std::vector<std::wstring> names;
    names.reserve(images.size());
    for (const auto& image : images) {
        names.push_back(image.filename().wstring());
    }
    const auto order = swc::CandidateAttemptOrder(names, settings.lastImage);
    const auto [width, height] = PrimaryDisplaySize();

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CheckHr(comResult, L"Could not initialize Windows imaging");
    struct ComUninitializer {
        ~ComUninitializer() {
            CoUninitialize();
        }
    } comUninitializer;
    auto factory = CreateImagingFactory();

    std::vector<std::wstring> failures;
    std::size_t selectedIndex = images.size();
    const auto temporaryOutput =
        executableDirectory / L"wallpaper-background.png.tmp";
    for (const std::size_t index : order) {
        try {
            CreateBackgroundPng(
                factory.Get(),
                images[index],
                width,
                height,
                temporaryOutput);
            selectedIndex = index;
            break;
        } catch (const Failure& failure) {
            failures.push_back(names[index] + L": " + failure.message);
        }
    }

    if (selectedIndex == images.size()) {
        std::wstring message = L"No usable image could be processed.";
        for (std::size_t index = 0; index < failures.size() && index < 8; ++index) {
            message += L"\r\n\r\n" + failures[index];
        }
        if (failures.size() > 8) {
            message += L"\r\n\r\nAdditional images also failed.";
        }
        Fail(message);
    }

    if (!MoveFileExW(
            temporaryOutput.c_str(),
            outputPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        FailWin32(L"Could not publish the generated wallpaper");
    }
    ApplyWallpaper(outputPath);
    WriteSettings(settingsPath, settings, names[selectedIndex]);
}

void ShowError(const std::wstring& message) {
    MessageBoxW(
        nullptr,
        message.c_str(),
        L"Simple Wallpaper Changer",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

} // namespace

int main() {
    if (!SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS)) {
        ShowError(L"Could not set the process to low priority: " + Win32Message(GetLastError()));
        return 1;
    }

    try {
        Run();
        return 0;
    } catch (const Failure& failure) {
        ShowError(failure.message);
        return 1;
    }
}
