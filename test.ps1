$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Path tests -Force | Out-Null
    function Get-GpuSample {
        $values = @{}
        $sample = Get-Counter '\GPU Process Memory(*)\Local Usage','\GPU Process Memory(*)\Non Local Usage'
        foreach ($counter in $sample.CounterSamples) {
            if ($counter.Status -ne 0 -or $counter.InstanceName -notmatch '^pid_(\d+)_luid_0x([0-9a-f]+)_0x([0-9a-f]+)_phys_') { continue }
            $processId = [uint32]$Matches[1]
            $adapterId = '{0:x8}{1:x8}' -f [Convert]::ToUInt32($Matches[2],16),[Convert]::ToUInt32($Matches[3],16)
            $kind = if ($counter.Path -like '*\Local Usage') { 'local' } else { 'nonLocal' }
            $key = "$adapterId/$processId/$kind"
            if (-not $values.ContainsKey($key)) { $values[$key] = [uint64]0 }
            $values[$key] += [uint64]$counter.CookedValue
        }
        return $values
    }
    $inventory = Start-Process .\GPU-VRAM-Map.exe -ArgumentList '--adapters tests\adapters.csv' -WindowStyle Hidden -Wait -PassThru
    if ($inventory.ExitCode -ne 0) { throw 'Adapter enumeration failed.' }
    $adapters = @(Import-Csv tests\adapters.csv)
    $stable = 0
    $processRows = 0
    $adapterReports = @()
    foreach ($adapter in $adapters) {
        $before = Get-GpuSample
        $output = "tests\comparison-$($adapter.AdapterId).csv"
        $app = Start-Process .\GPU-VRAM-Map.exe -ArgumentList "--snapshot $output --gpu $($adapter.AdapterId)" -WindowStyle Hidden -Wait -PassThru
        if ($app.ExitCode -ne 0) { throw "Snapshot failed for $($adapter.Name)." }
        $after = Get-GpuSample
        $rows = @(Import-Csv $output)
        $seen = @{}
        $previousBytes = [uint64]::MaxValue
        foreach ($row in $rows) {
            if ($row.AdapterId -ne $adapter.AdapterId) { throw 'Memory from a different GPU leaked into the selected GPU.' }
            $localBytes = [uint64]$row.LocalBytes
            if ($localBytes -gt $previousBytes) { throw 'Rows are not sorted descending.' }
            $previousBytes = $localBytes
            if ($seen.ContainsKey($row.PID)) { throw 'Duplicate PID within selected GPU.' }
            $seen[$row.PID] = $true
            foreach ($kind in @('local','nonLocal')) {
                $key = "$($adapter.AdapterId)/$($row.PID)/$kind"
                $actual = if ($kind -eq 'local') { $localBytes } else { [uint64]$row.NonLocalBytes }
                if (-not $before.ContainsKey($key) -and -not $after.ContainsKey($key)) { throw "Unexpected adapter/PID counter: $key" }
                if ($before.ContainsKey($key) -and $after.ContainsKey($key) -and $before[$key] -eq $after[$key]) {
                    if ($actual -ne $before[$key]) { throw "Counter mismatch for $key : app=$actual Windows=$($before[$key])" }
                    ++$stable
                }
            }
        }
        foreach ($key in $before.Keys) {
            if ($key -notlike "$($adapter.AdapterId)/*" -or -not $after.ContainsKey($key)) { continue }
            if ($before[$key] -gt 0 -and $after[$key] -eq $before[$key]) {
                $processIdText = ($key -split '/')[1]
                if (-not $seen.ContainsKey($processIdText)) { throw "Missing stable nonzero process: $key" }
            }
        }
        $processRows += $rows.Count
        $adapterReports += "$($adapter.Name): $($rows.Count) process rows"
    }
    if ($stable -lt 5) { throw 'Too few stable counter readings to verify; rerun with less GPU activity.' }
    $bad = Start-Process .\GPU-VRAM-Map.exe -ArgumentList '--snapshot tests\invalid-gpu.csv --gpu ffffffffffffffff' -WindowStyle Hidden -Wait -PassThru
    if ($bad.ExitCode -eq 0) { throw 'Invalid adapter was accepted.' }
    $ui = Start-Process .\GPU-VRAM-Map.exe -ArgumentList '--ui-test tests\gui-test.txt' -WindowStyle Hidden -Wait -PassThru
    if ($ui.ExitCode -ne 0) { throw (Get-Content tests\gui-test.txt -Raw) }
    Add-Type -AssemblyName System.Drawing
    foreach ($capture in (Get-ChildItem tests\gui-test.txt*.bmp)) {
        $bmp = [System.Drawing.Image]::FromFile($capture.FullName)
        $pngPath = if ($capture.Name -eq 'gui-test.txt.bmp') { Join-Path $PWD 'tests\gui-preview.png' } else { [System.IO.Path]::ChangeExtension($capture.FullName, '.png') }
        try { $bmp.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $bmp.Dispose() }
    }
    $report = "PASS: $($adapters.Count) named GPUs; $processRows process rows; $stable stable per-GPU readings match Windows counters exactly; filtering, sorting, completeness, unique PIDs and invalid adapter rejection verified.`r`n" + ($adapterReports -join "`r`n") + "`r`n" + (Get-Content tests\gui-test.txt -Raw)
    $report | Set-Content tests\verification.txt
    Write-Host $report
} finally { Pop-Location }
