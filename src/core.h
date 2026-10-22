#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <istream>
#include <string>
#include <utility>
#include <vector>

namespace swc {

struct Settings {
    std::wstring sourceDirectory;
    std::wstring lastImage;
};

struct FitRect {
    std::uint32_t left;
    std::uint32_t top;
    std::uint32_t width;
    std::uint32_t height;
};

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
        const auto separator = line.find(L'=');
        if (separator == std::wstring::npos) {
            continue;
        }

        const auto identifier = Trim(line.substr(0, separator));
        const auto value = Trim(line.substr(separator + 1));
        if (identifier == L"source_directory") {
            settings.sourceDirectory = value;
        } else if (identifier == L"last_image") {
            settings.lastImage = value;
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

} // namespace swc
