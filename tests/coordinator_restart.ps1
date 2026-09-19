# Congestion Recovery - coordinator restart and epoch advancement proof.
# Copyright 2026 Summon Software Labs.
#
# Two real coordinator processes share one durable state directory. The second
# run advances the epoch, refuses to restore live authority or liveness, and
# requires revalidation before the restored plan may advance again.
param(
  [Parameter(Mandatory = $true)][string]$ToolsDir,
  [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0

function Check($Condition, [string]$Message) {
  $ok = [bool]$Condition
  if ($ok) {
    Write-Host "OK   $Message"
  } else {
    Write-Host "FAIL $Message"
    $script:Failures += 1
  }
}

function New-FreePort {
  $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
  $listener.Start()
  $port = $listener.LocalEndpoint.Port
  $listener.Stop()
  return $port
}

function Wait-ForLine([string]$Path, [string]$Pattern, [int]$Iterations = 3000) {
  for ($i = 0; $i -lt $Iterations; $i++) {
    if (Test-Path $Path) {
      $content = ''
      try { $content = Get-Content -Path $Path -Raw -ErrorAction Stop } catch { $content = '' }
      if ($content -and $content -match $Pattern) { return $true }
    }
    Start-Sleep -Milliseconds 10
  }
  return $false
}

function Quote-Arg([string]$Value) {
  if ($Value -match '[\s"]') { return '"' + $Value + '"' }
  return $Value
}

function Start-Logged([string]$File, [string[]]$Arguments, [string]$Log) {
  if (Test-Path $Log) { Remove-Item -Force $Log }
  $errLog = $Log + '.err'
  $argString = (($Arguments | ForEach-Object { Quote-Arg $_ }) -join ' ')
  return Start-Process -FilePath $File -ArgumentList $argString -RedirectStandardOutput $Log -RedirectStandardError $errLog -PassThru -NoNewWindow
}

function Stop-Hard($Process) {
  if ($null -ne $Process -and -not $Process.HasExited) {
    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
  }
}

function Read-Log([string]$Path) {
  if (Test-Path $Path) { return (Get-Content -Path $Path -Raw) }
  return ''
}

# A previous run must never leave a coordinator holding a port or a log file.
Get-Process crcoordinator, crworker -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200
# Each run starts from a clean scratch directory: a leftover durable state would
# legitimately change the epoch the coordinator restores under.
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
$coordinator = Join-Path $ToolsDir 'crcoordinator.exe'
$worker = Join-Path $ToolsDir 'crworker.exe'
$cli = Join-Path $ToolsDir 'crcli.exe'
Check (Test-Path $coordinator) 'crcoordinator binary exists'
if ($script:Failures -gt 0) { exit 1 }

Write-Host ''
Write-Host '--- run 1: staged recovery stops at HOLD and persists ---'
$stateDir = Join-Path $WorkDir 'state'
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
$port1 = New-FreePort
$log1 = Join-Path $WorkDir 'run1.log'
$recordFile = Join-Path $WorkDir 'frames1.txt'
$coord1 = Start-Logged $coordinator @('--state', $stateDir, '--port', "$port1", '--scenario', 'restart-part1', '--workers', '2') $log1
Check (Wait-ForLine $log1 'LISTEN') 'run 1 coordinator is listening'
$applier1 = Start-Logged $worker @('--port', "$port1", '--worker', '1', '--boot', '1', '--role', 'applier', '--record', $recordFile) (Join-Path $WorkDir 'run1_applier.log')
$observer1 = Start-Logged $worker @('--port', "$port1", '--worker', '2', '--boot', '1', '--role', 'observer') (Join-Path $WorkDir 'run1_observer.log')
$coord1.WaitForExit(180000) | Out-Null
Stop-Hard $applier1
Stop-Hard $observer1
$text1 = Read-Log $log1
Check ($text1 -match 'RESULT=OK') 'run 1 finished cleanly'
Check ($text1 -match 'STOPPED at index=3') 'run 1 stopped at the HOLD rung'
Check (Test-Path (Join-Path $stateDir 'recovery.snapshot')) 'durable snapshot was written'
Check (Test-Path (Join-Path $stateDir 'recovery.journal')) 'durable journal was written'

Write-Host ''
Write-Host '--- inspection of durable state ---'
$cliLog = Join-Path $WorkDir 'cli.log'
$cliProcess = Start-Logged $cli @('--state', $stateDir) $cliLog
$cliProcess.WaitForExit(60000) | Out-Null
$cliText = Read-Log $cliLog
Check ($cliText -match 'SNAPSHOT version=1') 'snapshot is versioned and readable by crcli'
Check ($cliText -match 'RESTORE policies=1 plans=1 requiring_revalidation=1') 'inspection requires revalidation'
Check ($cliText -match 'RESTORE_NOTE .*live authority and liveness were not') 'restore note states what is not durable'
Check ($cliText -match 'CLI_OK') 'crcli completed'

Write-Host ''
Write-Host '--- run 2: restart advances the epoch and requires revalidation ---'
$port2 = New-FreePort
$log2 = Join-Path $WorkDir 'run2.log'
$coord2 = Start-Logged $coordinator @('--state', $stateDir, '--port', "$port2", '--scenario', 'restart-part2', '--workers', '2', '--peer-timeout-ms', '30000') $log2
Check (Wait-ForLine $log2 'LISTEN') 'run 2 coordinator is listening'
$applier2 = Start-Logged $worker @('--port', "$port2", '--worker', '1', '--boot', '2', '--role', 'applier') (Join-Path $WorkDir 'run2_applier.log')
$observer2 = Start-Logged $worker @('--port', "$port2", '--worker', '2', '--boot', '2', '--role', 'observer') (Join-Path $WorkDir 'run2_observer.log')
Check (Wait-ForLine $log2 'REVALIDATED' 6000) 'run 2 revalidated the restored plan'
$replay = Start-Logged $worker @('--replay', $recordFile, '--host', '127.0.0.1', '--port', "$port2") (Join-Path $WorkDir 'run2_replay.log')
$replay.WaitForExit(60000) | Out-Null
$coord2.WaitForExit(240000) | Out-Null
Stop-Hard $applier2
Stop-Hard $observer2
$text2 = Read-Log $log2
$replayText = Read-Log (Join-Path $WorkDir 'run2_replay.log')
Check ($text2 -match 'RESTORED plans=1 requiring_revalidation=1 .*epoch_before=1 epoch_after=2') 'restart advanced the coordinator epoch'
Check ($text2 -match 'RESTORED plans=1 requiring_revalidation=1') 'restart reports a plan needing revalidation'
# Worker liveness is deliberately not durable: nothing about boot 1 is restored,
# and the pre-restart incarnation is refused because boot 2 now holds the id.
Check ($text2 -notmatch 'FENCED worker=1 boot=1') 'no pre-restart incarnation was restored as live'
Check ($text2 -match 'STALE_BOOT_REJECT worker=1 boot=1 current=2') 'pre-restart boot replay rejected'
Check ($replayText -match 'REPLAY_FRAME .*epoch=1 .*result=REJECT reason=STALE_EPOCH') 'pre-restart epoch work rejected'
Check ($text2 -match 'RESULT=OK') 'run 2 completed the restored plan'
Check ($text2 -match 'index=5 kind=COMPLETE decision=COMPLETE .*state=COMPLETED') 'restored plan reached COMPLETED'

Write-Host ''
if ($script:Failures -eq 0) {
  Write-Host 'cr_coordinator_restart: all checks passed'
  exit 0
}
Write-Host "cr_coordinator_restart: $script:Failures failures"
exit 1
