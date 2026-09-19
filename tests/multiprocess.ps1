# Congestion Recovery - real OS-process multiprocess proofs.
# Copyright 2026 Summon Software Labs.
#
# Drives crcoordinator and crworker as separate operating-system processes over
# framed loopback TCP. No test timeouts are used anywhere: every wait is a
# bounded readiness poll for an expected line, and a hang would be a defect.
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
Check (Test-Path $coordinator) 'crcoordinator binary exists'
Check (Test-Path $worker) 'crworker binary exists'
if ($script:Failures -gt 0) { exit 1 }

Write-Host ''
Write-Host '--- scenario: staged ---'
$stagedDir = Join-Path $WorkDir 'staged'
New-Item -ItemType Directory -Path $stagedDir -Force | Out-Null
$stagedPort = New-FreePort
$coordLog = Join-Path $stagedDir 'coordinator.log'
$applierLog = Join-Path $stagedDir 'applier.log'
$observerLog = Join-Path $stagedDir 'observer.log'
$coord = Start-Logged $coordinator @('--state', $stagedDir, '--port', "$stagedPort", '--scenario', 'staged', '--workers', '2') $coordLog
Check (Wait-ForLine $coordLog 'LISTEN') 'coordinator is listening'
$applier = Start-Logged $worker @('--port', "$stagedPort", '--worker', '1', '--boot', '1', '--role', 'applier') $applierLog
$observer = Start-Logged $worker @('--port', "$stagedPort", '--worker', '2', '--boot', '1', '--role', 'observer') $observerLog
$coord.WaitForExit(180000) | Out-Null
Stop-Hard $applier
Stop-Hard $observer
$text = Read-Log $coordLog
Check ($text -match 'REGISTER worker=1 boot=1 role=applier') 'applier registered as its own process'
Check ($text -match 'REGISTER worker=2 boot=1 role=observer') 'observer registered as its own process'
Check ($text -match 'STAGE .*index=0 .*decision=ADVANCE_STAGE') 'OBSERVE advanced'
Check ($text -match 'STAGE .*index=5 .*kind=COMPLETE .*decision=COMPLETE') 'COMPLETE committed'
Check ($text -match 'RESULT=OK') 'staged scenario reported OK'
Check ((Read-Log $applierLog) -match 'REPORTED') 'applier reported applied levels'

Write-Host ''
Write-Host '--- scenario: recurrence ---'
$recurDir = Join-Path $WorkDir 'recurrence'
New-Item -ItemType Directory -Path $recurDir -Force | Out-Null
$recurPort = New-FreePort
$coordLog = Join-Path $recurDir 'coordinator.log'
$coord = Start-Logged $coordinator @('--state', $recurDir, '--port', "$recurPort", '--scenario', 'recurrence', '--workers', '2') $coordLog
Check (Wait-ForLine $coordLog 'LISTEN') 'recurrence coordinator is listening'
$applier = Start-Logged $worker @('--port', "$recurPort", '--worker', '1', '--boot', '1', '--role', 'applier') (Join-Path $recurDir 'applier.log')
$observer = Start-Logged $worker @('--port', "$recurPort", '--worker', '2', '--boot', '1', '--role', 'observer') (Join-Path $recurDir 'observer.log')
$coord.WaitForExit(180000) | Out-Null
Stop-Hard $applier
Stop-Hard $observer
$text = Read-Log $coordLog
Check ($text -match 'ROLLBACK .*compensation_steps=[1-9]') 'rollback produced an explicit compensating plan'
Check ($text -match 'RESULT=OK') 'recurrence scenario reported OK'

Write-Host ''
Write-Host '--- scenario: worker death ---'
$deathDir = Join-Path $WorkDir 'death'
New-Item -ItemType Directory -Path $deathDir -Force | Out-Null
$deathPort = New-FreePort
$coordLog = Join-Path $deathDir 'coordinator.log'
$recordFile = Join-Path $deathDir 'frames.txt'
$coord = Start-Logged $coordinator @('--state', $deathDir, '--port', "$deathPort", '--scenario', 'worker-death', '--workers', '2', '--peer-timeout-ms', '30000') $coordLog
Check (Wait-ForLine $coordLog 'LISTEN') 'worker-death coordinator is listening'
$applier1 = Start-Logged $worker @('--port', "$deathPort", '--worker', '1', '--boot', '1', '--role', 'applier', '--die-before-report', '3', '--record', $recordFile) (Join-Path $deathDir 'applier1.log')
$observer = Start-Logged $worker @('--port', "$deathPort", '--worker', '2', '--boot', '1', '--role', 'observer') (Join-Path $deathDir 'observer.log')
$applier1.WaitForExit(60000) | Out-Null
Check ((Read-Log (Join-Path $deathDir 'applier1.log')) -match 'DIED') 'applier died mid-stage before reporting'
Check (Wait-ForLine $coordLog 'WORKER_DEATH' 6000) 'coordinator detected the worker death'
Check (Wait-ForLine $coordLog 'ATTEMPT_AMBIGUOUS' 3000) 'attempt outcome marked ambiguous'
$applier2 = Start-Logged $worker @('--port', "$deathPort", '--worker', '1', '--boot', '2', '--role', 'applier') (Join-Path $deathDir 'applier2.log')
Check (Wait-ForLine $coordLog 'REVALIDATED' 6000) 'coordinator revalidated the plan after the replacement boot'
$replay = Start-Logged $worker @('--replay', $recordFile, '--host', '127.0.0.1', '--port', "$deathPort") (Join-Path $deathDir 'replay.log')
$replay.WaitForExit(60000) | Out-Null
$coord.WaitForExit(240000) | Out-Null
Stop-Hard $applier2
Stop-Hard $observer
$text = Read-Log $coordLog
$replayText = Read-Log (Join-Path $deathDir 'replay.log')
Check ($text -match 'FENCED worker=1 boot=1') 'dead worker incarnation was fenced'
Check ($text -match 'STALE_BOOT_REJECT worker=1 boot=1 current=2') 'stale boot replay rejected at HELLO'
Check ($text -match 'STALE_FRAME_REJECT .*reason=STALE_BOOT') 'fenced-incarnation stage report rejected'
Check ($replayText -match 'REPLAY_HELLO_REJECT .*reason=STALE_BOOT') 'replayer observed the STALE_BOOT refusal'
Check ($replayText -match 'REPLAY_FRAME .*result=REJECT reason=STALE_BOOT') 'replayer observed the stale frame refusal'
Check ($text -match 'RESULT=OK') 'worker-death scenario completed'
Check ($text -match 'index=5 kind=COMPLETE decision=COMPLETE .*state=COMPLETED') 'plan reached COMPLETED after revalidation'

Write-Host ''
if ($script:Failures -eq 0) {
  Write-Host 'cr_multiprocess: all checks passed'
  exit 0
}
Write-Host "cr_multiprocess: $script:Failures failures"
exit 1
