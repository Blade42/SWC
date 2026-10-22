#include "../src/core.h"

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

    const std::vector<std::wstring> names = {L"1.jpg", L"2.jpg", L"3.jpg"};
    assert((swc::CandidateAttemptOrder(names, L"") == std::vector<std::size_t>{0, 1, 2}));
    assert((swc::CandidateAttemptOrder(names, L"2.jpg") == std::vector<std::size_t>{2, 0, 1}));
    assert((swc::CandidateAttemptOrder(names, L"3.jpg") == std::vector<std::size_t>{0, 1, 2}));
    assert((swc::CandidateAttemptOrder(names, L"missing.jpg") == std::vector<std::size_t>{0, 1, 2}));
    assert(swc::CandidateAttemptOrder({}, L"").empty());
}
