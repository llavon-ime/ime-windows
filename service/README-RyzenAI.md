# 在 AMD NPU 上使用輸入法

此後端以 Windows ML 取得 AMD 的 `VitisAIExecutionProvider`，並以 ONNX Runtime
GenAI 執行 Llama decoder。推論端不需要 Ryzen AI SDK 或 Python；Python 僅用於
建置時匯出模型。

## 套用流程

1. 安裝支援 Ryzen AI 7 350 的 AMD NPU 驅動，並使用 Windows 11 24H2 或更新版本。
2. 正式安裝包已包含 ONNX 模型。自行建置或換模型時，也可將模型目錄放在 GGUF
   旁邊；預設名稱為 `llavon-ime-llama-250m-Q4_K_M-onnx`。
3. 在設定中選擇 **AMD Ryzen AI NPU**，按下套用。

套用期間，服務會啟動 `llavon-ime-npu-compiler.exe`，由它安裝或更新 Windows ML 的
VitisAI EP、建立 ONNX session 並編譯 NPU 專用的 EPContext。成功後，服務才載入
快取。編譯器會重新載入落盤快取，驗證 prefill、decode 和回退重算，服務再做一次
實際 decode。編譯程序的輸出會轉到現有 Debugger；編譯器異常退出時
套用會失敗，既有服務與後端會保留。快取載入與推論仍在服務內執行。
設定畫面的處理中提示會維持到
這些工作全部成功。編譯結果存於：

```text
%LOCALAPPDATA%\Llavon IME\onnx-npu-cache
```

模型或 VitisAI EP 的 DLL 路徑、大小或修改時間改變時會使用新的快取。
目前模型與快取必須位於同一磁碟，以符合 GenAI 的相對路徑載入方式。
Windows ML 未回報 EP 版本時仍可繼續載入。舊快取無法載入時會在同一次套用中
重新編譯。

匯出工具會修正 Constant 的 shape 註記，並以 ONNX checker（載入 ORT 算子 schema）
及嚴格 shape inference 驗證。這些檢查不代表 AMD EP 已能成功編譯該模型。

## ONNX 模型

開發環境可用 `ime-core/tools/export_onnx.py` 從原始 Hugging Face 模型產生模型：

```powershell
python -m pip install -r ime-core/tools/requirements-npu-export.txt
python -m pip install --no-deps --index-url https://pypi.amd.com/simple model-generate==1.5.1
python ime-core/tools/export_onnx.py `
  --output "build/windows/models/llavon-ime-llama-250m-Q4_K_M-onnx"
```

匯出器使用非對稱 INT4、128 權重 block，再執行 AMD 的 BF16 與 SSMLP 轉換。
目前支援此模型的 16-head、64-dimension 架構，透過等價的 head 複製與輸出切片
配合 AMD 32-head attention kernel。權重與原模型的 hidden size 不變。
`npu_model.json` 用於區別完成轉換的模型；舊版混合 INT4/INT8 模型必須重新匯出。

矩陣乘法、融合 MLP 和 attention 使用 NPU。CPU 處理 shape、型別轉換、embedding
與 AMD attention 的呼叫入口，因此不能禁止所有 CPU EP 節點。服務要求成功產生
並重載 VitisAI EPContext，避免把純 CPU 執行當成 NPU 成功。

刪字時只回退 KV cache 的有效長度，保留前綴。AMD attention 不支援在既有 cache
後一次補入多個 token，因此最多 4 個 token 的補字會逐個執行；更多 token 才重算
完整前綴，以免大量逐 token 呼叫更慢。清空輸入時會釋放該 session 的 cache。

可用 `LLAVON_IME_NPU_MODEL_PATH` 指定其他 ONNX GenAI 模型目錄。

## 硬體驗證

2026-09-29 在 Ryzen AI 7 H 350、NPU 驅動 `32.0.203.314`、Windows ML AMD EP
`1.8.75.0` 實測：61 個 NPU 子圖，包含 41 個矩陣投影與 20 個融合 MLP；attention
由 AMD custom op 呼叫 NPU。使用相同 INT4 權重的 CPU 參考模型，在長度
3、17、255、256、383 上比較 prefill、decode、截尾改字與清空重輸入，共 35 組。
第一候選 35/35 相同，最低 logits 餘弦相似度 0.996361。這不代表與 GGUF Q4_K_M
量化結果逐位相同。

建置 `npu-hardware-tests` 後，提供 NPU 模型、編譯快取及 AMD 轉換前的 CPU INT4
參考模型目錄執行。此測試需要實體 NPU，不加入一般 CTest。

## 診斷

錯誤會直接出現在 Llavon IME Debugger 的 **Diagnostics** 頁。`[NPU]` 訊息會顯示
ONNX 模型目錄、快取目錄、VitisAI DLL 路徑與版本（若有回報），以及正在載入快取
或重新編譯。

請在套用前開啟 Debugger。編譯程序的 stdout/stderr 會以 `[NPU compiler]` 逐行轉送；
異常退出時，套用錯誤也會保留最後 8 KiB 輸出與退出碼。若程序沒有輸出，錯誤會明確標示。
`0xC0000409` 本身不足以辨認是哪個 CHECK 失敗，需連同編譯器的原始訊息判斷。

- [Windows ML execution providers](https://learn.microsoft.com/windows/ai/new-windows-ml/initialize-execution-providers)
- [AMD VitisAI EP for Windows ML](https://ryzenai.docs.amd.com/projects/WinML/en/latest/winml_ep.html)
- [ONNX Runtime GenAI model builder](https://onnxruntime.ai/docs/genai/howto/build-model.html)
