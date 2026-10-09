# GPU RAM Map

Double-click **GPU-VRAM-Usage.exe**. This is a portable 64-bit C++ Windows app; no installer or extra runtime files are needed.

Select a GPU by name in the dropdown. The table shows **only that GPU's** process memory usage and refreshes every two seconds. It shows process names, PIDs, **VRAM / local** and **RAM / non-local** memory, in MiB. The row below the list totals each memory column for the listed processes on the selected GPU. Click any column heading to sort; click it again to reverse the order. The initial sort is highest local memory first, and your chosen sort stays active when changing GPUs. Changing the GPU immediately filters the latest sample and requests a fresh reading. Zero-use processes are omitted. Click Refresh for an immediate sample. Resize the window or scroll to see more rows.

## Requirements and interpretation

- Windows 10/11 with GPU Process Memory performance counters and a compatible WDDM driver. It normally runs without administrator rights.
- The app reads `GPU Process Memory\Local Usage` and `Non Local Usage`. On discrete GPUs, local memory is VRAM and non-local memory is system RAM. On integrated/UMA GPUs, local memory is also system RAM. Microsoft explains the distinction here: https://learn.microsoft.com/en-us/windows/win32/direct3d12/memory-management-strategies
- Values are kept separate by adapter LUID and process ID, and DXGI supplies the adapter names. Multiple physical nodes within one adapter are combined. The dropdown includes only DXGI-enumerated hardware adapters; performance-counter LUIDs without a matching hardware adapter are ignored. The first DXGI hardware adapter is selected at startup; this list's order does not claim to match Task Manager's GPU numbers.
- Windows can attribute the same allocation to multiple processes, so adding process values does not give the card's total physical memory consumption. Integrated GPU local memory is system RAM, not discrete VRAM.
- An unavailable or exited process name falls back to its PID. Counter failures are shown in the window and retried on subsequent refreshes.
- The original version used `Dedicated Usage`. On this machine that counter reported Firefox PID 16336 as 5,133 MiB, while `Local Usage` reported approximately 301 MiB, consistent with the user's Task Manager reading. The updated version uses local usage. Matching an incorrect counter with PowerShell did not validate the original metric's accuracy.
- Process local usage and overall adapter allocated VRAM describe different accounting scopes; they need not sum to the same amount. HWiNFO's overall allocated VRAM is not a per-process residency total. These remain Windows-reported values; driver accounting and shared allocations can affect interpretation.
- GPU memory reporting background: https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/

## Build

Source is in main.cpp. With MinGW-w64 g++ and windres on PATH, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Only the resulting EXE is needed to run the app. app.manifest and app.rc are build inputs.

## Verification / CSV snapshot

Run `powershell -ExecutionPolicy Bypass -File .\test.ps1` to compare the app with independent Windows PowerShell GPU counters and run the GUI checks. Results and a preview image are saved under tests.

If Windows' performance-counter API is temporarily unavailable, `--ui-test-fixture report.txt` tests the real window with changing sample values across available GPUs, including both total columns, sorting, refresh, and resizing. This fixture check does not validate live GPU readings.

```powershell
Start-Process .\GPU-VRAM-Usage.exe -ArgumentList '--snapshot snapshot.csv' -Wait
Start-Process .\GPU-VRAM-Usage.exe -ArgumentList '--adapters adapters.csv' -Wait
# Use an AdapterId from adapters.csv to request a specific GPU:
Start-Process .\GPU-VRAM-Usage.exe -ArgumentList '--snapshot snapshot.csv --gpu 0000000000011b4f' -Wait
Start-Process .\GPU-VRAM-Usage.exe -ArgumentList '--ui-test gui-test.txt' -Wait
```

Snapshot mode defaults to the first adapter and writes UTF-8 CSV with exact byte counts in `LocalBytes` and `NonLocalBytes`, plus `AdapterId`. Adapter identifiers can change after reboot. UI test mode opens the real window, checks three live updates, switches through every GPU, verifies filtered names, values, PIDs, descending sort and empty lists, exercises resize and manual refresh, saves client-area BMPs, then closes. Exit code 0 means success; 1 means failure. Output paths are relative to the current working directory unless absolute paths are supplied. Restart the app after adding/removing hardware to refresh adapter names.
