#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <istream>
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

struct Settings {
    std::vector<SourceDirectory> sourceDirectories;
    std::wstring lastImage;
    bool changeOnStart = true;
};

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

struct ColorSample {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha;
};

struct RgbColor {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
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
        } else if (identifier == L"change_on_start") {
            std::wstring normalized = value;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            settings.changeOnStart = normalized == L"false" ? false : true;
        }
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
