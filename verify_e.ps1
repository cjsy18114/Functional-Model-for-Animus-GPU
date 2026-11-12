param(
    [string]$Compiler = 'g++',
    [string]$Python = 'python',
    [switch]$SkipBuild
)

# One entry point for the E document's checks. This script prepares/runs tools
# and compares known answers; it does not implement any GPU or shader behavior.
$ErrorActionPreference = 'Stop'
$previousPath = $env:PATH
$activeStage = 'preflight'
$activeLog = ''
$succeeded = $false

function Invoke-CheckedProgram {
    param([string]$Program, [string[]]$CommandArguments, [string]$LogPath, [switch]$ShowOutput)
    $resolvedProgram = (Get-Command $Program -ErrorAction Stop).Source
    # Native stderr is captured as text; nonzero process status is checked
    # explicitly, including on Windows PowerShell 5.1.
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $captured = @(& $resolvedProgram @CommandArguments 2>&1)
        $processCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    $captured | Out-File -LiteralPath $LogPath -Encoding utf8
    if ($processCode -ne 0) {
        $captured | Select-Object -Last 12 | ForEach-Object { Write-Host $_ }
        throw "Program failed with exit code ${processCode}: $Program"
    }
    if ($ShowOutput) { $captured | ForEach-Object { Write-Host $_ } }
}

Push-Location -LiteralPath $PSScriptRoot
try {
    New-Item -ItemType Directory -Path 'build' -Force | Out-Null
    $compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
    $pythonPath = (Get-Command $Python -ErrorAction Stop).Source
    $env:PATH = (Split-Path $compilerPath) + ';' + $previousPath
    $activeLog = Join-Path $PSScriptRoot 'build/verify_e_python.log'
    Invoke-CheckedProgram -Program $pythonPath -CommandArguments @('-c', 'import sys; assert sys.version_info >= (3, 8)') -LogPath $activeLog

    $activeStage = '1/6 build and A-E C++ regression tests'
    Write-Host "[$activeStage]"
    if (-not $SkipBuild) {
        $activeLog = Join-Path $PSScriptRoot 'build/verify_e_build.log'
        & (Join-Path $PSScriptRoot 'build.ps1') -Compiler $compilerPath *> $activeLog
        if ($LASTEXITCODE -ne 0) { throw 'Build or regression script failed.' }
    } else {
        # Skipping compilation never skips the existing regression executables.
        foreach ($target in @('animus_model_tests', 'animus_model_tests_b', 'animus_model_tests_c', 'animus_model_tests_d', 'animus_model_tests_e', 'animus_management_tests')) {
            $activeLog = Join-Path $PSScriptRoot ('build/verify_e_' + $target + '.log')
            Invoke-CheckedProgram -Program (Join-Path $PSScriptRoot ('build/' + $target + '.exe')) -CommandArguments @() -LogPath $activeLog
        }
    }
    Write-Host 'PASS A-E regressions (63 groups) and management regressions (13 groups).'

    $activeStage = '2/6 submission summary and all 64 color/depth pixels'
    Write-Host "[$activeStage]"
    $activeLog = Join-Path $PSScriptRoot 'build/e_submission_summary.json'
    $json = @(& '.\build\animus_submission_demo.exe' --output '.\build\e_framebuffer.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Submission demo failed.' }
    $json | Out-File -LiteralPath $activeLog -Encoding utf8
    $summary = ($json -join "`n") | ConvertFrom-Json
    if ($summary.status -ne 2 -or $summary.error_code -ne 0 -or $summary.commands -ne 5 -or
        $summary.primitives -ne 2 -or $summary.fragments -ne 16 -or $summary.shader_attempts -ne 18 -or
        $summary.fence -ne 0x12345678 -or $summary.width -ne 8 -or $summary.height -ne 8 -or
        $summary.raster_profile -ne 'reference-v1') { throw 'Submission summary differs from the known fixture result.' }
    $colors = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'build/e_framebuffer.bin'))
    $depth = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'build/e_framebuffer.bin.depth.bin'))
    if ($colors.Length -ne 256 -or $depth.Length -ne 256) { throw 'Framebuffer files must each contain 256 bytes.' }
    if (-not [BitConverter]::IsLittleEndian) { throw 'This verification entry point requires a little-endian host.' }
    $red = [Convert]::ToUInt32('FF0000FF', 16)
    $black = [Convert]::ToUInt32('FF000000', 16)
    for ($y = 0; $y -lt 8; $y++) {
        $line = ''
        for ($x = 0; $x -lt 8; $x++) {
            $offset = 4 * ($y * 8 + $x)
            $inside = $x -ge 2 -and $x -lt 6 -and $y -ge 2 -and $y -lt 6
            $expectedColor = if ($inside) { $red } else { $black }
            $expectedDepth = if ($inside) { 0x3F000000 } else { 0x3F800000 }
            if ([BitConverter]::ToUInt32($colors, $offset) -ne $expectedColor -or
                [BitConverter]::ToUInt32($depth, $offset) -ne $expectedDepth) { throw "Wrong color/depth at pixel ($x,$y)." }
            $line += if ($inside) { 'R' } else { '.' }
        }
        Write-Host $line
    }
    Write-Host 'PASS summary, FENCE/DONE, and all 64 exact color/depth words.'

    $referenceChecks = @(
        @{ Stage = '3/6 independent raster reference (120 submissions)'; Name = 'raster' },
        @{ Stage = '4/6 independent integer reference (810 vectors)'; Name = 'integer' },
        @{ Stage = '5/6 independent floating/SFU reference (900 + 42 vectors)'; Name = 'float' },
        @{ Stage = '6/6 independent texture reference (1024 UV pairs)'; Name = 'texture' }
    )
    foreach ($check in $referenceChecks) {
        $activeStage = $check.Stage
        Write-Host "[$activeStage]"
        $activeLog = Join-Path $PSScriptRoot ('build/verify_e_' + $check.Name + '.log')
        Invoke-CheckedProgram -Program $pythonPath -CommandArguments @('.\tests\differential_' + $check.Name + '.py') -LogPath $activeLog -ShowOutput
    }
    $succeeded = $true
    Write-Host 'PASS E verification: all six stages passed.'
    Write-Host 'Reports and framebuffer artifacts are saved under build/.'
} catch {
    Write-Host "FAIL E verification at [$activeStage]: $($_.Exception.Message)"
    if ($activeLog) { Write-Host "Details: $activeLog" }
} finally {
    $env:PATH = $previousPath
    Pop-Location
}
if (-not $succeeded) { exit 1 }
exit 0
