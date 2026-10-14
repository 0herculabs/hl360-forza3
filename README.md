# Forza Motorsport 3 — recompilación para PC

Recompilación estática de *Forza Motorsport 3: Ultimate Collection* para
ejecutarlo de forma nativa en PC. El código PowerPC del ejecutable de Xbox 360
se convierte a C++ mediante [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk),
mientras el runtime proporciona los servicios de la consola que el juego
necesita: GPU, audio, entrada, perfiles, sistema de archivos y contenido.

El proyecto incluye un launcher para instalar y verificar los dos discos,
gestionar perfiles, configurar el port y administrar Project Forza Plus 3.

> **No se incluyen archivos del juego.** Necesitas tus propias copias de los
> discos originales de Xbox 360.

> **Estado:** trabajo en curso. El juego arranca, permite navegar por los
> menús, reproduce los vídeos y ejecuta carreras a 60 FPS, pero la trayectoria
> completa, el multijugador local y otros modos todavía necesitan más pruebas.
> Xbox Live no está disponible.

## Versión compatible

Actualmente solo se admite la edición **Forza Motorsport 3: Ultimate
Collection (USA)**, título `4D53084D`, Media ID `6F74359D`, con
`default.xex` versión `0.0.0.8`.

Estas son las imágenes exactas usadas para desarrollar y probar el port:

| Disco | Tamaño | MD5 |
|-------|-------:|-----|
| Disc 1 — Play Disc | 7.838.695.424 bytes | `A527154D55C7B39849EC68CD46F17281` |
| Disc 2 — Content Install Disc | 7.838.695.424 bytes | `E65FF1CDC189F3EB9B1D0F14F97BAEA4` |

Los MD5 sirven para identificar el volcado, no como garantía criptográfica.
El launcher comprueba después los archivos extraídos del Disco 1 mediante
SHA-256. Otras regiones, ediciones o revisiones pueden contener un XEX distinto
y no están soportadas actualmente.

## Requisitos

- Windows 10/11 x64
- Visual Studio 2022 Build Tools (MSVC + Windows SDK)
- LLVM/Clang 20 o superior (`winget install LLVM.LLVM`)
- CMake 3.25+ y Ninja
- Git (con `sh` de Git for Windows en el PATH)

## Compilar

```bat
:: 1. SDK (una vez; aplica patches\sdk\*.patch e instala en out\install)
scripts\build_sdk.cmd

:: 2. Juego + launcher + herramientas -> out\build\win-amd64-release
scripts\build.cmd release
```

`scripts\devenv.cmd` carga el entorno de MSVC y pone Clang
(`C:\Program Files\LLVM\bin`) al principio del PATH.

El código recompilado se genera en `generated\default\` a partir de
`fm3_manifest.toml` y de `config\*.toml`; la compilación vuelve a ejecutar el
codegen cuando cambia alguno de ellos.

## Launcher (`fm3-launcher.exe`)

Basado en el launcher de la recompilación de Forza Motorsport 2 (SDL3 +
SDL_GPU + Dear ImGui), en español o inglés (botones EN/ES; la primera vez
sigue el idioma del sistema).

| Perfiles | Gráficos |
|----------|----------|
| ![Perfiles locales con partidas independientes](doc/media/launcher-profiles.png) | ![Preajustes y opciones gráficas](doc/media/launcher-graphics.png) |

| Project Forza Plus 3 | Instalación y verificación |
|----------------------|----------------------------|
| ![Configuración de Project Forza Plus 3](doc/media/launcher-fm3-plus.png) | ![Instalación de los dos discos](doc/media/launcher-install.png) |

- **Instalación** — asistente en dos pasos; el disco se detecta solo:
  1. *Disco 1 (Play Disc)*: se extrae la imagen ISO (unos 7 GB, sin
     `$SystemUpdate`) en la carpeta del juego y se comprueba cada archivo con
     SHA-256 contra `game_files.sha256`.
  2. *Disco 2 (Content Install Disc)*: sus 8 paquetes (coches y circuitos)
     se sacan de la ISO y `fm3tool install-dlc` los instala en los datos de
     usuario con su cabecera de licencia, como los lee el juego. La ruta de
     los datos de usuario no puede pasar de 96 caracteres (los archivos del
     Disco 2 llegan a 162 más y Windows limita a 260).
  - La portada de la columna izquierda se extrae del `nxeart` de tu Disco 1.
- **Perfiles** — crear, activar y eliminar. Cada perfil tiene un XUID offline
  propio, así que sus partidas van en carpetas separadas (`user\<XUID>\…`);
  al eliminarlo se mueven a `user\deleted-profiles\`.
- **Gráficos** — pantalla completa/ventana, monitor, tamaño de ventana y
  16:9; preajustes (720p / 1440p / 4K), resolución interna 1×–4×, filtrado
  anisótropo, FXAA y escalado (bilineal / AMD CAS / AMD FSR). El port fija
  Direct3D 12 y `readback_resolve = "fast"`: FM3 necesita la lectura para la
  carretera y las miniaturas, y el modo síncrono limita el juego a 30 FPS.
- **Controles** — SDL/XInput, teclado como mando con teclas reasignables,
  ratón como stick derecho.
- **Idioma y sonido** — idioma del juego y silencio.
- **FM3 Plus** — instala, actualiza y desinstala Project Forza Plus 3 desde
  sus ZIP. Permite elegir modo de trayectoria, IA difícil, partidas de inicio
  rápido y FOV. Sus 18 paquetes de contenido sustituyen temporalmente a los 8
  de Ultimate Collection; los originales se conservan y vuelven al quitar el
  mod.

Modos sin interfaz (los mismos caminos que la UI, para pruebas):

```bat
fm3-launcher --import-iso <iso> --game-dir <dir> --user-dir <dir> [--log <archivo>]
fm3-launcher --verify --game-dir <dir> [--log <archivo>]
fm3-launcher --screenshot <archivo.bmp> [--tab profiles|graphics|controls|audio|plus|install]
```

### Configuración

Junto a `fm3.exe`:

| Archivo | Contenido |
|---------|-----------|
| `fm3.toml` | Ajustes del port (`config\fm3.toml`), se copia al compilar |
| `fm3.user.toml` | Lo que elige el jugador; lo escribe el launcher y gana sobre `fm3.toml`. Incluye carpetas y perfil activo |
| `launcher.toml` | Idioma del launcher y lista de perfiles |
| `game_files.sha256` | Lista de archivos del Disco 1 (`tools\make_manifest.py`) |
| `cover.jpg` | Portada extraída del disco |
| `logs\` | Registros (`fm3_*.log`) e informes de fallo (`crash_*.txt` + `.dmp`) |

`fm3.exe` se puede ejecutar sin argumentos (usa esos archivos). Dentro del
juego, F4 abre el editor de cvars del SDK y F3 la superposición de depuración.

## Estructura

| Ruta | Contenido |
|------|-----------|
| `fm3_manifest.toml` | Manifiesto de ReXGlue (XEX de entrada, salida del codegen) |
| `config/codegen.toml` | Límites de funciones, `setjmp`/`longjmp` y fibras (`[rexcrt]`) |
| `config/fptr_functions.toml` | Funciones solo alcanzables por punteros |
| `config/fm3.toml` | Ajustes del port que acompañan al ejecutable |
| `src/` | Aplicación del juego (`Fm3App`) e informe de fallos |
| `launcher/` | Launcher e instalador |
| `tools/fm3tool/` | Extracción de ISO/STFS, instalación del DLC, volcado del XEX |
| `tools/*.py` | Desensamblador, búsqueda de punteros a funciones, manifiesto |
| `patches/sdk/` | Cambios sobre el SDK (ver abajo) |
| `scripts/` | Compilación e instalación por línea de comandos |

## Notas del port

- **Codegen**: 15 funciones solo alcanzables por *tail call* o vtables y 3
  getters con un epílogo compartido necesitaron límites explícitos.
  `config/fptr_functions.toml` declara ~200 funciones a las que solo se llega
  por punteros (vtables, callbacks, entradas de hilos): lo genera
  `tools/find_fptrs.py` (punteros en `.rdata`/`.data` y pares `lis`+`addi`) y
  lo depura `tools/prune_fptrs.py`. Si el juego se detiene con *"Call to
  invalid or unregistered function at guest address X"*, falta X ahí.
- **Fibras** de XAPI → implementación nativa del SDK (`[rexcrt]`).
- **setjmp/longjmp** (`0x82534B20` / `0x825345E0`): sin declararlos, el
  manejo de errores de libjpeg dejaba registros corruptos y el juego fallaba
  al entrar en carrera.
- **`cache:`**: el juego escribe ahí (p. ej. `cache:\replay_stream` en
  carrera) sin montarlo; `Fm3App` lo monta en `user\cache_partition`.
- **Disco 2**: el contenido necesita su cabecera con licencia; se instala con
  el `ContentManager::InstallContent` del SDK.
- **Project Forza Plus 3**: el mod trae un conjunto alternativo de 18 paquetes
  ya extraídos. El launcher aparta los 8 paquetes de Ultimate Collection,
  instala los del mod y revierte toda la operación al desinstalarlo. Su
  `default.xex` no se usa porque pertenece a la edición original de 2009; el
  port recalcula en memoria la tabla de firmas de los archivos protegidos.
- **Render-to-texture**: `readback_resolve = "fast"` queda fijado y no se
  muestra en el launcher. La lectura es necesaria para la carretera y las
  miniaturas de coches; `full` es síncrono y limita FM3 a 30 FPS.
- **Ritmo de fotogramas**: el cvar `vsync` del SDK no es la sincronización del
  monitor sino el temporizador de vblank de la consola; FM3 se sincroniza con
  él y sin él va acelerado, así que `fm3.toml` lo fija y el launcher no lo
  ofrece. La notificación de logros se desactiva: mientras existe, el
  presentador repinta la ventana a la frecuencia del monitor (cientos de
  "FPS" con tirones). Además, compilación de pipelines síncrona y
  `gpu_allow_invalid_fetch_constants`, como en la recompilación de FM2.
- **Audio**: FM3 entrega bloques de solo 256 muestras (5,33 ms a 48 kHz). Se
  usa `audio_maxqframes = 16` para tolerar tirones breves de CPU/GPU sin que la
  cola se vacíe y SDL introduzca silencios audibles; supone unos 85 ms de
  margen máximo.

### Cambios sobre el SDK (`patches/sdk/`)

1. `0001-user-profile-setters` — nombre y XUID del perfil activo.
2. `0002-content-diagnostics` — registros de enumeración/apertura de contenido.
3. `0003-gpu-fixes-from-xenia` — arreglos de GPU de xenia-canary/xenia-edge
   posteriores a la base del SDK (xenia-edge, marzo de 2026), relacionados con
   las sombras cuadradas y la carretera de FM3 (informe de compatibilidad #409):
   - `d119505`: lecturas de texturas con `num_format` entero reescaladas.
   - edge `c0e1129d7`: alfa del color de borde del sampler forzado a 1.
   - edge `4e2964f85`: muestreo puntual para formatos sin filtrado lineal (D3D12).
   - edge `9781a75a2`: no dar por cargada una textura cuya memoria se invalidó.
   - edge `c02060f22`: *downscale* de la lectura de resolves con escala de
     resolución > 1× (sombreadores HLSL y GLSL recompilados: `fxc` del Windows
     SDK y `glslangValidator`, que compila `scripts\build_glslang.cmd`).
   - edge `2d7ca4fb3`: los resolves vacíos no son un error (~170 errores/s de
     registro menos en FM3).

`scripts\build_sdk.cmd` compila el SDK con `REXGLUE_USE_VULKAN=ON`; el port
selecciona Direct3D 12 porque el backend Vulkan todavía renderiza FM3 de forma
incorrecta.

## Ajustes de Xenia

Los ajustes de compatibilidad de partida proceden de
[xenia-manager/optimized-settings](https://github.com/xenia-manager/optimized-settings)
y del
[informe de compatibilidad #409](https://github.com/xenia-canary/game-compatibility/issues/409).
Xenia recomienda `readback_resolve = "full"` buscando exactitud, pero este port
usa `fast`: conserva las lecturas necesarias de FM3 sin reducir la salida a
30 FPS.
El único parche de `game-patches` para FM3 (*Disable hash checks*) no
coincide con este `default.xex` y no se aplica.
