param(
    [Parameter(Mandatory = $true)][int]$ConnectionA,
    [Parameter(Mandatory = $true)][int]$ConnectionB,
    [int]$Iterations = 20,
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\build\Release\fairyfly.exe' }
if ($Iterations -lt 1) { throw 'Iterations must be positive' }
$exe = (Resolve-Path -LiteralPath $Fairyfly).Path

$worker = {
    param([string]$Exe, [int]$Connection, [int]$Count)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $Count; $i++) {
        $raw = & $Exe screen read --connection $Connection --only-buttons --no-tabs --compact --output json
        if ($LASTEXITCODE -ne 0) { throw "Connection $Connection read $i failed: $raw" }
        $response = $raw | ConvertFrom-Json
        if ($response.status -ne 'success' -or $response.data.connection_id -ne $Connection) {
            throw "Connection $Connection read $i returned an unexpected result"
        }
    }
    $watch.Stop()
    [pscustomobject]@{ connection = $Connection; calls = $Count; elapsed_ms = $watch.ElapsedMilliseconds }
}

$serialWatch = [Diagnostics.Stopwatch]::StartNew()
$serialA = & $worker $exe $ConnectionA $Iterations
$serialB = & $worker $exe $ConnectionB $Iterations
$serialWatch.Stop()

function Invoke-Pair([int]$First, [int]$Second) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $jobs = @(
        (Start-Job -ScriptBlock $worker -ArgumentList $exe, $First, $Iterations),
        (Start-Job -ScriptBlock $worker -ArgumentList $exe, $Second, $Iterations)
    )
    try {
        $jobs | Wait-Job | Out-Null
        $results = @($jobs | Receive-Job -ErrorAction Stop)
        if ($jobs[0].State -ne 'Completed' -or $jobs[1].State -ne 'Completed') {
            throw "Probe jobs failed: $($jobs[0].State), $($jobs[1].State)"
        }
        $watch.Stop()
        return [pscustomobject]@{ wall_ms = $watch.ElapsedMilliseconds; workers = $results }
    } finally {
        $jobs | Remove-Job -Force
    }
}

$different = Invoke-Pair $ConnectionA $ConnectionB
$same = Invoke-Pair $ConnectionA $ConnectionA

[pscustomobject]@{
    process_model = 'one fairyfly CLI process per read'
    operation = 'screen read --only-buttons --no-tabs --compact'
    iterations_per_worker = $Iterations
    serial = [pscustomobject]@{ wall_ms = $serialWatch.ElapsedMilliseconds; workers = @($serialA, $serialB) }
    different_sessions = $different
    same_session = $same
} | ConvertTo-Json -Depth 8
