# qemu_sweep_kapa_fault_test.ps1 - Boot the R6 sweep-kernel KAPA fault-test
# image under QEMU and validate that a controlled CPL3 fault in a KAPA
# payload is attributed to its KDB instance over the serial console.
#
# Usage:
#   pwsh -ExecutionPolicy Bypass -File ./tools/qemu_sweep_kapa_fault_test.ps1 \
#       <path-to-sweep-kapa-fault-qemu.bin> [output-log-path]
#
# Exit codes:
#   0  all assertions passed
#   1  assertion/boot/connect failure
#   2  QEMU not found or invalid arguments

param(
    [string]$Image,
    [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

if (-not $Image) {
    Write-Host "usage: qemu_sweep_kapa_fault_test.ps1 <flat multiboot image> [log-path]"
    exit 2
}
if (-not (Test-Path $Image)) {
    Write-Host "missing file: $Image"
    exit 2
}

$qemu = $null
$qemuCommand = Get-Command "qemu-system-x86_64" -ErrorAction SilentlyContinue
if ($qemuCommand) { $qemu = $qemuCommand.Source }
if (-not $qemu) {
    Write-Host "qemu-system-x86_64 not found"
    exit 2
}

$work = Join-Path $env:TEMP "openwinkrnl_qemu_sweep_kapa_fault"
New-Item -ItemType Directory -Path $work -Force | Out-Null
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $work "sweep_kapa_fault_qemu.log"
}
$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$port = 15000 + ($PID % 20000)
$stderrPath = Join-Path $work "qemu_stderr.txt"
Remove-Item -Path $stderrPath -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $qemu -ArgumentList @(
    "-kernel", $Image,
    "-nographic", "-monitor", "none",
    "-chardev", "socket,id=com0,port=$port,host=127.0.0.1,server=on,wait=on,ipv4=on",
    "-serial", "chardev:com0",
    "-m", "256M", "-smp", "1", "-no-reboot", "-no-shutdown"
) -NoNewWindow -RedirectStandardError $stderrPath -PassThru

$client = $null
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect("127.0.0.1", $port)
        break
    } catch {
        $client = $null
    }
}
if (-not $client) {
    Write-Host "FAILED to connect to QEMU serial"
    Set-Content -Path $LogPath -Value "FAILED to connect to QEMU serial"
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    exit 1
}

$stream = $client.GetStream()
$sb = New-Object System.Text.StringBuilder
$buf = New-Object byte[] 8192

function Read-Available {
    while ($stream.DataAvailable) {
        $n = $stream.Read($buf, 0, $buf.Length)
        if ($n -gt 0) { [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
    }
}

function Wait-For {
    param([string]$Pattern, [int]$Seconds)
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        Read-Available
        if ($sb.ToString() -match $Pattern) { return $true }
        Start-Sleep -Milliseconds 100
    }
    Read-Available
    return ($sb.ToString() -match $Pattern)
}

$failed = $false

Write-Host "Waiting for sweep-kapa-fault selftest boot milestones..."
if (-not (Wait-For -Pattern "\[SWEEP-FAULT\] selftest instance" -Seconds 180)) {
    Write-Host "[FAIL] [SWEEP-FAULT] selftest instance milestone not observed"
    $failed = $true
}
Start-Sleep -Milliseconds 500
Read-Available

$out = $sb.ToString()
Set-Content -Path $LogPath -Value $out
$stream.Close(); $client.Close()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }

# R6 fault-attribution assertions: the payload must run, fault exactly once,
# be attributed to its KDB instance, and the instance must end FAULTED.
$checks = @(
    "[KDB] instance 1: app 'sweep-kapa-fault' loaded 'sweep'",
    "[CIS] sweep: TRUSTED, signed by dev, licence accepted",
    "[PS] sweep: image entered at CPL3",
    "[KDB] instance 1: bound thread resumed on CPU (state=RUNNING)",
    "kapa-fault: about to fault deliberately",
    "[KDB] instance 1 faulted (0x1); resources released",
    "[SWEEP-FAULT] selftest instance 1 finished (state=faulted fault=0x1 exit=0)"
)

$forbidden = @(
    "[VFS] OWFS: primary database mounted",
    "[VFS] USFS: secure volume mounted",
    "[NET] router online",
    "[VIP] UniVIP online",
    "[SENT] active defense armed",
    "[SYSC] system call table installed",
    "handing off to shell 'sh' as init process",
    "[OWINIT] state ",
    "[OWINIT] userspace survey:",
    "OpenWindows DEBUG SYSTEM CONTROL SHELL",
    "[SWEEP-KERNEL] Reduced profile idle",
    "no live KAPA binding",
    "[KDB] gateway refused",
    "gateway bridge call FAILED",
    "state=stopped"
)

foreach ($c in $checks) {
    if ($out.Contains($c)) { Write-Host "[PASS] $c" }
    else { Write-Host "[FAIL] missing: $c"; $failed = $true }
}
foreach ($c in $forbidden) {
    if ($out.Contains($c)) { Write-Host "[FAIL] unexpected (excluded/refusal/fault marker): $c"; $failed = $true }
    else { Write-Host "[PASS] absent: $c" }
}

Write-Host ""
Write-Host "Log: $LogPath"
if ($failed) {
    Write-Host "=== SWEEP KAPA FAULT QEMU BOOT VALIDATION FAILED ==="
    if (Test-Path $stderrPath) {
        Write-Host "--- qemu stderr ---"
        Get-Content $stderrPath -TotalCount 40 | ForEach-Object { Write-Host $_ }
    }
    exit 1
}
Write-Host "=== SWEEP KAPA FAULT QEMU BOOT VALIDATION PASSED ==="
exit 0
