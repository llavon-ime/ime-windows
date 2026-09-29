param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$Model,
    [Parameter(Mandatory)][string]$Tables,
    [Parameter(Mandatory)][string]$Artifacts,
    [string]$Device = 'Vulkan0'
)
$ErrorActionPreference = 'Stop'
$Probe = (Resolve-Path -LiteralPath $Probe).Path
$Model = (Resolve-Path -LiteralPath $Model).Path
$Tables = (Resolve-Path -LiteralPath $Tables).Path
$Artifacts = [IO.Path]::GetFullPath($Artifacts)
New-Item -ItemType Directory -Path $Artifacts -Force | Out-Null

function Invoke-Probe([string]$Name, [string]$Cache, [string]$Expected) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Probe
    $info.Arguments = '"{0}" "{1}" "{2}" --load-only' -f $Model, $Tables, $Device
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['GGML_VK_PIPELINE_CACHE_DIR'] = $Cache
    $info.EnvironmentVariables.Remove('GGML_VK_PIPELINE_CACHE_DISABLE')
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    if (-not $process.Start()) { throw 'Cannot start Vulkan probe' }
    $outputTask = $process.StandardOutput.ReadToEndAsync()
    $errorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) {
        $process.Kill()
        throw "Probe timed out: $Name"
    }
    $output = $outputTask.GetAwaiter().GetResult()
    $errorText = $errorTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $Artifacts "$Name.log"), $output)
    [IO.File]::WriteAllText((Join-Path $Artifacts "$Name.err"), $errorText)
    $load = [regex]::Match($output, 'LOAD_MS,([0-9.]+)')
    if ($process.ExitCode -ne 0 -or $errorText -notmatch $Expected -or -not $load.Success) {
        throw "Probe failed: $Name (exit $($process.ExitCode))"
    }
    $loadMs = [double]$load.Groups[1].Value
    $process.Dispose()
    Write-Output ([pscustomobject]@{ case=$Name; load_ms=$loadMs; status='passed' })
}

# A unique directory makes the first run cold without deleting existing data.
$cache = Join-Path $Artifacts ([Guid]::NewGuid().ToString('N'))
Invoke-Probe 'cold' $cache '\[VK_CACHE\] saved bytes='
$file = Get-ChildItem -LiteralPath $cache -Filter '*.bin' | Select-Object -First 1
if (-not $file) { throw 'No pipeline cache written after model preparation' }
$valid = [IO.File]::ReadAllBytes($file.FullName)
Invoke-Probe 'restart' $cache '\[VK_CACHE\] loaded bytes='

foreach ($case in 'truncated', 'payload-corrupt', 'driver-mismatch', 'uuid-mismatch') {
    $bytes = [byte[]]$valid.Clone()
    switch ($case) {
        'truncated' { $bytes = [byte[]]$bytes[0..15] }
        'payload-corrupt' { $bytes[$bytes.Length-1] = $bytes[$bytes.Length-1] -bxor 1 }
        'driver-mismatch' { $bytes[20] = $bytes[20] -bxor 1 }
        'uuid-mismatch' { $bytes[32] = $bytes[32] -bxor 1 }
    }
    [IO.File]::WriteAllBytes($file.FullName, $bytes)
    Invoke-Probe $case $cache '\[VK_CACHE\] miss bytes=0'
}
Invoke-Probe 'repaired-restart' $cache '\[VK_CACHE\] loaded bytes='

# A file in place of the directory must not prevent model loading.
$blocked = Join-Path $Artifacts ('blocked-' + [Guid]::NewGuid().ToString('N'))
[IO.File]::WriteAllText($blocked, 'not a directory')
Invoke-Probe 'unwritable-path' $blocked '\[VK_CACHE\] save failed; inference remains available'
