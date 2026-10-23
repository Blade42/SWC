# Simple Wallpaper Changer

A small Windows C++ program that runs as a background resident with a system-tray icon. The tray menu can advance wallpapers on demand or exit the resident. It chooses images from a folder, creates a display-sized PNG for each connected display, and applies them as the desktop wallpapers. Image decoding and encoding use Windows Imaging Component (WIC); there are no bundled runtime libraries or network requirements. The process runs at idle priority.

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
change_on_start=true
```

`change_on_start` accepts `true` or `false`, case-insensitively. It defaults to `true` when missing or invalid. When true, the resident changes wallpapers once after starting; when false, it waits for the tray menu's **Next** command. An empty or unavailable source directory falls back to the process current directory. Relative source paths are resolved from the executable directory. If `last_image` is missing or not present in the folder, selection starts at the first image.

Supported filename extensions are `.jpg`, `.jpeg`, `.png`, and `.webp`, case-insensitively. Files are sorted with Windows logical filename ordering. If a selected image cannot be decoded, the program tries subsequent images in order.

## Output and limitations

One PNG is generated beside the executable for each connected display. The primary display uses `wallpaper-background.png`; additional displays use `wallpaper-background-2.png`, `wallpaper-background-3.png`, and so on. Each image is created at that display's current resolution, with its source scaled to fit fully and centered. Any uncovered area is filled with a color estimated from the image: a border-weighted dominant color, sampled from a small thumbnail. Fully transparent pixels are ignored and partially transparent pixels contribute in proportion to their opacity. The Windows per-monitor desktop wallpaper API applies each generated image to its corresponding display.

Displays are processed primary-first. Each display gets the next readable image in the sorted sequence; unreadable images are skipped as before. The sequence wraps and may reuse images if there are fewer readable images than displays. `last_image` records the final image assigned, so the next run continues after the full display sequence. Generated wallpaper PNGs are not considered source images.

WebP decoding depends on a WebP WIC decoder being installed and registered on the computer. Stock Windows 10 installations may not provide one. Without a registered decoder, WebP files are treated as unreadable and skipped; install the relevant Windows codec while online if WebP support is needed. JPG, JPEG, and PNG use Windows' built-in WIC codecs.

Launching the executable starts a resident process and exits. Repeated launches do not create duplicate residents. The tray icon tooltip is **Simple Wallpaper Changer**. Right-click it to select **Next** to advance the image sequence, **Previous** to step backward through it, or **Exit** to remove the tray icon and stop the resident after any current wallpaper change has cleaned up. Previous walks the sorted list backward for each display, skipping unreadable images and wrapping from the first image to the last. The final display's selected image is saved to `last_image`, so a subsequent Next returns to the sequence that preceded the Previous operation.

The tray icon is an original generic wallpaper-themed icon included in `src\tray-icon.ico`; it does not redistribute the linked stock photograph.

If the folder contains no supported image files, a wallpaper change ends without changing the wallpaper or settings. If every supported image is missing or unreadable, it reports an error and leaves `last_image` unchanged; the resident remains available for another attempt.
