# 打包

頂層 CMake 專案在同一份建置設定中編譯所有 Windows 執行檔與 DLL，再透過 CPack 和 WiX 產生 MSI。CI 另外將 MSI 與小型模型下載輔助程式封裝成 WiX Burn 網路安裝程式。跨平台的 `ime-core` 儲存庫仍是子模組，但會以一般靜態函式庫目標參與建置。

## 前置需求

- CMake 3.31 或更新版本。
- 已安裝 C++ 工作負載的 Visual Studio Build Tools。
- 已安裝 .NET SDK，且可從 `PATH` 執行 `dotnet`。

打包前，CMake 會從 NuGet 還原 WiX 至 `build/windows/.wix-tools`，並將 WiX UI 擴充套件安裝至建置目錄內的快取，因此不需要安裝全域的 WiX。

本專案使用 WiX 7 建置安裝檔。WiX 7 要求建置者明確接受其 [EULA 與維護費條款](https://docs.firegiant.com/wix/osmf/)；確認條款後，打包時在 CMake 設定指令加入 `-DLLAVON_IME_WIX_ACCEPT_EULA=ON`。此設定會在還原 WiX 時執行 `wix eula accept wix7`。專案的 GitHub Actions 已設定接受條款。

設定介面與系統匣設定選單使用原生 WinUI 3 XAML Islands。候選字視窗繼續使用作業系統提供的 UWP XAML Islands。`cmake/WinUI3.cmake` 使用 Microsoft 實驗性 Windows App SDK CMake 目標與固定版本的 NuGetCMakePackage 輔助程式；套件版本和內容雜湊記錄於 `cmake/winui3.packages.lock.json`。專案不需要手寫 Visual Studio 方案或專案檔，也不需要自行維護 PowerShell 建置指令碼。Microsoft 的套件輔助程式可能會在內部呼叫 PowerShell 轉換資訊清單。

第一次執行 CMake 設定時會還原 NuGet 套件。WinUI 產生 C++ 投影程式碼時需要 WebView2 中繼資料，但這些介面只使用原生 XAML 控制項，不會建立或部署 WebView2 瀏覽器執行階段。設定介面的 DLL 會在自己的 STA 啟用內嵌資訊清單，並明確解析原生控制項的 PRI。CMake 會將可獨立部署的 Windows App SDK DLL、PRI 檔案、資產及語言資源複製並安裝在後端程式旁邊，使用者不必另外安裝 Windows App Runtime。

XAML 原始檔位於 `service/src/settings/ui/`，並以資源形式嵌入設定介面的 DLL。執行時 API 與設定回呼維持不變。只建置這些 UI 目標時，執行：

```powershell
cmake --build build/windows --config Release --target llavon-ime-settings-ui llavon-ime-candidate-ui --parallel
```

## 選擇正確的建置指令

不同目標會產生不同成品。特別注意，`package` 預設組態只會重新建置獨立 MSI，**不會**重新建置留在建置目錄中的 `*-setup.exe`。

只建置程式、不打包：

```powershell
cmake --preset windows
cmake --build --preset windows --parallel
```

如果先前設定 `build/windows` 時未明確指定 Visual Studio 平台，請先執行一次 `cmake --fresh --preset windows`。Windows App SDK 的 CMake 目標需要 `CMAKE_GENERATOR_PLATFORM=x64` 才能找到匯入函式庫。

建置獨立 MSI：

```powershell
cmake --preset windows -DLLAVON_IME_WIX_ACCEPT_EULA=ON
cmake --build --preset package --parallel
```

建置內含 MSI、並可下載預設模型的網路安裝程式：

```powershell
cmake --preset windows -DLLAVON_IME_WIX_ACCEPT_EULA=ON
cmake --build build/windows --config Release --target llavon-ime-setup --parallel
```

CI 會將固定的模型修訂版本、SHA-256 和檔案大小提供給安裝程式建置流程。本機建置安裝程式時，會從 Hugging Face 取得相同的中繼資料。

> [!WARNING]
> 如果安裝程式建置失敗，CMake 可能會在 `build/windows` 留下先前產生的 `*-setup.exe`。不要假設該檔案已更新；安裝前請檢查修改時間。

## 建置 MSI

獨立 MSI 包含應用程式與授權文件，但不含 GGUF 模型。一般安裝請使用網路安裝程式。若以受管理方式部署 MSI，必須另外提供 GGUF 模型，或在設定中指定模型路徑。

```powershell
cmake --preset windows -DLLAVON_IME_WIX_ACCEPT_EULA=ON
cmake --build --preset package
```

標準建置預設停用 CUDA，因此不需要 CUDA Toolkit。若要讓 `ime-core` 使用 CUDA 後端，請在建置前啟用 CUDA 設定：

```powershell
cmake --preset windows -DLLAVON_IME_ENABLE_CUDA=ON -DLLAVON_IME_WIX_ACCEPT_EULA=ON
cmake --build --preset package
```

啟用 CUDA 時，必須安裝 CUDA Toolkit，並讓 vcpkg 可以找到 `nvcc`。

`package` 目標會執行下列工作：

- 從同一份 CMake 設定建置前端 DLL、後端執行檔、UI DLL 與除錯工具執行檔。
- 將後端程式直接連結至 `ime-core` 靜態函式庫目標。
- 從 NuGet 將 WiX `4.0.4` 還原至建置目錄。
- 將 `WixToolset.UI.wixext` 安裝至建置目錄內的 WiX 擴充套件快取。
- 收集統一 manifest 安裝項目中的 vcpkg 套件授權文件。
- 將模型來源聲明與 CC BY-NC 4.0 條款納入 MSI 授權協議，並另行安裝為授權文件。
- 將 `bin`、`tables` 與 `licenses` 打包為適用於整台電腦的 x64 MSI。
- 安裝時以 `regsvr32` 註冊 `llavon-ime.dll`，解除安裝時取消註冊。
- 新增適用於整台電腦的後端開機啟動項目，並在解除安裝時移除。
- 新增開始功能表解除安裝捷徑，透過 Windows Installer 移除已安裝的產品。其提權後執行的 TSF 取消註冊與檔案移除流程，和從 Windows 設定或維護模式解除安裝時相同。
- 解除安裝時，清除舊版建置或執行時診斷在已知安裝目錄留下的檔案，再移除空的安裝目錄。
- 如果具名管道無法使用，前端也會視需要啟動後端。

MSI 輸出路徑：

```text
build/windows/llavon-ime-0.0.1-windows.msi
```

GitHub Actions 會以亞洲／台北時區的 `YYYY.MM.DD.<1000+GITHUB_RUN_NUMBER>` CalVer 格式發布版本，寫入版本化的 `v<CalVer>` GitHub Release 與 `latest.json`。MSI 和安裝程式的內部版本固定為 `0.0.1`。

設定頁面開啟時會檢查 `latest.json`，但只有使用者點擊「立即更新」才會開始安裝。第 1 版資訊清單也記錄安裝程式資產在版本化發行版中的網址、位元組大小及 SHA-256。網址取自實際發布的資產名稱，因此更改安裝程式執行檔名稱不需要修改更新程式。設定介面的 DLL 會將安裝程式下載至使用者的 LocalAppData，驗證大小與 SHA-256，再以 `runas` 啟動 WiX Burn 安裝套件，並傳入 `-quiet -norestart`。Windows 可能顯示 UAC 提示；WiX 不會顯示安裝介面。

在 MSI 開始計算檔案占用狀態前，生命週期輔助程式會停止後端，並要求 TSF 釋放輸入處理器。MSI 和 Burn 安裝套件都設定 `MSIRESTARTMANAGERCONTROL=DisableShutdown`，讓 Restart Manager 偵測檔案占用，但不要求它關閉仍載入 TSF DLL 的其他應用程式。先前使用 `Disable` 時，Windows Installer 會改用較慢的內建 FilesInUse 掃描；一次更新的紀錄顯示 `InstallValidate` 兩次各停留約 99 秒。如果應用程式仍持有舊 DLL，Windows Installer 可能將檔案替換延後至下次重新啟動 Windows。MSI 停止正在執行的後端後，已下載的安裝程式仍會繼續執行。

設定頁面的靜默更新不顯示安裝介面。以互動模式直接執行 `*-setup.exe` 時，Burn 也會略過「檔案使用中」對話框；被占用的檔案可能等到重新啟動 Windows 後才替換。直接執行獨立 MSI 時仍可能看到該對話框。下載中斷或檔案不符時，不會啟動安裝程式。更新資訊經 HTTPS 取自專案的 GitHub Release，並信任該來源；資訊清單沒有另外簽章。

`*-setup.exe` bundle 內含核心 MSI 與小型模型下載輔助程式。輔助程式會下載建置時指定的 GGUF，驗證大小與 SHA-256，然後儲存至 `%ProgramData%\Llavon IME\models\<revision>`。後續安裝或更新時，會驗證並重複使用現有檔案。模型位於 MSI 安裝目錄之外，因此 MSI 升級不會移除模型。

CMake 只嵌入固定的 LoRA Trainer 子模組提交版本，不會呼叫 LoRA 發行版 API。需要使用時，訓練對話框會從 `commit-<SHA>` 發行版取得資訊清單、驗證其提交版本，並安裝對應的 LoRA Trainer ZIP。若發行版提供相應的 Windows 資產，使用者可以選擇 CPU、CUDA 或 ROCm。

CI 會建置一份使用可載入 ggml CPU 後端的 CPU 安裝包。執行時 ggml 會選擇最快且相容的 CPU 版本，因此 AVX2 與 AVX-512 系統使用同一份 MSI。

## 執行時路徑

安裝後的目錄結構：

```text
<install-root>/
  bin/
    llavon-ime.dll
    llavon-ime-service.exe
    llavon-ime-debugger.exe
    llavon-ime-candidate-ui.dll
    llavon-ime-settings-ui.dll
    start-llavon-ime-service.vbs
  licenses/
    LICENSE.txt
    MODEL-LICENSE.txt
    ime-core-LICENSE.txt
    THIRD-PARTY-vcpkg-LICENSES.txt
    vcpkg/
      <package>.txt
  tables/
    bopomofo_char.json
    tokens/
      bpmf.json
      chars.json
      latin.json
      special_tokens.json
```

後端依下列順序決定路徑：

- 命令列明確指定：`llavon-ime-service.exe <model-path> <tables-dir>`。
- 環境變數：`LLAVON_IME_MODEL_PATH` 與 `LLAVON_IME_TABLES_DIR`。
- 由 `%ProgramData%\Llavon IME\models\current.revision` 選出的已驗證模型；資料表位於 `bin/../tables`。
- 如果 ProgramData 標記檔不存在，使用 `bin/../models` 中的舊版模型。

前端 DLL 依下列順序尋找 `bopomofo_char.json`：

- 環境變數：`LLAVON_IME_TABLES_DIR`。
- 相對於已安裝 DLL 的 `bin/../tables`。
- 開發版使用原始碼位置作為備援。

前端若無法連線至 `\\.\pipe\llavon-ime`，會嘗試以不帶參數的方式啟動後端，然後短暫重試連線。尋找後端執行檔時，會先檢查 `LLAVON_IME_SERVICE_PATH`，再使用前端 DLL 旁邊的 `llavon-ime-service.exe`。

因此，開發版與 MSI 安裝版都不依賴目前的工作目錄。

候選字顯示使用獨立的 `\\.\pipe\llavon-ime-candidate-ui` 傳輸管道。後端會在需要時載入 `llavon-ime-candidate-ui.dll`；該 DLL 管理安裝包唯一的候選字 HWND 與專用 STA 執行緒。
