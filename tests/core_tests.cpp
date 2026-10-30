#include "../src/core.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include <vector>

int main() {
    {
        std::wistringstream input(
            L"; whole-line comment\n"
            L"source_directories = C:\\Wallpapers ; inline comment\n"
            L"last_image=two.jpg\n"
            L"display_image=\\\\?\\DISPLAY#1|C:\\Wallpapers\\one.jpg\n"
            L"not an assignment\n"
            L"ignored=value\n"
            L"last_image=three=final.png\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.sourceDirectories.size() == 1);
        assert(settings.sourceDirectories[0].path == L"C:\\Wallpapers");
        assert(settings.sourceDirectories[0].enabled);
        assert(settings.lastImage == L"three=final.png");
        assert(settings.displayImages.size() == 1);
        assert(settings.displayImages[0].devicePath == L"\\\\?\\DISPLAY#1");
        assert(settings.displayImages[0].imagePath == L"C:\\Wallpapers\\one.jpg");
        assert(settings.changeOnStart);
    }

    {
        std::wistringstream input(
            L"source_directories=C:\\First\n"
            L"source_directories=-D:\\Disabled\n"
            L"source_directories=E:\\First\n"
            L"source_directories=C:\\First\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.sourceDirectories.size() == 4);
        assert(settings.sourceDirectories[0].path == L"C:\\First");
        assert(settings.sourceDirectories[0].enabled);
        assert(settings.sourceDirectories[1].path == L"D:\\Disabled");
        assert(!settings.sourceDirectories[1].enabled);
        assert(settings.sourceDirectories[2].path == L"E:\\First");
        assert(settings.sourceDirectories[2].enabled);
        assert(settings.sourceDirectories[3].path == L"C:\\First");
        assert(settings.sourceDirectories[3].enabled);
        assert(swc::SerializeSourceDirectory(settings.sourceDirectories[1]) == L"-D:\\Disabled");
        assert(swc::HasEnabledSourceDirectory(settings.sourceDirectories));
        assert(!swc::HasEnabledSourceDirectory({}));
        assert(!swc::HasEnabledSourceDirectory({{L"C:\\First", false}}));
    }

    {
        std::wistringstream input(
            L"; a comment\n"
            L"source_directories=C:\\Wallpapers ; inline comment\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.sourceDirectories.size() == 1);
        assert(settings.sourceDirectories[0].path == L"C:\\Wallpapers");
    }

    {
        std::wistringstream input(L"source_directory=C:\\Legacy\n");
        assert(swc::ParseSettings(input).sourceDirectories.empty());
    }

    {
        std::wistringstream input(L"change_on_start=FALSE\n");
        assert(!swc::ParseSettings(input).changeOnStart);
    }

    {
        std::wistringstream input(L"change_on_start=TrUe\n");
        assert(swc::ParseSettings(input).changeOnStart);
    }

    {
        std::wistringstream input(L"change_on_start=invalid\n");
        assert(swc::ParseSettings(input).changeOnStart);
    }

    {
        std::wistringstream input(L"");
        assert(!swc::ParseSettings(input).openWindowOnStart);
    }

    {
        std::wistringstream input(L"open_window_on_start=TrUe\n");
        assert(swc::ParseSettings(input).openWindowOnStart);
    }

    {
        std::wistringstream input(L"open_window_on_start=FALSE\n");
        assert(!swc::ParseSettings(input).openWindowOnStart);
    }

    {
        std::wistringstream input(L"open_window_on_start=invalid\n");
        assert(!swc::ParseSettings(input).openWindowOnStart);
    }

    {
        std::wistringstream input(L"");
        assert(!swc::ParseSettings(input).startWithWindows);
    }

    {
        std::wistringstream input(L"start_with_windows=TrUe\n");
        assert(swc::ParseSettings(input).startWithWindows);
    }

    {
        std::wistringstream input(L"start_with_windows=invalid\n");
        assert(!swc::ParseSettings(input).startWithWindows);
    }

    {
        std::wistringstream input(L"");
        const auto settings = swc::ParseSettings(input);
        assert(!settings.hasWindowBounds);
    }

    {
        std::wistringstream input(L"settings_window_bounds=-1920,40,1200,900\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.hasWindowBounds);
        assert(settings.windowBounds.x == -1920);
        assert(settings.windowBounds.y == 40);
        assert(settings.windowBounds.width == 1200);
        assert(settings.windowBounds.height == 900);
    }

    {
        std::wistringstream input(
            L"settings_window_bounds=10,20,0,800\n"
            L"settings_window_bounds=10,20,800\n");
        assert(!swc::ParseSettings(input).hasWindowBounds);
    }

    {
        std::wistringstream input(L"");
        const auto settings = swc::ParseSettings(input);
        assert(settings.backgroundFillMode == swc::BackgroundFillMode::adaptiveToImage);
        assert(settings.backgroundFillColor.red == 0);
        assert(settings.backgroundFillColor.green == 0);
        assert(settings.backgroundFillColor.blue == 0);
    }

    {
        std::wistringstream input(
            L"background_fill=fill_color\n"
            L"background_color=3A7FC1\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.backgroundFillMode == swc::BackgroundFillMode::fillColor);
        assert(settings.backgroundFillColor.red == 0x3A);
        assert(settings.backgroundFillColor.green == 0x7F);
        assert(settings.backgroundFillColor.blue == 0xC1);
    }

    {
        std::wistringstream input(
            L"background_fill=unknown\n"
            L"background_color=nothex\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.backgroundFillMode == swc::BackgroundFillMode::adaptiveToImage);
        assert(settings.backgroundFillColor.red == 0);
        assert(settings.backgroundFillColor.green == 0);
        assert(settings.backgroundFillColor.blue == 0);
    }

    {
        std::wistringstream input(L"");
        const auto settings = swc::ParseSettings(input);
        assert(settings.imageSizingMode == swc::ImageSizingMode::autoFit);
        assert(settings.resizeToPercent == 90);
        assert(settings.smallImageMode == swc::SmallImageMode::fit);
        assert(settings.maxSmallImageEnlargePercent == 400);
        assert(settings.largeImageMode == swc::LargeImageMode::fit);
    }

    {
        std::wistringstream input(
            L"image_sizing=RESIZE_TO\n"
            L"resize_to_percent=125\n"
            L"autofit_small=tile\n"
            L"autofit_max_enlarge_percent=250\n"
            L"autofit_large=fill\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.imageSizingMode == swc::ImageSizingMode::resizeTo);
        assert(settings.resizeToPercent == 125);
        assert(settings.smallImageMode == swc::SmallImageMode::tile);
        assert(settings.maxSmallImageEnlargePercent == 250);
        assert(settings.largeImageMode == swc::LargeImageMode::fill);
    }

    {
        std::wistringstream input(
            L"resize_to_percent=0\n"
            L"resize_to_percent=1001\n"
            L"autofit_max_enlarge_percent=99\n"
            L"autofit_max_enlarge_percent=1001\n"
            L"autofit_small=invalid\n"
            L"autofit_large=invalid\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.resizeToPercent == 90);
        assert(settings.maxSmallImageEnlargePercent == 400);
        assert(settings.smallImageMode == swc::SmallImageMode::fit);
        assert(settings.largeImageMode == swc::LargeImageMode::fit);
    }

    assert(swc::IsSupportedExtension(L".JPG"));
    assert(swc::IsSupportedExtension(L".jpeg"));
    assert(swc::IsSupportedExtension(L".Png"));
    assert(swc::IsSupportedExtension(L".webp"));
    assert(!swc::IsSupportedExtension(L".bmp"));
    assert(swc::FormatFileSize(0) == L"0B");
    assert(swc::FormatFileSize(850) == L"850B");
    assert(swc::FormatFileSize(121 * 1024) == L"121k");
    assert(swc::FormatFileSize(1023 * 1024) == L"1023k");
    assert(swc::FormatFileSize(1024 * 1024) == L"1.0M");
    assert(swc::FormatFileSize(2406 * 1024) == L"2.3M");

    {
        std::vector<std::uint8_t> edid(128, 0);
        const std::size_t descriptor = 54;
        const std::uint16_t samsungManufacturer =
            static_cast<std::uint16_t>((19 << 10) | (1 << 5) | 13);
        edid[8] = static_cast<std::uint8_t>(samsungManufacturer >> 8);
        edid[9] = static_cast<std::uint8_t>(samsungManufacturer & 0xFF);
        edid[descriptor + 3] = 0xFC;
        const std::string model = "LU28R55";
        std::copy(model.begin(), model.end(), edid.begin() + descriptor + 5);
        edid[descriptor + 5 + model.size()] = '\n';
        assert(swc::ExtractEdidMonitorName(edid) == L"Samsung LU28R55");
        assert(swc::FormatDisplayName(
            swc::ExtractEdidMonitorName(edid), 0) == L"Samsung LU28R55 (#1)");

        std::fill(edid.begin() + descriptor + 5, edid.begin() + descriptor + 18, 0);
        const std::string generic = "PnP Monitor";
        std::copy(generic.begin(), generic.end(), edid.begin() + descriptor + 5);
        assert(swc::ExtractEdidMonitorName(edid).empty());
        assert(swc::IsGenericMonitorName(L"Generic PnP Monitor"));
        assert(!swc::IsGenericMonitorName(L"Samsung LU28R55"));
        assert(swc::FormatDisplayName({}, 1) == L"Display #2");
        assert(swc::ExtractEdidMonitorName(std::vector<std::uint8_t>(32)).empty());
    }

    const auto landscape = swc::CalculateFitRect(800, 600, 1920, 1080);
    assert(landscape.left == 240);
    assert(landscape.top == 0);
    assert(landscape.width == 1440);
    assert(landscape.height == 1080);
    const auto portrait = swc::CalculateFitRect(600, 800, 1920, 1080);
    assert(portrait.left == 555);
    assert(portrait.top == 0);
    assert(portrait.width == 810);
    assert(portrait.height == 1080);
    assert(swc::CalculateFitRect(0, 10, 10, 10).width == 0);

    {
        const swc::Settings settings;
        const auto placement =
            swc::CalculateImagePlacement(800, 600, 1920, 1080, settings);
        assert(placement.left == 240 && placement.top == 0);
        assert(placement.width == 1440 && placement.height == 1080);
    }

    {
        swc::Settings settings;
        settings.imageSizingMode = swc::ImageSizingMode::resizeTo;
        const auto placement =
            swc::CalculateImagePlacement(50, 40, 100, 100, settings);
        assert(placement.left == 5 && placement.top == 14);
        assert(placement.width == 90 && placement.height == 72);
    }

    {
        swc::Settings settings;
        settings.imageSizingMode = swc::ImageSizingMode::resizeTo;
        settings.resizeToPercent = 90;
        const auto placement =
            swc::CalculateImagePlacement(200, 100, 100, 100, settings);
        assert(placement.left == -40 && placement.top == 5);
        assert(placement.width == 180 && placement.height == 90);
    }

    {
        swc::Settings settings;
        settings.smallImageMode = swc::SmallImageMode::centered;
        auto placement = swc::CalculateImagePlacement(50, 40, 100, 100, settings);
        assert(placement.left == 25 && placement.top == 30);
        assert(placement.width == 50 && placement.height == 40);

        settings.smallImageMode = swc::SmallImageMode::tile;
        placement = swc::CalculateImagePlacement(30, 30, 100, 100, settings);
        assert(placement.left == 5 && placement.top == 5);
        assert(placement.tileColumns == 3 && placement.tileRows == 3);

        placement = swc::CalculateImagePlacement(90, 100, 100, 100, settings);
        assert(placement.left == 5 && placement.top == 0);
        assert(placement.tileColumns == 1 && placement.tileRows == 1);
    }

    {
        swc::Settings settings;
        settings.smallImageMode = swc::SmallImageMode::fit;
        settings.maxSmallImageEnlargePercent = 150;
        auto placement = swc::CalculateImagePlacement(50, 40, 100, 100, settings);
        assert(placement.left == 12 && placement.top == 20);
        assert(placement.width == 75 && placement.height == 60);

        settings.maxSmallImageEnlargePercent = 400;
        placement = swc::CalculateImagePlacement(50, 40, 100, 100, settings);
        assert(placement.width == 100 && placement.height == 80);
    }

    {
        swc::Settings settings;
        settings.largeImageMode = swc::LargeImageMode::centered;
        auto placement = swc::CalculateImagePlacement(200, 100, 100, 100, settings);
        assert(placement.left == -50 && placement.top == 0);
        assert(placement.width == 200 && placement.height == 100);

        placement = swc::CalculateImagePlacement(120, 80, 100, 100, settings);
        assert(placement.left == -10 && placement.top == 10);
        assert(placement.width == 120 && placement.height == 80);

        settings.largeImageMode = swc::LargeImageMode::fill;
        placement = swc::CalculateImagePlacement(200, 150, 100, 100, settings);
        assert(placement.left == -16 && placement.top == 0);
        assert(placement.width == 133 && placement.height == 100);

        settings.largeImageMode = swc::LargeImageMode::fit;
        placement = swc::CalculateImagePlacement(200, 100, 100, 100, settings);
        assert(placement.left == 0 && placement.top == 25);
        assert(placement.width == 100 && placement.height == 50);
    }

    {
        std::vector<swc::ColorSample> pixels(100, {0, 0, 255, 255});
        for (std::size_t y = 0; y < 10; ++y) {
            for (std::size_t x = 0; x < 10; ++x) {
                if (x == 0 || x == 9 || y == 0 || y == 9) {
                    pixels[y * 10 + x] = {255, 0, 0, 255};
                }
            }
        }
        const auto edgeDominant = swc::EstimateBackgroundColor(pixels, 10, 10);
        assert(edgeDominant.red == 255);
        assert(edgeDominant.green == 0);
        assert(edgeDominant.blue == 0);
    }

    {
        std::vector<swc::ColorSample> pixels(4, {0, 0, 255, 0});
        pixels[0] = {15, 183, 47, 255};
        pixels[1] = {8, 176, 40, 64};
        const auto visibleDominant = swc::EstimateBackgroundColor(pixels, 2, 2);
        assert(visibleDominant.red == 14);
        assert(visibleDominant.green == 182);
        assert(visibleDominant.blue == 46);
    }

    {
        const auto noVisibleColor = swc::EstimateBackgroundColor(
            std::vector<swc::ColorSample>(4, {255, 0, 255, 0}),
            2,
            2);
        assert(noVisibleColor.red == 0);
        assert(noVisibleColor.green == 0);
        assert(noVisibleColor.blue == 0);
    }

    const std::vector<std::wstring> names = {L"1.jpg", L"2.jpg", L"3.jpg"};
    assert((swc::CandidateAttemptOrder(names, L"") == std::vector<std::size_t>{0, 1, 2}));
    assert((swc::CandidateAttemptOrder(names, L"2.jpg") == std::vector<std::size_t>{2, 0, 1}));
    assert((swc::CandidateAttemptOrder(names, L"3.jpg") == std::vector<std::size_t>{0, 1, 2}));
    assert((swc::CandidateAttemptOrder(names, L"missing.jpg") == std::vector<std::size_t>{0, 1, 2}));
    assert(swc::CandidateAttemptOrder({}, L"").empty());
    assert((swc::CandidateAttemptOrderFrom(names, L"2.jpg") ==
        std::vector<std::size_t>{1, 2, 0}));
    assert((swc::CandidateAttemptOrderFrom(names, L"missing.jpg") ==
        std::vector<std::size_t>{0, 1, 2}));
    assert(swc::CandidateAttemptOrderFrom({}, L"").empty());
    {
        std::vector<std::wstring> sequence;
        const auto order = swc::CandidateAttemptOrderFrom(names, L"3.jpg");
        for (std::size_t display = 0; display < 3; ++display) {
            sequence.push_back(names[order[display]]);
        }
        assert((sequence == std::vector<std::wstring>{
            L"3.jpg", L"1.jpg", L"2.jpg"}));
    }
    assert((swc::PreviousCandidateAttemptOrder(names, L"2.jpg") ==
        std::vector<std::size_t>{0, 2, 1}));
    assert((swc::PreviousCandidateAttemptOrder(names, L"1.jpg") ==
        std::vector<std::size_t>{2, 1, 0}));
    assert((swc::PreviousCandidateAttemptOrder(names, L"") ==
        std::vector<std::size_t>{2, 1, 0}));
    assert((swc::PreviousCandidateAttemptOrder(names, L"missing.jpg") ==
        std::vector<std::size_t>{2, 1, 0}));
    assert(swc::PreviousCandidateAttemptOrder({}, L"").empty());

    {
        const std::vector<std::wstring> sourcesInListOrder = {
            L"0|C:\\First\\1.jpg",
            L"0|C:\\First\\2.jpg",
            L"2|D:\\Second\\1.jpg"};
        assert((swc::CandidateAttemptOrder(sourcesInListOrder, L"") ==
            std::vector<std::size_t>{0, 1, 2}));
        assert((swc::CandidateAttemptOrder(sourcesInListOrder, L"0|C:\\First\\1.jpg") ==
            std::vector<std::size_t>{1, 2, 0}));
    }

    {
        const std::vector<std::wstring> sequence = {
            L"1.jpg", L"2.jpg", L"3.jpg", L"4.jpg", L"5.jpg"};
        std::wstring cursor = L"4.jpg";
        const auto skippedCurrentFirst = swc::PreviousCandidateAttemptOrder(sequence, cursor);
        cursor = sequence[skippedCurrentFirst.front()];

        std::vector<std::wstring> previousImages(2);
        for (std::size_t offset = 0; offset < previousImages.size(); ++offset) {
            const std::size_t display = previousImages.size() - 1 - offset;
            const auto order = swc::PreviousCandidateAttemptOrder(sequence, cursor);
            cursor = sequence[order.front()];
            previousImages[display] = cursor;
        }
        assert((previousImages == std::vector<std::wstring>{L"1.jpg", L"2.jpg"}));
        assert(cursor == L"1.jpg");

        std::wstring lastDisplayImage = previousImages.back();
        cursor = lastDisplayImage;

        std::vector<std::wstring> nextImages;
        for (std::size_t display = 0; display < previousImages.size(); ++display) {
            const auto order = swc::CandidateAttemptOrder(sequence, cursor);
            cursor = sequence[order.front()];
            nextImages.push_back(cursor);
        }
        assert((nextImages == std::vector<std::wstring>{L"3.jpg", L"4.jpg"}));
    }

    {
        const std::vector<std::wstring> sequence = {
            L"1.jpg", L"bad-a.jpg", L"2.jpg", L"bad-b.jpg", L"3.jpg", L"4.jpg"};
        std::wstring cursor = L"4.jpg";
        const auto skipOrder = swc::PreviousCandidateAttemptOrder(sequence, cursor);
        const auto skippedCurrentFirst = std::find_if(
            skipOrder.begin(),
            skipOrder.end(),
            [&](std::size_t index) { return sequence[index].find(L"bad") == std::wstring::npos; });
        assert(skippedCurrentFirst != skipOrder.end());
        cursor = sequence[*skippedCurrentFirst];

        std::vector<std::wstring> previousImages(2);
        for (std::size_t offset = 0; offset < previousImages.size(); ++offset) {
            const std::size_t display = previousImages.size() - 1 - offset;
            const auto order = swc::PreviousCandidateAttemptOrder(sequence, cursor);
            const auto selected = std::find_if(
                order.begin(),
                order.end(),
                [&](std::size_t index) { return sequence[index].find(L"bad") == std::wstring::npos; });
            assert(selected != order.end());
            cursor = sequence[*selected];
            previousImages[display] = cursor;
        }
        assert((previousImages == std::vector<std::wstring>{L"1.jpg", L"2.jpg"}));
    }

    {
        const std::vector<std::wstring> sequence = {
            L"1.jpg", L"2.jpg", L"3.jpg", L"4.jpg", L"5.jpg", L"6.jpg"};

        std::wstring cursor = L"4.jpg";
        std::vector<std::wstring> singleDisplay;
        const auto singleOrder = swc::PreviousCandidateAttemptOrder(sequence, cursor);
        cursor = sequence[singleOrder.front()];
        singleDisplay.push_back(cursor);
        assert((singleDisplay == std::vector<std::wstring>{L"3.jpg"}));
        cursor = singleDisplay.back();
        assert(sequence[swc::CandidateAttemptOrder(sequence, cursor).front()] == L"4.jpg");

        cursor = L"6.jpg";
        for (std::size_t skipped = 1; skipped < 3; ++skipped) {
            const auto order = swc::PreviousCandidateAttemptOrder(sequence, cursor);
            cursor = sequence[order.front()];
        }
        std::vector<std::wstring> threeDisplays(3);
        for (std::size_t offset = 0; offset < threeDisplays.size(); ++offset) {
            const std::size_t display = threeDisplays.size() - 1 - offset;
            const auto order = swc::PreviousCandidateAttemptOrder(sequence, cursor);
            cursor = sequence[order.front()];
            threeDisplays[display] = cursor;
        }
        assert((threeDisplays == std::vector<std::wstring>{
            L"1.jpg", L"2.jpg", L"3.jpg"}));
        cursor = threeDisplays.back();
        std::vector<std::wstring> restored;
        for (std::size_t display = 0; display < threeDisplays.size(); ++display) {
            const auto order = swc::CandidateAttemptOrder(sequence, cursor);
            cursor = sequence[order.front()];
            restored.push_back(cursor);
        }
        assert((restored == std::vector<std::wstring>{
            L"4.jpg", L"5.jpg", L"6.jpg"}));
    }

    {
        std::wstring lastImage = L"1.jpg";
        std::vector<std::wstring> assigned;
        for (std::size_t display = 0; display < 4; ++display) {
            const auto order = swc::CandidateAttemptOrder(names, lastImage);
            const auto selected = std::find_if(order.begin(), order.end(), [](std::size_t index) {
                return index != 1;
            });
            assert(selected != order.end());
            lastImage = names[*selected];
            assigned.push_back(lastImage);
        }
        assert((assigned == std::vector<std::wstring>{
            L"3.jpg", L"1.jpg", L"3.jpg", L"1.jpg"}));
    }
}
