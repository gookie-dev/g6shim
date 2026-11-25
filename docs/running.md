# Running a game

Work on a copy of the game folder. Deploying writes DLLs into it, and the game writes logs, saves and settings next to its executable.

## Workflow

```bat
:: system functions the game needs and how many g6shim provides
g6shim-scan D:\work\Game --hle hle

:: one replacement DLL project per missing DLL; names the PC's kernel32.dll also exports are forwarded
g6shim-stubgen D:\work\Game -o hle --forward kernel32.dll

:: route imports of DLLs Windows always loads itself (KnownDLLs) through g6shim_<dll>
g6shim-relink D:\original\Game.exe -o D:\work\Game\Game.exe --rename combase.dll
g6shim-stubgen D:\work\Game -o hle --forward combase.dll

:: build the DLLs and copy them next to the game
cmake --preset release -DG6SHIM_GAME_DIR=D:\work\Game
cmake --build --preset release --target g6shim_deploy

:: run it
g6shim-run D:\work\Game\Game.exe
```

`unimplemented: <dll>!<function>` in the output is the next function to write. [docs/dev/hle.md](dev/hle.md) explains how.

## GDK titles

GDK titles call the PC's file functions directly, so they need the virtual drive without `kernelx.dll`. Point their file imports at the forwarders:

```bat
g6shim-relink D:\original\Game.exe -o D:\work\Game\Game.exe ^
  --rename api-ms-win-core-file-l1-1-0.dll=g6shim_file.dll ^
  --rename api-ms-win-core-file-l2-1-0.dll=g6shim_file2.dll ^
  --rename api-ms-win-core-libraryloader-l1-2-0.dll=g6shim_libloader.dll ^
  --rename api-ms-win-core-libraryloader-l1-2-1.dll=g6shim_libloader2.dll ^
  --rename api-ms-win-crt-filesystem-l1-1-0.dll=g6shim_crtfs.dll ^
  --rename api-ms-win-crt-stdio-l1-1-0.dll=g6shim_crtstdio.dll ^
  --rename api-ms-win-crt-time-l1-1-0.dll=g6shim_crttime.dll ^
  --rename kernel32.dll=g6shim_kernel32.dll ^
  --rename advapi32.dll=g6shim_advapi32.dll
```

Each forwarder re-exports the whole DLL it replaces: names it does not translate reach Windows unchanged, and the path-taking ones rewrite `G:\` to the folder of the executable. `G:\common.rpf` is `<exe folder>\common.rpf`. Build and deploy the forwarders like the other replacement DLLs. `g6shim_advapi32.dll` reports the selected console model as the OEM `ConsoleMode` registry value, so code that reads it does not mistake the host for a PC.

The two library-loader forwarders make `LoadLibraryExW`/`LoadLibraryW` of `xgameruntime.dll` load g6shim's offline runtime `g6shim_xgameruntime.dll` from the folder of the executable instead of the PC's runtime, so the title asks g6shim for its runtime services ([abi/xgameruntime.md](abi/xgameruntime.md)).

## Tools

| Tool | Does |
|---|---|
| `g6shim-scan <folder or files> [--hle <dir>] [--all] [--importers] [--json]` | Lists imports that neither Windows nor the game provides, with coverage `[x]` implemented, `[~]` stubbed, `[ ]` missing. Skips folders named `EmbeddedXvd`. |
| `g6shim-stubgen <folder or files> -o <hle dir> [--dll <name>] [--forward <dll>]` | Creates or extends `hle/<name>/`. Never overwrites; appends what is missing. |
| `g6shim-relink <input> -o <output> [--rename <dll>[=<new>]] [--keep-subsystem]` | Writes a patched copy with renamed imports and the Windows GUI subsystem. Refuses to write over its input. |
| `g6shim-run <exe> [--timeout <s>] [-v] [--log <file>] [-- <args>]` | Starts the game under a minimal debugger and prints debug output, DLLs loaded from outside System32, exceptions as module+offset, stowed WinRT errors and the exit code. Default timeout 30 s. |
| `g6shim-run -p <pid> -tid <tid> ...` | Attaches to a suspended process and resumes it, the way `plmdebug /enableDebug` starts a package debugger. |
| `g6shim-shader <folder or files> [--json <out.json>] [--title-id <8 hex digits>] [--out <dir>] [-v]` | Finds the shader containers a game ships, decodes their native programs, translates them and counts unique shaders per stage. `--json` writes the unique per-stage counts for the progress maps. `--out` writes each translated PC container as `<hash>.dxil` into that folder; a folder inside the repository is refused. |

## Settings

`g6shim.ini` next to the executable is read once, as UTF-8. A missing file changes nothing. A set environment variable overrides the file, including a value of `0`; an unset or empty variable does not. A flag is on when its value is non-empty and does not start with `0`. Unknown sections, unknown keys and malformed lines are logged once and ignored. Section and key names are case-insensitive.

| Section | Key | Variable | Default | Effect |
|---|---|---|---|---|
| `log` | `file` | `G6SHIM_LOG` | `g6shim.log` in the working directory | Log file. Under `g6shim-run` the working directory is the game folder. |
| `log` | `stderr` | `G6SHIM_LOG_STDERR` | on | `0` stops copying the log to stderr. The log always goes to the debugger output too. |
| `log` | `trace` | `G6SHIM_TRACE` | off | Verbose diagnostics: repeated activations and queries, first calls of fast-table entries and draws, packet kinds, topology values. Graphics objects also remember their last 32 calls, and a command list that fails to close logs them. |
| `console` | `model` | `G6SHIM_CONSOLE` | `series-x` | Console model the title sees. `series-s` is the lighter console (7,776 MiB of title memory, Series S adapter memory and analytics). An unknown value is logged and the default used. |
| `window` | `size` | `G6SHIM_WINDOW` | the title's size, fitted to the work area | Client size `widthxheight`, positive, at most 32767. The picture is scaled to it. A forced size is not fitted, and the window keeps its position. |
| `window` | `fullscreen` | `G6SHIM_FULLSCREEN` | off | Borderless fullscreen: the presentation window covers the whole monitor it opens on, including the taskbar. |
| `graphics` | `capture_frames` | `G6SHIM_CAPTURE_FRAMES` | none | Save the back buffer of those presented frames (counted from 1), comma-separated, as `g6shim_frame_<n>.bmp` next to the executable. Works with the window hidden or the desktop locked. |
| `graphics` | `d3d_debug` | `G6SHIM_D3D_DEBUG` | off | Create the device with the Direct3D debug layer; invalid calls appear as debug output. The D3D11 device enables the layer only when the value is `1`. |
| `graphics` | `d3d_dred` | `G6SHIM_D3D_DRED` | off | Record GPU progress and page faults, so the log says where the GPU stopped when the device is removed. The reason for a removal is logged without it. |
| `graphics` | `d3d_warp` | `G6SHIM_D3D_WARP` | the default adapter | Create the host device on the software adapter. |
| `graphics` | `cache_dir` | `G6SHIM_CACHE_DIR` | `%LOCALAPPDATA%\g6shim\cache\<title ID or exe name>` | Folder for translated shaders and compiled host pipelines. |
| `audio` | `capture` | `G6SHIM_AUDIO_CAPTURE` | off | Record what the title plays through WASAPI as a 48 kHz stereo 16-bit WAV, with silence where nothing played. The recorder runs in a helper process and stops with the title; it starts when `AcpHal.dll` is first used. The helper is given the resolved path. |
| `audio` | `xma_dump` | `G6SHIM_XMA_DUMP` | off | Write each XMA voice's output ring from enable to disable as `voice_<index>_<sequence>.wav` at the voice's rate and channel count, and one line per voice in `voices.txt` ([abi/acphal.md](abi/acphal.md)). |
| `input` | `autopress` | `G6SHIM_INPUT_AUTOPRESS` | none | Hold buttons for 0.2 s at those seconds after the first controller read, for example `6:Menu,9:A`. Buttons: `Menu View A B X Y Up Down Left Right`. |
| `storage` | `bcpack_tables` | `G6SHIM_BCPACK_TABLES` | `bcpack-tables.bin` next to `XCRDAPI.dll` | BCPack constant tables for DirectStorage texture decompression. Without a table file BCPack requests fail; zlib and plain requests still run. |
| — | — | `G6SHIM_DXIL` | the newest `Windows Kits\10\bin\<version>\x64\dxil.dll` | Environment only, not read from `g6shim.ini`. `dxil.dll` the shader translator and its tests validate and sign DXIL with. Without one the validator tests are skipped; when the variable is set and the file cannot be loaded, they fail. CI sets it to the Windows SDK's copy. |

```ini
[window]
size = 1920x1080
fullscreen = 1

[graphics]
capture_frames = 300,600
d3d_debug = 1

[audio]
capture = D:\capture.wav
```

## Caches

Translated shaders and compiled host pipelines are kept outside the game folder so the game folder stays read-only:

| Folder | Content |
|---|---|
| `%LOCALAPPDATA%\g6shim\cache\<title ID or exe name>\shaders` | one entry per translated shader container |
| `%LOCALAPPDATA%\g6shim\cache\<title ID or exe name>\pipelines` | the host pipeline library and the list of title pipelines to start early |

`G6SHIM_CACHE_DIR`, or `[graphics] cache_dir`, replaces the `<title ID or exe name>` folder. A changed build, adapter or driver discards the entries it cannot use; deleting the folders is always safe.

## Files next to the executable

| File | Content |
|---|---|
| `g6shim.ini` | Settings. Environment variables override it. |
| `g6shim.log` | The log, appended on every run. |
| `g6shim_input.ini` | Keyboard and controller mapping, written with defaults on first start. XInput controllers need no mapping. |
| `g6shim_saves\<container>\<blob>` | Saves. Files larger than 64 MiB are treated as damaged. |
| `g6shim_persistent\` | Persistent local storage, made on the first request for it and mounted as its own drive on the highest letter of `D:`-`Z:` that neither the host nor another mount uses. The log names the letter. |
| `g6shim_frame_<n>.bmp` | Captured frames. |
| `console_xg_x.dll` | Optional: a copy of the console's own `xg_x.dll` from your dump. When present, texture layouts come from it instead of g6shim's own code. |

## Notes

- The console's game drive `G:\` is the game folder; for a GDK title it is the folder of the executable, and the file forwarders translate it.
- The console's temporary drive is `%TEMP%\g6shim_temp_<process id>_<tick count>`, made on the first request for it and mounted on `T:`, or on the highest free letter when the host or another mount uses `T:`. It is deleted with everything in it when the process exits normally.
- DLLs the game ships for its own WinRT classes are activated from the game folder. Some need the Visual C++ 2012 x64 runtime.
- A window close ends the game. A window larger than the monitor's work area is shrunk to fit it, keeping the title's aspect ratio. The title bar shows the render resolution, the window's client size when it differs, and the frame rate.
- On CPUs that lack the console's AMD-only instructions, the runtime emulates them (`clzero`, `monitorx`/`mwaitx`, `clwb` and SSE4a's `extrq`/`insertq`) instead of stopping at an illegal instruction; [docs/abi/cpu.md](abi/cpu.md) lists the encodings.
