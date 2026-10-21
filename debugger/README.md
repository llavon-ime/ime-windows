# 拉風輸入法 Windows 偵錯工具

拉風輸入法的獨立多程序記錄檢視器，以及記錄產生端的傳輸元件。
`llavon-ime-debugger.exe` 擁有固定的 `\\.\pipe\llavon-ime-debugger`
伺服器，可同時接受背景服務與 TSF 前端等多個記錄產生端連線。

介面以前端端對端延遲（`frontend_e2e_ms`）為主要指標，計算範圍從
`OnTestKeyDown` 開始，直到 `ITfTextEditSink::OnEndEdit` 確認由預測觸發的
讀寫 edit session 完成為止；核心推論延遲（`predict_ms`）則保留為次要指標。

偵錯工具的外殼宣告於 `src/debugger/ui/debugger_page.xaml`。其根節點
`Pivot` 為頁面容器；新增偵錯頁面時，只要加入同層的 `PivotItem` 元素即可，
不需要在 C++ 中重建視窗版面。
「Context」頁面會顯示推論核心收到的前文，以及依該次請求所用 token
重建出的前文。前文資料透過注入的 logger 以 `LogInformation::context`
記錄傳送；一般診斷訊息則使用 `LogInformation::general`。

「Debug messages」頁面接收 `LogInformation::debug` 記錄，包含候選字視窗的
生命週期、版面配置、繪製與錯誤訊息。此頁的文字歷史有固定上限，且與
「Diagnostics」頁面彼此獨立，因此 UI 追蹤訊息不會擠掉延遲記錄，也不會影響
延遲樣本。

候選字 UI runtime 擁有自己的 logger 轉接器，並將 `ime-core::Logger`
注入其視窗。它會在銷毀這些視窗之後、候選字 UI DLL 卸載之前停止 logger
工作執行緒。延後處理的訊息會擷取值而非視窗指標，並在工作執行緒上格式化
UTF-8 文字。

內部靜態函式庫 `llavon::debug-client` 提供非同步的
`llavon::debug::Logger`。它的兩個 `log` 多載分別接受既有的 UTF-8 字串，
或延遲求值的訊息工廠函式。格式化與 pipe 寫入都在 logger 工作執行緒上執行；
未連線或遭拒絕時，訊息工廠函式永遠不會被呼叫。
