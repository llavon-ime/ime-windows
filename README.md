# Llavon 輸入法（Windows）

Llavon 是一套以語言模型輔助選字的 Windows 注音輸入法。目前專案仍在開發階段，主要包含 Windows TSF 輸入法前端、背景推論服務，以及用來產生 MSI 安裝檔的建置設定。

## 專案組成

- `ime-windows-frontend`：Windows TSF 輸入法前端，負責組字、候選字介面與輸入法註冊。
- `ime-service`：載入 GGUF 模型並提供候選字推論的背景服務。
- `ime-windows-settings`：設定介面的原型，目前尚未接入頂層建置與實際設定功能。
- `cmake`：模型下載、WiX 安裝檔與第三方授權檔案的建置腳本。

前端與背景服務透過 Windows Named Pipe `\\.\pipe\llavon-ime` 通訊。

## 模型

本專案預設使用 Hugging Face 上的 [`llavon-ime-llama-250m-Q4_K_M.gguf`](https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/blob/main/llavon-ime-llama-250m-Q4_K_M.gguf)。建立 MSI 安裝檔時，建置系統會自動下載這個模型；其他版本與相關資訊可在 [`llavon-ime-llama-250m-GGUF`](https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF) 模型頁查看。

模型權重另依 [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/deed.zh-hant) 授權，僅限非商業用途，使用或散布時須註明來源；此授權與本專案程式碼的 BSD 2-Clause License 分開適用。

## 安裝

可從本專案的 [最新版本](../../releases/tag/latest) 下載 MSI 安裝檔。安裝程式會註冊輸入法並設定背景服務隨 Windows 啟動；安裝完成後，可至 Windows 的語言與鍵盤設定加入 Llavon 輸入法。

> 本專案仍在早期開發階段，功能、操作方式及安裝流程都可能變動。

## 從原始碼建置

建置環境需要：

- Windows 10 或更新版本（x64）
- Visual Studio 2022 Build Tools，並安裝「使用 C++ 的桌面開發」工作負載
- CMake 3.30 或更新版本
- .NET 8 SDK
- Git

先取得原始碼與子模組：

```powershell
git clone --recurse-submodules https://github.com/llavon-ime/ime-windows.git
cd ime-windows
./vcpkg/bootstrap-vcpkg.bat -disableMetrics
```

建置各元件並將產物輸出至 `dist`：

```powershell
cmake --preset windows
cmake --build --preset windows --parallel
```

產生 MSI 安裝檔：

```powershell
cmake --build --preset package --parallel
```

完成後，安裝檔位於：

```text
build/windows/llavon-ime-0.1.0-windows.msi
```

首次打包時會從網路下載模型、vcpkg 相依套件及 WiX 工具，因此需要可用的網路連線。更完整的打包與執行路徑說明請參考 [PACKAGING.md](PACKAGING.md)。

## 授權

本專案依 [BSD 2-Clause License](LICENSE) 授權。安裝檔也會一併收錄所使用之第三方套件授權資訊。
