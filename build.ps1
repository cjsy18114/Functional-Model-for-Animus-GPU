param([string]$Compiler = 'g++')
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    $compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
    $compilerDirectory = Split-Path $compilerPath
    $cCompiler = Join-Path $compilerDirectory 'gcc.exe'
    if (-not (Test-Path -LiteralPath $cCompiler)) { throw 'This script requires gcc.exe next to g++.exe; other toolchains can use CMake.' }
    New-Item -ItemType Directory -Path 'build/softfloat-objects', 'build/model-objects' -Force | Out-Null
    $sfRoot = Join-Path $PSScriptRoot 'third_party/softfloat'
    $sfSources = @(Get-Content -LiteralPath (Join-Path $sfRoot 'sources.txt') | ForEach-Object { Join-Path $sfRoot $_ })
    $sfIncludes = @('-I', $sfRoot, '-I', (Join-Path $sfRoot 'source/include'), '-I', (Join-Path $sfRoot 'source/8086-SSE'))
    Push-Location -LiteralPath 'build/softfloat-objects'
    try {
        & $cCompiler '-std=c11' '-O2' '-DSOFTFLOAT_FAST_INT64' @sfIncludes '-c' @sfSources
        if ($LASTEXITCODE -ne 0) { throw 'SoftFloat C build failed.' }
    } finally { Pop-Location }
    $sourceNames = @('common', 'memory', 'loader', 'decode', 'engine', 'trace', 'exec_integer',
                     'exec_memory', 'exec_simt', 'scheduler', 'exec_fp', 'fp_backend', 'exec_csr', 'exec_graphics',
                     'resources', 'command', 'raster', 'draw')
    $sources = @($sourceNames | ForEach-Object { Join-Path $PSScriptRoot ('src/' + $_ + '.cpp') })
    $flags = @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-g', '-ffp-contract=off', '-frounding-math', '-DSOFTFLOAT_FAST_INT64',
               '-I', (Join-Path $PSScriptRoot 'include'), '-I', (Join-Path $sfRoot 'source/include'))
    Push-Location -LiteralPath 'build/model-objects'
    try {
        & $compilerPath @flags '-c' @sources
        if ($LASTEXITCODE -ne 0) { throw 'Model C++ build failed.' }
    } finally { Pop-Location }
    # Use the explicit manifest, not a wildcard, so old build artifacts cannot enter a new link.
    $objects = @($sourceNames | ForEach-Object { Join-Path $PSScriptRoot ('build/model-objects/' + $_ + '.o') })
    $objects += @($sfSources | ForEach-Object { Join-Path $PSScriptRoot ('build/softfloat-objects/' + [IO.Path]::GetFileNameWithoutExtension($_) + '.o') })
    $targets = @(
        @{ Source = 'tests/model_tests.cpp'; Output = 'animus_model_tests' },
        @{ Source = 'tests/stage_b_tests.cpp'; Output = 'animus_model_tests_b' },
        @{ Source = 'tests/stage_c_tests.cpp'; Output = 'animus_model_tests_c' },
        @{ Source = 'tests/stage_d_tests.cpp'; Output = 'animus_model_tests_d' },
        @{ Source = 'tests/stage_e_tests.cpp'; Output = 'animus_model_tests_e' },
        @{ Source = 'tools/graphics_demo.cpp'; Output = 'animus_graphics_demo' },
        @{ Source = 'tools/submission_demo.cpp'; Output = 'animus_submission_demo' },
        @{ Source = 'src/main.cpp'; Output = 'animus_model_cli' }
    )
    foreach ($target in $targets) {
        & $compilerPath @flags $target.Source @objects '-o' ('build/' + $target.Output + '.exe')
        if ($LASTEXITCODE -ne 0) { throw ('Build failed: ' + $target.Output) }
    }
    $oldPath = $env:PATH
    try {
        $env:PATH = $compilerDirectory + ';' + $oldPath
        foreach ($name in @('animus_model_tests', 'animus_model_tests_b', 'animus_model_tests_c', 'animus_model_tests_d', 'animus_model_tests_e')) {
            & ('.\build\' + $name + '.exe')
            if ($LASTEXITCODE -ne 0) { throw ('Tests failed: ' + $name) }
        }
    } finally { $env:PATH = $oldPath }
} finally { Pop-Location }
