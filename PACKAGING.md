# 打包與發布

這份文件說明如何從原始碼產生安裝檔、安裝檔做了哪些事，以及 CI 發布與自動更新的運作方式。一般開發只需看「快速開始」。

## 產物總覽

| 產物 | 建置 target | 說明 |
| --- | --- | --- |
| 程式本體 | `llavon-ime-artifacts`（preset `windows`） | 輸出至 `build/windows/bin/`，可直接用於開發測試 |
| `llavon-ime-1.0.0-windows.msi` | `llavon-ime-msi`（preset `package`） | 獨立 MSI，**不含模型**，適合系統管理部署 |
| `llavon-ime-1.0.0-setup.exe` | `llavon-ime-setup` | Web installer，內含 MSI 與模型下載輔助程式，一般使用者用這個 |

所有產物都在 `build/windows/` 下。`llavon-ime-setup` 會先建置 MSI，因此要發布只需建置它即可。

## 快速開始

需要的工具：Visual Studio 2022 Build Tools（C++ 桌面開發）、CMake 3.31+、.NET 8 SDK（`dotnet` 需在 `PATH` 中）。WiX 會在建置時自動從 NuGet 下載到建置目錄，不需全域安裝。

```powershell
# 第一次：初始化 vcpkg 並 configure
./vcpkg/bootstrap-vcpkg.bat -disableMetrics
cmake --preset windows -DLLAVON_IME_WIX_ACCEPT_EULA=ON

# 只建置程式
cmake --build --preset windows --parallel

# 建置 MSI
cmake --build --preset package --parallel

# 建置 setup.exe（含 MSI）
cmake --build build/windows --config Release --target llavon-ime-setup --parallel
```

只改背景服務或 UI 時，可以只建置服務（設定與候選字視窗都在其中）：

```powershell
cmake --build build/windows --config Release --target llavon-ime-service --parallel
```

> [!WARNING]
> `package` preset 只會重建 MSI，**不會**更新 `build/windows` 裡既有的 `setup.exe`。另外，`setup.exe` 建置失敗時舊檔會留在原地，安裝前請先確認檔案修改時間。

## 建置選項

| CMake 選項 | 預設 | 說明 |
| --- | --- | --- |
| `LLAVON_IME_WIX_ACCEPT_EULA` | `OFF` | 打包必須開啟。WiX 7 要求建置者接受其 [EULA 與維護費條款](https://docs.firegiant.com/wix/osmf/)；開啟後會自動執行 `wix eula accept wix7`。CI 已開啟。 |
| `LLAVON_IME_ENABLE_CUDA` | `OFF` | 額外加入 CUDA 後端。需安裝 CUDA Toolkit，且 vcpkg 要能找到 `nvcc`。 |
| `LLAVON_IME_MODEL_SHA256` / `LLAVON_IME_MODEL_SIZE` | 空 | `setup.exe` 要下載的模型雜湊與大小。CI 會傳入固定值；本機留空時會從 Hugging Face 取得。 |

預設建置包含 CPU 與 Vulkan 後端（`vcpkg.json` 的預設 feature `llama-vulkan`），使用者可在設定中選擇推論裝置。CPU 後端會將各指令集版本都編成可載入模組，執行時才挑選最快且相容的版本，所以 AVX2 與 AVX-512 電腦共用同一份安裝檔。

## 安裝檔做了什麼

### MSI

- 安裝至整台電腦（x64），內容為 `bin/`、`tables/`、`licenses/`。
- 以 `regsvr32` 註冊 `llavon-ime.dll`，解除安裝時取消註冊。
- 新增後端服務的開機自動啟動項目。
- 新增開始功能表的解除安裝捷徑（同時帶有 Toast 通知需要的 AppUserModelID）。
- 授權協議包含本專案、`ime-core`、vcpkg 套件授權，以及模型的 CC BY-NC 4.0 條款。
- 解除安裝時清除安裝目錄內殘留的檔案與空目錄。

安裝前會先停止後端服務。MSI 設定 `MSIRESTARTMANAGERCONTROL=DisableShutdown`：會偵測檔案占用，但不會強制關閉仍載入 DLL 的應用程式，被占用的檔案會在重新開機後替換。

### setup.exe

WiX Burn bundle，內含上述 MSI 與一個小型模型下載輔助程式：

1. 下載建置時指定的 GGUF 模型，驗證大小與 SHA-256。
2. 存放至 `%ProgramData%\Llavon IME\models\<revision>`；若已有相同且完整的檔案則略過下載。
3. 安裝 MSI。

模型放在安裝目錄之外，所以升級或重裝 MSI 不會刪除模型。

LoRA Trainer 不打包進安裝檔。CMake 只記錄 LoRA Trainer 子模組的 commit。使用者開啟訓練視窗時，背景服務只讀取對應 `commit-<SHA>` Release 的 manifest 來檢查版本；按下安裝後，才下載並驗證 CPU 版 ZIP（`win-x64-cpu`）。需要 NVIDIA GPU 加速時，可再另外安裝 CUDA 加速元件，由訓練器從 NuGet 下載。Windows 版不提供 ROCm。

## 發布與自動更新

### 版本號

- CI 以台北時區產生 CalVer：`YYYY.MM.DD.<1000 + GITHUB_RUN_NUMBER>`，用於 GitHub Release tag（`v<CalVer>`）與 `latest.json`。
- MSI 與 setup.exe 的內部版本固定為 `1.0.0`，本機改版不需要重新 configure。

### 更新流程

1. 設定頁面開啟時讀取 `latest.json`，其中記錄最新版 setup.exe 的網址、大小與 SHA-256。
2. 使用者按下「立即更新」後，下載至 LocalAppData 並驗證大小與 SHA-256；不符則中止。
3. 以 `runas` 啟動 setup.exe，帶入 `-quiet -norestart`。Windows 會跳出 UAC，但不會顯示安裝介面。

更新資訊透過 HTTPS 從 GitHub Release 取得，`latest.json` 本身沒有另外簽章。

### 重大更新通知

要把某次發布標為重大更新，在主分支**該次發布的最後一筆 commit** 訊息末尾加上 trailer：

```text
改善輸入法候選字與設定體驗

Major-Update: 候選字選取與自訂詞庫已全面更新，請開啟設定頁安裝新版。
```

- 訊息最多 200 字元，且每次發布只能有一行，CI 會將它寫入 `latest.json` 的 `major_update` 與 Release 說明。
- 之後的一般發布會保留最近一次重大更新，讓較晚檢查的舊版仍會收到通知；已經是該版或更新版本的使用者不會收到。
- squash 或 merge 時，記得確認 trailer 仍在最後一筆 commit 的結尾。

後端啟動約 10 分鐘後會在背景檢查一次，若有尚未安裝的重大更新且使用者開啟「更新通知」（預設開啟），就顯示 Windows Toast 通知。通知不會自動安裝。

Toast 需要安裝程式建立的開始功能表捷徑，直接執行建置目錄裡的服務程式不會顯示通知。目前沒有註冊通知中心需要的 COM 啟用程式，所以通知消失後不一定會留在通知中心。

## 安裝後的目錄結構

```text
<install-root>/
  bin/
    llavon-ime.dll                 TSF 前端
    llavon-ime-service.exe         背景服務（含設定與候選字視窗）
    llavon-ime-debugger.exe        診斷工具
    （Windows App SDK 執行階段 DLL 與資源）
  tables/
    bopomofo_char.json
    tokens/
  licenses/
```

Windows App SDK 以自含方式部署在 `bin/` 中，使用者不需要另外安裝 Windows App Runtime。

### 路徑解析順序

程式不依賴目前工作目錄，開發版與安裝版使用相同邏輯。

**背景服務**找模型與字表：

1. 命令列參數：`llavon-ime-service.exe <model-path> <tables-dir>`
2. 環境變數：`LLAVON_IME_MODEL_PATH`、`LLAVON_IME_TABLES_DIR`
3. `%ProgramData%\Llavon IME\models\current.revision` 指向的已驗證模型；字表為 `bin/../tables`
4. 舊版位置 `bin/../models`

**前端 DLL** 找 `bopomofo_char.json`：

1. 環境變數 `LLAVON_IME_TABLES_DIR`
2. `bin/../tables`

前端與服務透過 `\\.\pipe\llavon-ime` 溝通。連不上時，前端會自行啟動服務（優先使用 `LLAVON_IME_SERVICE_PATH`，否則找 DLL 旁的 `llavon-ime-service.exe`）再重試。候選字視窗使用另一條 `\\.\pipe\llavon-ime-candidate-ui`。

## 疑難排解

**Configure 時找不到 Windows App SDK 的匯入函式庫**
舊的 `build/windows` 可能沒有指定平台。執行一次 `cmake --fresh --preset windows -DLLAVON_IME_WIX_ACCEPT_EULA=ON`。

**vcpkg 每次都整批重編**
preset 已設定 `--x-abi-tools-use-exact-versions`，讓 vcpkg 使用固定版本的 CMake 與 PowerShell，避免 runner 上工具升級改變 ABI hash。若仍發生非預期重編，下載 CI 的 `llavon-ime-vcpkg-abi` artifact，比較前後的 `vcpkg_abi_info.txt`，找出是哪個輸入變了（`powershell`、`cmake`、`triplet_abi` 等），不要直接改 cache key。

**更新後輸入法還是舊版**
被應用程式占用的 DLL 會在重新開機後才替換，請重新啟動 Windows。

## 附錄：UI 技術說明

- 設定視窗、系統匣選單與候選字視窗都使用 Windows App SDK（WinUI 3）XAML Islands。
- XAML 原始檔位於 `service/src/settings/ui/` 與 `service/src/candidate/ui/`，由 `service/src/ui_resources.rc` 以資源嵌入 `llavon-ime-service.exe`。
- `cmake/WinUI3.cmake` 使用 Microsoft 實驗性的 Windows App SDK CMake 整合，套件版本與雜湊鎖定在 `cmake/winui3.packages.lock.json`。不需要 Visual Studio 方案檔。
- 產生 C++/WinRT 投影時需要 WebView2 中繼資料，但程式本身不使用也不部署 WebView2。
