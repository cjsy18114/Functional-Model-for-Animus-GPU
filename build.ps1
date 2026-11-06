param([string]$Compiler = 'g++')
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    New-Item -ItemType Directory -Path 'build' -Force | Out-Null
    $sources = @('src/common.cpp', 'src/memory.cpp', 'src/loader.cpp', 'src/decode.cpp', 'src/engine.cpp', 'src/trace.cpp', 'src/exec_integer.cpp', 'src/exec_memory.cpp', 'src/exec_simt.cpp')
    $flags = @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Iinclude', '-g')
    & $Compiler @flags @sources 'tests/model_tests.cpp' '-o' 'build/animus_model_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }
    & $Compiler @flags @sources 'tests/stage_b_tests.cpp' '-o' 'build/animus_model_tests_b.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Stage B test build failed.' }
    & $Compiler @flags @sources 'src/main.cpp' '-o' 'build/animus_model_cli.exe'
    if ($LASTEXITCODE -ne 0) { throw 'CLI build failed.' }
    # MinGW runtime DLLs are normally next to the selected compiler.
    $compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
    $oldPath = $env:PATH
    try {
        $env:PATH = (Split-Path $compilerPath) + ';' + $oldPath
        & '.\build\animus_model_tests.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Stage A tests failed.' }
        & '.\build\animus_model_tests_b.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Stage B tests failed.' }
    } finally { $env:PATH = $oldPath }
} finally { Pop-Location }
