# TSF 前端 DLL 卸載流程

## 範圍

`llavon-ime.dll` 是 in-process COM server。每個載入輸入法的應用程式各自擁有一份
DLL mapping、COM objects 與 module reference，因此卸載判定必須在各宿主程序內成立。

## 卸載流程總覽

```text
ITfInputProcessorProfileMgr::DeactivateProfile
  ↓
TSF 呼叫 TextService::Deactivate
  ↓
解除 composition、UI、sinks、ITfThreadMgr references
  ↓
TSF 釋放 ITfTextInputProcessorEx 與 class factory
  ↓
Globals::dll_ref_count 歸零
  ↓
ITfInputProcessorProfileMgr::ReleaseInputProcessor(
    CLSID,
    TF_RIP_FLAG_FREEUNUSEDLIBRARIES)
  ↓
COM 呼叫 DllCanUnloadNow
  ├─ S_FALSE：仍有 DLL-owned COM object，保留 DLL
  └─ S_OK：COM 可以 FreeLibrary，發生 DLL_PROCESS_DETACH
```

`DllCanUnloadNow` 只回答 DLL 是否可卸載，本身不會執行卸載。實際卸載由 COM 在
`CoFreeUnusedLibrariesEx` 流程中完成；`TF_RIP_FLAG_FREEUNUSEDLIBRARIES` 會要求
`ReleaseInputProcessor` 進入這個流程。

## 1. 要求 TSF 停用 profile

由 DLL 以外、生命週期不依賴 `llavon-ime.dll` 的協調者取得
`ITfInputProcessorProfileMgr`，再呼叫：

```cpp
profile_mgr->DeactivateProfile(
    TF_PROFILETYPE_INPUTPROCESSOR,
    tsf::Globals::tsf_language_id,
    tsf::Globals::text_service_clsid,
    tsf::Globals::text_service_profile_guid,
    nullptr,
    TF_IPPMF_FORSESSION);
```

`TF_IPPMF_FORSESSION` 表示停用目前 desktop 中各 thread 使用的指定 profile。呼叫者不能
假設所有宿主都會同步完成；實際是否已卸載仍須以各程序的 module 狀態判定。

不要在 `DllMain` 內執行 TSF、COM 或同步等待操作，也不要由 DLL 在自己的 call stack
仍執行時直接呼叫 `FreeLibrary`。

## 2. `TextService::Deactivate` 釋放 TSF 狀態

TSF 停用 profile 後會呼叫 `ITfTextInputProcessor::Deactivate()`。目前實作委派給
`TextService::deactivate()`，清理順序如下：

1. `unadvise_text_edit_sink()`，解除目前 context 的 edit sink。
2. 隱藏 candidate UI。
3. 若 composition 存在，呼叫 `EndComposition` 並清除 composition state。
4. 從 language bar 移除 `InputModeLangBarItem`，然後釋放該 COM object。
5. 呼叫 `ITfKeystrokeMgr::UnadviseKeyEventSink`。
6. 呼叫 `ITfSource::UnadviseSink` 解除 thread-manager event sink。
7. 釋放 `ITfThreadMgr`。
8. 將 client id 重設為 `TF_CLIENTID_NULL`。
9. detach candidate UI，釋放它保存的 thread-manager reference。

相關實作：

- `frontend/src/tsf/textService.cpp` 的 `TextService::Deactivate()` 與
  `TextService::deactivate()`。
- `frontend/src/tsf/candidateUiController.cpp` 的 `CandidateUiController::detach()`。

`Deactivate()` 必須可重複呼叫且不能留下 callback、sink 或 background work 指向 DLL
內的程式碼。TSF 釋放 `TextService` 前必須完成這些清理。

## 4. 要求 COM 釋放 input processor 與 DLL

完成 profile deactivation 後，由協調者呼叫：

```cpp
profile_mgr->ReleaseInputProcessor(
    tsf::Globals::text_service_clsid,
    TF_RIP_FLAG_FREEUNUSEDLIBRARIES);
```

此呼叫會要求 TSF 釋放指定 CLSID 的 `ITfTextInputProcessorEx` instance，並透過
`CoFreeUnusedLibrariesEx` 查詢可卸載的 COM DLL。

只有下列條件全部成立時，`DllCanUnloadNow()` 才應回傳 `S_OK`：

- `TextService` 已完成 `Deactivate()`。
- TSF 已釋放 `TextService` instance。
- class factory 沒有 outstanding reference 或 `LockServer(TRUE)`。
- candidate UI、language bar item 與其他 DLL-owned COM objects 都已解構。
- 沒有仍會執行 DLL 程式碼的 callback、thread 或非同步工作。

若回傳 `S_FALSE`，COM 會保留 DLL；這是正常且安全的結果，不能以強制
`FreeLibrary` 覆蓋 reference/lifetime 判定。

## 5. 已驗證行為

2026-09-21 使用目前建置的 `llavon-ime.dll` 做過最小 COM host 測試，結果如下：

```text
無 COM object                         DllCanUnloadNow = S_OK
IClassFactory 存活                    DllCanUnloadNow = S_FALSE
TextService 存活、factory 已釋放       DllCanUnloadNow = S_FALSE
所有 COM objects 已釋放               DllCanUnloadNow = S_OK
透過 CoGetClassObject 載入後執行
CoFreeUnusedLibrariesEx(0, 0)          DLL 已從測試程序卸載
```

同一測試若以 `CoLoadLibrary` 直接載入 DLL，則不等同正常 COM activation 管理的載入；
即使 `DllCanUnloadNow()` 回傳 `S_OK`，一次 `CoFreeUnusedLibrariesEx` 也未卸載該直接載入
reference。因此卸載測試必須使用實際的 COM／TSF activation 路徑，不能只用
`LoadLibrary` 模擬。

## 參考

- [ITfInputProcessorProfileMgr::DeactivateProfile](https://learn.microsoft.com/windows/win32/api/msctf/nf-msctf-itfinputprocessorprofilemgr-deactivateprofile)
- [ITfInputProcessorProfileMgr::ReleaseInputProcessor](https://learn.microsoft.com/windows/win32/api/msctf/nf-msctf-itfinputprocessorprofilemgr-releaseinputprocessor)
- [DllCanUnloadNow](https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-dllcanunloadnow)
- [CoFreeUnusedLibrariesEx](https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-cofreeunusedlibrariesex)
