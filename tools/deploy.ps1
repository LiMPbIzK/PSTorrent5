<#
.SYNOPSIS
    Despliega el payload PSTorrent5 en la PS5 sin reiniciar la consola.

.DESCRIPTION
    Un payload no puede reemplazar a otro que sigue en marcha: la instancia
    antigua mantiene ocupado el puerto 12389 y la nueva falla al arrancar. Este
    script hace el ciclo completo:

      1. (opcional) compila el ELF con build.ps1
      2. pide al payload en marcha que se cierre  ->  POST /api/quit
      3. espera a que el puerto 12389 quede libre
      4. sube el ELF nuevo al loader                ->  POST http://IP:9021/
      5. espera a que el panel vuelva y muestra su estado

    Si no hay ninguna instancia corriendo, los pasos 2 y 3 se saltan solos.

.EXAMPLE
    .\tools\deploy.ps1 -PS5Host 192.168.1.50

.EXAMPLE
    .\tools\deploy.ps1 -PS5Host 192.168.1.50 -Build

.EXAMPLE
    # Otro loader (por ejemplo el 9020 de algunos enviadores)
    .\tools\deploy.ps1 -PS5Host 192.168.1.50 -LoaderPort 9020

.NOTES
    Requisitos: curl.exe (incluido en Windows 10+) para subir el binario.
    SPDX-License-Identifier: GPL-3.0-or-later
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PS5Host,
    [int]$LoaderPort = 9021,
    [int]$PanelPort = 12389,
    [string]$Elf,
    [switch]$Build,
    [int]$StopTimeoutSec = 15,
    [int]$StartTimeoutSec = 30
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path $PSScriptRoot -Parent
if (-not $Elf) { $Elf = Join-Path $Root 'pstorrent5.elf' }

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-Ok([string]$Text)   { Write-Host "    $Text" -ForegroundColor Green }
function Write-Warn2([string]$Text){ Write-Host "    $Text" -ForegroundColor Yellow }

function Test-TcpPort {
    param([string]$Target, [int]$Port, [int]$TimeoutMs = 1000)
    $client = New-Object Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($Target, $Port, $null, $null)
        if ($iar.AsyncWaitHandle.WaitOne($TimeoutMs)) {
            $client.EndConnect($iar)
            return $true
        }
        return $false
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

function Get-PanelStatus {
    param([string]$Target, [int]$Port)
    try {
        return Invoke-RestMethod -Uri "http://${Target}:${Port}/api/status" `
            -TimeoutSec 4 -UseBasicParsing
    } catch {
        return $null
    }
}

# ------------------------------------------------------------------ build ----
if ($Build) {
    Write-Step 'Compilando el ELF'
    & (Join-Path $Root 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'build.ps1 falló' }
}

if (-not (Test-Path $Elf)) {
    throw "no se encuentra el ELF: $Elf (usa -Build o compílalo antes)"
}
$elfInfo = Get-Item $Elf
Write-Step ("ELF: {0} ({1:N0} bytes)" -f $elfInfo.Name, $elfInfo.Length)

# --------------------------------------------------------- parar el viejo ---
if (Test-TcpPort -Target $PS5Host -Port $PanelPort) {
    Write-Step "Pidiendo al payload en marcha que se cierre (POST /api/quit)"
    try {
        Invoke-WebRequest -Uri "http://${PS5Host}:${PanelPort}/api/quit" `
            -Method Post -TimeoutSec 4 -UseBasicParsing | Out-Null
        Write-Ok 'cerrado solicitado'
    } catch {
        Write-Warn2 "no se pudo pedir el cierre ($($_.Exception.Message))"
    }

    Write-Step 'Esperando a que se libere el puerto del panel'
    $deadline = (Get-Date).AddSeconds($StopTimeoutSec)
    while ((Get-Date) -lt $deadline -and (Test-TcpPort -Target $PS5Host -Port $PanelPort)) {
        Start-Sleep -Milliseconds 400
    }
    if (Test-TcpPort -Target $PS5Host -Port $PanelPort) {
        Write-Warn2 "el puerto $PanelPort sigue ocupado tras $StopTimeoutSec s"
        Write-Warn2 '¿hay otra instancia lanzada a mano? Puede que la respuesta del loader falle al reutilizar el puerto.'
    } else {
        Write-Ok "puerto $PanelPort libre"
    }
} else {
    Write-Step "No hay instancia escuchando en el puerto $PanelPort; se despliega directamente"
}

# ------------------------------------------------------------ subir nuevo ---
Write-Step "Enviando el ELF al loader ${PS5Host}:${LoaderPort}"
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
if (-not $curl) { throw 'curl.exe no encontrado; es necesario para subir el binario' }

& $curl -sS --max-time 120 --data-binary "@$Elf" "http://${PS5Host}:${LoaderPort}/"
if ($LASTEXITCODE -ne 0) { throw "el loader rechazó el ELF (curl exit $LASTEXITCODE)" }
Write-Ok 'ELF enviado'

# --------------------------------------------------------- esperar panel ----
Write-Step 'Esperando a que el panel responda'
$deadline = (Get-Date).AddSeconds($StartTimeoutSec)
$status = $null
while ((Get-Date) -lt $deadline) {
    $status = Get-PanelStatus -Target $PS5Host -Port $PanelPort
    if ($status) { break }
    Start-Sleep -Milliseconds 700
}

if (-not $status) {
    Write-Warn2 "el panel no respondió en $StartTimeoutSec s"
    Write-Warn2 "comprueba el log: http://${PS5Host}:${PanelPort}/api/log"
    exit 1
}

Write-Ok ("PSTorrent5 v{0} en marcha" -f $status.version)
Write-Output ("    torrents: {0}/{1} · etaHEN: {2}" -f $status.torrents, $status.capacity, $status.jailbroken)
if ($status.log_path) { Write-Output ("    log: {0}" -f $status.log_path) }
Write-Output ("    panel: http://{0}:{1}/" -f $PS5Host, $PanelPort)
