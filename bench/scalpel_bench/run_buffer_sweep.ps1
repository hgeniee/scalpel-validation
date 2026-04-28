param(
    [Parameter(Mandatory = $true)]
    [string]$SourceSubdir,

    [Parameter(Mandatory = $true)]
    [string]$ImagePath,

    [string]$ConfigPath,

    [int[]]$BufferSizesMB = @(64, 32, 16, 10),
    [string]$OutputRoot = ".\bench\scalpel_bench\results\buffer-sweep",
    [string]$GeneratedSourcesRoot = ".\bench\scalpel_bench\generated_sources\buffer-sweep",
    [string]$TagPrefix = "scalpel-buffer-sweep",
    [int]$RepeatCount = 1,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"

function Resolve-ConfigPath {
    param(
        [string]$ConfigPath
    )

    if ($ConfigPath) {
        return (Resolve-Path $ConfigPath).Path
    }

    $candidates = @(
        ".\\worker\\scalpel2.conf",
        ".\\worker\\scalpel.conf"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Config file not found. Checked: $($candidates -join ', ')"
}

function Get-PythonPath {
    $candidates = @(
        ".\.venv\Scripts\python.exe",
        (Get-Command py -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
        (Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue)
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Python interpreter not found. Checked .venv, py, and python."
}

function New-VariantSource {
    param(
        [string]$SourceSubdir,
        [string]$GeneratedSourcesRoot,
        [int]$BufferSizeMB
    )

    $sourcePath = (Resolve-Path $SourceSubdir).Path
    $variantName = "optimized-buffer-{0:D3}mb" -f $BufferSizeMB
    $variantPath = Join-Path $GeneratedSourcesRoot $variantName

    if (Test-Path $variantPath) {
        Remove-Item -Recurse -Force $variantPath
    }

    New-Item -ItemType Directory -Force -Path $variantPath | Out-Null
    Copy-Item -Path (Join-Path $sourcePath "*") -Destination $variantPath -Recurse -Force

    $commonHeader = Join-Path $variantPath "src\common.h"
    if (-not (Test-Path $commonHeader)) {
        throw "common.h not found under $variantPath"
    }

    $raw = Get-Content $commonHeader -Raw
    $updated = $raw -replace `
        '#define SIZE_OF_BUFFER\s+\([0-9]+\s+\*\s+MEGABYTE\)', `
        "#define SIZE_OF_BUFFER            ($BufferSizeMB * MEGABYTE)"

    if ($updated -eq $raw) {
        throw "Failed to replace SIZE_OF_BUFFER in $commonHeader"
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($commonHeader, $updated, $utf8NoBom)

    return $variantPath
}

function Build-WorkerImage {
    param(
        [string]$Tag,
        [string]$SourceSubdir
    )

    Write-Host "Building $Tag from $SourceSubdir"
    docker build `
        -f worker/Dockerfile `
        --build-arg SCALPEL_FETCH_MODE=local `
        --build-arg SCALPEL_SOURCE_SUBDIR=$SourceSubdir `
        -t $Tag `
        .
}

function Invoke-DockerProcess {
    param(
        [string[]]$ArgumentList,
        [string]$StdoutPath,
        [string]$StderrPath
    )

    $process = Start-Process `
        -FilePath "docker" `
        -ArgumentList $ArgumentList `
        -NoNewWindow `
        -Wait `
        -PassThru `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath

    return $process.ExitCode
}

function Normalize-TextLog {
    param(
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return
    }

    $raw = Get-Content $Path -Raw
    if ($null -eq $raw) {
        return
    }

    $normalized = $raw.Replace("`r`n", "`n").Replace("`r", "`n")
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Resolve-Path $Path), $normalized, $utf8NoBom)
}

function Invoke-ScalpelRun {
    param(
        [string]$PythonPath,
        [string]$Tag,
        [string]$Label,
        [string]$ImagePath,
        [string]$ConfigPath,
        [string]$OutputRoot
    )

    $runRoot = Join-Path $OutputRoot $Label
    $outDir = Join-Path $runRoot "carved"
    if (Test-Path $runRoot) {
        Remove-Item -Recurse -Force $runRoot
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $timeFile = Join-Path $runRoot "time.txt"
    $stdoutFile = Join-Path $runRoot "stdout.txt"
    $stderrFile = Join-Path $runRoot "stderr.txt"
    $summaryFile = Join-Path $runRoot "summary.json"

    $imageDir = Split-Path -Parent (Resolve-Path $ImagePath)
    $imageName = Split-Path -Leaf $ImagePath
    $configDir = Split-Path -Parent (Resolve-Path $ConfigPath)
    $configName = Split-Path -Leaf $ConfigPath
    $outDirResolved = Resolve-Path $outDir

    $command = "/usr/bin/time -v scalpel2 -c /config/$configName -o /output /input/$imageName"
    $dockerArgs = @(
        "run",
        "--rm",
        "-v", "${imageDir}:/input:ro",
        "-v", "${configDir}:/config:ro",
        "-v", "${outDirResolved}:/output",
        "--entrypoint", "sh",
        $Tag,
        "-lc",
        "`"$command`""
    )

    $exitCode = Invoke-DockerProcess -ArgumentList $dockerArgs -StdoutPath $stdoutFile -StderrPath $stderrFile
    Normalize-TextLog -Path $stdoutFile
    Normalize-TextLog -Path $stderrFile
    if ($exitCode -ne 0) {
        $stderrPreview = ""
        if (Test-Path $stderrFile) {
            $stderrPreview = (Get-Content $stderrFile -Tail 20) -join [Environment]::NewLine
        }
        throw "docker run failed for $Label (exit code $exitCode).`n$stderrPreview"
    }

    $stderrContent = Get-Content $stderrFile -Raw
    $timeLines = $stderrContent -split "`r?`n" | Where-Object { $_ -match ":" }
    Set-Content -Path $timeFile -Value ($timeLines -join [Environment]::NewLine) -Encoding UTF8

    $summary = & $PythonPath bench\scalpel_bench\summarize_scalpel_run.py $timeFile $stdoutFile $outDir
    Set-Content -Path $summaryFile -Value $summary -Encoding UTF8
}

if (-not $BufferSizesMB -or $BufferSizesMB.Count -eq 0) {
    throw "Specify at least one buffer size with -BufferSizesMB"
}

$pythonPath = Get-PythonPath
$resolvedOutputRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputRoot))
$resolvedGeneratedSourcesRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $GeneratedSourcesRoot))

New-Item -ItemType Directory -Force -Path $resolvedOutputRoot | Out-Null
New-Item -ItemType Directory -Force -Path $resolvedGeneratedSourcesRoot | Out-Null

$ConfigPath = Resolve-ConfigPath -ConfigPath $ConfigPath
$uniqueBufferSizes = $BufferSizesMB | Sort-Object -Descending -Unique
$variantSpecs = @()

foreach ($bufferSizeMB in $uniqueBufferSizes) {
    if ($bufferSizeMB -le 0) {
        throw "Buffer sizes must be positive integers"
    }

    $variantSourcePath = New-VariantSource `
        -SourceSubdir $SourceSubdir `
        -GeneratedSourcesRoot $resolvedGeneratedSourcesRoot `
        -BufferSizeMB $bufferSizeMB

    $variantLabel = "buffer-{0:D3}mb" -f $bufferSizeMB
    $variantTag = "{0}:{1}mb" -f $TagPrefix, $bufferSizeMB

    $variantSpecs += [PSCustomObject]@{
        BufferSizeMB = $bufferSizeMB
        Label = $variantLabel
        Tag = $variantTag
        SourcePath = $variantSourcePath
    }
}

foreach ($variant in $variantSpecs) {
    $relativeSourcePath = [System.IO.Path]::GetRelativePath((Get-Location).Path, $variant.SourcePath)
    Build-WorkerImage -Tag $variant.Tag -SourceSubdir $relativeSourcePath
}

if ($BuildOnly) {
    Write-Host "Build completed. Skipping benchmark runs because -BuildOnly was set."
    exit 0
}

foreach ($variant in $variantSpecs) {
    if ($RepeatCount -le 1) {
        Invoke-ScalpelRun `
            -PythonPath $pythonPath `
            -Tag $variant.Tag `
            -Label $variant.Label `
            -ImagePath $ImagePath `
            -ConfigPath $ConfigPath `
            -OutputRoot $resolvedOutputRoot
        continue
    }

    for ($runIndex = 1; $runIndex -le $RepeatCount; $runIndex++) {
        $runLabel = "{0}-run-{1:D2}" -f $variant.Label, $runIndex
        Invoke-ScalpelRun `
            -PythonPath $pythonPath `
            -Tag $variant.Tag `
            -Label $runLabel `
            -ImagePath $ImagePath `
            -ConfigPath $ConfigPath `
            -OutputRoot $resolvedOutputRoot
    }
}

$referenceBufferMB = $uniqueBufferSizes | Measure-Object -Maximum | Select-Object -ExpandProperty Maximum
$summaryPath = Join-Path $resolvedOutputRoot "buffer_sweep_summary.json"
$summary = & $pythonPath bench\scalpel_bench\buffer_sweep_summary.py $resolvedOutputRoot $referenceBufferMB
Set-Content -Path $summaryPath -Value $summary -Encoding UTF8

Write-Host "Buffer sweep completed. Summary written to $summaryPath"
