$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$session = Join-Path $root 'build/ipc-test'
$build = Join-Path $root 'build/goat-sim.exe'
if (-not (Test-Path -LiteralPath $build)) { throw 'Build goat-sim.exe before running this test.' }
if (Test-Path -LiteralPath $session) { Remove-Item -LiteralPath $session -Recurse -Force }
New-Item -ItemType Directory -Path $session | Out-Null
$result = Join-Path $session 'result.txt'
$process = Start-Process -FilePath $build -ArgumentList @(
    'duel', 'decks/starter/flc1-3rd-chaos-con.ydk', 'decks/starter/flc2-3rd-goat.ydk',
    '--human-player', '1', '--decision-dir', $session, '--result-file', $result, '--quiet'
) -WorkingDirectory $root -PassThru -WindowStyle Hidden
try {
    for ($attempt = 0; $attempt -lt 6000 -and -not $process.HasExited; $attempt++) {
        $request = Join-Path $session 'request.txt'
        $response = Join-Path $session 'response.txt'
        if ((Test-Path -LiteralPath $request) -and -not (Test-Path -LiteralPath $response)) {
            # goat-sim.exe polls for this file's existence every 100ms and reads
            # it the instant it sees it; writing directly to $response left a
            # window where it could be read mid-write (created but empty),
            # which fails to parse and falls back to an always-out-of-range
            # default index, throwing "invalid graphical action index" and
            # ending the duel. Write-then-rename (matching how the engine
            # itself publishes request.txt) makes it appear atomically.
            #
            # A "#SELECT <min> <max>" prompt (choose_multi_menu's click-based
            # multi-select — see src/main.cpp) expects a comma-separated list
            # of *candidate* indices whose count is within [min,max], not a
            # single index; answering it the same way as every other prompt
            # ('0') is invalid input the engine will never accept, and this
            # dumb driver never notices because it only (re)writes response.txt
            # when one doesn't already exist yet. That silently deadlocked
            # this test against the engine's own response-wait timeout.
            # request.txt is published via write-then-rename, same as
            # response.txt above, but the rename itself is not atomic against
            # a concurrent reader on Windows — a read landing in that instant
            # can find the file locked. Treat that the same as "not ready
            # yet" and just retry on the next 100ms tick, rather than letting
            # $ErrorActionPreference='Stop' abort the whole test over a
            # transient lock.
            $firstLine = $null
            try { $firstLine = (Get-Content -LiteralPath $request -TotalCount 1 -ErrorAction Stop) } catch {}
            if ($null -ne $firstLine) {
                if ($firstLine -match '^#SELECT (\d+) (\d+)') {
                    $min = [int]$Matches[1]
                    $answer = if ($min -le 0) { '' } else { (0..($min - 1)) -join ',' }
                } else {
                    $answer = '0'
                }
                $responseTemp = "$response.tmp"
                Set-Content -LiteralPath $responseTemp -Value $answer -NoNewline
                Move-Item -LiteralPath $responseTemp -Destination $response -Force
            }
        }
        Start-Sleep -Milliseconds 100
    }
    $process.Refresh()
    if (-not $process.HasExited -or $process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw 'The player decision IPC session did not complete successfully.'
    }
    $text = Get-Content -LiteralPath $result -Raw
    if ($text -notmatch 'winner=[01]') { throw 'The IPC session did not write a valid winner.' }
    $state = Join-Path $session 'state.txt'
    if (-not (Test-Path -LiteralPath $state)) { throw 'The IPC session did not publish a board state snapshot.' }
    $stateText = Get-Content -LiteralPath $state -Raw
    if ($stateText -notmatch 'lp=-?\d+,-?\d+' -or $stateText -notmatch 'hand=\d+,\d+') { throw 'The board state snapshot is incomplete.' }
    Write-Output $text.Trim()
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
