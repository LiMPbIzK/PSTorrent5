# Build del payload en Windows (nativo)

Compila `pstorrent5.elf` en Windows **sin WSL, sin `sudo`, sin `make` y sin
dependencias del sistema**. Todo queda dentro del proyecto o en `J:`.

## Requisitos

| Pieza | Ruta esperada | Notas |
|---|---|---|
| Toolchain C/C++ | `J:\llvm-mingw\bin` | clang + lld portátiles. Se puede sobreescribir con `$env:LLVM_MINGW` |
| SDK de PS5 | `.deps\ps5-payload-sdk` | ps5-payload-sdk **v0.44**. Se puede sobreescribir con `$env:PS5_PAYLOAD_SDK` |
| Python | `J:\Python\Python313\python.exe` | Solo para regenerar el panel web embebido |

Preparar la toolchain:

```powershell
# llvm-mingw (portátil, no requiere administrador)
curl.exe -sSL -o llvm-mingw.zip https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip
& 'C:\Program Files\7-Zip\7z.exe' x llvm-mingw.zip -o'J:\' -y
Rename-Item 'J:\llvm-mingw-20261006-ucrt-x86_64' 'J:\llvm-mingw'

# SDK v0.44
New-Item -ItemType Directory -Force -Path .deps | Out-Null
curl.exe -sSL -o ps5-payload-sdk.zip https://github.com/ps5-payload-dev/sdk/releases/download/v0.44/ps5-payload-sdk.zip
Expand-Archive ps5-payload-sdk.zip -DestinationPath .deps -Force
```

## Uso

```powershell
.\build.ps1              # build incremental
.\build.ps1 -Clean       # borra objetos y ELF
.\build.ps1 -Rebuild     # recompila todo sin borrar
```

Salida: `pstorrent5.elf` en la raíz del proyecto.

El script **lee `SRCS`, `CFLAGS`, `INCLUDES` y `LDLIBS` del `Makefile`**, que
sigue siendo la única fuente de verdad, así que no puede desincronizarse del
build de Linux/CI. También regenera `src/web_content.h` desde
`src/web/index.html` cuando cambia.

Enviar a la consola (loader en el puerto 9021):

```powershell
curl.exe --data-binary "@pstorrent5.elf" http://<IP_PS5>:9021/
```

## Cómo funciona (y por qué hay un shim)

El script reproduce lo que hace `toolchain/prospero.mk` + `bin/prospero-clang`
en Linux, pero sobre clang de Windows:

1. **clang 20 o superior ya incluye la toolchain de PS5.** Con
   `-target x86_64-sie-ps5` y `SCE_PROSPERO_SDK_DIR` apuntando al SDK, clang
   añade por sí mismo los objetos de arranque (`crt1.o`, `crti`, `crtbegin`,
   `crtend`, `crtn`), el script de enlazado `main.script` y las libs stub
   (`-lc_stub_weak`, `-lkernel_stub_weak`). **No hay que pasar `crt1.o` ni
   `-T ldscripts/elf_x86_64.x`**: eso era la ruta de clang < 20.
2. **clang exige un enlazador llamado `prospero-lld`** e ignora `--ld-path`.
3. **lld elige su modo por el nombre del ejecutable (`argv[0]`).** Renombrar un
   `ld.lld.exe` a `prospero-lld.exe` falla con *"lld is a generic driver"*.
   Solución: `tools/prospero-lld/prospero-lld.c`, un shim mínimo que hace
   `_execvp("ld.lld.exe", argv)` con `argv[0]` correcto. El script lo compila
   solo a `.deps\bin\prospero-lld.exe` (se puede forzar otro lld con
   `$env:PROSPERO_LLD`).

## Escollos conocidos

- **Las libs SCE del SDK son `.so`, no `.a`** (`libSceSystemService.so`,
  `libkernel_web.so`…). `-lSceSystemService` resuelve bien vía `-L target\lib`.
- **El volumen de `J:` no trunca al sobrescribir.** Si el `pstorrent5.elf`
  anterior era mayor, el nuevo queda con cola del binario viejo y un tamaño
  falso. El script borra el ELF antes de enlazar; si compilas a mano, hazlo tú.
- **`J:` puede tardar en publicar un fichero recién creado**, así que
  `Test-Path` justo después de enlazar puede dar falso. El script reintenta
  hasta 5 s y además valida la cabecera ELF (magic `\x7fELF`) para no dar por
  bueno un fichero parcial.
- **Cada enlace produce bytes distintos** porque clang añade
  `--build-id=uuid`. El tamaño es estable; el hash no. No lo uses para
  comparar builds; compara secciones o símbolos.
- `llvm-readelf` avisa de `invalid PT_DYNAMIC`: es esperable, el script de
  enlazado de PS5 no sigue la disposición ELF estándar.

## Validación

Un build correcto debe mostrar:

```
.text            01280c    (≈ 74 KB)
.rodata          009c00    (≈ 39 KB, incluye el panel web embebido)
.dynsym          0006d8
.data.rel.ro     000890
```

Y `.text` en la dirección `0x4000` con secciones alineadas a 16 KB (la página
de memoria de la PS5). El binario del autor (v2.0.0) coincide en `.dynsym`,
`.data.rel.ro` y `.rodata`; sólo difiere `.text`, por la versión de compilador.
