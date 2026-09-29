# 在 AMD NPU 上使用輸入法

此後端以 Windows ML 取得 AMD 的 `VitisAIExecutionProvider`，並以 ONNX Runtime
GenAI 執行完整 Llama decoder。它不使用 Ryzen AI SDK、XRT API、Python 編譯器或
`ryzenai.json`。

## 套用流程

1. 安裝支援 Ryzen AI 7 350 的 AMD NPU 驅動，並使用 Windows 11 24H2 或更新版本。
2. 正式安裝包已包含 ONNX 模型。自行建置或換模型時，也可將模型目錄放在 GGUF
   旁邊；預設名稱為 `llavon-ime-llama-250m-Q4_K_M-onnx`。
3. 在設定中選擇 **AMD Ryzen AI NPU**，按下套用。

套用期間，服務會啟動 `llavon-ime-npu-compiler.exe`，由它安裝或更新 Windows ML 的
VitisAI EP、建立 ONNX session 並編譯 NPU 專用的 EPContext。成功後，服務才載入
快取並做一次實際 decode。編譯程序的輸出會轉到現有 Debugger；編譯器異常退出時
套用會失敗，既有服務與後端會保留。快取載入與推論仍在服務內執行。
設定畫面的處理中提示會維持到
這些工作全部成功。編譯結果存於：

```text
%LOCALAPPDATA%\Llavon IME\onnx-npu-cache
```

模型或 VitisAI EP 的 DLL 路徑、大小或修改時間改變時會使用新的快取。
Windows ML 未回報 EP 版本時仍可繼續載入。舊快取無法載入時會在同一次套用中
重新編譯。

匯出工具會修正 Constant 的 shape 註記，並以 ONNX checker（載入 ORT 算子 schema）
及嚴格 shape inference 驗證。這些檢查不代表 AMD EP 已能成功編譯該模型。

## ONNX 模型

開發環境可用 `ime-core/tools/export_onnx.py` 從原始 Hugging Face 模型產生模型：

```powershell
python -m pip install onnxruntime-genai==0.15.2 onnxruntime torch transformers onnx onnx-ir
python ime-core/tools/export_onnx.py `
  --output "build/windows/models/llavon-ime-llama-250m-Q4_K_M-onnx"
```

匯出器使用 `MatMulNBits`、32 權重 block、INT4 主體，以及對應 llama.cpp mixed
策略的 INT8 敏感矩陣。NPU 執行時禁止 CPU EP fallback；VitisAI 若不能接下完整
decoder，套用會失敗。

可用 `LLAVON_IME_NPU_MODEL_PATH` 指定其他 ONNX GenAI 模型目錄。

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
