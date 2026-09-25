# LoRA 訓練分支與任意基底：實作規劃

## 目標與這次範圍

- 新訓練可指定「原始 Base model」或任一已完成訓練作為基底。`parent_id` 表示實際載入的 adapter，不再暗中指向最新一筆。
- 一筆輸入是否已訓練，取決於所選基底及其祖先的訓練紀錄。其他分支用過的輸入仍可在這個基底訓練。
- 每次完成訓練，SQLite 保存當次完整且實際生效的訓練請求、結果與輸入關聯。
- 先以平面選單選基底，歷程可用平面清單顯示；可點擊的樹狀 UI 留待後續。資料庫先保留樹所需的 ID 與父子關係。
- 維持現有訓練完成後匯出 GGUF、預設套用最新訓練模型的流程。套用成功後只保留目前使用中的 GGUF；歷史版保留 adapter safetensors，切換回該版時才重新匯出。

## 現況與必改處

1. `training_commits.training_state='trained'` 是全域標記；`pending_items()`、`pending_records()` 及 `pending_count_` 都以它排除資料。因此切回舊基底也看不到其他分支用過的輸入。
2. `lora_training_runs` 已有 `parent_id`，但 `start_training_async()` 和 `training_worker()` 都固定讀 `latest_lora_training_run()`；`--resume-adapter` 也固定使用最新 adapter。
3. `complete_lora_training()` 同一交易插入 run 並把 commit 改成全域 `trained`；目前資料庫只有部分參數。完整請求目前寫在 run 目錄的 `training_request.json`。
4. `exclude_unselected()` 在啟動訓練前把本次未選資料永久設為 `excluded`。分支模式下這會無意中取消其他基底的候選資料。
5. `prune_obsolete_gguf_models()` 目前在最新 GGUF 套用後移除所有舊 GGUF，符合節省空間方向，但切換歷史版時必須先有重建與套用流程；不可在新檔套用失敗時刪掉目前使用中的檔案。
6. Settings UI 的歷程 ABI 只傳時間、筆數與 steps，未傳 run ID 或 parent ID；現有時間軸會把不同分支誤畫成一條線。

## SQLite 資料模型

保留 `lora_training_runs.id` 與 `parent_id`。`parent_id IS NULL` 表示直接從原始基底訓練；run 必須保存不可變的 `base_model_revision`、`adapter_path`、`output_model_path`、完成時間、當次筆數、沿祖先累計筆數及 steps。建立/開啟連線時維持 `PRAGMA foreign_keys=ON`。

新增關聯表，**只有實際寫入訓練資料集的事件**才插入：

```sql
CREATE TABLE lora_run_commits (
    run_id INTEGER NOT NULL REFERENCES lora_training_runs(id),
    commit_id INTEGER NOT NULL REFERENCES training_commits(id) ON DELETE CASCADE,
    PRIMARY KEY (run_id, commit_id)
) WITHOUT ROWID;
CREATE INDEX lora_run_commits_commit_run ON lora_run_commits(commit_id, run_id);
CREATE INDEX lora_training_runs_parent_id ON lora_training_runs(parent_id);
CREATE TABLE legacy_training_unknown (
    commit_id INTEGER PRIMARY KEY REFERENCES training_commits(id) ON DELETE CASCADE
);
```

`training_commits.training_state` 在相容期只表示可供選取或明確排除；新訓練不得再寫入 `trained`。之後的 schema migration 可將欄位改名為 `disposition` 並移除 `trained` 值。`mark_trained()` 這類沒有 run ID 的入口須移除，或改成必須附帶 run ID 的關聯寫入，不能留下全域標記捷徑。

在 `lora_training_runs` 增加 `training_request_json TEXT`。新 run 必填，JSON 需通過 `json_valid()`；舊 run 可為 `NULL`。格式版本保存在 JSON 的 `preset_version`。JSON 用實際生效的訓練請求序列化，至少包含：

- 選定 parent ID、基底 revision、preset 版本/強度、`only_manually_selected`；
- rank、alpha、dropout、target modules、batch、gradient accumulation、epochs、max steps、learning rate、weight decay、warmup、max gradient norm、save every、device、dtype、seed、shuffle、max sequence length；
- 實際的 pad token ID、trainer commit/version/backend、資料集有效筆數、跳過筆數、sample 數、supervised position 數；
- 完成後的實際 optimizer steps、adapter/輸出模型路徑與時間。必要時把請求與結果分為兩個 JSON 欄位，避免完成後改寫原始請求。

不得把密碼、解密後的輸入內容或暫存 JSONL 寫進此欄。`training_request.json` 可繼續作為除錯副本；SQLite 是歷程的正式來源。`record_count` 應等於本次關聯表列數；`cumulative_record_count = parent.cumulative_record_count + record_count`，根節點從零開始。

## 候選資料與回退語義

讀取候選資料、讀取已選明文紀錄、顯示數量及完成時驗證，都傳同一個 `base_run_id`；`NULL` 表示原始 Base model。用 recursive CTE 取得「所選 run 加全部祖先」：

```sql
WITH RECURSIVE lineage(id, parent_id) AS (
    SELECT id, parent_id FROM lora_training_runs WHERE id = :base_run_id
    UNION ALL
    SELECT p.id, p.parent_id
    FROM lora_training_runs AS p
    JOIN lineage AS child ON p.id = child.parent_id
)
SELECT c.*
FROM training_commits AS c
WHERE c.training_state = 'pending'
  AND NOT EXISTS (
      SELECT 1 FROM legacy_training_unknown AS legacy
      WHERE legacy.commit_id = c.id
  )
  AND NOT EXISTS (
      SELECT 1 FROM lora_run_commits AS used
      JOIN lineage ON lineage.id = used.run_id
      WHERE used.commit_id = c.id
  )
ORDER BY c.id;
```

實作時要先驗證 `base_run_id` 存在且版本相容，避免不存在的 ID 被查成空祖先而意外放行全部資料。原始基底的 lineage 為空，因此能重訓過去分支的資料。例：`Base → A(X) → B(Y)`，另從 `A` 建 `C(Y,Z)`；選 `A` 時 `X` 不可選、`Y/Z` 可選；選 `B` 時 `X/Y` 不可選、`Z` 可選。平行分支用過的 `Y` 不會讓 `B` 的候選集合改變。

本次未選的資料維持可供其他分支選取；停止從訓練啟動流程呼叫全域 `exclude_unselected()`。使用者明確刪除或明確排除的資料仍可維持全域排除。`pending_count_` 的全域原子計數不能再當「所選基底可訓練筆數」；改由基底感知的 SQL 計數提供訓練畫面，切換基底後重載清單與預估 steps。

## 訓練、完成與模型套用

1. 在平面選單中選 Base model 或 run ID；啟動要求顯式傳 `base_run_id`，在工作執行期間固定這個值。預設可選目前最新 run，以延續既有流程，但畫面要顯示實際選定的基底。
2. 若選 run，驗證該 run、adapter safetensors、其 `base_model_revision` 對應的 checkpoint 與相容參數；preset 從所選 run 繼承 rank/alpha/dropout/target modules。若選原始基底，不帶 `--resume-adapter`。兩條路都沿用 CLI 現有的 base checkpoint 加可選 adapter 流程。
3. `pending_records(base_run_id, ids)` 解密並建立資料集。只用 `dataset.included_event_ids` 建立關聯。若資料集剔除了無效資料，那些事件仍是可訓練候選。
4. 訓練完成後，維持現有的 GGUF 匯出、量化與檔案驗證。接著在單一 `BEGIN IMMEDIATE` 交易再次驗證父節點、所選事件仍存在且未被該基底祖先使用；插入 run、完整參數及 `lora_run_commits`。任何筆數或關聯檢查失敗即 `ROLLBACK`，不可留下半筆歷程。記錄的是啟動時選定的 parent，不在 worker 中重新讀最新 run。
5. 維持現有「新訓練完成後提示套用，關閉訓練視窗時預設套用」的行為，最新 run 的 GGUF 已在訓練流程產生。確認新模型設定保存/載入成功後，清除其他 run 的 GGUF 與匯出時的 F16 暫存檔；目前套用中的 GGUF 要保留供重啟使用。套用失敗時保留舊模型與舊檔。
6. 只有明確選擇切換到歷史 run 時，若其 GGUF 已清除，才用該 run 的 base revision、vocab/config、adapter 執行 `export-gguf`。先輸出到暫存路徑、驗證，再原子發布及套用；成功後清掉前一個 GGUF。歷史切換需新增依 run ID 重建及套用的 service 操作。
7. 歷史 adapter、其設定檔和可重取得的精確基底 revision 是重建必需品。若對應 checkpoint 已不存在且無法取得，該 run 應標為「無法重建/接續」，而非偷偷改用新版基底。純粹選它作訓練基底時，GGUF 無須先重建。

## 舊資料遷移

以 `PRAGMA user_version` 管理版本，在交易中建立新表/欄位和索引；先備份資料庫，遷移後以 `PRAGMA foreign_key_check` 驗證。舊 run 的 `parent_id` 與基本欄位保留；可從各 run 的 `training_request.json` 補齊參數，但要驗證路徑與內容，缺少檔案的欄位保持未知，不猜測。

舊版 `training_state='trained'` 沒有記錄它屬於哪次 run，而且舊 `mark_trained()` 可在沒有 run 的情況下呼叫；**無法準確反推舊資料的祖先關係**。遷移交易中先把這些事件記入獨立的 `legacy_training_unknown(commit_id)`，再把其 `training_state` 改為 `pending`；候選查詢需排除 `legacy_training_unknown`，以免無意重訓。不要假造與某個 run 的精確關聯。新建立的 run 一律以 `lora_run_commits` 精確判斷。若未來要讓使用者在舊基底重選這些事件，需提供明確的「舊資料歸屬未知，允許重新訓練」操作。原本 `excluded` 的事件維持排除。

「重設對話資料」仍刪除所有 commits 與保護設定，保留歷史 run 與 adapter；`ON DELETE CASCADE` 清除事件關聯，而每次訓練的筆數與參數快照仍留在 run。資料庫遷移不可解密或輸出舊加密欄位。

## 程式切點與驗收

- `training_data_writer.hpp/.cpp`：schema migration、依基底查詢與計數、交易式完成及歷程參數；移除全域 trained 寫入路徑。
- `lora_training_manager.hpp/.cpp`：固定 parent ID、所選 adapter 與 revision、完整 request 入庫、維持新訓練匯出 GGUF、歷史 GGUF 重建與清理。
- `settings_ui_api.h`、`settings_ui_loader.cpp`、`settings_window.cpp`、`ui/lora_dialog.xaml`：傳 run ID/parent ID，增加平面基底選擇與基底感知資料重載。若維持跨版本 DLL 相容，新增版本化 UI API，避免直接改動既有 C ABI 結構大小。
- `training_data_writer_tests.cpp`：覆蓋根節點、祖先排除、兄弟分支重用、回退再訓練、無效/重複 ID 交易回滾、參數完整保存、舊庫遷移、重設資料後的 FK；manager 整合測試覆蓋選定 adapter、缺失基底檔、新訓練預設套用最新模型、歷史切換重建成功/失敗及空間清理。

樹狀 UI 後續只需依 `parent_id` 建立節點，點擊 run ID 即重用這次的基底選擇與歷史模型套用流程。樹完成前，平面歷程不要再用線條表示所有 run 都是直接接續關係。

## 大量 commit 的查詢檢查

- `training_commits` 保留 `INTEGER PRIMARY KEY`、唯一 `event_id` 索引和 `(training_state,id)` 索引；`lora_run_commits` 以複合主鍵的 `WITHOUT ROWID` 減少重複的 rowid B-tree，另設 `(commit_id,run_id)` 供反向查詢與刪除時的外鍵檢查。
- 訓練完成時逐筆依唯一 `event_id` 查詢及驗證，只處理本次選取的 M 筆資料，不掃全部 N 筆 pending。未解鎖的清單只讀事件 ID 與選取旗標，不讀加密內容。舊格式清理按 512 筆分批，完成後用 `user_version=3` 避免每次啟動重掃。
- 以內建 SQLite 做的記憶體合成資料測試：20 萬筆 commit、選取 100 筆，舊查詢 27 ms，逐筆索引查詢 10 ms；20 萬筆 commit、100 層祖先的可用筆數查詢 52 ms。這是單機查詢基準，不代表磁碟或使用者資料庫的固定延遲。
- Release 測試建立 5 萬筆 commit，以 `EXPLAIN QUERY PLAN` 驗證單筆完成查詢使用 `event_id` 索引，並確認新建關聯表採 `WITHOUT ROWID`。查詢計畫檢查不綁定 SQLite 的完整文字輸出。
- 開啟訓練對話框仍需列舉全部候選事件，以供「全部訓練」與勾選；因此記憶體與清單建立時間仍隨候選筆數線性增加。若實際資料量進入百萬級，下一步應將清單改為游標分頁，並由 SQLite 表達全選及少量排除，而非把全部 ID 複製進 UI。
