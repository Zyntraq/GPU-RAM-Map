$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $compiler = Get-Command g++.exe -ErrorAction SilentlyContinue
    if (-not $compiler) { throw 'Install MinGW-w64 g++ and put its bin directory on PATH.' }
    $resourceCompiler = Join-Path (Split-Path $compiler.Source) 'windres.exe'
    & $resourceCompiler app.rc -O coff -o app-resource.o
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    & $compiler.Source main.cpp app-resource.o -o GPU-VRAM-Usage.exe -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static -static-libgcc -static-libstdc++ -lpdh -lcomctl32 -lgdi32 -luser32 -lshell32 -ldxgi -s
    if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed.' }
    Write-Host 'Built GPU-VRAM-Usage.exe'
} finally { Pop-Location }
