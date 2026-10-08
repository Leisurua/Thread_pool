param(
    [ValidateRange(10, 1000)][int]$Rounds = 10,
    [ValidateSet('basic', 'advanced')][string]$Suite = 'basic',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$releaseRoot = Join-Path $projectRoot 'build/ucrt64-release'
$executable = Join-Path $releaseRoot 'thread_pool_benchmark.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build with cmake --preset ucrt64-release and cmake --build --preset ucrt64-release first.' }
$cache = Get-Content (Join-Path $releaseRoot 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') { throw 'Release build required' }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot ('results/' + [DateTimeOffset]::Now.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyyMMdd-HHmmss'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new output directory; existing measurements are never overwritten.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$cpuKey = 'HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0'
$osKey = 'HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$commands = @(Get-Content (Join-Path $releaseRoot 'compile_commands.json') -Raw | ConvertFrom-Json)
$hashes = @{}
foreach ($file in @('include/thread_pool.hpp', 'src/thread_pool.cpp', 'benchmarks/thread_pool_benchmark.cpp', 'benchmarks/workloads.cpp', 'benchmarks/workloads.hpp', 'benchmarks/run.ps1', 'benchmarks/summarize.ps1', 'benchmarks/summarize_advanced.ps1')) {
    $hashes[$file] = (Get-FileHash -LiteralPath (Join-Path $projectRoot $file) -Algorithm SHA256).Hash
}
$metadata = [ordered]@{
    started_at = [DateTimeOffset]::Now.ToOffset([TimeSpan]::FromHours(8)).ToString('o')
    completed_at = $null
    rounds = $Rounds
    suite = $Suite
    configurations = if ($Suite -eq 'advanced') { 84 } else { 42 }
    warmups_per_configuration = 1
    shuffle_seed = 20261008
    cpu_name = [Microsoft.Win32.Registry]::GetValue($cpuKey, 'ProcessorNameString', $null)
    logical_processors = [Environment]::ProcessorCount
    os_display_version = [Microsoft.Win32.Registry]::GetValue($osKey, 'DisplayVersion', $null)
    os_build = [Microsoft.Win32.Registry]::GetValue($osKey, 'CurrentBuildNumber', $null)
    os_ubr = [Microsoft.Win32.Registry]::GetValue($osKey, 'UBR', $null)
    power_scheme = ((& powercfg /getactivescheme) -join ' ')
    affinity_and_priority = 'OS defaults; no frequency pinning; background applications not stopped'
    compiler = ((& 'C:/msys64/ucrt64/bin/g++.exe' --version) -join "`n")
    git_revision = ((& git -C $projectRoot rev-parse HEAD) -join '')
    git_status = @(& git -C $projectRoot status --short)
    build_commands = @($commands | Where-Object { $_.file -match 'src[/\\]thread_pool.cpp$|benchmarks[/\\](workloads|thread_pool_benchmark).cpp$' })
    source_sha256 = $hashes
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
}
$metadataPath = Join-Path $OutputDirectory 'environment.json'
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding utf8
$previousPath = $env:PATH
try {
    $env:PATH = 'C:/msys64/ucrt64/bin;' + $previousPath
    & $executable (Join-Path $OutputDirectory 'samples.csv') $Rounds $Suite | Tee-Object -FilePath (Join-Path $OutputDirectory 'run.log')
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark failed; preserve partial data, do not publish it as a completed run.' }
} finally { $env:PATH = $previousPath }
$metadata.completed_at = [DateTimeOffset]::Now.ToOffset([TimeSpan]::FromHours(8)).ToString('o')
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding utf8
$summaryScript = if ($Suite -eq 'advanced') { 'summarize_advanced.ps1' } else { 'summarize.ps1' }
& (Join-Path $PSScriptRoot $summaryScript) -ResultsDirectory $OutputDirectory
Write-Output "Saved complete measurements: $OutputDirectory"
