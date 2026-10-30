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

In VS Code, select the **MinGW Makefiles** configure preset (`mingw`) from the CMake preset selector, then run **CMake: Build**.

## Settings

Place `settings.ini` beside the executable. You can rename the included `settings.sample.ini` to `settings.ini`, or let the program create a default `settings.ini` automatically on its first run. Values use `Identifier=value`; lines beginning with `;` are comments, and malformed lines are ignored. The recognized keys are:

```ini
; Add one source_directories entry for each folder, in the order they should be scanned.
; Prefix a path with - to disable that folder. Duplicate folders are allowed.
source_directories=C:\Users\Example\Pictures\Wallpapers
source_directories=-D:\SeasonalWallpapers
source_directories=E:\MoreWallpapers

; Last image identity, maintained automatically by the program.
last_image=

; Current playlist image per connected display, maintained automatically.
; Do not edit these entries; they are used to highlight current images in the Playlist tab.
; display_image=\\?\DISPLAY#ABC|C:\Users\Example\Pictures\Wallpapers\wallpaper2.jpg

; Change wallpapers once when the program starts: true or false.
change_on_start=true

; Open the Settings window when the program starts: true or false.
open_window_on_start=false

; Settings window position and size (left,top,width,height); maintained automatically.
; settings_window_bounds=100,100,1040,800
```

`change_on_start` accepts `true` or `false`, case-insensitively. It defaults to `true` when missing or invalid. When true, the resident changes wallpapers once after starting; when false, it waits for the tray menu's **Next** command. `open_window_on_start` accepts `true` or `false`, case-insensitively. It defaults to `false` when missing or invalid; when true, the Settings window opens automatically when the resident starts. The program maintains `settings_window_bounds` as the Settings window's left, top, width, and height, saving it when the window closes or the resident exits. If it is missing or unusable on the current display setup, the window uses its default size and Windows' default position. Source directories are scanned in list order, and images within each directory use Windows logical filename ordering; duplicate directory entries intentionally repeat their candidates. Relative source paths are resolved from the executable directory. If all source folders are disabled or no source entries are present, the process current directory is used. Unavailable configured folders are skipped; if every enabled folder is unavailable, the program reports those folders. The `last_image` and repeated `display_image` values are internal identities maintained automatically by the program. If `last_image` is missing or no longer matches an image, selection starts at the first candidate. `display_image` entries let the Playlist tab highlight the source file currently applied to each display; they are refreshed whenever the program changes wallpapers.

The previous single `source_directory` setting is not recognized; use the repeated `source_directories` entries shown above.

Supported filename extensions are `.jpg`, `.jpeg`, `.png`, and `.webp`, case-insensitively. Files are sorted with Windows logical filename ordering. If a selected image cannot be decoded, the program tries subsequent images in order.

## Output and limitations

One PNG is generated beside the executable for each connected display. The primary display uses `wallpaper-background.png`; additional displays use `wallpaper-background-2.png`, `wallpaper-background-3.png`, and so on. Setting an image on just one display uses a separate display-specific PNG so it cannot change another display that happens to share a generated wallpaper file. Each image is created at that display's current resolution, with its source scaled to fit fully and centered. Any uncovered area is filled with a color estimated from the image: a border-weighted dominant color, sampled from a small thumbnail. Fully transparent pixels are ignored and partially transparent pixels contribute in proportion to their opacity. The Windows per-monitor desktop wallpaper API applies each generated image to its corresponding display.

Displays are processed primary-first. Each display gets the next readable image in the combined sequence: each source folder's logically sorted images appended in configured folder order. Unreadable images are skipped as before. The sequence wraps and may reuse images if there are fewer readable images than displays. `last_image` records the final image assigned, so the next run continues after the full display sequence. Generated wallpaper PNGs are not considered source images.

WebP decoding depends on a WebP WIC decoder being installed and registered on the computer. Stock Windows 10 installations may not provide one. Without a registered decoder, WebP files are treated as unreadable and skipped; install the relevant Windows codec while online if WebP support is needed. JPG, JPEG, and PNG use Windows' built-in WIC codecs.

Launching the executable starts a resident process and exits. Repeated launches do not create duplicate residents. The tray icon tooltip is **Simple Wallpaper Changer**. Left-click the icon or choose **Settings...** from its right-click menu to open the resizable Settings window, initially 1040×800 when no saved bounds are available. Drag its borders or bottom-right corner to resize it; the content expands with the window and the footer buttons remain at the bottom-right. A minimum size prevents controls from overlapping. A monitor map above the tabs shows current wallpapers in monitor frames arranged like the Windows desktop; each monitor name is shown below its frame. Drag a Playlist image onto a monitor frame to set that display's wallpaper without changing the others. The **Startup** tab controls `change_on_start` and `open_window_on_start`; the **Sources** tab lets you add folders with the picker or by dropping folders from Explorer, remove folders, drag rows to reorder them with a folder ghost and insertion marker, and toggle each folder on or off. The **Playlist** tab shows the ordered candidate images with their source folder names (not full paths), file names, sizes, thumbnails, and displays currently showing each image. After the list loads or a wallpaper change completes, it scrolls the earliest current image into view near the top third when possible. Display names use the monitor model from EDID when available (for example `Samsung LU28R55 (#1)`); generic or unavailable names fall back to `Display #1`, numbered primary-first. Sizes use binary units with concise labels: rounded whole `k` below 1 MiB and one decimal `M` at or above 1 MiB; files under 1 KiB remain in bytes. Playlist scanning and thumbnail decoding run in the background; rows appear before thumbnails, which fill in progressively. Up to 512 thumbnails are cached in memory and reused while their file path, size, and modified time remain unchanged. Unreadable images use a small picture-outline placeholder with a red X. The list refreshes when opened and after source-list changes. Double-click an image to start the normal forward multi-display sequence with that image first; right-click to set it on a single display without changing the others. All settings are saved immediately. Close the window with **Close** or its title-bar X. The tray menu also offers **Next** to advance the image sequence, **Previous** to step backward through it, or **Exit** to remove the tray icon and stop the resident after any current wallpaper change has cleaned up. Previous walks backward past the images currently assigned to the earlier displays, then fills displays from last to first with preceding readable images. This makes Previous the inverse of Next across the complete multi-display sequence. Unreadable images are skipped and selection wraps from the first image to the last. If `last_image` is missing or stale, Previous starts at the last available candidate. The final display's selected image is saved to `last_image`.

The tray icon is an original generic wallpaper-themed icon included in `src\tray-icon.ico`; it does not redistribute the linked stock photograph.

If the combined source sequence contains no supported image files, a wallpaper change ends without changing the wallpaper or settings. If every supported image is missing or unreadable, it reports an error and leaves `last_image` unchanged; the resident remains available for another attempt.
