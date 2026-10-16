<#
.SYNOPSIS
    Compila el payload PSTorrent5 (ELF) en Windows sin WSL ni make.

.DESCRIPTION
    El Makefile del proyecto es la única fuente de verdad: este script lee de él
    SRCS, CFLAGS, INCLUDES y LDLIBS, de modo que nunca se desincronizan.

    Reproduce lo que hace el SDK en Linux (toolchain/prospero.mk + bin/prospero-clang):
      * compilador: clang con -target x86_64-sie-ps5 (llvm-mingw, portable)
      * enlazador:  ld.lld invocado como "prospero-lld" (shim en tools/prospero-lld)
      * clang >= 20 aporta por sí solo los crt, main.script y libs stub, por lo
        que se define SCE_PROSPERO_SDK_DIR y NO se pasa crt1.o.

.PARAMETER Clean
    Borra los objetos y el ELF generados.

.PARAMETER Rebuild
    Recompila todo (ignora la caché incremental de objetos).

.PARAMETER SdkPath
    Ruta al ps5-payload-sdk. Por defecto: $env:PS5_PAYLOAD_SDK o .deps\ps5-payload-sdk.

.PARAMETER ClangPath
    Directorio con clang.exe/ld.lld.exe. Por defecto: $env:LLVM_MINGW o J:\llvm-mingw\bin.

.EXAMPLE
    .\build.ps1
    Compila el ELF de forma incremental.

.EXAMPLE
    .\build.ps1 -Clean; .\build.ps1
    Build limpio.

.NOTES
    Requisitos: llvm-mingw (clang), ps5-payload-sdk y Python para el panel web.
    SPDX-License-Identifier: GPL-3.0-or-later
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Rebuild,
    [string]$SdkPath,
    [string]$ClangPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root    = $PSScriptRoot
$ObjDir  = Join-Path $Root '.deps\obj'
$BinDir  = Join-Path $Root '.deps\bin'
$Elf     = Join-Path $Root 'pstorrent5.elf'

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }
function Fail([string]$Text)       { Write-Host "ERROR: $Text" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- limpieza ---
if ($Clean) {
    Write-Step 'Limpiando objetos y ELF'
    foreach ($p in @($ObjDir, $Elf)) {
        if (Test-Path $p) { Remove-Item $p -Recurse -Force }
    }
    Write-Host 'Limpieza completada.'
    exit 0
}

# ------------------------------------------------------------------ rutas ----
if (-not $SdkPath) { $SdkPath = $env:PS5_PAYLOAD_SDK }
if (-not $SdkPath) { $SdkPath = Join-Path $Root '.deps\ps5-payload-sdk' }
if (-not (Test-Path (Join-Path $SdkPath 'toolchain\prospero.mk'))) {
    Fail "SDK no encontrado en '$SdkPath'. Ejecuta setup.sh en Linux/WSL o descarga ps5-payload-sdk v0.44 en .deps\ps5-payload-sdk"
}
$SdkPath = (Resolve-Path $SdkPath).Path

if (-not $ClangPath) { $ClangPath = $env:LLVM_MINGW }
if (-not $ClangPath) { $ClangPath = 'J:\llvm-mingw\bin' }
$Clang = Join-Path $ClangPath 'clang.exe'
$Lld   = Join-Path $ClangPath 'ld.lld.exe'
if (-not (Test-Path $Clang)) { Fail "clang.exe no encontrado en '$ClangPath'. Descarga llvm-mingw o define `$env:LLVM_MINGW" }
if (-not (Test-Path $Lld))   { Fail "ld.lld.exe no encontrado en '$ClangPath'" }

# shim prospero-lld: clang busca ese nombre y lld elige driver por argv[0]
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
$Shim = Join-Path $BinDir 'prospero-lld.exe'
$ShimSrc = Join-Path $Root 'tools\prospero-lld\prospero-lld.c'
if ((Test-Path $ShimSrc) -and (-not (Test-Path $Shim) -or (Get-Item $ShimSrc).LastWriteTime -gt (Get-Item $Shim).LastWriteTime)) {
    Write-Step 'Compilando el shim prospero-lld'
    & $Clang --target=x86_64-w64-windows-gnu -O2 -Wall -Wextra -o $Shim $ShimSrc
    if ($LASTEXITCODE -ne 0) { Fail 'no se pudo compilar prospero-lld' }
}
if (-not (Test-Path $Shim)) { Fail "falta el shim '$Shim'" }

# ------------------------------------------------------------------ Makefile --
$Makefile = Join-Path $Root 'Makefile'
$mkLines = Get-Content -Path $Makefile
# Lee el valor de una variable del Makefile respetando continuaciones con '\'.
function Get-MakeVar([string]$Name) {
    $esc = [regex]::Escape($Name)
    $chunks = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $mkLines.Count; $i++) {
        if ($mkLines[$i] -notmatch "^$esc\s*[:?+]?=") { continue }
        $chunk = $mkLines[$i]
        while ($chunk.TrimEnd().EndsWith('\') -and ($i + 1) -lt $mkLines.Count) {
            $chunk = $chunk.TrimEnd() -replace '\\$', ''
            $i++
            $chunk += ' ' + $mkLines[$i].Trim()
        }
        $chunks.Add(($chunk -replace "^$esc\s*[:?+]?=", ''))
    }
    if ($chunks.Count -eq 0) { return @() }
    return (($chunks -join ' ') -split '\s+' | Where-Object { $_ -ne '' })
}

$Srcs     = @(Get-MakeVar 'SRCS'    | Where-Object { $_ -like '*.c' })
$CFlags   = @(Get-MakeVar 'CFLAGS')
$Includes = @(Get-MakeVar 'INCLUDES')
$LdLibs   = @(Get-MakeVar 'LDLIBS')
if ($Srcs.Count -eq 0) { Fail 'no se pudo leer SRCS del Makefile' }

Write-Step "Makefile: $($Srcs.Count) fuentes, $($LdLibs.Count) libs de enlace"

# entorno
$env:PATH = "$BinDir;$ClangPath;$env:PATH"
$env:SCE_PROSPERO_SDK_DIR = $SdkPath

# flags comunes del SDK (equivalen a toolchain/prospero.mk + bin/prospero-clang)
$TargetFlags = @(
    '--start-no-unused-arguments',
    '-target', 'x86_64-sie-ps5',
    '-fvisibility-nodllstorageclass=default',
    '-isysroot', $SdkPath,
    '-isystem', (Join-Path $SdkPath 'target\include'),
    '-fno-stack-protector', '-fno-plt', '-femulated-tls',
    '--end-no-unused-arguments'
)
$LinkSearchFlags = @(
    '--start-no-unused-arguments',
    '-target', 'x86_64-sie-ps5',
    '-L', (Join-Path $SdkPath 'target\lib'),
    '-L', (Join-Path $SdkPath 'target\user\homebrew\lib'),
    '-fno-stack-protector', '-fno-plt', '-femulated-tls', '-lc',
    '--end-no-unused-arguments'
)
# libs implícitas que el SDK añade al enlazar
$SdkLibs = @('-lkernel_web', '-lSceLibcInternal', '-lSceNet')

# ---------------------------------------------------------- panel web (gen) ---
$WebIndex = Join-Path $Root 'src\web\index.html'
$WebEmbed = Join-Path $Root 'scripts\embed_web.py'
$WebHeader = Join-Path $Root 'src\web_content.h'
$NeedEmbed = (-not (Test-Path $WebHeader)) -or
             ((Get-Item $WebIndex).LastWriteTime -gt (Get-Item $WebHeader).LastWriteTime) -or
             ((Get-Item $WebEmbed).LastWriteTime -gt (Get-Item $WebHeader).LastWriteTime)
if ($NeedEmbed) {
    $python = 'J:\Python\Python313\python.exe'
    if (-not (Test-Path $python)) { $python = (Get-Command python -ErrorAction SilentlyContinue).Source }
    if (-not $python) { Fail 'Python no encontrado (necesario para generar src\web_content.h)' }
    Write-Step 'Generando src\web_content.h desde src\web\index.html'
    & $python $WebEmbed $WebIndex $WebHeader
    if ($LASTEXITCODE -ne 0) { Fail 'falló scripts/embed_web.py' }
}

# --------------------------------------------------------------- compilar ----
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null
$Headers = Get-ChildItem (Join-Path $Root 'include') -Filter '*.h' -ErrorAction SilentlyContinue

$objects = @()
$compiled = 0
foreach ($src in $Srcs) {
    $srcPath = Join-Path $Root $src
    if (-not (Test-Path $srcPath)) { Fail "fuente no encontrada: $src" }
    $obj = Join-Path $ObjDir (([IO.Path]::GetFileNameWithoutExtension($src)) + '.o')
    $objects += $obj

    $objTime = if (Test-Path $obj) { (Get-Item $obj).LastWriteTime } else { [datetime]::MinValue }
    $newer = (Get-Item $srcPath).LastWriteTime -gt $objTime
    if (-not $newer -and -not $Rebuild) {
        $newer = ($Headers | Where-Object { $_.LastWriteTime -gt $objTime } | Select-Object -First 1) -ne $null
    }
    if (-not $newer) { continue }

    Write-Host ("    CC  " + $src) -ForegroundColor DarkGray
    & $Clang @TargetFlags @CFlags @Includes $srcPath '-c' '-o' $obj
    if ($LASTEXITCODE -ne 0) { Fail "error compilando $src" }
    $compiled++
}
Write-Step "Objetos: $compiled recompilados, $($objects.Count - $compiled) al día"

# ----------------------------------------------------------------- enlazar ----
Write-Step 'Enlazando pstorrent5.elf'
# El volumen del proyecto no siempre trunca al sobrescribir: si el ELF anterior
# era mayor, quedaria cola del binario viejo. Se borra antes de enlazar.
if (Test-Path $Elf) { Remove-Item $Elf -Force }

& $Clang @LinkSearchFlags `
    @objects `
    '--start-no-unused-arguments' '--sysroot' $SdkPath `
    @SdkLibs @LdLibs `
    '--end-no-unused-arguments' `
    '-o' $Elf
if ($LASTEXITCODE -ne 0) { Fail 'el enlazado falló' }

# Algunos volumenes (unidades sincronizadas/virtuales) tardan en publicar el
# fichero recien creado, asi que se reintenta antes de darlo por perdido.
for ($i = 0; $i -lt 20 -and -not (Test-Path $Elf); $i++) { Start-Sleep -Milliseconds 250 }
if (-not (Test-Path $Elf)) { Fail 'no se generó el ELF' }

# Verifica que es un ELF real (evita dar por bueno un fichero parcial).
$magic = Get-Content $Elf -Encoding Byte -TotalCount 4
if ($magic.Count -lt 4 -or $magic[0] -ne 0x7F -or $magic[1] -ne 0x45 -or $magic[2] -ne 0x4C -or $magic[3] -ne 0x46) {
    Fail "el fichero generado no es un ELF válido ($Elf)"
}

$size = (Get-Item $Elf).Length
Write-Host ''
Write-Host ('Build correcto: pstorrent5.elf ({0:N0} bytes / {1:N1} KB)' -f $size, ($size / 1KB)) -ForegroundColor Green
Write-Host 'Enviar a la consola:  make test PS5_HOST=<IP> PS5_PORT=9021   (o usa un enviador de payloads)'
