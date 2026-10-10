# Llavon IME Windows Debugger

Standalone multi-process log viewer and producer transport for Llavon IME.
`llavon-ime-debugger.exe` owns the fixed `\\.\pipe\llavon-ime-debugger`
server and accepts concurrent service and TSF frontend producers.

The UI emphasizes frontend end-to-end latency (`frontend_e2e_ms`) from
`OnTestKeyDown` until `ITfTextEditSink::OnEndEdit` confirms completion of the
prediction-triggered read/write edit session, while retaining core inference
latency (`predict_ms`) as a secondary metric.

The debugger shell is declared in `src/debugger/ui/debugger_page.xaml`. Its
root `Pivot` is the page host; additional debugger pages can be added as sibling
`PivotItem` elements without rebuilding the window layout in C++.
The Context page displays the preceding text received by the inference core and
the context reconstructed from the tokens used for that request. Context data
travels through the injected logger as a `LogInformation::context` record;
ordinary diagnostics use `LogInformation::general`.

The Debug messages page receives `LogInformation::debug` records, including
candidate window lifecycle, layout, rendering, and error messages. Its bounded
text history is independent of Diagnostics, so UI tracing does not displace
latency records or change the latency samples.

The candidate UI runtime owns its logger adapter and injects `ime-core::Logger`
into its windows. It stops the logger worker after destroying those windows,
before the candidate UI DLL can unload. Deferred messages capture values rather
than window pointers and format UTF-8 text on the worker.

The internal `llavon::debug-client` static library provides an asynchronous
`llavon::debug::Logger`. Its two `log` overloads accept an existing UTF-8 string
or a lazy message factory. Formatting and pipe writes run on the logger worker;
disconnected or rejected factories are never evaluated.
