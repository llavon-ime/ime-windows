# Packaging

The top-level CMake project configures every Windows executable and DLL in one
build graph, obtains the latest packaged model, then creates a Windows MSI with
CPack and WiX. The cross-platform `ime-core` repository remains a submodule,
but participates in this build as a normal static-library target.

## Prerequisites

- CMake 3.30 or newer.
- Visual Studio Build Tools with the C++ workload.
- .NET SDK with `dotnet` available on `PATH`.

CMake restores WiX from NuGet into `build/windows/.wix-tools` before
packaging. It also installs the WiX UI extension into a build-local extension
cache, so no global WiX installation is required.

## Build MSI

For local packaging, CMake resolves the current Hugging Face `main` revision
and downloads the model when that revision differs from the recorded local
revision. CMake does not perform SHA-256 verification. The GitHub Actions
workflow separately handles cache invalidation and SHA-256 verification for CI.

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
- Locally, resolves the current Hugging Face revision and downloads the model
  when it changes. In CI, keys the model cache by its LFS SHA-256 and verifies
  the downloaded file before CMake runs.
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
  models/
    llavon-ime-llama-250m-Q4_K_M.gguf
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

Candidate presentation uses the separate
`\\.\pipe\llavon-ime-candidate-ui` transport. The service lazily loads
`llavon-ime-candidate-ui.dll`, which owns the package's single candidate HWND
and its dedicated STA thread.
