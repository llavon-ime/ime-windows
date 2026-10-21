# 拉風輸入法 Windows 背景服務

拉風輸入法的 Windows named pipe 背景服務。與平台無關的模型載入、
tokenization 與 llama.cpp 推論由 `ime-core` 子模組提供。

背景服務也負責每位使用者各自的互動式程序外殼：

- `llavon-ime-service.exe` 在推論工作執行緒上執行 named pipe 伺服器，
  主執行緒則擁有通知區域圖示。
- `llavon-ime-settings-ui.dll` 於需要時從執行檔所在目錄載入，擁有設定視窗的
  HWND 與 WinUI 3 `Microsoft.UI.Xaml` island。
- 設定 UI 的呼叫會排入共用的 WinUI STA，絕不會在推論工作執行緒上執行 XAML
  或模型相關工作。
- 模型路徑與推論裝置設定儲存於 `%LOCALAPPDATA%\Llavon IME\settings.json`。
  背景服務在程序啟動時讀取此檔並轉換為 `ime-core::CoreConfig`；在設定視窗中
  變更模型或裝置時，會立即重新載入推論核心。
- 模型載入完成後，設定視窗會收到 `ime-core` 回報的目前後端、硬體描述、
  裝置 ID 與 GPU offload 狀態；這份執行階段狀態與「下次啟動時套用」的設定
  分開保存。
- GPU Boost 預設啟用，可在設定視窗中關閉。在支援的 NVIDIA 或 AMD 驅動程式上，
  背景服務會在 `Ready` 與 `Predict` 之前啟用 Boost，並在兩秒內都沒有這兩種
  請求時釋放。關閉此設定後會停止續期正在生效的 Boost，因此它會在原本的兩秒
  期限到時失效；此選擇在背景服務重新啟動後仍會保留。
  NVIDIA 使用 NVAPI 低延遲提示，作用於一個與推論 GPU PCI 位址相符的離屏
  D3D11 裝置。AMD 則從系統 OpenCL 驅動程式動態解析
  [`clSetDeviceClockModeAMD`](https://github.com/ROCm/clr/blob/develop/opencl/amdocl/cl_profile_amd.h)，
  在租約有效期間請求 `Peak`，並在閒置、更換模型／裝置或關閉時改回 `Default`。
  OpenCL 裝置必須同時符合 AMD 的 vendor ID 與推論 GPU 的 PCI 位址。
  若推論後端只提供 `VulkanN`（例如 Radeon 860M），背景服務會解析名稱唯一的
  Vulkan 實體裝置、取得其 Windows LUID，再查詢對應 DXGI adapter 的 PCI 位址。
  名稱重複、缺少 LUID 或身分不明確的情況一律拒絕；絕不假設 Vulkan 與 DXGI
  的列舉索引彼此對應。
  這是作用於整個裝置的驅動程式電源模式請求，受電源與溫度限制約束；可能提高
  記憶體時脈、功耗、溫度與風扇轉速。不會建立 OpenCL context、queue、kernel
  或保活工作負載，也不設定手動頻率目標、不超頻、不寫入註冊表。推論仍在
  Vulkan 上執行。
  此 AMD profiling 擴充功能是選用的，並非通用的消費級低延遲 API。缺少進入點、
  拓撲資訊或驅動程式支援時，只會停用 Boost，不會停用推論。唯讀查詢成功即視為
  可用；只有 `Peak` 請求成功時才會記錄 `AMD GPU boost applied`。
  驅動程式輸出的單位與更新頻率各不相同，因此這些欄位不會被解讀為即時時脈、
  MHz 或比例。實際時脈請以獨立的遙測工具驗證。
  暫時性的啟用失敗可在之後的輸入時重試，每秒最多一次。
  啟用請求失敗後，會先送出 `Default` 才允許再次嘗試。
  AMD 驅動程式呼叫由一個隱藏的背景服務執行檔實例負責，它會監看從父背景服務
  繼承的 handle，並在 singleton、模型或 UI 初始化之前執行。因此即使父程序被
  終止，仍存活的工作程序也會釋放 Peak。IPC 使用未命名 event 與共用記憶體，
  並搭配明確的繼承 handle 清單；不對外公開任何命令 pipe。
  工作程序會獨立於父程序重試還原，最多 30 次。
  每個裝置各有一個 mutex，避免前一個工作程序仍負責還原時，接替的工作程序
  就啟用 Peak。IPC 逾時會暫停後續 Boost 並通知工作程序還原；推論執行緒上
  等待驅動程式的時間有上限。
  若驅動程式停止回應、還原持續失敗，或工作程序本身（包含整個程序樹）被強制
  終止，則無法保證一定會還原。此 API 還原的是 `Default`，而非其他 profiling
  應用程式設定的模式快照；請避免同時使用其他外部時脈模式控制工具。
  `AMD GPU boost restored` 代表已成功收到 `Default` 的確認。
  效能驗證必須包含間隔 250 ms 的請求，以及閒置超過兩秒後恢復輸入的情境，
  不能只測連續 token 吞吐量。
- `llavon-ime-candidate-ui.dll` 在第一次顯示候選字時載入。它在共用 STA 上
  擁有一個候選字 HWND 與自己的 WinUI 3 XAML island。其 runtime 擁有一個
  非同步 logger 轉接器，透過 `ime-core::Logger` 注入候選字視窗；UI 追蹤
  訊息會以 `LogInformation::debug` 記錄出現在偵錯工具的 **Debug messages**
  頁面。
- 候選字顯示快照經由獨立的 `\\.\pipe\llavon-ime-candidate-ui` pipe 傳送。
  此傳輸通道不與預測 pipe 共用連線或協定。
- 每次完成的輸入法組字都會以原始前文、送出的文字與原始注音讀音傳送給背景
  服務。背景服務將讀音轉換為類似驗證集的 `syllable`/`tone` 結構，並在優先權
  低於一般的寫入執行緒上寫入 SQLite。預測用的 `io_context` 絕不開啟、寫入或
  flush 訓練資料庫。
- 另外建置的 `llavon-ime-debugger.exe` 擁有可接受多個用戶端的
  `\\.\pipe\llavon-ime-debugger` 伺服器。系統匣選單會在需要時啟動隨安裝包
  附帶的執行檔。
- 背景服務連結共用的 `llavon::debug-client` 記錄產生端函式庫，並將其轉接為
  `ime-core::Logger`。偵錯工具的傳輸元件與 UI 不由本專案建置。
- 每次開啟設定視窗時，都會自動比對內嵌的建置識別資訊與滾動發布版本的
  `latest.json` manifest。CI 建置依建置編號排序。本機開發建置仍會顯示其
  commit 是否與 `latest` 不同，但不會猜測哪個 commit 較新。
  HTTP 請求在設定工作執行緒上執行，兩秒後即取消，因此 GitHub 回應緩慢時
  絕不會卡住 UI 執行緒。
- 只有在使用者按下 **立即更新** 後，才會開始下載並安裝更新。發布 manifest
  提供安裝程式的 URL、位元組大小與 SHA-256；設定工作執行緒會先驗證下載內容，
  再以靜默模式啟動 WiX 安裝程式，並為全系統安裝顯示 UAC 提示。MSI 維持固定
  版號，讓本機建置與正式發布的版本可以互相取代。MSI 在檢查使用中檔案之前，
  會先釋放背景服務與 TSF processor；其他應用程式則保持執行。若仍有應用程式
  持有舊的 TSF DLL，Windows 可能需要重新啟動。

設定與候選字模組是各自獨立、擁有不同 HWND 的 DLL。
`llavon-ime-ui-runtime.dll` 擁有兩者共用的 STA、Win32 訊息迴圈、dispatcher
與整個程序唯一的 WinUI Application。每個 UI 用戶端都會取得一份 runtime
參考；關閉其中一個用戶端不會影響另一個。在背景服務結束前，框架都會保持可用，
以便重新開啟用戶端；背景服務結束時，宿主會在兩個用戶端都關閉後關閉 runtime。

原始碼依執行階段職責而非作業系統名稱劃分：

- `src/service/`：常駐 EXE 的職責，包含預測 IPC、系統匣圖示與載入 UI 模組。
- `src/ui/`：共用的 WinUI STA runtime 與 XAML 資源輔助工具。
- `src/settings/`：設定 DLL，包含其 UI 用戶端、HWND 與 XAML island。
- `src/candidate/`：候選字 UI DLL，包含其 UI 用戶端、單一 HWND 與 XAML
  island。
- `src/service/debug/`：`ime-core::Logger` 與另外安裝的
  `llavon::debug-client` 記錄產生端之間的輕量轉接層。

背景服務沿用既有的執行檔與 IPC 相容名稱：

- 執行檔：`llavon-ime-service.exe`
- 設定 UI 模組：`llavon-ime-settings-ui.dll`
- 候選字 UI 模組：`llavon-ime-candidate-ui.dll`
- 偵錯工具執行檔：`llavon-ime-debugger.exe`
- named pipe：`\\.\pipe\llavon-ime`
- 候選字 UI named pipe：`\\.\pipe\llavon-ime-candidate-ui`
- 偵錯工具 named pipe：`\\.\pipe\llavon-ime-debugger`
- 預設模型路徑覆寫：`LLAVON_IME_MODEL_PATH`（一般啟動時以已儲存的設定優先）
- 字表路徑：`LLAVON_IME_TABLES_DIR`
- 收集的訓練資料庫：`%LOCALAPPDATA%\Llavon IME\training-data\commits.sqlite3`
- 收集的訓練資料庫路徑覆寫：`LLAVON_IME_TRAINING_DATABASE_PATH`

一般注音送出內容儲存在 SQLite 的 `training_commits` 資料表中。
`context`、`answer` 與 `padding_json` 的結構與公開驗證集相同。只要該次送出中
任一位置曾手動選字，布林欄位 `revice` 即為 true；原始注音讀音保持不變。
混合內容的送出會將字面字元與未完成的輸入保留為 `literal` 或 `rawReading`
項目。完全沒有任何注音讀音的送出會被捨棄，背景服務啟動時也會一併清除既有、
仍為 pending 且只含字面字元的資料列。`training_state` 欄位只允許 `pending`、
`excluded` 或 `trained`。另外也會儲存 `event_id`、`event_type` 與可為 null 的
`revision_of`；最後一欄為未來偵測打錯字／退格／重打等修正行為預留。
轉換資料集時，ime-core 提供與推論時相同的 token 與候選字。字面字元可完整
作為後續位置的前文。每筆記錄仍需至少一個完整、可訓練的注音讀音；未完成的
`rawReading` 項目無法用於訓練。

送出內容會先以明文形式只保存在背景服務程序中十秒。若前端回報同一個 TSF
context 針對同一筆最新送出立即按下 Backspace，該筆暫存內容就會被捨棄，
永遠不會寫入 SQLite。否則，優先權低於一般的寫入執行緒會在時間到後將其加密
並寫入；同一收集工作階段中的下一筆送出會提前確認並寫入前一筆。修正偵測絕不
解密或讀取已儲存的送出內容。SQLite 使用 WAL 模式，選取與狀態變更都在交易中
執行，因此訓練資料的 I/O 不會出現在推論路徑上。此預覽版結構刻意不匯入先前的
JSONL 原型資料。

## AMD 筆電 GPU Boost 驗證

1. 在設定中選擇 AMD Vulkan 推論裝置，並確認目前 runtime 回報已啟用 GPU
   offload。若推論在 CPU 上執行，啟用 GPU Boost 並不會測試到 AMD 後端。
2. 若要擷取驅動程式診斷資訊，先從系統匣選單結束現有的背景服務，再於新版
   建置的 `bin` 目錄中以 PowerShell 啟動：
   `./llavon-ime-service.exe 2> amd-boost.log`。
   若自動啟動搶先執行，記錄中會出現 `already running`；請結束該實例後再試。
   帶有內部參數 `--amd-gpu-boost-worker` 的隱藏實例是時脈擁有者，並不是第二個
   輸入法背景服務。
3. 以約 250 ms 的間隔輸入數個字，暫停至少五秒後再繼續輸入。確認記錄中先出現
   `AMD GPU boost applied`，之後出現 `AMD GPU boost restored`。這些訊息每個
   後端只記錄一次；驅動程式呼叫成功並不代表延遲有所改善。
4. 比較 GPU Boost 啟用與停用時的差異，特別是閒置後的第一次預測。也要檢查
   切換模型／裝置與正常結束的情況。若有工具可用，請另外監看實際的 GPU 時脈
   與功耗。
5. 測試當機復原時，請在 Peak 生效期間只終止父背景服務，並獨立確認仍存活的
   工作程序會還原為 `Default`。終止整個程序樹也會一併終止負責還原的工作程序。
6. 若記錄顯示時脈模式工作程序無法使用，請記下 GPU 名稱與驅動程式版本。推論
   應該仍能繼續。支援 Vulkan 並不代表支援 AMD 選用的 OpenCL 時脈模式擴充功能。

`amd-gpu-boost-worker-tests` 以模擬驅動程式測試實際的子程序／IPC 生命週期，
包含父程序終止、延遲呼叫與還原失敗等情境，且絕不會變更硬體時脈。硬體驗證
仍須在目標驅動程式上檢查 Peak/Default，並量測實際的候選字延遲。
在搭載 Windows 驅動程式 32.0.22032.6002 的 Radeon 860M 上，Peak/Default 請求
皆被接受。在一次排除暖機區段的本機基準測試中，每次預測完成後都完整閒置
250 ms，每種設定各量測 56 個樣本：Boost 關閉時中位數為 9.227 ms，開啟時為
9.119 ms；P95 則分別為 15.668 ms 與 15.857 ms。實際閒置間隔為
250.022–250.530 ms。這種些微且方向不一的差異，不足以證明延遲有穩定的改善。
以上是暖機後請求的統計，並非互動延遲的上限。先前一次原始基準測試在被排除的
暖機區段中記錄到一筆 473.767 ms 的請求。實際運作中的背景服務也在其他介於
3–15 ms 的請求之間，記錄到 603.283 ms 與 764.094 ms 的預測，且每次突增的時間
幾乎全都落在快取對齊用的 `llama_decode()` 呼叫中。後端插樁重現了這些卡頓，
原因是 Vulkan 矩陣 pipeline 的延遲建立。閒置時的 `ready()` 暖機只在另一個
context 中解碼一個 token，並未準備實際輸入所需的小 batch 變體。

核心現在會在載入模型時、開始處理請求之前，一次準備好 Vulkan 推論 pipeline。
涵蓋小 batch、較大的矩陣 kernel 與已填入資料的 attention 快取，接著清除合成的
KV 狀態，並保留已準備好的 context 給第一個工作階段使用。在沒有相容磁碟快取的
情況下，這會讓測試用的 860M 多花約 4–5 秒，但不會產生週期性的 GPU 工作，也不會
改變實際推論的 batch 大小。CUDA 與 CPU 的初始化維持不變。
驗證時必須涵蓋所有第一次使用的請求、新工作階段、250 ms 的閒置間隔，以及更長的
閒置時間。長前文的運算時間仍取決於工作負載；單靠驅動程式時脈提示並無法消除
重現出的卡頓。

背景服務會明確將 `%LOCALAPPDATA%\Llavon IME\vulkan-cache` 傳給 ime-core。
跨平台的 ggml Vulkan 後端不會自行假設路徑；宿主未提供路徑時，持久快取保持停用。
之後的啟動會重複使用相容的資料，但仍會準備模型及其推論 context。
快取識別資訊包含 GPU、驅動程式版本、Vulkan 快取 UUID 與後端結構版本。
資料內容附有 checksum，並以不可分割的方式替換。
遺失、損毀或不相容的資料會重新建立；快取 I/O 失敗不會阻止推論。核心在模型準備
完成後立即儲存，因此不需要正常關閉背景服務也能保留這些成果。預測路徑上沒有任何
快取寫入，也不會變更註冊表。
`[VK_CACHE] loaded`、`miss` 與 `saved` 分別代表這幾種啟動結果。
固定使用的 ggml fork 與診斷方式請參考 `ime-core/vcpkg-ports/README.md`。

在 `Default` 成功後，時脈模式查詢仍會回傳看似 Peak 的數值；獨立的 ADL 遙測則
顯示已回到閒置時的低時脈。無論是 API 呼叫成功或其時脈查詢輸出，都不能證明實際
頻率或延遲有所改善。

## 本機 LoRA 訓練

每次開啟設定視窗中的 LoRA 對話框時，都會列出目前狀態為 `pending` 的記錄，且預設
全部勾選。開始訓練時，所有未勾選的 pending 記錄會改為 `excluded`；成功寫入訓練器
資料集的記錄，要等訓練與 GGUF 匯出都完成後才會改為 `trained`。無法轉換的記錄
維持 `pending`。
含有手動選字的可訓練記錄會貢獻三個樣本，其他記錄則貢獻一個。啟用洗牌（預設）時，
訓練器每個 epoch 都會將所有樣本重新洗牌。訓練歷史只計算原始記錄數，不含額外樣本。

選用的「訓練到記住為止」模式會在每個 epoch 之後重新評估每個手動選字的訓練目標，
並重複訓練，直到每個目標都在其候選集合中勝出為止。此模式會拒絕相同因果輸入前綴
卻有互相衝突目標的情況，且無法搭配超低預設組合使用。

獨立的「只訓練錯誤項目」模式同樣會重新評估所有訓練目標，但下一個 epoch 只針對
驗證失敗的目標計算 loss。除非另外啟用「訓練到記住為止」，否則仍遵守設定的
epoch 上限。

對話框提供的非空預設值源自 `lora-trainer/docs/step-search-results.md`：
rank/alpha 為 8/16、dropout 0、batch size 與梯度累積皆為 1、5 個 epoch、
max steps `-1`、學習率 `1e-4`、FP32、目標模組 `q_proj,v_proj`、無 warmup 與
weight decay、最大梯度範數 1、seed 42、啟用洗牌，裝置為 `auto`。UI 會輪詢背景
服務狀態以顯示下載／訓練進度，可取消進行中的作業，也可透過既有的模型重新載入
回呼載入完成的 `personalized-Q4_K_M.gguf`。

基礎模型固定為 `tony65535/llavon-ime-llama-250m`（CC-BY-NC-4.0）。
它絕不會在未經同意的情況下下載：使用者必須按下下載／更新按鈕。背景服務會將每次
下載固定在 Hugging Face 回傳的儲存庫 revision，並將 `config.json`、
`ime_vocab.json` 與 `model.safetensors` 儲存於：

```text
%LOCALAPPDATA%\Llavon IME\training-assets\tony65535--llavon-ime-llama-250m\<revision>
```

每次訓練都在 `training-assets\runs` 下有自己的目錄。訓練歷史以及每次訓練的
`adapter_model.safetensors`、`adapter_config.json` 與 `training_state.json`
都會保留，讓先前的 adapter 可以搭配對應的基礎模型 revision 再次匯出。新模型成功
套用後，只保留最近一次完成訓練的量化 GGUF 供推論使用。若載入或儲存新模型失敗，
則保留舊的 GGUF 檔案。中間產物 f16 GGUF 在匯出後即刪除。將選取的 SQLite 資料列
轉換為 CLI 可接受的數值 JSONL、啟動隱藏的訓練器程序、匯出 adapter，以及所有
相關的檔案操作，都由背景服務負責，而非設定 DLL。舊版安裝的備用 CLI 路徑為：

```text
%ProgramFiles%\Llavon IME\tools\lora\llavon-lora.exe
```

固定使用的 LoRA Trainer 子模組 commit 會在 CMake configure 時內嵌。
訓練對話框會直接請求其 `commit-<SHA>` manifest，並將 manifest 中的 `commit`
與固定的子模組 commit 比對。訓練器只下載發行版中的 `win-x64-cpu` 檔案，它內含
CPU 版 LibTorch。在應用程式內安裝時，會下載該 ZIP、驗證其大小與 SHA-256、檢查
壓縮檔內的路徑，再解壓縮並驗證安裝後的 manifest。安裝位置為
`%LocalAppData%\Llavon IME\tools\lora\<version>\win-x64-cpu`，目前選用的版本記錄在
`current.install`。背景服務會優先使用此選擇，其次才是 Program Files 中的安裝。
成功選定某個發布版本後，背景服務會從此個人目錄移除其他版本的訓練器（包含舊版的
`win-x64-cuda` 與 `win-x64-rocm` 安裝），重新選擇已下載的版本時也一樣。清理時絕不
追蹤 reparse point，也不刪除無關檔案；若無法移除舊檔案，安裝仍會成功，但會顯示
清理警告。

以 NVIDIA 顯示卡訓練需要另外下載「CUDA 加速元件」（約 3 GB）。對話框的按鈕會讓
背景服務執行 `llavon-lora runtime install --backend cuda --json`，由訓練器自己從
NuGet 下載並驗證 TorchSharp 使用的 CUDA LibTorch，並把逐行的 JSON 進度轉成進度列；
取消會結束子程序，已完成的套件會保留，下次可從中斷處繼續。執行階段放在
`%LocalAppData%\Llavon IME\tools\torch-runtimes`，不屬於任何訓練器版本目錄，所以
更新訓練器時不會被清理，只要 LibTorch 版本不變就能沿用。背景服務在檢查版本及
安裝、移除後以 `llavon-lora runtime list --json` 取得狀態，訓練時傳入
`--runtime-root`；選擇 `cuda` 裝置但尚未安裝時，會在啟動訓練前直接提示。ROCm 沒有
NuGet 套件，Windows 版不再提供。

模型檢查與下載使用 WinRT `Windows.Web.Http`，並以 `co_await` 處理網路操作。
背景服務的專用工作執行緒只在最外層邊界等待。下載時以固定大小的區塊讀取回應，
並將進度回報給設定 UI；取消時也會一併取消進行中的 HTTP 請求。

開發與測試時，可用 `LLAVON_IME_LORA_ASSETS_DIR` 覆寫資源／訓練根目錄，並用
`LLAVON_IME_LORA_CLI_PATH` 覆寫訓練器執行檔。

## 建置

候選字與設定 DLL 都在同一個共用 STA 執行緒上承載 WinUI 3
`DesktopWindowXamlSource` island。兩者共用 `src/ui` 中的啟用、dispatcher、
XAML metadata 與內嵌資源輔助工具。兩個 UI 都不在應用程式的 TSF 執行緒中執行。

候選字 UI 的修改應放在 `src/candidate/ui/candidate_page.xaml`（欄位與頁尾）、
`candidate_item.xaml`（每一列候選字）與 `theme.xaml`（字型、欄寬、淺色／深色／
高對比筆刷）。這些檔案由 `candidate_ui.rc` 以 RCDATA 內嵌；編輯後需重新建置 DLL。
C++ 負責更新候選字文字、選取狀態、編號與分頁狀態；彈出視窗會量測 XAML 樹，以取得
目前 DPI 下的原生大小。

從儲存庫根目錄設定並建置完整的 Windows 專案：

```powershell
git submodule update --init --recursive
cmake --preset windows
cmake --build --preset windows
```

頂層專案將 `ime-core` 與本元件加入同一個 CMake 建置圖，並使用同一份 vcpkg
manifest。Vulkan 支援預設啟用。CUDA 支援為選用，需在 configure 時設定
`LLAVON_IME_ENABLE_CUDA=ON`。

在互動式 Windows 桌面上，可啟用選用的候選字／設定整合測試，檢查兩種啟動順序、
不搶焦點的顯示、欄寬調整、螢幕邊緣定位、隱藏／顯示，以及 UI 用戶端重新啟動：

```powershell
cmake --preset windows -DLLAVON_IME_ENABLE_UI_TESTS=ON
cmake --build build/windows --config Release --target candidate-ui-tests candidate-pipe-protocol-tests
ctest --test-dir build/windows -C Release -R 'candidate-(ui|pipe-protocol)' --output-on-failure
```
