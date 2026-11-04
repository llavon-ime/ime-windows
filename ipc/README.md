# 共用後端的 Windows named pipe

同一個 Windows 登入工作階段只啟動一個模型後端；不同帳號透過 pipe 使用既有後端。
保留 session pipe 名稱與 Local\\LlavonImeServiceInstance 單一實例鎖，
不依 user SID 啟動另一份模型。不同桌面工作階段仍沿用原本各自一份的設計。

## 採用的實作參考

- [azooKey-Windows](https://github.com/fkunn1326/azooKey-Windows/blob/master/crates/server/src/lib.rs)：
  MIT；放行本機 Users（BU）、Administrators（BA）、SYSTEM（SY）、AppContainer（AC）、
  restricted code（RC），並設定 Low integrity label。此專案保留 LPAC 授權。
- [Microsoft PowerToys](https://github.com/microsoft/PowerToys/blob/main/src/common/interop/two_way_pipe_message_ipc.cpp)：
  MIT；client 使用明確資料／屬性權限，server 首次宣告名稱後，在交出已接受的連線前
  先建立替補 listener，維持名稱持續被持有。
- [Google Mozc](https://github.com/google/mozc/blob/master/src/base/win32/win_sandbox.cc)：
  BSD 3-Clause；明確 owner／group、OW ACE、mutex，以及 client 的 SQOS。
  此專案對 BU 擴充跨帳號權限。

授權聲明位於 AZOOKEY-LICENSE.txt、POWERTOYS-LICENSE.txt、MOZC-LICENSE.txt，
並隨安裝套件放入 licenses。

## 權限

| 物件 | Owner／SYSTEM／Administrators | Users／AC／LPAC／RC |
| --- | --- | --- |
| Pipe | GA | GRGW；保留舊版 generic read/write client 相容性 |
| 啟動／單一實例 mutex | GA | GX；開啟 handle 只要求 SYNCHRONIZE |

新版 pipe client 以明確資料／屬性權限開啟，不要求 FILE_CREATE_PIPE_INSTANCE。
Server ACL 仍授予 GRGW，因為更新後既有 SearchHost 等 TSF 宿主可能繼續載入舊 DLL，
使用 GENERIC_READ／GENERIC_WRITE。若 ACL 只授予新版 client 要求的部分權限，
舊版 AppContainer client 會得到 ERROR_ACCESS_DENIED；不能要求所有宿主同步更新。
Windows 的 FILE_GENERIC_WRITE 包含與 FILE_CREATE_PIPE_INSTANCE 同位元的
FILE_APPEND_DATA，因此此相容性授權也包含建立 instance 的權限。
一般 client 不會取得 WRITE_DAC、WRITE_OWNER。
BU 是跨帳號一般使用者的 allow ACE，因此未提權管理員即使 BA 是 deny-only，
仍能透過 enabled BU 取得 client 權限。RC／package ACE 滿足額外的限制檢查。
沒有匹配 allow SID 的 token 仍會被拒絕。

Pipe 和 mutex 採 S:(ML;;NW;;;LW)。Low IL client 可以寫入；Untrusted IL 仍被拒絕。
單一實例 mutex 也採此政策，避免其他帳號看到 ACCESS_DENIED 而無法正確辨識既有後端。
首次建立 mutex 時以 CREATE_MUTEX_INITIAL_OWNER 原子取得所有權；開啟既有 mutex 的
ERROR_ALREADY_EXISTS 會保留給單一實例判斷。

Client 直接開啟 session pipe，不查詢 server 程序或比對執行檔路徑，
也不以宿主帳號和 pipe owner 相等作為連線條件。
連線時限制 impersonation 層級，並拒絕遠端 client。

只有本程式建立的 pipe／mutex 會設定權限。
沒有更改執行檔 ACL、Registry、帳號、UAC 或系統安全政策。
放行 BU 是明確的共用選擇：符合權限的本機帳號可以向同一後端送出協定命令，
請求與訓練資料仍由執行後端的帳號儲存。

## 避免後端活著卻永久無法接受連線

- 首次建立 pipe 使用 FILE_FLAG_FIRST_PIPE_INSTANCE，已有其他 server 時保留錯誤並重試，
  不默默加入其 pipe。
- 接受 client 後先建立替補 instance，再交給 client handler；立即斷線不會釋放名稱。
- 建立或交接失敗保留 listener，每 250 ms 以非同步 timer 重試，不退出監聽或重載模型。
- ConnectNamedPipe 的同步成功、ERROR_PIPE_CONNECTED 與 ERROR_IO_PENDING 都會處理；
  等待完成後檢查實際 I/O 結果。取消或 context 銷毀時，先取消並收完 pending I/O，
  再釋放 OVERLAPPED 與 event。
- 個別 client 處理失敗不影響下一條 listener。預測／候選 listener 的例外會記錄並重試。
- 前端以 OutputDebugStringW 記錄 CreateFileW 失敗的錯誤碼、PID、access、flags 及 pipe 名稱。

這些恢復路徑不能克服持續的資源耗盡、其他程序長期占用名稱、限制更嚴格的 sandbox
或系統政策；相關失敗會被記錄並重試。更新後須重啟 backend 和既有宿主，讓新 ACL、
存取權限與 client 連線程式碼生效。

## 驗證

- pipe-security：Authz 模型涵蓋不同帳號 BU、未提權／已提權管理員、SYSTEM、
  AC／LPAC／RC、舊版 generic read/write 權限，以及拒絕 client 修改 ACL。
  Win32 實測 Low IL 預測與單向候選 pipe 讀寫、共用 mutex 與單一實例競爭。
- service-pipe-listener：直接測試服務使用的 async accept helper，注入四次建立失敗，
  再確認立即斷線的 client 不會阻止後續五次請求／回應。
- pipe-appcontainer：透過 NtCreateLowBoxToken 與 CreateProcessAsUserW 啟動真正的
  AppContainer 子程序，父子均確認 TokenIsAppContainer。比較舊／新 server ACL
  與舊／新 client 的四種組合，實際測試雙向 overlapped、單向同步與單向 overlapped
  讀寫；新版 client 直接使用前端的 connect_service_pipe。另確認移除 package grant
  後，即使 Everyone／Low label 允許，真正的 AppContainer 仍被拒絕。
- logger-transport 與 frontend-dll-load 檢查既有 transport 與 DLL 載入。

測試僅使用測試程序 PID 命名的暫存物件、降低權限的 token，以及測試程式自己的
AppContainer／一般子程序；不建立帳號或 AppContainer profile、不寫 Registry、
不安裝輸入法。Authz 不等於實際跨帳號登入；AppContainer 子程序也不等於 SearchHost
本體。實際 UAC 與使用者回報 app 仍需現場驗證，不能將這些測試解讀為所有
Windows 環境的可用性保證。

## Microsoft 文件

- [Named Pipe Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
- [Mandatory Integrity Control](https://learn.microsoft.com/en-us/windows/win32/secauthz/mandatory-integrity-control)
- [SID Attributes in an Access Token](https://learn.microsoft.com/en-us/windows/win32/secauthz/sid-attributes-in-an-access-token)
- [Restricted Tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens)
- [NtCreateLowBoxToken](https://learn.microsoft.com/en-us/windows/win32/secauthz/ntcreatelowboxtoken)
