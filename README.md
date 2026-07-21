# CEFDecklink

[日本語版 (Japanese Version) はこちら](README_ja.md)

CEFDecklink is a lightweight, single-purpose, highly stable, and broadcast-quality software. It captures web pages rendered with an embedded browser (CEF) and outputs them via SDI as separate **"straight alpha Fill and Key"** signals at an output rate of 1080i59.94 or 1080i50.

(Internally, it uses the Chromium Embedded Framework to render HTML/CSS/JS contents off-screen, and generates precise Unmultiplied signals for hardware switchers via Blackmagic DeckLink devices.)

It is configured specifically for the following setup:
- **Devices**: Compatible with **UltraStudio HD Mini** (and similar DeckLink devices).
- **Format**: **1080i59.94** (NTSC) / **1080i50** (PAL).
- **Output**:
  - **SDI A**: Fill Signal
  - **SDI B**: Key Signal

## Features
- **Unmultiplied Keying**: Generates proper Fill/Key signals with straight alpha channels for correct composting on broadcast switchers.
- **CEF Integration**: Renders the latest Web contents (HTML5, WebGL, CSS, JS).
- **DirectX 11**: Implements efficient GPU-based texture sharing and color space conversions.
- **60fps Free-Run CEF**: Renders via CEF's autonomous timer (60fps/50fps) and synchronizes with 29.97fps/25fps hardware outputs using an asynchronous, lock-free queue. This prevents HTML animation stuttering and juddering when static.
- **External Keying**: Supports hardware external keying mode.
- **Simulator Mode**: Automatically falls back to a desktop preview window if no DeckLink device is found.

---

## Development Prerequisites (Requirements)

Requirements for building the project and reconstructing the environment:

1. **Windows 10 or 11 (x64)**
2. **Visual Studio 2022** (Standard MSVC C++ toolchain)
   - Ensure the `Desktop development with C++` workload is selected during installation.
   - Verify that `C++ CMake tools for Windows` and `Windows 11 SDK` (or 10) are installed via "Individual components".
3. **Blackmagic Desktop Video Driver**
   - Must be installed on the PC where execution/testing is carried out.

---

## Cloning and Dependency Setup

To compile the project successfully, it is **absolutely critical** that dependencies (CEF and DeckLink SDK) are placed in the `vender` directory with the exact folder names specified below.

### 1. Clone the Repository
```powershell
git clone <repository-url>
cd CEFDecklink
```

### 2. Manual Placement of External Libraries (under `vender/`)

Place the following two components in the `vender` directory at the project root. The CMake configuration will fail if the files are missing or folders are named incorrectly.

**A. Chromium Embedded Framework (CEF)**
1. Download the **Windows 64-bit** **Standard Distribution** (e.g., `CEF 132.0.26+gea273c5+chromium-132.0.6834.83`) (tar.bz2 format) from [CEF Builds](https://cef-builds.spotifycdn.com/index.html).
2. Extract the archive. (Use tools like 7-Zip if standard Windows extract fails).
3. Rename the extracted folder (e.g. `cef_binary_...`) to exactly `cef`.
4. Place it so the path becomes `vender/cef`.

**B. Blackmagic DeckLink SDK**
1. Download **Desktop Video SDK 15.3** (or 14.0+) from the [Blackmagic Design Developer Website](https://www.blackmagicdesign.com/developer/).
2. Extract the archive.
3. Place the SDK folder directly into the `vender` directory.

#### Required Directory Structure
Verify that your directory tree looks like this:
```text
CEFDecklink/
 ├─ vender/
 │   ├─ Blackmagic DeckLink SDK 15.3/   <-- Matches this exact name (including version)
 │   │   └─ Win/
 │   │       └─ include/
 │   │           └─ DeckLinkAPI.idl     <-- Looked up by CMake
 │   └─ cef/                            <-- Renamed from cef_binary_...
 │       ├─ cmake/
 │       └─ CMakeLists.txt              <-- Looked up by CMake
 ├─ src/
 ├─ CMakeLists.txt
 ├─ build.bat
 └─ ...
```

---

## Project Build Instructions

Once requirements are met and folders are prepared, you can build the project by simply running the included batch script.

Run the following in Command Prompt or PowerShell:
```powershell
.\build.bat
```

**What `build.bat` does automatically:**
1. Detects paths for Visual Studio 2022 `MSBuild.exe` and `cmake.exe`.
2. Generates the CMake project in `cmake -S . -B build`.
3. Builds the Release configuration using `cmake --build build --config Release`.
4. Copies necessary resources (CEF binaries, shaders, and `config.json`) to the `build\Release` directory.

---

## Execution and Deployment

### About Security Warnings
- **SmartScreen Alert:** Since this is a privately developed software, the executable is not digitally signed with a code signing certificate. On the first run, Windows Defender SmartScreen might block it with a blue warning screen. Click **"More info"** and then select **"Run anyway"** to launch.
- **Firewall Alert:** Since the internal browser (CEF) opens a port for Developer Tools (DevTools) access, a Windows Firewall alert may pop up. If you do not plan to use DevTools, you can select **"Cancel (Do not allow)"**; the SDI transmission will function normally.

### Portable (ZIP) Deployment (Recommended)
This application is distributed as a portable folder and does not use an installer.
To deploy to other PCs, simply compress the built `build/Release` folder into a ZIP archive, copy it, and extract it to any directory (user directories with write permissions are recommended) on the target PC.

### Direct Execution from Build Folder
After a successful build using the script, you can run the executable directly from:
```powershell
.\build\Release\DeckLinkDX11.exe
```
*Note: Make sure `libcef.dll`, the `shaders` folder, and `config.json` are present alongside the executable (copied automatically by `build.bat`).*

---

## Application Configuration

The startup URL and alpha threshold are resolved in the following priority:
1. **Command Line Arguments**
2. **`config.json`** (in the executable directory)
3. **Default Values**

### 1. Command Line Arguments
```powershell
.\DeckLinkDX11.exe --url "http://localhost:3000/cg" --unmult_thresh 0.5 --il_filter_mode 1
```
- `--url`: Specifies the initial URL to load.
- `--unmult_thresh`: Sets the initial Unmultiply alpha threshold (0.0 - 1.0).
- `--il_filter_mode`: Sets the vertical low-pass filter mode (0: None, 1: 3-tap, 2: 5-tap).

### 2. config.json
Specify persistent settings in `config.json` next to `DeckLinkDX11.exe` (or in the deployed folder):
```json
{
    "url": "https://example.com",
    "unmult_thresh": 0.0,
    "format": "5994i",
    "il_filter_mode": 1
}
```
- `url`: The web page to render (default: "https://example.com").
- `unmult_thresh`: The threshold for unmultiplied processing (default: 0.0, perfectly straight alpha).
- `format`: SDI output format. Supported values: "5994i" or "50i" (default: "5994i").
- `il_filter_mode`: Interlace vertical filter mode (0=None, 1=3-tap, 2=5-tap) (default: 1).


## Operations and Controls (Console TUI Shortcuts)

This application displays live status updates using a console-based TUI (Text User Interface).
With focus on the console window, you can use the following shortcuts:

### Windows Shortcuts
- **`Ctrl + O`** : **Cycle View Mode** (0: Interlace Standard / 1: Diff difference visualization / 2: Progressive / 3: 30p Blend)
- **`Ctrl + F`** : **Cycle Vertical LPF (Low Pass Filter) Mode** (None / 3-tap LPF / 5-tap LPF)
- **`Ctrl + K`** : **Toggle Keyer Mode** (Internal / External)
- **`Ctrl + A` / `Ctrl + Z`** : Fine-tune Unmult Alpha Threshold (+0.001 / -0.001)
- **`Ctrl + Up` / `Ctrl + Down`** : Coarse-tune Unmult Alpha Threshold (+0.1 / -0.1)
- **`Ctrl + R`** : Force reload page (ignoring cache)
- **`Ctrl + C`** : Safe exit

### macOS Shortcuts
- **`Ctrl + P`** : **Cycle View Mode** (0: Interlace Standard / 1: Diff difference visualization / 2: Progressive / 3: 30p Blend)
- **`Ctrl + F`** : **Cycle Vertical LPF Mode** (None / 3-tap LPF / 5-tap LPF)
- **`Ctrl + K`** : **Toggle Keyer Mode** (Internal / External)
- **`<` / `>`** (Shift+`,`/`.`): Fine-tune Unmult Alpha Threshold (+0.001 / -0.001)
- **`Ctrl + [` / `Ctrl + ]`** (or `Ctrl + Up` / `Ctrl + Down`): Coarse-tune Unmult Alpha Threshold (+0.1 / -0.1)
- **`Ctrl + R`** : Force reload page (ignoring cache)
- **`Ctrl + C`** : Safe exit

---
*Note: Press **`F11`** while the preview window is focused to toggle fullscreen.*

## Troubleshooting

- **Build Error: "CMake Error at ... include(cef_variables)"**
  → Check if the CEF directory is renamed to exactly `cef` under `vender/`.
- **Build Error: "DeckLink SDK not found"**
  → Verify the path to `vender/Blackmagic DeckLink SDK 15.3` and check if `DeckLinkAPI.idl` is located in the proper subdirectory.
- **Startup Error: "Shader Compile Failed"**
  → Verify that the `shaders` folder is copied alongside the executable. If missing, manually copy `src/render/shaders`.
- **Startup Message: "Decklink not found"**
  → The DeckLink driver is not installed or no compatible card was found. The application will automatically boot in **Simulator Mode** (desktop window preview).
- **Startup Crash: "Invalid file descriptor to ICU data"**
  → CEF library files (like `libcef.dll`) are missing from the executable directory. Make sure the build script completed without errors.

---

## Documentation & References

Reference manuals, simulators, and Japanese/English internal specifications for the project:

### Detailed Data Processing Flow Specification
*   **[Japanese: data_processing_flow.md](docs/data_processing_flow.md)**
*   **[English: data_processing_flow_en.md](docs/data_processing_flow_en.md)**
    *   Covers internal details on CEF-to-DeckLink inter-thread synchronization, timestamp-based buffering, Compute Shader interlace weaving, LPFs, and Unpremultiply processing.

### Pipeline Simulator
*   **[Japanese: pipeline_simulator.html](docs/pipeline_simulator.html)**
*   **[English: pipeline_simulator_en.html](docs/pipeline_simulator_en.html)**
    *   Interactive simulator demonstrating CEF free-run (60fps) to DeckLink (59.94i) frame buffering, drop/duplicate behavior, Compute Shader blending, LPFs, and Unpremultiply math down to the pixel level.
