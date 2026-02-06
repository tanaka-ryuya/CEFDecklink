# CEFDecklink

# CEFDecklink

This application renders HTML/CSS/JS content using **Chromium Embedded Framework (CEF)** and outputs it as **Unmultiplied Fill & Key** signals via **Blackmagic DeckLink** SDI.

It is specifically configured for:
- **Device**: Compatible with **UltraStudio HD Mini** (and similar DeckLink devices).
- **Format**: **1080i59.94** (NTSC).
- **Output**:
  - **SDI A**: Fill Signal
  - **SDI B**: Key Signal

## Features
- **Unmultiplied Keying**: Generates proper Fill and Key signals for broadcast switchers.
- **CEF Integration**: Renders modern web content off-screen.
- **DirectX 11**: Efficient GPU-based texture sharing and color space conversion.
- **External Keying**: Supports hardware external keying modes.
- **Simulator Mode**: Falls back to windowed preview if no DeckLink device is found.

## Prerequisites
- **Windows 10/11** (x64)
- **Visual Studio 2022** (Standard MSVC C++ toolchain)
- **CMake** (3.20 or later)
- **Blackmagic Desktop Video** drivers installed.

## Clone & Build Instructions

### 1. Clone the Repository
```powershell
git clone <repository-url>
cd CEFDecklink
```

### 2. Download Dependencies

**A. Chromium Embedded Framework (CEF)**
1. Download **CEF 132.0.26+gea273c5+chromium-132.0.6834.83 (Windows 64-bit)** (or similar compatible version) from [CEF Builds](https://cef-builds.spotifycdn.com/index.html).
2. Extract the archive.
3. Rename the extracted folder to `cef` and place it in the `vender` directory.
   - Structure: `vender/cef/LICENSE.txt`, `vender/cef/libcef_dll`, etc.

**B. Blackmagic DeckLink SDK**
1. Download **Desktop Video SDK 14.0+** from [Blackmagic Design Developer Website](https://www.blackmagicdesign.com/developer/).
2. Extract the archive.
3. Place the SDK folder in the `vender` directory.
   - Structure: `vender/Blackmagic DeckLink SDK 15.3/Win/include/...`

### 3. Build the Project
We provide a convenient batch script to configure and build the project automatically.

Run `build.bat` from the project root:
```powershell
.\build.bat
```
This will:
1. Locate Visual Studio Build Tools and CMake.
2. Configure the project using CMake (`build` folder).
3. Compile the project in **Release** mode.
4. Copy necessary resources (Shaders, CEF blobs) to the output directory.

### 4. Run the Application
After a successful build, the executable involves resources that must be found relative to it.

```powershell
.\build\Release\DeckLinkDX11.exe
```

## Configuration

The application determines settings (URL, Alpha Threshold) in the following priority order:
1.  **Command Line Arguments**
2.  **`config.json`** (in the same directory as the executable)
3.  **Default Values**

### 1. Command Line Arguments
```powershell
.\DeckLinkDX11.exe --url "http://localhost:3000/cg" --alpha 0.5
```
- `--url`: Specifies the initial URL to load.
- `--alpha`: Sets the initial alpha threshold (0.0 - 1.0).

### 2. config.json
Create a `config.json` file next to `DeckLinkDX11.exe`:
```json
{
    "url": "http://localhost:9090/graphics/on_air.html",
    "alpha": 0.01
}
```

## Controls & Features
- **Project Structure**:
    - `src/`: Source code organized by module (core, render, decklink, cef).
    - `scripts/`: Utility scripts.
    - `docs/`: Documentation and logs.
- **Controls**:
    - **F11**: Toggle Full Screen.
    - **D**: Toggle **Diff Mode** (Visualizes difference between frame T and T+1).
    - **P**: Toggle **Progressive Mode** (High frame rate window output logic).
    - **+ / -**: Adjust the Alpha Threshold (for keying).
    - **Ctrl + C**: Exit the application.

## Troubleshooting
- **"Shader Compile Failed"**: Ensure `shaders` folder exists next to the executable. `build.bat` handles this copy automatically.
- **"Decklink not found"**: Ensure Blackmagic Desktop Video drivers are installed. If no device is found, the app starts in **Simulator Mode**.
- **"Invalid file descriptor to ICU data"**: This happens if you run the executable from the wrong directory.
