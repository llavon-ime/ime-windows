# IME Windows Service

Windows named-pipe service for Llavon IME. Platform-neutral model loading,
tokenization, and llama.cpp inference are supplied by the `ime-core` submodule.

The service also owns the interactive per-user process shell:

- `llavon-ime-service.exe` keeps the named-pipe server on an inference worker
  while its main thread owns the notification-area icon.
- `llavon-ime-settings-ui.dll` is loaded on demand from the executable
  directory. It owns the settings HWND and WinUI 3 `Microsoft.UI.Xaml` island.
- Settings UI calls are queued to the shared WinUI STA and never execute XAML
  or model work on the inference worker.
- The model path and inference-device setting are stored in
  `%LOCALAPPDATA%\Llavon IME\settings.json`. The service reads it at process
  startup and translates it into `ime-core::CoreConfig`; model and device
  changes made in the settings window reload the inference core immediately.
- After the model has loaded, the settings window receives the active backend,
  hardware description, device ID, and GPU-offload state reported by
  `ime-core`; this runtime status is kept separate from the next-start setting.
- GPU Boost is enabled by default and can be turned off in the settings window.
  With supported NVIDIA or AMD drivers, the service enables it before `Ready`
  and `Predict`, then releases it after two seconds without either request.
  Turning the setting off stops renewing an active boost, so it expires at the
  usual two-second deadline; the choice persists across service restarts.
  NVIDIA uses the NVAPI low-latency hint on an offscreen D3D11
  device matched to the inference GPU's PCI address. AMD dynamically resolves
  [`clSetDeviceClockModeAMD`](https://github.com/ROCm/clr/blob/develop/opencl/amdocl/cl_profile_amd.h)
  from the system OpenCL driver and requests `Peak` while the lease is active,
  then `Default` on idle, model/device replacement, or shutdown. The OpenCL
  device must match both AMD's vendor ID and the inference GPU's PCI address.
  If the inference backend supplies only `VulkanN` (as on the Radeon 860M),
  the service resolves a uniquely named Vulkan physical device, obtains its
  Windows LUID, and queries that DXGI adapter's PCI address. Duplicate names,
  missing LUIDs, and ambiguous identities are rejected; Vulkan and DXGI
  enumeration indices are never assumed to match.
  This is a device-wide driver power-mode request, subject to power and thermal
  limits; it can raise memory clocks, power consumption, temperature, and fan
  speed. No OpenCL context, queue, kernel, keepalive workload, manual frequency
  target, overclock, or registry write is used. Inference remains on Vulkan.
  This AMD profiling extension is optional, not a universal consumer low-latency
  API. Missing entry points, topology information, or driver support disable
  boost without disabling inference. A successful read-only query establishes
  availability; only a successful `Peak` request logs `AMD GPU boost applied`.
  Driver output units and freshness vary, so those fields are not interpreted
  as live clocks, MHz, or ratios. Verify actual clocks with independent telemetry.
  Temporary enable failures can retry on later input, at most once per second.
  Failed enable requests are followed by `Default` before allowing another try.
  A hidden instance of the service executable owns the AMD driver calls and
  watches an inherited handle to the parent service. It runs before singleton,
  model, or UI initialization. Thus terminating the parent still causes the
  surviving worker to release Peak. IPC uses unnamed events and shared memory
  with an explicit inherited-handle list; no public command pipe is exposed.
  The worker retries restoration up to 30 times independently of the parent.
  A per-device mutex prevents a replacement worker from enabling Peak while
  its predecessor still owns restoration. IPC timeouts suspend further boosts
  and tell the worker to restore; driver waits on the inference thread are bounded.
  This cannot guarantee restoration if the driver hangs, restoration repeatedly
  fails, or the worker itself (including the whole process tree) is forcibly
  terminated. The API restores Default, not a snapshot of another profiling
  application's mode; avoid simultaneous external clock-mode controllers.
  `AMD GPU boost restored` records a successful Default acknowledgement.
  Performance verification must include 250 ms request spacing and resumption
  after more than two seconds idle, not only continuous token throughput.
- `llavon-ime-candidate-ui.dll` is loaded on the first candidate presentation.
  It owns one candidate HWND and its own WinUI 3 XAML island on the shared STA.
  Its runtime owns an asynchronous logger adapter, injected into the candidate
  windows through `ime-core::Logger`; UI traces appear in the debugger's
  **Debug messages** page as `LogInformation::debug` records.
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
- Downloading and installing an update starts only after the user clicks
  **Update now**. The release manifest supplies the setup URL, byte size, and
  SHA-256; the settings worker verifies the download before starting the WiX
  setup silently with a UAC prompt for per-machine installation. The MSI keeps
  its fixed version so local and published builds can replace each other. The
  MSI releases the backend and TSF processor before checking for files in use;
  other apps are left running. Windows may need a restart if an app still holds
  the old TSF DLL.

The settings and candidate modules are separate DLLs with separate HWNDs.
`llavon-ime-ui-runtime.dll` owns their common STA, Win32 message loop, dispatcher,
and process-wide WinUI Application. Each UI client acquires a runtime reference;
closing one client leaves the other alive. The framework stays available for
reopening clients until the service exits, when the host shuts down the runtime
after both clients have closed.

Source code is divided by runtime responsibility rather than operating-system
name:

- `src/service/`: resident EXE responsibilities, including prediction IPC,
  tray ownership, and loading UI modules.
- `src/ui/`: the shared WinUI STA runtime and XAML resource helpers.
- `src/settings/`: the settings DLL, its UI client, HWND, and XAML island.
- `src/candidate/`: the candidate UI DLL, its UI client, single HWND, and XAML
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
During dataset conversion, ime-core supplies the same tokens and candidates as
inference. Literal characters provide context for later positions without loss.
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

## AMD laptop GPU Boost verification

1. In settings, select the AMD Vulkan inference device and confirm that the
   active runtime reports GPU offload. Enabling GPU Boost while inference runs
   on the CPU does not exercise the AMD backend.
2. To capture driver diagnostics, exit the existing service from its tray menu,
   then launch the new build from PowerShell in its `bin` directory:
   `./llavon-ime-service.exe 2> amd-boost.log`.
   If automatic startup wins the race, the log reports `already running`;
   repeat after exiting that instance. The hidden instance with the internal
   `--amd-gpu-boost-worker` argument is the clock owner, not a second IME service.
3. Type several characters about 250 ms apart, pause for at least five seconds,
   then resume. Look for `AMD GPU boost applied` followed by
   `AMD GPU boost restored`. These messages are logged once per backend;
   a successful driver call does not prove a latency improvement.
4. Compare GPU Boost enabled and disabled, especially the first prediction
   after idle. Also check switching models/devices and exiting normally.
   Monitor observed GPU clocks and power separately if available.
5. For crash recovery testing, terminate only the parent service while Peak is
   active and independently verify that the surviving worker restores Default.
   Killing the entire process tree also kills the restoration worker.
6. If the log reports the clock-mode worker unavailable, record the GPU name and
   driver version. Inference should continue. Vulkan support alone does not
   imply support for AMD's optional OpenCL clock-mode extension.

`amd-gpu-boost-worker-tests` exercises the actual child-process/IPC lifecycle
with a fake driver, including parent termination, delayed calls, and failed
restoration. It never changes hardware clocks. Hardware validation must also
check Peak/Default on the target driver and measure actual candidate latency.
On a Radeon 860M with Windows driver 32.0.22032.6002, Peak/Default requests were
accepted. In a local benchmark that excluded the warmup block, with a full
250 ms idle gap after each completed prediction, 56 samples per
setting measured 9.227 ms median with boost off and 9.119 ms on; P95 was
15.668 ms off and 15.857 ms on. Actual idle gaps were 250.022–250.530 ms.
This small mixed difference does not establish a consistent latency benefit.
These are warm-request statistics, not a bound on interactive latency. An
earlier raw benchmark recorded a 473.767 ms request in the excluded warmup
block. The live service also recorded 603.283 ms and 764.094 ms predictions
among otherwise 3–15 ms requests, with almost all of each spike inside the
cache-alignment `llama_decode()` call. Backend instrumentation reproduced
these stalls in lazy Vulkan matrix pipeline creation. The idle `ready()`
warmup decodes one token in a separate context and does not prepare the
small-batch variants needed by actual input.

The core now prepares Vulkan inference pipelines once during model loading,
before serving requests. It covers small batches, larger matrix kernels,
and populated attention caches, clears the synthetic KV state, and retains
the prepared context for the first session. Without a compatible disk cache,
this adds roughly 4–5 seconds on the tested 860M, without periodic GPU work or changing
the actual inference batch size. CUDA and CPU initialization are unchanged.
Validation must include every first-use request, new sessions, 250 ms idle
gaps, and longer idle periods. Long-context compute time is still workload
dependent; the driver clock hint alone did not remove the reproduced stalls.

The service explicitly passes `%LOCALAPPDATA%\Llavon IME\vulkan-cache` to
ime-core. The cross-platform ggml Vulkan backend does not assume a path and
leaves persistent caching disabled when its host supplies none. Subsequent
starts reuse compatible data while still preparing the model and its inference
contexts.
Cache identities include the GPU, driver version, Vulkan cache UUID and
backend schema. Payloads have a checksum and are replaced atomically.
Missing, corrupt or incompatible data is rebuilt; a cache I/O failure does
not prevent inference. The core saves immediately after model preparation,
so normal service shutdown is not required to preserve that work. There are
no cache writes on the prediction path and no registry changes.
`[VK_CACHE] loaded`, `miss`, and `saved` distinguish these startup outcomes.
See `ime-core/vcpkg-ports/README.md` for the pinned ggml fork and diagnostics.

The clock-mode query also retained Peak-looking values after Default succeeded;
independent ADL telemetry returned to low idle clocks. Neither API success nor
its clock-query output proves an actual frequency or latency improvement.

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

The optional train-until-remembered mode re-evaluates every manually selected
training target after each epoch and repeats until every target wins within its
candidate set. The mode rejects conflicting targets for an identical causal
input prefix and is unavailable for the ultra-low preset.

The independent incorrect-only mode also re-evaluates all training targets but
computes the next epoch's loss only for targets that failed validation. It keeps
the configured epoch limit unless train-until-remembered is enabled separately.

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
and performs every related file operation. The fallback CLI path for an older
installation is:

```text
%ProgramFiles%\Llavon IME\tools\lora\llavon-lora.exe
```

The pinned LoRA Trainer submodule commit is embedded during CMake configure.
The training dialog requests its `commit-<SHA>` manifest directly and checks
the manifest `commit` against the pinned submodule. It shows the installed
version and CPU, CUDA, and ROCm choices. Download is enabled when that exact
release contains the corresponding Windows asset. The automatic upstream
release currently contains Windows CPU; CUDA and ROCm require matching Windows
assets in that release. An in-app install downloads the selected ZIP, verifies
its size and SHA-256, checks the archive paths, then extracts and validates its
installed manifest. It installs under
`%LocalAppData%\Llavon IME\tools\lora\<version>\<asset>` and records the active
selection in `current.install`. The service uses that selection before the
Program Files installation.

Model checks and downloads use WinRT `Windows.Web.Http` with `co_await` for
network operations. The service's dedicated worker waits only at the outer
boundary. Downloads read the response in bounded chunks and publish progress
to the settings UI; canceling also cancels an in-flight HTTP request.

For development and tests, `LLAVON_IME_LORA_ASSETS_DIR` overrides the asset/run
root and `LLAVON_IME_LORA_CLI_PATH` overrides the trainer executable.

## Build

The candidate and settings DLLs both host WinUI 3 `DesktopWindowXamlSource`
islands on one shared STA thread. They share the activation, dispatcher, XAML
metadata, and embedded-resource helpers in `src/ui`. Neither UI runs inside
the application's TSF thread.

Candidate UI changes belong in `src/candidate/ui/candidate_page.xaml`
(columns and footer), `candidate_item.xaml` (each candidate row), and
`theme.xaml` (fonts, column widths, light/dark/high-contrast brushes). These
files are embedded as RCDATA by `candidate_ui.rc`; rebuild the DLL after
editing them. C++ updates candidate text, selection, numbering, and page state;
the popup measures the XAML tree to obtain its native size in the current DPI.

Configure and build the complete Windows project from the repository root:

```powershell
git submodule update --init --recursive
cmake --preset windows
cmake --build --preset windows
```

The top-level project adds `ime-core` and this component to one CMake build
graph backed by one vcpkg manifest. Vulkan support is enabled by default. CUDA
support is optional and requires configuring with `LLAVON_IME_ENABLE_CUDA=ON`.

On an interactive Windows desktop, enable the optional candidate/settings
integration tests to check both startup orders, non-activating presentation,
column sizing, monitor-edge placement, hide/show, and UI client restart:

```powershell
cmake --preset windows -DLLAVON_IME_ENABLE_UI_TESTS=ON
cmake --build build/windows --config Release --target candidate-ui-tests candidate-pipe-protocol-tests
ctest --test-dir build/windows -C Release -R 'candidate-(ui|pipe-protocol)' --output-on-failure
```
