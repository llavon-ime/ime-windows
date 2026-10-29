# 拉風輸入法（Windows）

拉風輸入法（Llavon IME）是一套專為繁體中文注音輸入打造的輸入法。它使用約 2.5 億參數的語言模型，根據前文與注音內容預測更合適的文字，同時確保候選字符合輸入的讀音。

所有輸入內容與模型推論都在電腦本機完成，不需要連線至雲端，也不會為了選字而上傳正在輸入的文字。

> [!IMPORTANT]
> 本專案仍在早期開發階段，功能、操作方式及安裝流程都可能變動。目前僅支援 x64 版本的 Windows 10 或更新版本。

## 專案特點

- **理解上下文的選字**：依據前後文、注音序列與已選文字預測候選字，不只依靠固定詞頻。
- **專為注音輸入訓練**：採用專用繁體中文注音模型，而非將一般聊天模型直接接到傳統輸入法。
- **完全本機運作**：模型與推論引擎皆在本機執行，輸入內容無須傳送至雲端。
- **熟悉的操作方式**：使用標準注音鍵盤配置，選字操作力求與微軟注音相似。
- **可自訂常用名字**：可在設定中加入名字及其注音，改善人名輸入結果。
- **推論裝置設定**：可查看目前使用的推論裝置，並依電腦環境選擇自動(最高效能)、CPU 或指定的 GPU 後端。
- **內建更新檢查**：可從設定視窗檢查是否有新的測試版本。
- **精美UI**: 使用 WinUI (WinUI3) 作為 GUI 框架，力求符合 M$ Windows 系統原生 UI 風格

## 安裝

1. 前往 [最新版本下載頁面](https://github.com/llavon-ime/ime-windows/releases/tag/latest)。
2. 一般使用者下載 `*-setup.exe`；離線或系統管理部署可改用 `.msi`。
3. 執行安裝程式並依畫面指示完成安裝。安裝程式會下載並驗證預設模型；更新時若
   電腦已有相同且完整的模型，就會略過下載。這是全系統安裝，Windows 可能會
   要求系統管理員權限。
4. 安裝完成後，按 `Windows 鍵 + 空白鍵`，或點選工作列右下角的輸入法選單，切換至「拉風輸入法」。

> [!NOTE]
> 安裝程式目前沒有程式碼簽章，也不可能簽，因為要付錢給微軟，不可能給微軟賺。Windows 可能會顯示「Windows 已保護您的電腦」，系統管理員權限提示中的發行者也會顯示為「未知」。請點選「其他資訊」→「仍要執行」繼續安裝。安裝程式由本儲存庫的 GitHub Actions 自動建置並發布，原始碼皆公開於此；如有疑慮，可自行檢視或交由 AI 工具掃描，但須注意本軟體採用之安全模型範圍，勿誤解 AI 提供之資安資訊。本軟體沒有自己的伺服器、資料庫或遙測，不會收集或上傳任何輸入內容。

如果選單中沒有出現拉風輸入法，可前往 Windows 的「設定」→「時間與語言」→「語言與地區」，在繁體中文的鍵盤選項中確認輸入法是否已加入。

## 使用方式

切換至拉風輸入法後，即可使用標準注音鍵盤輸入。候選字與選字操作力求與微軟注音相似，熟悉微軟注音的使用者可以直接開始輸入。

- 單按 `Shift` 可切換中文與英文輸入模式。
- 工作列上的「中／英」輸入模式圖示也可以用來切換模式。
- 按住 `Shift` 輸入時，可暫時輸入英文；放開後會回到原本的中文模式。
- 如需使用 `Shift + 空白鍵` 切換全形／半形，可先在設定視窗中開啟這項功能。

首次使用時，背景服務需要載入本機模型，開始輸入前可能需要稍候片刻。單獨安裝
`.msi` 不會下載模型；請使用 `*-setup.exe`，或在設定中指定已下載的 GGUF 檔案。

## 開啟設定

可使用以下任一方式開啟設定視窗：

- 使用拉風輸入法時，右鍵點擊工作列右下角的「中／英」輸入模式圖示，再選擇「設定」。
- 展開工作列右下角的隱藏圖示，單擊「拉風輸入法」圖示。

設定視窗目前提供：

- 指定並立即套用本機 GGUF 模型檔案。
- 選擇推論裝置。
- 開啟或關閉 `Shift + 空白鍵` 全形／半形切換。
- 新增、修改與移除自訂名字及其注音。
- 查看目前版本並檢查更新。

## 更新

可在設定視窗中檢查更新，或直接前往[最新版本下載頁面](https://github.com/llavon-ime/ime-windows/releases/tag/latest)下載並執行新版 web installer。

> [!IMPORTANT]
> 現階段更新完成後需要重新啟動 Windows，才能確保新版輸入法與相關系統元件完整套用。

## 解除安裝

可從 Windows「設定」→「應用程式」→「已安裝的應用程式」解除安裝拉風輸入法，也可使用開始功能表「Llavon IME」資料夾中的解除安裝捷徑。

## 模型與隱私

本專案預設使用 [`llavon-ime-llama-250m-Q4_K_M.gguf`](https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/blob/main/llavon-ime-llama-250m-Q4_K_M.gguf)。模型以 GGUF 量化格式透過 llama.cpp 在本機執行；其他版本與相關資訊可在 [`llavon-ime-llama-250m-GGUF`](https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF) 模型頁查看。

模型權重另依 [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/deed.zh-hant) 授權，僅限非商業用途，使用或散布時須註明來源；此授權與本專案程式碼的 BSD 2-Clause License 分開適用。

## 開發者資訊

### 專案結構

| 目錄 | 說明 |
| --- | --- |
| `frontend` | Windows TSF 輸入法前端，負責組字、選字狀態與輸入法註冊 |
| `service` | 本機背景服務，負責模型推論、設定視窗、候選字視窗與程序生命週期 |
| `ime-core` | 跨平台 C++ 推論函式庫，負責模型載入、tokenization 與 llama.cpp 推論 |
| `debugger` | 開發與診斷用的多程序記錄檢視器 |
| `cmake` | 模型下載、WiX 安裝檔與第三方授權檔案的建置腳本 |

前端與背景服務透過 Windows Named Pipe 通訊，所有 Windows targets 由頂層 CMake 統一建置。打包與執行路徑的詳細說明請見 [PACKAGING.md](PACKAGING.md)。

### 建置環境

- Windows 10 或更新版本（x64）
- Visual Studio 2022 Build Tools，並安裝「使用 C++ 的桌面開發」工作負載
- CMake 3.31 或更新版本
- .NET 8 SDK
- Git

### 從原始碼建置

1. 取得原始碼與子模組，並初始化 vcpkg：

   ```powershell
   git clone --recurse-submodules https://github.com/llavon-ime/ime-windows.git
   cd ime-windows
   ./vcpkg/bootstrap-vcpkg.bat -disableMetrics
   ```

2. Configure（只需執行一次）：

   ```powershell
   cmake --preset windows -DLLAVON_IME_WIX_ACCEPT_EULA=ON
   ```

   打包需要接受 WiX 7 的 [EULA 與維護費條款](https://docs.firegiant.com/wix/osmf/)；只建置程式時可省略 `-DLLAVON_IME_WIX_ACCEPT_EULA=ON`。

3. 依需求選擇建置目標：

   | 目的 | 指令 | 產物 |
   | --- | --- | --- |
   | 只建置程式 | `cmake --build --preset windows --parallel` | `build/windows/bin/` |
   | MSI 安裝檔 | `cmake --build --preset package --parallel` | `build/windows/llavon-ime-1.0.0-windows.msi` |
   | Web installer（`setup.exe`） | `cmake --build build/windows --config Release --target llavon-ime-setup --parallel` | `build/windows/llavon-ime-1.0.0-setup.exe` |

   `llavon-ime-setup` 會一併建置 MSI，因此要發布時直接建置這個 target 即可。

### 打包注意事項

- 首次打包需要網路，以取得 vcpkg 相依套件與 WiX 工具。
- `setup.exe` 不包含模型檔，只記錄 Hugging Face 模型的 revision、大小與 SHA-256；安裝時才下載至 `%ProgramData%\Llavon IME\models`，若既有檔案驗證相符則略過下載。
- LoRA Trainer 同樣不打包，可在訓練視窗選擇 CPU、CUDA 或 ROCm 版本下載。
- 安裝器與 MSI 的內部版號固定為 `1.0.0`，CalVer 只用於 GitHub 發布 tag 與 `latest.json`，因此改版不需要重新 configure。

## 授權

本專案依 [BSD 2-Clause License](LICENSE) 授權。安裝檔也會一併收錄所使用之第三方套件授權資訊。
