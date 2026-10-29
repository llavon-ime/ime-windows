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
```

## 執行邊界

DLL 在本地處理 TSF 組字、候選字狀態、鍵盤行為與
`ITfCandidateListUIElementBehavior`。預測與輸入模式請求使用
`\\.\pipe\llavon-ime`；可直接繪製的候選字快照則使用獨立的
`\\.\pipe\llavon-ime-candidate-ui`。候選字 HWND 與 XAML island 由背景服務
`llavon-ime-service.exe` 擁有。

SearchHost 等 AppContainer 宿主程序會依據宿主自身的套件相依關係與系統目錄
解析前端 DLL 的相依項目，不會搜尋輸入法安裝目錄。因此兩個儲存庫 triplet
都以靜態方式連結 yyjson（reflectcpp 的 JSON 後端）。前端不得依賴私有的
執行階段 DLL；只要缺少任何相依項目，TSF 就完全無法啟用。

建置 `frontend-dll-load-tests` 後，執行 `ctest --preset windows -R frontend-dll-load`，
即可檢查在只有系統相依項目時前端能否成功載入。此測試僅驗證 DLL 載入邊界；
互動切換與實際打字仍需在真正的 AppContainer 宿主中測試。

`TextService` 直接擁有一個來自共用非同步函式庫 `llavon::debug-client`
的 `llavon::debug::Logger`。每個 TSF 宿主程序都會以獨立記錄產生端的身分
連線至 `\\.\pipe\llavon-ime-debugger`。若宿主提供對應的 test 回呼，前端計時
從 `OnTestKeyDown` 開始，否則從 `OnKeyDown` 開始；每筆記錄以 `start`
標示其起點。只有在 `ITfTextEditSink::OnEndEdit` 確認由預測觸發的讀寫
edit session 完成時，才會輸出 `frontend_e2e_ms`。同一筆記錄會將此區間切分為
輸入模式、ready、前文擷取、預測往返、edit session 等待／套用，以及編輯通知
等階段。`edit_apply_ms` 進一步細分為編輯準備、`SetText`、顯示屬性與選取範圍；
`partition_error_ms` 必須維持在趨近於零。
