param([Parameter(Mandatory = $true)][int]$TargetProcessId)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class NativeDialogCapture {
    public delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr window, StringBuilder name, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, IntPtr wparam, StringBuilder text, uint flags, uint timeout, out IntPtr result);

    public static string ReadText(IntPtr window) {
        var text = new StringBuilder(4096);
        IntPtr result;
        SendMessageTimeoutW(window, 0x000D, new IntPtr(text.Capacity), text, 0x0002, 500, out result);
        if (text.Length == 0) GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }
    public static string ReadClass(IntPtr window) {
        var name = new StringBuilder(256);
        GetClassNameW(window, name, name.Capacity);
        return name.ToString();
    }
}
'@

$windows = [System.Collections.Generic.List[object]]::new()
[NativeDialogCapture]::EnumWindows({
    param($window, $parameter)
    [uint32]$owner = 0
    [void][NativeDialogCapture]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -ne $TargetProcessId) { return $true }
    $children = [System.Collections.Generic.List[object]]::new()
    [NativeDialogCapture]::EnumChildWindows($window, {
        param($child, $unused)
        $children.Add([pscustomobject]@{
            Class = [NativeDialogCapture]::ReadClass($child)
            Text = [NativeDialogCapture]::ReadText($child)
        })
        return $true
    }, [IntPtr]::Zero) | Out-Null
    $windows.Add([pscustomobject]@{
        Handle = $window.ToInt64()
        ProcessId = $owner
        Class = [NativeDialogCapture]::ReadClass($window)
        Title = [NativeDialogCapture]::ReadText($window)
        Children = $children
    })
    return $true
}, [IntPtr]::Zero) | Out-Null
$windows | ConvertTo-Json -Depth 5
