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
- The model path and inference-device setting are stored in
  `%LOCALAPPDATA%\Llavon IME\settings.json`. The service reads it at process
  startup and translates it into `ime-core::CoreConfig`; model and device
  changes made in the settings window reload the inference core immediately.
- After the model has loaded, the settings window receives the active backend,
  hardware description, device ID, and GPU-offload state reported by
  `ime-core`; this runtime status is kept separate from the next-start setting.
- With supported NVIDIA drivers, the service requests a device-scoped latency
  boost before `Ready` and `Predict`, then releases it after two seconds without
  either request. This keeps GPU clocks from dropping between keystrokes while
  retaining the existing ime-core warmup. It uses the public NVAPI low-latency
  boost hint on an offscreen D3D11 device matched to the inference GPU's PCI
  address; inference still uses the selected backend. No driver profile or
  global power setting is changed, and no background inference work is added.
  Switching model/device or shutting down also releases the hint. Missing or
  unsupported drivers retain ordinary inference; boost errors are logged.
  Performance verification must include 250 ms request spacing and resumption
  after more than two seconds idle, not only continuous token throughput.
- `llavon-ime-candidate-ui.dll` is loaded on the first candidate presentation.
  It owns one dedicated STA thread, one candidate HWND, and its own XAML island.
- Candidate presentation snapshots arrive through the independent
  `\\.\pipe\llavon-ime-candidate-ui` pipe. This transport does not share the
  prediction pipe's connection or protocol.
- Every completed IME composition is sent to the service as raw context,
  committed text, and raw Bopomofo readings. The service converts readings to
  the validation-like `syllable`/`tone` schema and inserts them into SQLite on
  a below-normal-priority writer thread. The prediction `io_context` never
  opens, writes, or flushes the training database.
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
- default model path override: `LLAVON_IME_MODEL_PATH` (the saved setting takes
  precedence during normal startup)
- tables path: `LLAVON_IME_TABLES_DIR`
- collected training database: `%LOCALAPPDATA%\Llavon IME\training-data\commits.sqlite3`
- collected training database path override: `LLAVON_IME_TRAINING_DATABASE_PATH`

Ordinary Bopomofo commits are stored in the SQLite `training_commits` table.
`context`, `answer`, and `padding_json` preserve the same shape as the public
validation set. The `revice` boolean is true when any position in the commit
was manually selected; the original Bopomofo reading remains unchanged.
Mixed commits retain literal characters and incomplete input as `literal` or
`rawReading` entries. Commits with no Bopomofo reading anywhere are discarded,
including pre-existing pending literal-only rows when the service starts. The
`training_state` column is constrained to `pending`, `excluded`, or `trained`.
`event_id`, `event_type`, and nullable `revision_of` are also stored; the last
column reserves space for future typo/backspace/retype correction detection.
During dataset conversion, literal characters use the same fixed character
tokens as inference and provide context for later positions without loss.
Records still need at least one complete, trainable Bopomofo reading; incomplete
`rawReading` entries remain untrainable.

Commits are first held as plaintext only in the service process for ten seconds.
If the frontend reports an immediate Backspace for the same TSF context and the
same latest commit, that staged commit is discarded and never reaches SQLite.
Otherwise the below-normal-priority writer encrypts and inserts it after the
window expires; a following commit in the same collection session confirms and
flushes the preceding one early. Correction detection never decrypts or reads a
stored commit. SQLite uses WAL mode, and selection/state changes run in
transactions, so training-data I/O does not run on the inference path. This
pre-release schema intentionally does not import the former JSONL prototype.

## Local LoRA training

The settings window's LoRA action lists the current `pending` records whenever
the dialog is opened. All records start checked. Starting a run changes every
unchecked pending record to `excluded`; records successfully written into the
trainer dataset change to `trained` only after both training and GGUF export
complete. Records that cannot be converted stay `pending`.
Trainable records with a manual candidate selection contribute three samples;
other records contribute one. The trainer shuffles the combined samples each
epoch when shuffling is enabled (the default). Training history counts the
original records, without the extra samples.

The dialog supplies non-empty defaults derived from
`lora-trainer/docs/step-search-results.md`: rank/alpha 8/16, dropout 0, batch
size and gradient accumulation 1, 5 epochs, max steps `-1`, learning rate `1e-4`,
FP32, target modules `q_proj,v_proj`, no warmup or weight decay, max gradient
norm 1, seed 42, shuffling enabled, and device `auto`. The UI polls service
status for download/training progress, can cancel an active operation, and can
reload the completed `personalized-Q4_K_M.gguf` through the existing model
reload callback.

The base model is fixed to `tony65535/llavon-ime-llama-250m` (CC-BY-NC-4.0).
It is never downloaded implicitly: the user must press the download/update
button. The service pins each download to the repository revision returned by
Hugging Face and stores `config.json`, `ime_vocab.json`, and
`model.safetensors` below:

```text
%LOCALAPPDATA%\Llavon IME\training-assets\tony65535--llavon-ime-llama-250m\<revision>
```

Each run gets its own directory under `training-assets\runs`. Training history
and each run's `adapter_model.safetensors`, `adapter_config.json`, and
`training_state.json` are retained so an earlier adapter can be exported again
with its matching base-model revision. Only the latest completed run's quantized
GGUF is retained for inference after the new model is successfully applied.
Older GGUF files are kept if loading or saving the new model fails. The
intermediate f16 GGUF is removed after export. The service—not
the settings DLL—converts the selected SQLite rows into the numeric JSONL
accepted by the CLI, starts the hidden trainer process, exports the adapter,
and performs every related file operation. The default CLI path matches the
optional installer component:

```text
%ProgramFiles%\Llavon IME\tools\lora\llavon-lora.exe
```

Model checks and downloads use WinRT `Windows.Web.Http` with `co_await` for
network operations. The service's dedicated worker waits only at the outer
boundary. Downloads read the response in bounded chunks and publish progress
to the settings UI; canceling also cancels an in-flight HTTP request.

For development and tests, `LLAVON_IME_LORA_ASSETS_DIR` overrides the asset/run
root and `LLAVON_IME_LORA_CLI_PATH` overrides the trainer executable.

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
