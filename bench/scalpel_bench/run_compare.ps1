param(
    [Parameter(Mandatory = $true)]
    [string]$BaselineSourceSubdir,

    [Parameter(Mandatory = $true)]
    [string]$OptimizedSourceSubdir,

    [Parameter(Mandatory = $true)]
    [string]$ImagePath,

    [string]$ConfigPath,

    [string]$OutputRoot = ".\bench\scalpel_bench\results",
    [string]$BaselineTag = "scalpel-worker:baseline",
    [string]$OptimizedTag = "scalpel-worker:optimized",
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

    # Scalpel 2.02 기본 설정 파일명은 scalpel2.conf를 우선 사용한다.
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

    # Scalpel 진행바는 CR 문자로 같은 줄을 덮어쓰므로, 파일 저장 후에는 읽기 쉽게 줄바꿈으로 정리한다.
    $normalized = $raw.Replace("`r`n", "`n").Replace("`r", "`n")
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Resolve-Path $Path), $normalized, $utf8NoBom)
}

function Invoke-ScalpelRun {
    param(
        [string]$Tag,
        [string]$Label,
        [string]$ImagePath,
        [string]$ConfigPath,
        [string]$OutputRoot
    )

    $runRoot = Join-Path $OutputRoot $Label
    $outDir = Join-Path $runRoot "carved"
    if (Test-Path $runRoot) {
        # Scalpel은 비어 있지 않은 output 디렉터리를 거부하므로 이전 실행 결과를 먼저 비운다.
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

    $summary = & .\.venv\Scripts\python.exe bench\scalpel_bench\summarize_scalpel_run.py $timeFile $stdoutFile $outDir
    Set-Content -Path $summaryFile -Value $summary -Encoding UTF8
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$ConfigPath = Resolve-ConfigPath -ConfigPath $ConfigPath

Build-WorkerImage -Tag $BaselineTag -SourceSubdir $BaselineSourceSubdir
Build-WorkerImage -Tag $OptimizedTag -SourceSubdir $OptimizedSourceSubdir

if ($BuildOnly) {
    Write-Host "Build completed. Skipping benchmark runs because -BuildOnly was set."
    exit 0
}

if ($RepeatCount -le 1) {
    Invoke-ScalpelRun -Tag $BaselineTag -Label "baseline" -ImagePath $ImagePath -ConfigPath $ConfigPath -OutputRoot $OutputRoot
    Invoke-ScalpelRun -Tag $OptimizedTag -Label "optimized" -ImagePath $ImagePath -ConfigPath $ConfigPath -OutputRoot $OutputRoot
}
else {
    for ($runIndex = 1; $runIndex -le $RepeatCount; $runIndex++) {
        $baselineLabel = "baseline-run-{0:D2}" -f $runIndex
        $optimizedLabel = "optimized-run-{0:D2}" -f $runIndex
        Invoke-ScalpelRun -Tag $BaselineTag -Label $baselineLabel -ImagePath $ImagePath -ConfigPath $ConfigPath -OutputRoot $OutputRoot
        Invoke-ScalpelRun -Tag $OptimizedTag -Label $optimizedLabel -ImagePath $ImagePath -ConfigPath $ConfigPath -OutputRoot $OutputRoot
    }
}

$comparisonPath = Join-Path $OutputRoot "comparison.json"
$comparison = & .\.venv\Scripts\python.exe bench\scalpel_bench\compare_summaries.py $OutputRoot
Set-Content -Path $comparisonPath -Value $comparison -Encoding UTF8
