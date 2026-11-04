# 拉風輸入法 Windows 前端

拉風輸入法的 Windows TSF 前端 DLL。此元件負責同程序（in-process）載入的
TSF DLL，並透過專用的 named pipe 與背景服務通訊。

## 建置

從儲存庫根目錄設定並建置完整的 Windows 專案：

```powershell
cmake --preset windows
cmake --build --preset windows
```

開發版產物輸出至：

```text
build/windows/bin/llavon-ime.dll
build/windows/frontend-x86/bin/llavon-ime.dll
```

完整建置與 MSI 會同時包含 x64 和 x86 前端；推論服務、候選字視窗與除錯器維持 x64。
安裝位置分別為 `bin/llavon-ime.dll` 和 `bin/x86/llavon-ime.dll`，使用相同 CLSID
與 profile GUID，分別由 64 位元與 32 位元的 `regsvr32` 註冊。

只開發或驗證 x86 前端時，可略過模型、GPU 與 WinUI 相依項目：

```powershell
cmake --preset windows-x86
cmake --build --preset windows-x86
ctest --preset windows-x86
```

完整建置使用 Visual Studio generator；若現有 x64 build 使用非預設版本，
獨立設定 x86 時也須指定相同的 `-G`。x86 使用 `x86-windows-static`，
靜態連結 C++ runtime 和 JSON 相依項目，不要求宿主另有 x86 VC runtime。
開發版 x86 DLL 會尋找 `build/windows/bin/llavon-ime-service.exe`；
也可透過 `LLAVON_IME_SERVICE_PATH` 指定後端路徑。

CI 會執行兩種架構的 DLL／COM 載入、組字與 Pipe 測試，以及由 x64 Pipe server
啟動 x86 AppContainer client 的跨架構測試。這些檢查不註冊輸入法；
真正的 TSF 載入、輸入與安裝／解除安裝仍需在安裝後進行互動驗證。

兩種前端都從共用的 `bin/../tables` 載入字表；x86 DLL 會先離開 `bin/x86`。
儲存庫 preset 的開發版另可直接讀取 `ime-core/table`。
`frontend-mapping-*` 測試會把測試程式與真實字表放進 x64／x86 安裝和開發目錄，
清除字表環境變數並切換到無關的工作目錄，再輸入完整注音確認漢字候選可用。

## 執行邊界

DLL 在本地處理 TSF 組字、候選字狀態、鍵盤行為與
`ITfCandidateListUIElementBehavior`。預測與輸入模式請求使用
`\\.\pipe\llavon-ime`；可直接繪製的候選字快照則使用獨立的
`\\.\pipe\llavon-ime-candidate-ui`。候選字 HWND 與 XAML island 由背景服務
`llavon-ime-service.exe` 擁有。

SearchHost 等 AppContainer 宿主程序會依據宿主自身的套件相依關係與系統目錄
解析前端 DLL 的相依項目，不會搜尋輸入法安裝目錄。因此前端使用的 triplet
都以靜態方式連結 yyjson（reflectcpp 的 JSON 後端）。前端不得依賴私有的
執行階段 DLL；只要缺少任何相依項目，TSF 就完全無法啟用。

建置 `frontend-dll-load-tests` 後，執行 `ctest --preset windows -R frontend-dll-load`，
即可檢查在只有系統相依項目時前端能否成功載入，並直接呼叫 COM factory 建立實例。
互動切換與實際打字仍需在真正的 AppContainer 宿主中測試。

## 工作列圖示

「中／英」按鈕依工作列主題選用黑字或白字圖示。主題由唯讀的
`HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\SystemUsesLightTheme`
判斷，並監聽該 key 的變更，取得焦點時也會重新檢查；讀不到時視為深色。
設定中的「工作列圖示顏色」可改為固定黑色或白色，供工作列被第三方工具改色的環境使用。
無法連線至後端時改顯示 X，避免看起來像是停在英文模式。
後端不可用且工作佇列空閒時，前端每 500 毫秒重試查詢輸入模式；後端就緒後會自動
恢復「中／英」圖示，不需要先按 Shift 或重新取得焦點。
Windows 11 工作列不會依 `TF_LBI_STYLE_TEXTCOLORICON` 替圖示配色，因此不使用該旗標。

品牌圖示沿用服務的 `service/src/assets/llavon_tray.ico`。工作列只對微軟內建輸入法
依主題重繪品牌圖示，第三方輸入法的圖示會原樣顯示，因此使用白底圖示以適應深淺色。

`input_mode_*_white.ico` 與 `input_mode_*.ico` 為同一字形，修改其中一組時需一併更新另一組。

`TextService` 直接擁有一個來自共用非同步函式庫 `llavon::debug-client`
的 `llavon::debug::Logger`。每個 TSF 宿主程序都會以獨立記錄產生端的身分
連線至 `\\.\pipe\llavon-ime-debugger`。若宿主提供對應的 test 回呼，前端計時
從 `OnTestKeyDown` 開始，否則從 `OnKeyDown` 開始；每筆記錄以 `start`
標示其起點。只有在 `ITfTextEditSink::OnEndEdit` 確認由預測觸發的讀寫
edit session 完成時，才會輸出 `frontend_e2e_ms`。同一筆記錄會將此區間切分為
輸入模式、ready、前文擷取、預測往返、edit session 等待／套用，以及編輯通知
等階段。`edit_apply_ms` 進一步細分為編輯準備、`SetText`、顯示屬性與選取範圍；
`partition_error_ms` 必須維持在趨近於零。
