# IME Windows Service

Windows named-pipe service for Llavon IME. Platform-neutral model loading,
tokenization, and llama.cpp inference are supplied by the `ime-core` submodule.

The service also owns the interactive per-user process shell:

- `llavon-ime-service.exe` keeps the named-pipe server on an inference worker
  while its main thread owns the notification-area icon.
- `llavon-ime-settings-ui.dll` is loaded on demand from the executable
  directory. It owns a dedicated STA thread, Win32 message loop, settings HWND,
  and inbox `Windows.UI.Xaml` island.
- Settings UI calls are queued to the DLL's STA thread and never execute XAML
  or model work on the inference worker.
- The inference-device setting is stored in
  `%LOCALAPPDATA%\Llavon IME\settings.json`. The service reads it at process
  startup and translates it into `ime-core::CoreConfig`; changes made in the
  settings window take effect on the next service start.
- After the model has loaded, the settings window receives the active backend,
  hardware description, device ID, and GPU-offload state reported by
  `ime-core`; this runtime status is kept separate from the next-start setting.
- `llavon-ime-candidate-ui.dll` is loaded on the first candidate presentation.
  It owns one dedicated STA thread, one candidate HWND, and its own XAML island.
- Candidate presentation snapshots arrive through the independent
  `\\.\pipe\llavon-ime-candidate-ui` pipe. This transport does not share the
  prediction pipe's connection or protocol.
- The separately built `llavon-ime-debugger.exe` owns the multi-client
  `\\.\pipe\llavon-ime-debugger` server. The tray menu launches the packaged
  executable on demand.
- The service links the shared `llavon::debug-client` producer library and
  adapts it to `ime-core::Logger`. The debugger transport and UI are not built
  from this project.
- Whenever the settings window opens, it automatically compares its embedded
  build identity with the rolling release's `latest.json` manifest. CI builds
  are ordered by build number. Local development builds still show whether
  their commit differs from `latest`, without guessing which commit is newer.
  The HTTP request runs on a settings worker and is canceled after two seconds,
  so a slow GitHub response never blocks the UI thread.

The settings and candidate modules are intentionally separate DLLs rather than
additional executables. They do not share an HWND or STA thread.

Source code is divided by runtime responsibility rather than operating-system
name:

- `src/service/`: resident EXE responsibilities, including prediction IPC,
  tray ownership, and loading UI modules.
- `src/settings/`: the settings DLL, its STA runtime, HWND, and XAML island.
- `src/candidate/`: the candidate UI DLL, its STA runtime, single HWND, and XAML
  island.
- `src/service/debug/`: the thin adapter between `ime-core::Logger` and the
  separately installed `llavon::debug-client` producer.

The service keeps the existing executable and IPC compatibility names:

- executable: `llavon-ime-service.exe`
- settings UI module: `llavon-ime-settings-ui.dll`
- candidate UI module: `llavon-ime-candidate-ui.dll`
- debugger executable: `llavon-ime-debugger.exe`
- named pipe: `\\.\pipe\llavon-ime`
- candidate UI named pipe: `\\.\pipe\llavon-ime-candidate-ui`
- debugger named pipe: `\\.\pipe\llavon-ime-debugger`
- model path: `LLAVON_IME_MODEL_PATH`
- tables path: `LLAVON_IME_TABLES_DIR`

## Build

Configure and build the complete Windows project from the repository root:

```powershell
git submodule update --init --recursive
cmake --preset windows
cmake --build --preset windows
```

The top-level project adds `ime-core` and this component to one CMake build
graph backed by one vcpkg manifest. Vulkan support is enabled by default. CUDA
support is optional and requires configuring with `LLAVON_IME_ENABLE_CUDA=ON`.
