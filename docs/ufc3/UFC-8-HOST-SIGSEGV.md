# UFC-8: SIGSEGV en conversión de hora local

## Evidencia y causa

Sesión `20261015_174129.log`, versión `ufc3-v0.1.2-31-g6145472`,
RTX 4070. No se ejecutó el juego durante esta investigación.
No hay core de esta sesión en `coredumpctl`; el diagnóstico usa el binario
exacto, el log y la imagen enlazada de esa compilación, leídos del checkout
principal. Las copias de análisis quedan en `out/ufc8-evidence/` (ignoradas).
SHA-256 del ejecutable exacto:
`a118a5cd17220362a66d17218661b1dc8dad7bafc04da706e84ad898c2a5b8a4`.

El ejecutable host es ELF EXEC, con base de carga **0x400000**. Por eso
`addr2line -fe sacredfire 0x1cd1c` no resuelve: la dirección virtual correcta
es **0x41cd1c**, que resuelve a `localtime_to_utc`,
`src/runtime_kernel.c:344` en el binario original. La instrucción es
`mov %rbx,(%r14)`: escribe ocho bytes en el segundo argumento original (RSI),
guardado en R14. La dirección de fallo es `0xffffffff`.

La imagen guest empieza en `0x800000000` (`runtime_low_map`). Su libc está
en el offset `0x2ba0000`, según `out/libc-link.json`. Así,
`0x802baa221` es libc + `0xa221`, dentro de **mktime**: su export
`n7AepwR0s34` empieza en `0xa110` y mide 500 bytes. Es el retorno de la
llamada de `0x802baa21c` al PLT `0x802ba0218`.
El PLT salta por `0x802c8fd18`; la relocación de libc `0xefd18`
apunta al NID `0NTHN1NKONI`, `sceKernelConvertLocaltimeToUtc`.

La llamada prepara estos registros:

| Registro | Argumento real |
| --- | --- |
| RDI | segundos locales |
| RSI | valor de `tm_isdst` leído de `tm+0x20` mediante ESI |
| RDX | puntero de salida a segundos UTC, `rbp-0x58` |
| RCX | puntero a timezone, `rbp-0x40` |
| R8 | puntero de salida DST, `rbp-0x64` |

La firma de cinco argumentos también está declarada en
`gpu/shadps4/core/libraries/kernel/time.h` y en el
[header de upstream](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/time.h).
El runtime tenía cuatro argumentos: faltaba el entero en segunda posición,
desplazando todas las salidas. `tm_isdst=-1`, pasado mediante ESI, deja
RSI en `0x00000000ffffffff`. La primera escritura intenta usarlo como
puntero y reproduce exactamente la dirección del SIGSEGV. No es una
dirección GPU ni una escritura a un recurso Vulkan.

Los dos `gamesetup.lreq -> not found` son los últimos mensajes observados;
no hay evidencia de que sean la causa. Hay muchas búsquedas `.lreq` fallidas
anteriores en la misma sesión que no interrumpen su ejecución.

## Cambio y alcance

Se agrega el argumento entero que faltaba en `localtime_to_utc`, restaurando
los registros correctos para las tres salidas. No se agrega un guard que
oculte el error ni se cambia código online/GPU. Se conserva la política
existente de zona horaria del host; el hint DST sigue sin interpretarse.
La exactitud de las conversiones alrededor de transiciones DST y la ABI de
la conversión inversa son asuntos separados de este arreglo de firma.

`tests/test_runtime.c` llama al import por su NID con cinco argumentos,
incluye el valor exacto `UINT32_MAX`, -1, 0 y 1, comprueba las tres salidas
y canarios, y cubre las salidas opcionales y el error por UTC nulo.

## Clasificación

**Defecto preexistente, no introducido por UFC-4/UFC-5.** La firma errónea
ya está en `4b644a8` y fue introducida en `b1f95e3` (9 de octubre de 2026).
`git diff 4b644a8..6145472 -- src/runtime_kernel.c` está vacío; el rango
sólo cambia GPU, sus dependencias y documentación. Las sesiones cortas sin
crash no ejercitan necesariamente esta ruta de `mktime`. Los cambios GPU
podrían afectar cuándo se llega a ella, pero no explican la discrepancia ABI.

## Validación automatizada

`ufc3-build --test` compila `out/sacredfire` y pasa `pad-test` y
`runtime-test`, incluida la regresión nueva. La suite completa se detiene
después en `test_file_mods.c:54`: su limpieza intenta borrar `temp0` sin
borrar los subdirectorios de retratos que `runtime_file_configure` crea
incondicionalmente para UFC 3. Ninguno de esos dos archivos fue modificado.
No se presenta esta suite como completamente verde.

`python -m unittest discover -s tests`: 92 pruebas, 29 omitidas,
seis fallos y un error en `test_game_check`/`test_patches` (expectativas de
CTR). Una copia temporal de `git archive HEAD` produce exactamente los
mismos siete casos fallidos. Logs en `out/ufc8-python-tests.log` y
`out/ufc8-python-baseline.log`. `git diff --check` pasa.

## Validación manual pendiente

Una sesión con el binario corregido, mismo driver/configuración/guardado:
Desde el checkout habitual con sus datos y guardados:
`BB_PREBUILT=1 BB_PROBE=/home/myzk/Projects/.ufc3-worktrees/UFC-8/out/sacredfire BB_SAVE_LOG=1 bash run.sh`.
Esto usa el binario corregido y evita recompilar. Recorrer la misma
entrada de game setup/career y permanecer allí al menos dos minutos.
Comprobar que se supera la transición y que el log no termina en SIGSEGV.
Esto confirma la corrección en el flujo real; no se ejecutó esa sesión aquí.
