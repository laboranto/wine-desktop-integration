**English** | [한국어](README.ko.md)

# wine-desktop-integration

Per-app desktop integration for Wine apps on Linux, without rebuilding Wine.

- **File dialogs**: `GetOpenFileNameW` / `GetSaveFileNameW` open the desktop's native file chooser (KDE, GNOME, …) through xdg-desktop-portal.
- **"Show in folder"**: `explorer /select,<file>` and `explorer <folder>` open your Linux file manager (Dolphin, Nautilus, …) through `org.freedesktop.FileManager1`.

Anything it can't handle is passed to Wine's own implementation, so the worst case is the usual Wine dialog.

Tested with wine-cachyos 10.0 and KakaoTalk on KDE Plasma 6.

## How it works

On Windows and in Wine, a program looks for DLLs in its own folder before `system32`. Put a `comdlg32.dll` next to an app's exe and tell Wine to prefer the native copy for that app only (`comdlg32=native,builtin`), and that app uses ours while the rest of the prefix is unaffected.

- `comdlg32.dll` is a small proxy. It handles the two file dialog calls by running `wine-desktop-integration-helper`, a Python script on the Linux side that talks to the portal. Every other call goes straight to the prefix's original `system32\comdlg32.dll`.
- `explorer.exe` works the same way. When an app runs `explorer`, Wine looks in the current folder first. If the current folder is the app's install folder, our copy runs and forwards to the file manager. Arguments it doesn't recognize go to Wine's real `explorer.exe`.

Because nothing in Wine is patched, it keeps working across Wine updates.

## Requirements

**Build**
- `clang` and `lld` (no mingw needed)
- Wine development files: `/usr/include/wine/windows` and `/usr/lib/wine/x86_64-windows` (part of the regular `wine` package on Arch)

**Run**
- 64-bit Wine prefix and a 64-bit app
- `python3` with `dbus-python` and `PyGObject`
- `xdg-desktop-portal` with a FileChooser backend (e.g. `xdg-desktop-portal-kde` or `-gnome`)
- A file manager that implements `org.freedesktop.FileManager1` (Dolphin, Nautilus, Nemo, …)

## Install

```sh
./build.sh
```

Then use the management script. It **copies** files rather than linking them, so you can move or delete this repository afterwards and installed apps keep working.

```sh
# list candidate apps in a prefix (from Wine's menu entries and the Uninstall registry)
./wine-desktop-integration scan ~/.wine

# pick from that list interactively
./wine-desktop-integration install ~/.wine

# or name the exe directly
./wine-desktop-integration install ~/.wine ~/.wine/drive_c/Program\ Files/App/App.exe
```

`install` copies `comdlg32.dll` and `explorer.exe` into the app's folder, adds the per-app override, and copies the helper to `~/.local/bin` (change it with `WDI_BIN_DIR`). That folder must be on your `PATH`. It refuses to overwrite files that belong to the app itself.

Other commands:

| Command | What it does |
|---|---|
| `status` | Show each installed app and whether its files and override are current |
| `update` | Reinstall every listed app (run after `./build.sh` or when an app update removed the files) |
| `uninstall <prefix> <exe>` | Remove the files and override from that app |

The list of installed apps lives in `~/.local/share/wine-desktop-integration/apps`.

## Manual install

1. Run `./build.sh`.
2. Copy `wine-desktop-integration-helper` to a folder on your `PATH`.
3. Copy `comdlg32.dll` into the folder that contains the app's exe. Copy `explorer.exe` there too if you want "show in folder".
4. Run `winecfg` for that prefix. On the **Applications** tab, add the app's exe. With it selected, go to **Libraries**, add `comdlg32` and set it to **Native then Builtin**.

Step 4 as a command:

```sh
WINEPREFIX=~/.wine wine reg add 'HKCU\Software\Wine\AppDefaults\App.exe\DllOverrides' /v comdlg32 /d native,builtin /f
```

`explorer.exe` needs no override.

## Limitations

- **64-bit only.** 32-bit apps would need a separate i386 build.
- **Only `GetOpenFileNameW` / `GetSaveFileNameW`.** The ANSI versions (`...A`) and apps that use `IFileDialog` (the COM dialog used by many newer apps) still get Wine's dialog. So do calls with hooks or custom templates, and old-style multi-select without `OFN_EXPLORER`.
- **The explorer replacement depends on the current folder.** Wine searches the current folder for `explorer`, not the app's folder. If the app doesn't start with its install folder as the current folder, Wine's own explorer opens instead. Starting the app from a launcher that `cd`s into the install folder fixes this.
- `WDI_EXPLORER_ORIGINAL` is for testing only.

## Tests

```sh
test/run.sh <throwaway WINEPREFIX>
```

It creates and removes a test app inside that prefix and redirects the app list and helper to a temp folder. Don't run it on a prefix you use.

## License

LGPL-2.1-or-later, the same as Wine. See [LICENSE](LICENSE). The code is written from scratch and only uses Wine's headers.
