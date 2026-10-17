# UFC-49 — sangre y sudor ausentes desde 0.1.3

## Segunda pasada: resultado y cambio de default

El usuario confirmó en Linux que `BB_PREUPLOAD=1 BB_WRITE_STEP=0 BB_WRITE_WINDOW=1`
recupera sangre y sudor en los golpes y mantiene correcta la piel del luchador creado
en career. Log: `logs/UFC-49-preupload1/20261016_172800.log` (2026-10-16).
La inspección visual es el resultado comunicado por el usuario, no una deducción del log.

Conclusión operativa: apagar PREUPLOAD en `dcde0dd` (launcher Windows) y `4b644a8`
(`run.sh`) quitó los efectos. Se restaura `BB_PREUPLOAD=1` por defecto para UFC 3
en ambos arranques. Se conserva la prioridad de valores explícitos del entorno:
`${BB_PREUPLOAD:-1}` en Linux y `game_env`/`managed_env_capture` en Windows.
No se modifican WRITE_STEP ni WRITE_WINDOW, ni `page_manager`/`buffer_cache` (UFC-48).

La piel rota que motivó apagar PREUPLOAD no se reproduce en esta prueba con STEP=0.
Revisión breve del historial: `5875804` (merge `878a767`) cambió las texturas
comprimidas por GPU para pasar por memoria y corrigió aliases/mips/layers, por lo que
es un candidato relevante; no se identificó un commit que explique de forma
concluyente la piel correcta con PREUPLOAD=1. Tampoco se aisló STEP como causa.
No se realizó RE adicional ni se cambió el renderizador.

Validación de esta pasada (todos exit 0):

- `ufc3-build`: Linux, `out/sacredfire` y `out/gpu/libbbgpu.so`, LTO ON/PGO off.
  Log: `out/ufc49-linux-build.log`; advertencias CMake de políticas no bloqueantes.
- `scripts/build-windows.sh --runtime`: Windows x86-64, LTO ON/PGO off, sin AvPlayer.
  Se tomó el mismo lock de compilación para esperar turno. Logs:
  `out/ufc49-windows-build.log` y `out/windows/build.log`.
- `BB_ALLOW_DIRTY=1 BB_LTO=ON scripts/package-windows.sh`: ZIP con sus inspecciones
  de PEs AMD64, DLLs, ausencia de ELF, rutas del host y datos de juego. Se explicitó
  LTO al empaquetar para que el manifiesto refleje el ON usado por el build.
  Log: `out/ufc49-windows-package.log`.
- `bash -n run.sh`, `git diff --check` y evaluación aislada de la línea de defaults:
  sin override toma PREUPLOAD=1 y conserva overrides PREUPLOAD=0/2.
  En Windows se conserva `game_env` y su captura del entorno original.

ZIP de prueba: `dist/UFC3Native-windows-ufc49-preupload-5a6ef96.zip`, con manifiesto
`.build.txt` y checksum `.sha256`. Base: `5a6ef96bb6d48895eb0caa6fb4bc93883bf127d5`,
versión `ufc3-v0.1.4-17-g5a6ef96-dirty`: incluye los cambios locales de esta pasada;
el sufijo identifica el HEAD anterior, no un commit final aún inexistente.
El sandbox sólo permite escribir en el worktree y temporales; queda para el
despachador copiarlo a `~/UFC3Native-windows-ufc49-preupload-<commit>.zip` y ajustar
el nombre al commit final si corresponde.

El agente no ejecutó el juego. Cambios sin commit para que los commitee el despachador.
Se revirtió sólo el parche FSR aplicado automáticamente por el build al submódulo
inicialmente limpio, para no entregarlo como modificación del ticket.

## Única prueba manual pendiente: Windows

Extraer el ZIP de esta pasada y abrir su `UFC3Native/UFC3.exe` normalmente, sin
`.bat` ni overrides de PREUPLOAD. Si existía un override persistente, quitarlo para
probar el default. Conservar los demás ajustes gráficos habituales.

En **una sola sesión**: Quick Fight, dar golpes y comprobar sangre/sudor; después
abrir career con el luchador creado y comprobar que la piel siga correcta (sin negro,
parches ni corrupción). Reportar ambos resultados y adjuntar el log de esa sesión.
No encadenar otra combinación de variables. La validación visual Windows queda pendiente.

## Investigación estática de la primera pasada (contexto histórico)

Sobre `ufc3-online`, HEAD inicial `8de5098`, sin ejecutar el juego. Las hipótesis
siguientes preceden al resultado positivo Linux; no son requisitos para este cambio
de default ni fallos de render confirmados.

## Cambios relevantes entre 0.1.2 y 0.1.3

Rango revisado completo: `git log --oneline ufc3-v0.1.2..ufc3-v0.1.3`.
Los merges y cambios de notas no introducen otro mecanismo independiente.

| Commits | Efecto y relación posible |
| --- | --- |
| `dcde0dd`, `4b644a8` | Apagan `BB_PREUPLOAD` y `BB_WRITE_STEP` en launcher Windows/run.sh para evitar piel negra/rota en creados/career. Cambia tanto la subida anticipada como la selección de fuente de texturas residentes. Hipótesis luego confirmada operativamente por la prueba Linux con PREUPLOAD=1 (ver segunda pasada). |
| `f53d611`, `e4e9bb2`, `612684a` | Write windows: apagado en Windows, reactivado luego en Linux y Windows para 0.1.3. STEP=1/WINDOW=1 con PREUPLOAD=0 ya fue probado por el usuario sin recuperar sangre/sudor; no repetir esa combinación como diagnóstico. |
| `5875804` (merge `878a767`) | T6a: las vistas BC sobre imágenes sin comprimir pasan por memoria GPU; escribe aliases storage antes de crear una BC; expande vistas compatibles con más mips/layers. Cambia el recorrido de texturas de piel. Falta explícitamente StorageImageSync tras dispatch y flush antes de escrituras CPU. |
| `45fd5b0` | Si el driver rechaza imágenes BC con storage/block-texel views, reintenta sin esos usos. Puede cambiar cómo se representan los aliases según GPU. |
| `706d686`, `6b7e17c`, `6fffa3e`, `6e86d0a`, `53dec01`, `d751769`, `06d0b6b` | Introducen y refinan DB_ALPHA_TO_MASK. El resultado final vuelve a aplicar dither en todos los draws habilitados por registros, también peleas; la regla de color map queda opt-in. Candidato alternativo: podría descartar fragmentos de un efecto si su alpha o las condiciones emuladas son incorrectos. No se conoce el shader de sangre/sudor para comprobarlo. |
| `631f49f`, `58b2425` | Ejecutan fast-clear en FMASK decompress y limpian imágenes MSAA nunca renderizadas. Afectan capas como tatuajes/resolve; no hay evidencia de que sean las capas de daño. |
| `0b289ce` | Lecturas de CMask como no comprimido (PS4 Pro). Es el único cambio de buffer_cache en el rango: PREUPLOAD no fue reimplementado entre esos tags. |
| `54e5471`, `338222e`, `25998f2`, `aa4bd2b` | Vistas de profundidad R16 por capa/2D, representación D16, escala de bias y Z32 como R32_UINT, liberación de copias. Relevantes para sombras, menos directos para piel. |
| `82b8396` | Fences de subgroup alrededor de stores LDS; posible efecto sobre productores compute. |
| `bc5b87c`, `bc438fe`, `0c15437`, `f34b78e`, `3775520`, `b2f01e7`, `20ce398`, `74ee10e` | NaN/min/max/clamp/recíprocos, EXEC, instancias, VGPR y versión de shader cache. Pueden cambiar cálculo de shaders; sin shader identificado no se pueden excluir individualmente. |
| `250495f` | Emulación de factores MIN/MAX zero/one del blend: candidato si el efecto depende de esos factores. |
| `83b1f3a` | Launcher deja BB_NEO según checkbox: comprobar que el reporte compara el mismo modo PS4/Pro. |

El resto del rango corresponde a saves, icono/portrait, hora local, lenguaje, límite
de FPS, estadísticas y documentación. No se encontró allí un cambio directo del
camino de actualización de texturas de daño.

## Qué hace PREUPLOAD realmente

- `buffer_cache.cpp::PreuploadMode`: 0 apaga; 1 reutiliza memoria ya residente;
  2 admite nuevas residencias (~3 GB extra según el comentario). GuestInPlace fuerza 0.
- `vk_rasterizer.cpp::MapMemory/UnmapMemory` notifican los mappings.
  `NotePreuploadMapping` selecciona direct memory tipo 3 (garlic) con acceso GPU
  mediante `src/runtime_memory.c::runtime_memory_vma_info`. El runtime consulta el
  mapping; no implementa una política específica de sangre/sudor.
- `DrawRecord` llama `Preupload` una vez por frame, presupuesto por defecto 16 MiB.
  Revisa hasta 256 regiones de 4 MiB; espera 10 frames sin writes, aplica backoff
  y busca páginas CPU-modificadas sin modificación GPU. En modo 1 sincroniza sólo
  las partes residentes. No debería sobrescribir datos GPU vigentes si el tracking
  es correcto.
- **También cambia las subidas bajo demanda**: `ObtainBufferForImage` primero toma
  la arena si la región está marcada GPU-modificada, independientemente de PREUPLOAD.
  Con PREUPLOAD activo también permite arena residente + `SynchronizeMemory` para
  datos CPU. Con 0 cae a staging desde guest RAM (`RunGuestCopy`/`CopySparseMemory`).
  Luego `RefreshImage` detilea y sube al VkImage.

Por tanto, **0 no desactiva la subida de una textura dinámica CPU**. Su escritura
debe invalidarla y `UpdateImage`/`RefreshImage` debe cargar los bytes nuevos por
staging. Tampoco desactiva el camino de datos marcados GPU-modificados.
Una diferencia visual con PREUPLOAD=1 apuntaría a una divergencia de coherencia
RAM/arena, invalidación o temporización del snapshot; no demostraría por sí sola
que faltaba la tarea de fondo. PREUPLOAD tampoco garantiza arreglar invalidaciones
perdidas: subir arena no marca automáticamente un VkImage limpio como Dirty.

## Mecanismos concretos que requieren identificar el recurso

1. **Alias BC ya existente tras una escritura storage nueva.** T6a hace writeback
   en `FindImage` dentro de `if (!image_id)`, al crear una BC. Un match exacto o
   el cache de búsqueda puede retornar antes. `FindTexture(Storage)` limpia
   `WrittenBack`; `WriteBackImage` es quien propaga `GpuDirty` a aliases BC.
   `DispatchRecord`/`DispatchIndirectRecord` no hacen StorageImageSync después
   del dispatch. Si sobreviven storage y BC separados, un nuevo write storage
   puede no alcanzar el alias BC previamente creado. Es una limitación concreta
   del código; **no se ha demostrado que sangre/sudor use ese recorrido**, ni que
   sobrevivan ambos aliases en ese caso.
2. **Writes de buffer a subrecursos.** `BindBuffers` invalida imágenes cuando el
   buffer es escrito, pero `InvalidateMemoryFromGPU` sólo considera imágenes con
   base exactamente igual a la dirección del buffer (TODO subresources). Un write
   interior puede dejar el VkImage limpio. Ese mecanismo ya estaba antes del rango;
   no basta para atribuirle la regresión sin demostrar que T6a cambió los aliases.
3. **RAM obsoleta frente a arena vigente.** Si el tracking pierde la autoridad GPU,
   PREUPLOAD=0 puede leer RAM vieja; PREUPLOAD=1 podría conservar bytes en la arena.
   Es una hipótesis de coherencia, no un fallo confirmado. Reactivarlo globalmente
   requería comprobar la piel creada/career; la segunda pasada Linux la encontró correcta.

Arreglo de fondo propuesto, condicionado a la captura del recurso: sincronizar el
productor storage hacia su alias BC **también al reutilizarlo**, después del write y
antes del muestreo, respetando orden/barriers y mips/layers. Para writes interiores,
invalidar el rango/subrecurso realmente escrito. Para divergencia RAM/arena,
corregir la autoridad y la invalidación en vez de depender de un upload de fondo.
No se implementó un filtro por formato/dimensión/dirección: hoy no hay un criterio
que distinga el daño de la piel creada y sería especulativo.

**Corrección del contexto:** `GROK_HANDOFF.md` §19 identifica el recurso de §11
(`fs_img3`, R32G32_UINT 512×512×6, ps `86b28487`) como mapa min/max de sombras,
producido por cs `34617c9f`/`aae9f6c1`. No sirve como identificador de sangre/sudor.
