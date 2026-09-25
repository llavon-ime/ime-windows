# Packaging

The top-level CMake project configures every Windows executable and DLL in one
build graph, then creates a Windows MSI with CPack and WiX. CI additionally
wraps that MSI and a small model download helper in a WiX Burn web installer. The
cross-platform `ime-core` repository remains a submodule, but participates in
this build as a normal static-library target.

## Prerequisites

- CMake 3.31 or newer.
- Visual Studio Build Tools with the C++ workload.
- .NET SDK with `dotnet` available on `PATH`.

CMake restores WiX from NuGet into `build/windows/.wix-tools` before
packaging. It also installs the WiX UI extension into a build-local extension
cache, so no global WiX installation is required.

Settings and the tray settings menu use native WinUI 3 XAML Islands. Candidate
windows continue to use the OS-provided UWP XAML Islands. `cmake/WinUI3.cmake`
uses Microsoft's experimental Windows App SDK CMake targets and the pinned
NuGetCMakePackage helper; package versions and content hashes are recorded in
`cmake/winui3.packages.lock.json`. No handwritten solution/project files or
project-owned PowerShell build scripts are needed. Microsoft's package helper
may internally invoke PowerShell for manifest transformation.

The first configure restores the NuGet packages. WinUI depends on WebView2
metadata for C++ projection generation, but these UIs contain only native XAML
controls and neither instantiate nor deploy a WebView2 browser runtime.
The settings DLL activates its embedded manifest on its own STA and resolves
the native control PRI explicitly. CMake copies and installs the self-contained
Windows App SDK DLLs, PRI files, assets and language resources beside the
service, so users do not need to install a Windows App Runtime separately.

XAML sources live under `service/src/settings/ui/` and are embedded into the
settings DLL as resources. The runtime API and settings callbacks remain the
same. Build just these UI targets with:

```powershell
cmake --build build/windows --config Release --target llavon-ime-settings-ui llavon-ime-candidate-ui --parallel
```

## Choose the Correct Build Command

The build targets produce different artifacts. In particular, the `package`
preset only rebuilds the standalone MSI; it does **not** rebuild an existing
`*-setup.exe` left in the build directory.

Build the binaries without packaging:

```powershell
cmake --preset windows
cmake --build --preset windows --parallel
```

If `build/windows` was previously configured without an explicit Visual Studio
platform, run `cmake --fresh --preset windows` once. The Windows App SDK CMake
targets need `CMAKE_GENERATOR_PLATFORM=x64` to locate their import libraries.

Build the standalone MSI:

```powershell
cmake --preset windows
cmake --build --preset package --parallel
```

Build the web setup that contains the MSI and downloads the default model:

```powershell
cmake --preset windows
cmake --build build/windows --config Release --target llavon-ime-setup --parallel
```

CI supplies a pinned model revision, SHA-256, and size to the setup build. Local
builds resolve the same metadata from Hugging Face when building the setup.

> [!WARNING]
> If a setup build fails, CMake may leave the previously generated
> `*-setup.exe` in `build/windows`. Do not assume that file was refreshed.
> Check its modification time before installing it.

## Build MSI

The standalone MSI contains the application and licenses, but no GGUF model.
Use the web setup for a normal installation. A managed MSI deployment must
provide a GGUF model separately or configure a model path in settings.

```powershell
cmake --preset windows
cmake --build --preset package
```

CUDA support is disabled by default so the standard build does not require a
CUDA Toolkit. To build `ime-core` with the CUDA backend, configure with CUDA
enabled before building:

```powershell
cmake --preset windows -DLLAVON_IME_ENABLE_CUDA=ON
cmake --build --preset package
```

The CUDA-enabled build requires a CUDA Toolkit with `nvcc` available to vcpkg.

The package target performs these steps:

- Builds the frontend DLL, backend executable, UI DLLs, and debugger executable
  from one CMake configuration.
- Links the service directly to the `ime-core` static-library target.
- Restores WiX `4.0.4` from NuGet into the build tree.
- Installs `WixToolset.UI.wixext` into a build-local WiX extension cache.
- Collects vcpkg package license files from the unified manifest installation.
- Includes the model attribution and CC BY-NC 4.0 terms in the MSI license
  agreement and as a separately installed license file.
- Packages `bin`, `tables`, and `licenses` into an x64 per-machine MSI.
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
build/windows/llavon-ime-0.0.1-windows.msi
```

GitHub Actions publishes an Asia/Taipei CalVer in the form
`YYYY.MM.DD.GITHUB_RUN_NUMBER` in the immutable `v<CalVer>` release and
`latest.json`. The MSI and setup use the fixed internal version `0.0.1`.

The `*-setup.exe` bundle embeds the core MSI and a small model download helper.
The helper downloads the GGUF pinned at build time, verifies its size and
SHA-256, and stores it in `%ProgramData%\Llavon IME\models\<revision>`.
It verifies and reuses an existing copy on subsequent installs or updates.
The model is outside the MSI installation tree, so MSI upgrades do not remove
it. CMake embeds only the pinned LoRA Trainer submodule commit and makes no
LoRA release API request. On demand, the training dialog fetches the manifest
from the `commit-<SHA>` release, verifies its commit, and installs the matching
LoRA Trainer ZIP; users can choose CPU,
CUDA, or ROCm when the release provides the corresponding Windows asset.

CI builds one CPU package using loadable ggml CPU backends. At runtime ggml
selects the fastest compatible CPU variant, so AVX2 and AVX-512 systems use the
same MSI.

## Runtime Paths

The installed layout is:

```text
<install-root>/
  bin/
    llavon-ime.dll
    llavon-ime-service.exe
    llavon-ime-debugger.exe
    llavon-ime-candidate-ui.dll
    llavon-ime-settings-ui.dll
    start-llavon-ime-service.vbs
  licenses/
    LICENSE.txt
    MODEL-LICENSE.txt
    ime-core-LICENSE.txt
    THIRD-PARTY-vcpkg-LICENSES.txt
    vcpkg/
      <package>.txt
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
- Verified model selected by `%ProgramData%\Llavon IME\models\current.revision`;
  tables at `bin/../tables`.
- Legacy model at `bin/../models` when the ProgramData marker is unavailable.

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

Candidate presentation uses the separate
`\\.\pipe\llavon-ime-candidate-ui` transport. The service lazily loads
`llavon-ime-candidate-ui.dll`, which owns the package's single candidate HWND
and its dedicated STA thread.
