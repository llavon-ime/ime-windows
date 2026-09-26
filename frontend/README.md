# IME Windows Frontend

Windows TSF frontend DLL for Llavon IME. This component owns the in-process
TSF DLL and talks to the backend service over dedicated named pipes.

## Build

Configure and build the complete Windows project from the repository root:

```powershell
cmake --preset windows
cmake --build --preset windows
```

The development output is written to:

```text
build/windows/bin/llavon-ime.dll
```

## Runtime Boundary

The DLL keeps TSF composition, candidate state, keyboard behavior, and
`ITfCandidateListUIElementBehavior` local. Prediction/input-mode requests use
`\\.\pipe\llavon-ime`; render-ready candidate snapshots use the independent
`\\.\pipe\llavon-ime-candidate-ui`. The candidate HWND and XAML island are
owned by `llavon-ime-candidate-ui.dll` in the service process.

AppContainer hosts such as SearchHost resolve the frontend's DLL dependencies
using the host's package graph and system directories. They do not search the
IME installation directory. Both repository triplets therefore link yyjson
(reflectcpp's JSON backend) statically. Keep private runtime DLL dependencies
out of the frontend; a missing dependency prevents TSF activation entirely.

Build `frontend-dll-load-tests`, then run `ctest --preset windows -R frontend-dll-load`
to check that the frontend loads when only system dependencies are available.
This checks the DLL loading boundary; interactive switching and typing still
need testing in an actual AppContainer host.

`TextService` directly owns a `llavon::debug::Logger` from the shared asynchronous
`llavon::debug-client` library. Every TSF host process connects as an independent
producer to `\\.\pipe\llavon-ime-debugger`. Frontend timing starts at
`OnTestKeyDown` when the host supplies the matching test callback, otherwise at
`OnKeyDown`. Each record identifies that boundary with `start`. It is emitted
as `frontend_e2e_ms` only when
`ITfTextEditSink::OnEndEdit` confirms completion of the prediction-triggered
read/write edit session. The same record partitions that interval into input
mode, ready, pre-context, prediction round-trip, edit-session wait/application,
and edit notification stages. `edit_apply_ms` is further split across edit
preparation, `SetText`, display attributes, and selection; `partition_error_ms`
must remain approximately zero.
