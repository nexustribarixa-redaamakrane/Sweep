# qemu_sweep_kernel_test.ps1 - Boot the reduced sweep-kernel multiboot image
# under QEMU and validate observable boot milestones over the serial console.
#
# Usage:
#   pwsh -ExecutionPolicy Bypass -File ./tools/qemu_sweep_kernel_test.ps1 \
#       <path-to-sweep-kernel-qemu.bin> [output-log-path]
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
    Write-Host "usage: qemu_sweep_kernel_test.ps1 <flat multiboot image> [log-path]"
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

$work = Join-Path $env:TEMP "openwinkrnl_qemu_sweep_kernel"
New-Item -ItemType Directory -Path $work -Force | Out-Null
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $work "sweep_kernel_qemu.log"
}
$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$port = 13000 + ($PID % 20000)
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

# Sweep profile halts in idle loop (no interactive shell). Wait for terminal marker.
Write-Host "Waiting for sweep-kernel boot milestones..."
if (-not (Wait-For -Pattern "\[SWEEP-KERNEL\] Reduced profile idle" -Seconds 120)) {
    Write-Host "[FAIL] sweep-kernel idle marker not observed"
    $failed = $true
}
Start-Sleep -Milliseconds 500
Read-Available

$out = $sb.ToString()
Set-Content -Path $LogPath -Value $out
$stream.Close(); $client.Close()
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }

$checks = @(
    "[STARTED] HAL Init",
    "[KDB] registry online:",
    "[KDB] service bridge online:",
    "sweep-kernel profile: KDB/KAPA substrate ready",
    "kernel boot complete in ",
    "[SWEEP-KERNEL] Reduced profile idle (no native Sweep execution)."
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
    "OpenWindows DEBUG SYSTEM CONTROL SHELL"
)

foreach ($c in $checks) {
    if ($out.Contains($c)) { Write-Host "[PASS] $c" }
    else { Write-Host "[FAIL] missing: $c"; $failed = $true }
}
foreach ($c in $forbidden) {
    if ($out.Contains($c)) { Write-Host "[FAIL] unexpected (excluded subsystem/profile marker): $c"; $failed = $true }
    else { Write-Host "[PASS] absent: $c" }
}

Write-Host ""
Write-Host "Log: $LogPath"
if ($failed) {
    Write-Host "=== SWEEP KERNEL QEMU BOOT VALIDATION FAILED ==="
    if (Test-Path $stderrPath) {
        Write-Host "--- qemu stderr ---"
        Get-Content $stderrPath -TotalCount 40 | ForEach-Object { Write-Host $_ }
    }
    exit 1
}
Write-Host "=== SWEEP KERNEL QEMU BOOT VALIDATION PASSED ==="
exit 0
