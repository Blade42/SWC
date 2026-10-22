# Simple Wallpaper Changer

A small Windows 10 C++ program that chooses the next image in a folder, creates a display-sized PNG, and applies it as the desktop wallpaper. Image decoding and encoding use Windows Imaging Component (WIC); there are no bundled runtime libraries or network requirements. The process runs at idle priority.

## Build

Use a Windows GCC toolchain with CMake 3.20 or later:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable is `build\simple-wallpaper-changer.exe`.

## Settings

Place `settings.ini` beside the executable. Values use `Identifier=value`; text after `;` and malformed lines are ignored. The recognized keys are:

```ini
source_directory=C:\Users\Example\Pictures\Wallpapers
last_image=wallpaper2.jpg
```

An empty or unavailable source directory falls back to the process current directory. Relative source paths are resolved from the executable directory. If `last_image` is missing or not present in the folder, selection starts at the first image.

Supported filename extensions are `.jpg`, `.jpeg`, `.png`, and `.webp`, case-insensitively. Files are sorted with Windows logical filename ordering. If a selected image cannot be decoded, the program tries subsequent images in order.

## Output and limitations

`wallpaper-background.png` is generated beside the executable at the primary display's current resolution. The input is scaled to fit fully, centered, and letterboxed in black if its aspect ratio differs. The generated PNG is applied using the Windows desktop wallpaper API, which may span or otherwise position the wallpaper according to the user's Windows desktop-background settings; it does not independently control each monitor.

WebP decoding depends on a WebP WIC decoder being installed and registered on the computer. Stock Windows 10 installations may not provide one. Without a registered decoder, WebP files are treated as unreadable and skipped; install the relevant Windows codec while online if WebP support is needed. JPG, JPEG, and PNG use Windows' built-in WIC codecs.

If the folder contains no supported image files, the program exits without changing the wallpaper or settings. If every supported image is missing or unreadable, it reports an error and leaves `last_image` unchanged.
