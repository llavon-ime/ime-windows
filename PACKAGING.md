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
- Downloads `llavon-ime-llama-250m-Q4_K_M.gguf` from the hard-coded Hugging Face URL.
- Packages `bin`, `tables`, and `models` into an x64 per-machine MSI.
- Registers `llavon-ime.dll` with `regsvr32` during install and unregisters it during uninstall.

The MSI is written under:

```text
build/windows/llavon-ime-0.1.0-windows.msi
```
