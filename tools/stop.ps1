<#
.SYNOPSIS
    Para PSTorrent5 en la consola sin pelearse con Payload Manager.

.DESCRIPTION
    Payload Manager muestra el proceso como "payload.elf" (es el fichero temporal
    desde el que el loader hace exec, no un fallo nuestro), asi que identificarlo
    a mano es engorroso. Este script le pide al payload que se cierre por su
    propia API (POST /api/quit) y espera a que libere el puerto del panel.

.EXAMPLE
    .\tools\stop.ps1 -PS5Host 192.168.1.50

.EXAMPLE
    # Con el puerto por defecto y salida silenciosa si ya estaba parado
    .\tools\stop.ps1 -PS5Host 192.168.1.50
#>
param(
    [Parameter(Mandatory = $true)][string]$PS5Host,
    [int]$PanelPort = 12389,
    [int]$TimeoutSec = 15
)

$ErrorActionPreference = 'Stop'

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

if (-not (Test-TcpPort $PS5Host $PanelPort)) {
    Write-Host "No hay nada escuchando en ${PS5Host}:${PanelPort}: ya esta parado." -ForegroundColor Yellow
    exit 0
}

Write-Host "==> Pidiendo el cierre a http://${PS5Host}:${PanelPort}/api/quit" -ForegroundColor Cyan
try {
    Invoke-WebRequest -Uri "http://${PS5Host}:${PanelPort}/api/quit" `
        -Method Post -TimeoutSec 10 -UseBasicParsing | Out-Null
} catch {
    Write-Host "    El payload no contesto al cierre: $($_.Exception.Message)" -ForegroundColor Yellow
}

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and (Test-TcpPort $PS5Host $PanelPort)) {
    Start-Sleep -Milliseconds 300
}

if (Test-TcpPort $PS5Host $PanelPort) {
    Write-Host "    El puerto $PanelPort sigue ocupado tras $TimeoutSec s." -ForegroundColor Red
    Write-Host "    Si el payload esta colgado, solo queda relanzar la consola." -ForegroundColor Red
    exit 1
}

Write-Host "    Parado: el puerto $PanelPort ha quedado libre." -ForegroundColor Green
