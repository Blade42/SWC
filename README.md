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

One PNG is generated beside the executable for each connected display. The primary display uses `wallpaper-background.png`; additional displays use `wallpaper-background-2.png`, `wallpaper-background-3.png`, and so on. Each image is created at that display's current resolution, with its source scaled to fit fully, centered, and letterboxed in black if its aspect ratio differs. The Windows per-monitor desktop wallpaper API applies each generated image to its corresponding display.

Displays are processed primary-first. Each display gets the next readable image in the sorted sequence; unreadable images are skipped as before. The sequence wraps and may reuse images if there are fewer readable images than displays. `last_image` records the final image assigned, so the next run continues after the full display sequence. Generated wallpaper PNGs are not considered source images.

WebP decoding depends on a WebP WIC decoder being installed and registered on the computer. Stock Windows 10 installations may not provide one. Without a registered decoder, WebP files are treated as unreadable and skipped; install the relevant Windows codec while online if WebP support is needed. JPG, JPEG, and PNG use Windows' built-in WIC codecs.

If the folder contains no supported image files, the program exits without changing the wallpaper or settings. If every supported image is missing or unreadable, it reports an error and leaves `last_image` unchanged.
