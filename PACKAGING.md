# Packaging

The top-level CMake project builds the backend and frontend as separate child
projects, downloads the packaged model, then creates a Windows MSI with CPack
and WiX.

## Prerequisites

- CMake 3.30 or newer.
- Visual Studio Build Tools with the C++ workload.
- .NET SDK with `dotnet` available on `PATH`.

CMake restores WiX from NuGet into `build/windows/.wix-tools` before
packaging. It also installs the WiX UI extension into a build-local extension
cache, so no global WiX installation is required.

## Build MSI

```powershell
cmake --preset windows
cmake --build --preset package
```

The package target performs these steps:

- Builds and installs `ime-service` into `dist/ime-service`.
- Builds and installs `ime-windows-frontend` into `dist/ime-windows-frontend`.
- Restores WiX `4.0.4` from NuGet into the build tree.
- Installs `WixToolset.UI.wixext` into a build-local WiX extension cache.
- Collects vcpkg package license files from both child projects.
- Includes the model attribution and CC BY-NC 4.0 terms in the MSI license
  agreement and as a separately installed license file.
- Downloads `llavon-ime-llama-250m-Q4_K_M.gguf` from the hard-coded Hugging Face URL.
- Packages `bin`, `tables`, `models`, and `licenses` into an x64 per-machine MSI.
- Registers `llavon-ime.dll` with `regsvr32` during install and unregisters it during uninstall.
- Adds a per-machine startup entry for the backend service and removes it during uninstall.
- Adds a Start menu uninstall shortcut that invokes Windows Installer for the
  installed product, preserving the same elevated TSF unregistration and file
  removal sequence as uninstalling from Windows Settings or maintenance mode.
- Removes files left in the known installation tree by older builds or runtime
  diagnostics, then removes the empty installation directories during uninstall.
- The frontend also starts the backend on demand if the named pipe is not available.

The MSI is written under:

```text
build/windows/llavon-ime-0.1.0-windows.msi
```

## Runtime Paths

The installed layout is:

```text
<install-root>/
  bin/
    llavon-ime.dll
    llavon-ime-service.exe
    start-llavon-ime-service.vbs
  models/
    llavon-ime-llama-250m-Q4_K_M.gguf
  licenses/
    LICENSE.txt
    MODEL-LICENSE.txt
    THIRD-PARTY-vcpkg-LICENSES.txt
    vcpkg/
      ime-service/
      ime-windows-frontend/
  tables/
    bopomofo_char.json
    tokens/
      bpmf.json
      chars.json
      latin.json
      special_tokens.json
```

The backend service resolves paths in this order:

- Explicit command line: `llavon-ime-service.exe <model-path> <tables-dir>`.
- Environment: `LLAVON_IME_MODEL_PATH` and `LLAVON_IME_TABLES_DIR`.
- Installed layout relative to the executable: `bin/../models` and `bin/../tables`.

The frontend DLL resolves `bopomofo_char.json` in this order:

- Environment: `LLAVON_IME_TABLES_DIR`.
- Installed layout relative to the DLL: `bin/../tables`.
- A source-location fallback for development builds.

When the frontend cannot connect to `\\.\pipe\llavon-ime`, it attempts to start
the backend with no arguments, then retries the pipe briefly. The backend
executable is resolved from `LLAVON_IME_SERVICE_PATH` first, then from
`llavon-ime-service.exe` next to the frontend DLL.

This keeps development builds and MSI deployments independent of the current
working directory.
