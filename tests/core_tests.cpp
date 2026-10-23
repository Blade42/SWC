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
            L"source_directory = C:\\Wallpapers ; inline comment\n"
            L"last_image=two.jpg\n"
            L"not an assignment\n"
            L"ignored=value\n"
            L"last_image=three=final.png\n");
        const auto settings = swc::ParseSettings(input);
        assert(settings.sourceDirectory == L"C:\\Wallpapers");
        assert(settings.lastImage == L"three=final.png");
        assert(settings.changeOnStart);
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

    assert(swc::IsSupportedExtension(L".JPG"));
    assert(swc::IsSupportedExtension(L".jpeg"));
    assert(swc::IsSupportedExtension(L".Png"));
    assert(swc::IsSupportedExtension(L".webp"));
    assert(!swc::IsSupportedExtension(L".bmp"));

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
