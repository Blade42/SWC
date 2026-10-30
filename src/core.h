#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <istream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace swc {

struct SourceDirectory {
    std::wstring path;
    bool enabled = true;
};

inline bool operator==(const SourceDirectory& left, const SourceDirectory& right) {
    return left.path == right.path && left.enabled == right.enabled;
}

inline bool operator!=(const SourceDirectory& left, const SourceDirectory& right) {
    return !(left == right);
}

struct DisplayImage {
    std::wstring devicePath;
    std::wstring imagePath;
};

struct WindowBounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RgbColor {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

enum class BackgroundFillMode {
    fillColor,
    adaptiveToImage
};

enum class ImageSizingMode {
    resizeTo,
    autoFit
};

enum class SmallImageMode {
    centered,
    tile,
    fit
};

enum class LargeImageMode {
    centered,
    fill,
    fit
};

enum class MultiMonitorImageMode {
    sameImage,
    separateImages
};

struct Settings {
    std::vector<SourceDirectory> sourceDirectories;
    std::vector<DisplayImage> displayImages;
    std::wstring lastImage;
    bool changeOnStart = true;
    bool openWindowOnStart = false;
    bool startWithWindows = false;
    WindowBounds windowBounds;
    bool hasWindowBounds = false;
    BackgroundFillMode backgroundFillMode = BackgroundFillMode::adaptiveToImage;
    RgbColor backgroundFillColor{};
    ImageSizingMode imageSizingMode = ImageSizingMode::autoFit;
    std::uint32_t resizeToPercent = 90;
    SmallImageMode smallImageMode = SmallImageMode::fit;
    std::uint32_t maxSmallImageEnlargePercent = 400;
    LargeImageMode largeImageMode = LargeImageMode::fit;
    MultiMonitorImageMode multiMonitorImageMode =
        MultiMonitorImageMode::separateImages;
    bool countdownEnabled = false;
    std::uint32_t countdownHours = 0;
    std::uint32_t countdownMinutes = 10;
    std::uint32_t countdownSeconds = 0;
};

inline std::uint32_t CountdownDurationSeconds(const Settings& settings) {
    return settings.countdownHours * 3600 +
        settings.countdownMinutes * 60 + settings.countdownSeconds;
}

inline bool HasEnabledSourceDirectory(
    const std::vector<SourceDirectory>& sourceDirectories) {
    return std::any_of(
        sourceDirectories.begin(),
        sourceDirectories.end(),
        [](const SourceDirectory& directory) { return directory.enabled; });
}

inline SourceDirectory ParseSourceDirectory(std::wstring value) {
    SourceDirectory directory;
    if (!value.empty() && value.front() == L'-') {
        directory.enabled = false;
        value.erase(value.begin());
    }
    directory.path = std::move(value);
    return directory;
}

inline std::wstring SerializeSourceDirectory(const SourceDirectory& directory) {
    return (directory.enabled ? L"" : L"-") + directory.path;
}

struct FitRect {
    std::uint32_t left;
    std::uint32_t top;
    std::uint32_t width;
    std::uint32_t height;
};

struct ImagePlacement {
    std::int64_t left;
    std::int64_t top;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t tileColumns = 1;
    std::uint32_t tileRows = 1;
};

inline std::uint32_t ScaleImageDimension(
    std::uint32_t dimension,
    long double scale) {
    const long double scaled = static_cast<long double>(dimension) * scale;
    return static_cast<std::uint32_t>(std::min<long double>(
        std::numeric_limits<std::uint32_t>::max(),
        std::max<long double>(1, scaled + 0.5L)));
}

inline ImagePlacement CalculateImagePlacement(
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight,
    const Settings& settings) {
    if (sourceWidth == 0 || sourceHeight == 0 ||
        outputWidth == 0 || outputHeight == 0) {
        return {};
    }

    const bool fitsWithinDisplay =
        sourceWidth <= outputWidth && sourceHeight <= outputHeight;
    long double scale = 1.0L;
    if (settings.imageSizingMode == ImageSizingMode::resizeTo) {
        const std::uint64_t widthGap = sourceWidth > outputWidth
            ? static_cast<std::uint64_t>(sourceWidth) - outputWidth
            : static_cast<std::uint64_t>(outputWidth) - sourceWidth;
        const std::uint64_t heightGap = sourceHeight > outputHeight
            ? static_cast<std::uint64_t>(sourceHeight) - outputHeight
            : static_cast<std::uint64_t>(outputHeight) - sourceHeight;
        if (widthGap <= heightGap) {
            scale = static_cast<long double>(outputWidth) *
                settings.resizeToPercent / (100.0L * sourceWidth);
        } else {
            scale = static_cast<long double>(outputHeight) *
                settings.resizeToPercent / (100.0L * sourceHeight);
        }
    } else if (fitsWithinDisplay &&
        settings.smallImageMode == SmallImageMode::centered) {
        scale = 1.0L;
    } else if (fitsWithinDisplay &&
        settings.smallImageMode == SmallImageMode::tile) {
        const std::uint32_t columns = std::max<std::uint32_t>(
            1, outputWidth / sourceWidth);
        const std::uint32_t rows = std::max<std::uint32_t>(
            1, outputHeight / sourceHeight);
        if (columns > 1 || rows > 1) {
            const auto tiledWidth =
                static_cast<std::uint64_t>(sourceWidth) * columns;
            const auto tiledHeight =
                static_cast<std::uint64_t>(sourceHeight) * rows;
            return {
                (static_cast<std::int64_t>(outputWidth) -
                    static_cast<std::int64_t>(tiledWidth)) / 2,
                (static_cast<std::int64_t>(outputHeight) -
                    static_cast<std::int64_t>(tiledHeight)) / 2,
                sourceWidth,
                sourceHeight,
                columns,
                rows};
        }
    } else if (fitsWithinDisplay) {
        const long double fitScale = std::min(
            static_cast<long double>(outputWidth) / sourceWidth,
            static_cast<long double>(outputHeight) / sourceHeight);
        scale = std::min(
            fitScale,
            static_cast<long double>(settings.maxSmallImageEnlargePercent) / 100.0L);
    } else if (settings.largeImageMode == LargeImageMode::centered) {
        scale = 1.0L;
    } else if (settings.largeImageMode == LargeImageMode::fill) {
        scale = std::max(
            static_cast<long double>(outputWidth) / sourceWidth,
            static_cast<long double>(outputHeight) / sourceHeight);
    } else {
        scale = std::min(
            static_cast<long double>(outputWidth) / sourceWidth,
            static_cast<long double>(outputHeight) / sourceHeight);
    }

    const std::uint32_t width = ScaleImageDimension(sourceWidth, scale);
    const std::uint32_t height = ScaleImageDimension(sourceHeight, scale);
    return {
        (static_cast<std::int64_t>(outputWidth) - width) / 2,
        (static_cast<std::int64_t>(outputHeight) - height) / 2,
        width,
        height};
}

struct ColorSample {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha;
};

inline RgbColor EstimateBackgroundColor(
    const std::vector<ColorSample>& samples,
    std::uint32_t width,
    std::uint32_t height) {
    if (width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height != samples.size()) {
        return {};
    }

    struct ColorBucket {
        std::uint64_t weight = 0;
        std::uint64_t red = 0;
        std::uint64_t green = 0;
        std::uint64_t blue = 0;
    };
    constexpr std::size_t bucketCount = 32 * 32 * 32;
    std::vector<ColorBucket> histogram(bucketCount);

    for (std::uint32_t y = 0; y < height; ++y) {
        const long double verticalEdgeDistance = height > 1
            ? static_cast<long double>(std::min(y, height - 1 - y)) / (height - 1)
            : 0.0L;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto& sample = samples[static_cast<std::size_t>(y) * width + x];
            if (sample.alpha == 0) {
                continue;
            }

            const long double horizontalEdgeDistance = width > 1
                ? static_cast<long double>(std::min(x, width - 1 - x)) / (width - 1)
                : 0.0L;
            const long double edgeDistance =
                std::min(horizontalEdgeDistance, verticalEdgeDistance);
            const long double edgeProximity = std::max(0.0L, 1.0L - edgeDistance * 4.0L);
            const std::uint64_t edgeWeight =
                100 + static_cast<std::uint64_t>(edgeProximity * 300.0L + 0.5L);
            const std::uint64_t weight = edgeWeight * sample.alpha;

            const std::size_t bucket =
                (static_cast<std::size_t>(sample.red >> 3) << 10) |
                (static_cast<std::size_t>(sample.green >> 3) << 5) |
                (sample.blue >> 3);
            auto& color = histogram[bucket];
            color.weight += weight;
            color.red += weight * sample.red;
            color.green += weight * sample.green;
            color.blue += weight * sample.blue;
        }
    }

    const auto dominant = std::max_element(
        histogram.begin(),
        histogram.end(),
        [](const ColorBucket& left, const ColorBucket& right) {
            return left.weight < right.weight;
        });
    if (dominant == histogram.end() || dominant->weight == 0) {
        return {};
    }

    return {
        static_cast<std::uint8_t>((dominant->red + dominant->weight / 2) / dominant->weight),
        static_cast<std::uint8_t>((dominant->green + dominant->weight / 2) / dominant->weight),
        static_cast<std::uint8_t>((dominant->blue + dominant->weight / 2) / dominant->weight)};
}

inline FitRect CalculateFitRect(
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight) {
    if (sourceWidth == 0 || sourceHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        return {};
    }

    const long double scale = std::min(
        static_cast<long double>(outputWidth) / sourceWidth,
        static_cast<long double>(outputHeight) / sourceHeight);
    const auto width = std::min(
        outputWidth,
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(sourceWidth * scale + 0.5L)));
    const auto height = std::min(
        outputHeight,
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(sourceHeight * scale + 0.5L)));
    return {(outputWidth - width) / 2, (outputHeight - height) / 2, width, height};
}

inline std::wstring Trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

inline bool ParsePercent(
    const std::wstring& value,
    std::uint32_t minimum,
    std::uint32_t maximum,
    std::uint32_t& result) {
    std::wistringstream input(value);
    std::uint32_t parsed = 0;
    if (!(input >> parsed)) {
        return false;
    }
    input >> std::ws;
    if (input.peek() != std::char_traits<wchar_t>::eof() ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    result = parsed;
    return true;
}

inline bool ParseCountdownDuration(
    const std::wstring& hours,
    const std::wstring& minutes,
    const std::wstring& seconds,
    std::uint32_t& totalSeconds) {
    std::uint32_t parsedHours = 0;
    std::uint32_t parsedMinutes = 0;
    std::uint32_t parsedSeconds = 0;
    if (!ParsePercent(hours, 0, 99, parsedHours) ||
        !ParsePercent(minutes, 0, 59, parsedMinutes) ||
        !ParsePercent(seconds, 0, 59, parsedSeconds)) {
        return false;
    }

    const std::uint32_t total =
        parsedHours * 3600 + parsedMinutes * 60 + parsedSeconds;
    if (total == 0) {
        return false;
    }
    totalSeconds = total;
    return true;
}

inline Settings ParseSettings(std::wistream& input) {
    Settings settings;
    std::wstring line;
    while (std::getline(input, line)) {
        const auto comment = line.find(L';');
        if (comment != std::wstring::npos) {
            line.erase(comment);
        }
        line = Trim(std::move(line));
        if (line.empty() || line.front() == L';') {
            continue;
        }
        const auto separator = line.find(L'=');
        if (separator == std::wstring::npos) {
            continue;
        }

        const auto identifier = Trim(line.substr(0, separator));
        const auto value = Trim(line.substr(separator + 1));
        if (identifier == L"source_directories") {
            if (!value.empty()) {
                settings.sourceDirectories.push_back(ParseSourceDirectory(value));
            }
        } else if (identifier == L"last_image") {
            settings.lastImage = value;
        } else if (identifier == L"display_image") {
            const auto separator = value.find(L'|');
            if (separator != std::wstring::npos &&
                separator != 0 &&
                separator + 1 < value.size()) {
                settings.displayImages.push_back({
                    value.substr(0, separator),
                    value.substr(separator + 1)});
            }
        } else if (identifier == L"change_on_start") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            settings.changeOnStart = normalized == L"false" ? false : true;
        } else if (identifier == L"open_window_on_start") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            settings.openWindowOnStart = normalized == L"true";
        } else if (identifier == L"start_with_windows") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            settings.startWithWindows = normalized == L"true";
        } else if (identifier == L"settings_window_bounds") {
            std::wistringstream bounds(value);
            wchar_t separator = 0;
            WindowBounds parsed;
            const bool parsedBounds =
                static_cast<bool>(bounds >> parsed.x >> separator) &&
                separator == L',' &&
                static_cast<bool>(bounds >> parsed.y >> separator) &&
                separator == L',' &&
                static_cast<bool>(bounds >> parsed.width >> separator) &&
                separator == L',' &&
                static_cast<bool>(bounds >> parsed.height);
            bounds >> std::ws;
            if (parsedBounds &&
                bounds.peek() == std::char_traits<wchar_t>::eof() &&
                parsed.width > 0 && parsed.height > 0) {
                settings.windowBounds = parsed;
                settings.hasWindowBounds = true;
            }
        } else if (identifier == L"background_fill") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            if (normalized == L"fill_color") {
                settings.backgroundFillMode = BackgroundFillMode::fillColor;
            } else if (normalized == L"adaptive") {
                settings.backgroundFillMode = BackgroundFillMode::adaptiveToImage;
            }
        } else if (identifier == L"background_color") {
            if (value.size() == 6) {
                unsigned int rgb = 0;
                bool valid = true;
                for (const wchar_t digit : value) {
                    rgb <<= 4;
                    if (digit >= L'0' && digit <= L'9') {
                        rgb |= static_cast<unsigned int>(digit - L'0');
                    } else if (digit >= L'a' && digit <= L'f') {
                        rgb |= static_cast<unsigned int>(digit - L'a' + 10);
                    } else if (digit >= L'A' && digit <= L'F') {
                        rgb |= static_cast<unsigned int>(digit - L'A' + 10);
                    } else {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    settings.backgroundFillColor = {
                        static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
                        static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
                        static_cast<std::uint8_t>(rgb & 0xFF)};
                }
            }
        } else if (identifier == L"image_sizing") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            if (normalized == L"resize_to") {
                settings.imageSizingMode = ImageSizingMode::resizeTo;
            } else if (normalized == L"autofit") {
                settings.imageSizingMode = ImageSizingMode::autoFit;
            }
        } else if (identifier == L"resize_to_percent") {
            ParsePercent(value, 1, 1000, settings.resizeToPercent);
        } else if (identifier == L"autofit_small") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            if (normalized == L"centered") {
                settings.smallImageMode = SmallImageMode::centered;
            } else if (normalized == L"tile") {
                settings.smallImageMode = SmallImageMode::tile;
            } else if (normalized == L"fit") {
                settings.smallImageMode = SmallImageMode::fit;
            }
        } else if (identifier == L"autofit_max_enlarge_percent") {
            ParsePercent(
                value, 100, 1000, settings.maxSmallImageEnlargePercent);
        } else if (identifier == L"autofit_large") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            if (normalized == L"centered") {
                settings.largeImageMode = LargeImageMode::centered;
            } else if (normalized == L"fill") {
                settings.largeImageMode = LargeImageMode::fill;
            } else if (normalized == L"fit") {
                settings.largeImageMode = LargeImageMode::fit;
            }
        } else if (identifier == L"multi_monitor_image_mode") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            if (normalized == L"same_image") {
                settings.multiMonitorImageMode = MultiMonitorImageMode::sameImage;
            } else if (normalized == L"separate_images") {
                settings.multiMonitorImageMode = MultiMonitorImageMode::separateImages;
            }
        } else if (identifier == L"countdown_enabled") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            settings.countdownEnabled = normalized == L"true";
        } else if (identifier == L"countdown_hours") {
            ParsePercent(value, 0, 99, settings.countdownHours);
        } else if (identifier == L"countdown_minutes") {
            ParsePercent(value, 0, 59, settings.countdownMinutes);
        } else if (identifier == L"countdown_seconds") {
            ParsePercent(value, 0, 59, settings.countdownSeconds);
        }
    }
    if (CountdownDurationSeconds(settings) == 0) {
        settings.countdownHours = 0;
        settings.countdownMinutes = 10;
        settings.countdownSeconds = 0;
        settings.countdownEnabled = false;
    }
    return settings;
}

inline bool IsSupportedExtension(std::wstring extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });

    return extension == L".jpg" || extension == L".jpeg" ||
        extension == L".png" || extension == L".webp";
}

inline std::wstring FormatFileSize(std::uintmax_t bytes) {
    constexpr std::uintmax_t kibibyte = 1024;
    constexpr std::uintmax_t mebibyte = kibibyte * 1024;
    if (bytes < kibibyte) {
        return std::to_wstring(bytes) + L"B";
    }
    if (bytes < mebibyte) {
        const auto roundedKilobytes = (bytes + kibibyte / 2) / kibibyte;
        if (roundedKilobytes < 1024) {
            return std::to_wstring(roundedKilobytes) + L"k";
        }
    }

    std::wostringstream formatted;
    formatted << std::fixed << std::setprecision(1)
        << static_cast<long double>(bytes) / mebibyte << L"M";
    return formatted.str();
}

inline bool IsGenericMonitorName(const std::wstring& value) {
    std::wstring normalized = Trim(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return normalized.empty() ||
        normalized == L"generic pnp monitor" ||
        normalized == L"generic non-pnp monitor" ||
        normalized == L"pnp monitor" ||
        normalized == L"non-pnp monitor";
}

inline std::wstring EdidManufacturerName(const std::vector<std::uint8_t>& edid) {
    if (edid.size() < 10) {
        return {};
    }

    const std::uint16_t encoded =
        (static_cast<std::uint16_t>(edid[8]) << 8) | edid[9];
    std::wstring code(3, L'\0');
    for (std::size_t index = 0; index < code.size(); ++index) {
        const unsigned int shift = static_cast<unsigned int>((2 - index) * 5);
        const auto letter = static_cast<wchar_t>((encoded >> shift) & 0x1F);
        if (letter < 1 || letter > 26) {
            return {};
        }
        code[index] = static_cast<wchar_t>(L'A' + letter - 1);
    }

    if (code == L"SAM") return L"Samsung";
    if (code == L"DEL") return L"Dell";
    if (code == L"GSM" || code == L"LGD") return L"LG";
    if (code == L"ACI") return L"ASUS";
    if (code == L"HWP") return L"HP";
    if (code == L"BNQ") return L"BenQ";
    if (code == L"AOC") return L"AOC";
    if (code == L"PHI") return L"Philips";
    if (code == L"SNY") return L"Sony";
    if (code == L"LEN") return L"Lenovo";
    if (code == L"MSI") return L"MSI";
    if (code == L"VSC") return L"ViewSonic";
    return {};
}

inline std::wstring ExtractEdidMonitorName(const std::vector<std::uint8_t>& edid) {
    if (edid.size() < 128) {
        return {};
    }

    for (std::size_t offset = 54; offset + 18 <= 126; offset += 18) {
        if (edid[offset] != 0 ||
            edid[offset + 1] != 0 ||
            edid[offset + 2] != 0 ||
            edid[offset + 3] != 0xFC ||
            edid[offset + 4] != 0) {
            continue;
        }

        std::wstring name;
        for (std::size_t index = 5; index < 18; ++index) {
            const auto character = edid[offset + index];
            if (character == 0 || character == '\r' || character == '\n') {
                break;
            }
            if (character < 0x20 || character > 0x7E) {
                return {};
            }
            name.push_back(static_cast<wchar_t>(character));
        }
        name = Trim(std::move(name));
        if (IsGenericMonitorName(name)) {
            return {};
        }

        const std::wstring manufacturer = EdidManufacturerName(edid);
        if (!manufacturer.empty()) {
            std::wstring prefix = name.substr(0, manufacturer.size());
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            std::wstring normalizedManufacturer = manufacturer;
            std::transform(
                normalizedManufacturer.begin(),
                normalizedManufacturer.end(),
                normalizedManufacturer.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            if (prefix != normalizedManufacturer) {
                return manufacturer + L" " + name;
            }
        }
        return name;
    }
    return {};
}

inline std::wstring FormatDisplayName(
    const std::wstring& modelName,
    std::size_t primaryFirstIndex) {
    const std::wstring model = Trim(modelName);
    if (model.empty()) {
        return L"Display #" + std::to_wstring(primaryFirstIndex + 1);
    }
    return model + L" (#" + std::to_wstring(primaryFirstIndex + 1) + L")";
}

inline std::vector<std::size_t> CandidateAttemptOrder(
    const std::vector<std::wstring>& sortedNames,
    const std::wstring& lastImage) {
    std::vector<std::size_t> order;
    if (sortedNames.empty()) {
        return order;
    }

    std::size_t start = 0;
    if (!lastImage.empty()) {
        const auto previous = std::find(sortedNames.begin(), sortedNames.end(), lastImage);
        if (previous != sortedNames.end()) {
            start = (static_cast<std::size_t>(previous - sortedNames.begin()) + 1) % sortedNames.size();
        }
    }

    order.reserve(sortedNames.size());
    for (std::size_t offset = 0; offset < sortedNames.size(); ++offset) {
        order.push_back((start + offset) % sortedNames.size());
    }
    return order;
}

inline std::vector<std::size_t> CandidateAttemptOrderFrom(
    const std::vector<std::wstring>& sortedNames,
    const std::wstring& firstImage) {
    std::vector<std::size_t> order;
    if (sortedNames.empty()) {
        return order;
    }

    std::size_t start = 0;
    if (!firstImage.empty()) {
        const auto first = std::find(sortedNames.begin(), sortedNames.end(), firstImage);
        if (first != sortedNames.end()) {
            start = static_cast<std::size_t>(first - sortedNames.begin());
        }
    }

    order.reserve(sortedNames.size());
    for (std::size_t offset = 0; offset < sortedNames.size(); ++offset) {
        order.push_back((start + offset) % sortedNames.size());
    }
    return order;
}

inline std::vector<std::size_t> PreviousCandidateAttemptOrder(
    const std::vector<std::wstring>& sortedNames,
    const std::wstring& lastImage) {
    std::vector<std::size_t> order;
    if (sortedNames.empty()) {
        return order;
    }

    std::size_t start = sortedNames.size() - 1;
    if (!lastImage.empty()) {
        const auto previous = std::find(sortedNames.begin(), sortedNames.end(), lastImage);
        if (previous != sortedNames.end()) {
            const auto current = static_cast<std::size_t>(previous - sortedNames.begin());
            start = current == 0 ? sortedNames.size() - 1 : current - 1;
        }
    }

    order.reserve(sortedNames.size());
    for (std::size_t offset = 0; offset < sortedNames.size(); ++offset) {
        order.push_back((start + sortedNames.size() - offset) % sortedNames.size());
    }
    return order;
}

} // namespace swc
