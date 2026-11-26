# ccc

ccc is a cmd.exe-compatible command shell with its own GPU-drawn terminal window. It runs on Windows, Linux (X11, WSLg) and macOS.

- **Shell.** Batch semantics follow cmd.exe byte for byte: variables and delayed expansion, `if`/`for`/`goto`/`call`, redirection, the
  builtins, and (on Windows) cmd's own messages in the system language. A test suite compares ccc's output with cmd.exe's.
- **Terminal.** A Vulkan renderer (MoltenVK on macOS) with tabs, scrollback, selection, search, links, and xterm-compatible VT emulation:
  truecolor, Kitty keyboard protocol, Kitty graphics, OSC 8/52/133, mouse reporting and synchronized output.
- **Console host.** On Windows, ccc is the console server for the programs it starts (python, vim, ssh, wsl, cmd.exe). No conhost or
  ConPTY is involved. On Linux and macOS, programs run on a pseudo-terminal.

## Platforms

| Platform | Window | Glyphs | Console |
|---|---|---|---|
| Windows 10/11 x64 | Win32 | DirectWrite | own console server (console driver) |
| Linux x64 | X11 / WSLg | FreeType + fontconfig | pty |
| macOS 13+ (Apple silicon, Intel) | Cocoa | CoreText | pty |

Cascadia Mono is bundled as the default font. Any installed monospace font can be chosen with `font` or `--font`.

## Usage

```
ccc                      open the terminal window
ccc /c command           run a command in the current console and exit (like cmd /c)
ccc /k command           run a command, then stay at the prompt
ccc --run "command"      open the window and run a command in the first tab
ccc --font NAME --size N window font and size in points
```

With `/c`, `/k`, `/?`, or redirected input/output, ccc runs as a text shell in the calling console instead of opening a window.

On Linux and macOS, a token that starts with `/` is a switch only when the builtin knows it. Otherwise it is a path, so `dir /s /tmp` and
`cd /d /` work as expected. Programs and `.bat`/`.cmd` files are found on `PATH`.

Window keys: Ctrl+Shift+T/N/W (reopen, new and close tab), Ctrl+Tab, Ctrl+Shift+F (search), Ctrl+wheel or Ctrl+= / Ctrl+- (font size),
Alt+Enter (full screen). On macOS the Command key covers the same actions: Cmd+T/W, Cmd+1..9, Cmd+C/V, Cmd+F and Cmd+=/-.

## Building

The program is one translation unit (`src/main.cpp`).

**Windows.** Needs Visual Studio 2022 or newer and the Vulkan SDK (`VULKAN_SDK` set; the project compiles the shaders with `glslangValidator`).

```
MSBuild ccc.vcxproj /p:Configuration=Release /p:Platform=x64
```

**Linux.** g++ 13 or clang, plus `libvulkan-dev libx11-dev libfreetype-dev libfontconfig-dev glslang-tools xxd`.

```
sh build.sh
```

**macOS.** Xcode command line tools and the LunarG Vulkan SDK (MoltenVK is linked statically, so the app needs no Vulkan loader). The
output is a universal binary.

```
sh build_mac.sh
sh package_mac.sh <version>                        # bin/ccc.app, ad-hoc signed
sh package_mac.sh <version> "<Developer ID>" <AuthKey.p8> <key id> <issuer id>
                                                   # signed, notarized and stapled bin/ccc-<version>-macos.dmg
```

The output goes to `bin/`.

## Tests

| Suite | Command | Checks |
|---|---|---|
| cmd.exe comparison (Windows) | `shell\tests\run_tests.cmd` | batch files run under cmd.exe and ccc, outputs diffed |
| Console server (Windows) | `shell\tests\console\run.ps1 [-Only name]` | headless scripts driving real programs through the console server |
| Portable batch files | `sh shell/tests/linux/run.sh` | `l1`-`l3` against recorded cmd.exe output |
| pty (Linux, macOS) | `bin/ccc --headless shell/tests/linux/pty.txt`, `shell/tests/macos/pty.txt` | controlling terminal, window size, Ctrl+C, full-screen programs |

`ccc --headless script.txt` runs the terminal logic without a window or GPU and prints screens. `ccc --shot file.bmp` writes every drawn
frame to a bitmap.

## Source layout

| Path | Contents |
|---|---|
| `shell/` | the interpreter, VT emulator, scrollback, console server (Windows) and pty host (Linux, macOS), platform layer (`os_*.h`), SIMD helpers (`simd.h`: SSE2/AVX2 and NEON) |
| `src/` | window (`window_win32.h`, `window_x11.h`, `window_cocoa.h`), glyph rasterizers, Vulkan renderer, tabs, input, headless driver |
| `shaders/` | HLSL cell shader, compiled to SPIR-V |
| `fonts/` | Cascadia Mono |
| `icons/` | app icon and the script that draws it |

## License

ccc is released under the MIT License (`LICENSE`). The bundled Cascadia Mono font is licensed under the SIL Open Font License 1.1
(`fonts/OFL.txt`).
