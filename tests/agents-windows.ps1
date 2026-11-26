param([Parameter(Mandatory)][string]$Think)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Think).Path
$root = Join-Path (Split-Path $PSScriptRoot) 'agent'
$work = Join-Path ([IO.Path]::GetTempPath()) ('think ' + [Guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($work)
$utf8 = New-Object Text.UTF8Encoding($false)
try {
    $task = [string][char]0x0645 + [char]0x0647 + [char]0x0645 + [char]0x0629
    $artifact = Join-Path $work ($task + '.bin')
    $status = Join-Path $work 'writer.txt'
    $log = Join-Path $work 'events.jsonl'
    [IO.File]::WriteAllBytes($artifact, [byte[]](0, 26, 13, 10, 255))
    $common = @('run','--root',$root,'--task',$task,'--agent','artifact-ready',
                '--poll-ms','10','--bind',("artifact=size:" + $artifact),'--progress',("file:" + $log))
    [IO.File]::WriteAllText($status,"ready`r`n",$utf8)
    & $exe @common --timeout-ms 3000 --bind ("writer=file:" + $status)
    if ($LASTEXITCODE -ne 0) { throw "status file exit $LASTEXITCODE" }
    $events = @(Get-Content -Encoding UTF8 -LiteralPath $log | ForEach-Object { $_ | ConvertFrom-Json })
    if ($events[-1].event -ne 'completed' -or $events[-1].task -cne $task) { throw 'task round trip' }
    if (!($events | Where-Object { $_.agent -eq 'build-started' -and $_.value -eq '5' })) { throw 'size missing' }
    'ready' | & $exe @common --timeout-ms 3000 --bind writer=stdin
    if ($LASTEXITCODE -ne 0) { throw "status pipe exit $LASTEXITCODE" }
    [IO.File]::WriteAllText($status,'ready',$utf8)
    & $exe @common --timeout-ms 100 --bind ("writer=file:" + $status)
    if ($LASTEXITCODE -ne 124) { throw "partial record exit $LASTEXITCODE" }
    [IO.File]::WriteAllText($status,"failed`n",$utf8)
    & $exe @common --timeout-ms 3000 --bind ("writer=file:" + $status)
    if ($LASTEXITCODE -ne 1) { throw "failure record exit $LASTEXITCODE" }
    $events = @(Get-Content -Encoding UTF8 -LiteralPath $log | ForEach-Object { $_ | ConvertFrom-Json })
    if (@($events | Where-Object event -eq 'completed').Count -ne 2 -or
        @($events | Where-Object event -eq 'timed_out').Count -ne 1 -or
        @($events | Where-Object event -eq 'failed').Count -ne 1) { throw 'terminal event counts' }
    'Windows agent checks passed'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force
}
