# UFC-13: preservación del alias del retrato (cuarta pasada)

## Evidencia disponible

En `logs/20261015_203329.log:4269`, el RT 1024x2048 de `0x1033548000`
tiene `rgba_nonzero=434693,434781,434912,435731`. En la línea 4271, la textura
864x1360 que lo reemplaza tiene los cuatro canales en cero. La misma pérdida
ocurre en las líneas 4366/4370 para `0x1032f40000` y 652x1080.

**Límite de esa evidencia:** ese archivo no contiene ninguna línea
`alias-region-host`. Los conteos del RT abarcan 1024x2048, no el recorte pedido;
no demuestran por sí solos contenido dentro de 864x1360 o 652x1080. La
instrumentación actual sí solicita ese recorte y el fix agrega
`alias-preserved-host`, que cuenta los píxeles visibles del destino después de
copiarlo. Esa comprobación queda pendiente de la única sesión manual nueva.

## Implementación y layout

Opción A: `vkCmdCopyImage` desde (0,0) del mip 0 del RT al mip 0 del destino,
con extensión exactamente igual al retrato. No pasa por CPU ni modifica memoria
guest o buffer cache.

La correspondencia está respaldada por el tiler local:

- `amdgpu/tiling.h`: tile 14 es `Thin2DThin`.
- `texture_cache/tile.h`: para tile 14, RGBA8 y una muestra, los macrotiles
  normales son 128x64; `ImageSizeMacroTiled` alinea pitch/altura por separado.
- `host_shaders/tiling.comp`, `ComputeSurfaceAddrFromCoordMacroTiled`: índice
  del macrotile = `(y / macro_height) * (pitch / macro_pitch) + x / macro_pitch`.
  Los macrotiles están ordenados por filas. La altura sólo afecta
  `slice_bytes`; su multiplicador es cero para slice 0 y sin tile split.
- Un microtile de 8x8 RGBA8, una muestra y espesor 1 ocupa 256 bytes.
  `amdgpu/tiling.cpp:CalculateTileSplit` da al menos 256 para color, por lo que
  no hay división del microtile. Pipe/bank dependen de coordenadas y parámetros
  de tiling/swizzle, iguales en ambos descriptores.

Así, igual dirección base, pitch efectivo, formato, tile mode, alt tile y bank
swizzle implican igual dirección guest para cada píxel del rectángulo de mip 0,
aunque cambie la altura de la superficie o el padding pow2. No es un blit ni un
reescalado: Vulkan copia las coordenadas de las dos imágenes ya detiled.

El filtro se aplica antes del rechazo por BlockDim, sólo al binding sampled,
sin otro resultado de merge, RT fuente GPU-modified sin Dirty,
1024x2048, pitch 1024, RGBA8, tile 14, 2D, una capa/muestra/mip; el destino debe
tener mips y ser uno de 864x1360, 652x1080, 600x1080 o 1000x1200. Además exige
igual pitch efectivo de mip 0, offset cero y mip 0 macro-tiled. Los aliases que
no cumplen siguen la ruta anterior. SafeToDownload descarta fuentes invalidadas
por escrituras CPU/buffer; el tamaño y uso acotan la excepción a esta transición.

Se inicializa el destino con `RefreshImage` **antes** de la copia. La única
región copiada es mip 0, capa 0. Se conserva GpuModified y el tick de escritura;
la textura queda limpia para evitar un reupload inmediato. La fuente se retira
con el mismo mecanismo de rebind/liberación diferida usado por ExpandImage.
Los mips 1..N no se escriben con la preservación: los genera el juego. La traza
existente muestra los bindings sampled/storage de mips sucesivos hasta 10
(por ejemplo líneas 4720–4741); al crecer la textura, ExpandImage copia los mips
ya existentes antes de que el juego escriba los siguientes.

`BB_T4_PRESERVE_ALIAS=0` desactiva el fix. `BB_T4_TRACE=1` registra
`[T4] alias-preserved` una vez por dirección y dimensiones.
`BB_T4_TRACE_PIXELS=1`, junto con TRACE, habilita las lecturas acotadas existentes
y la nueva `alias-preserved-host`; sin estos flags no agrega lecturas ni esperas.

## Una prueba manual, pendiente

Iniciar con `BB_T4_TRACE=1 BB_T4_TRACE_PIXELS=1 BB_SAVE_LOG=1` antepuestos al
comando habitual, sin `BB_T4_PRESERVE_ALIAS=0`. En una sola sesión:

1. Crear un peleador nuevo y guardar. Revisar sólo el `cap_*` recién generado
   con `python tools/ufc13_spg_report.py <ruta-real>/temp0/createplayer`.
   Comprobar alfa no vacío y color del retrato; las estadísticas por sí solas
   no distinguen un retrato de corrupción, por lo que también debe verse bien.
2. Entrar a career y abrir pause menu: comprobar visualmente el retrato.
3. Completar una pelea de career: comprobar que aparezca el jugador en post-fight.

Guardar el log y los captures nuevos; comprobar `alias-preserved` y los canales
del recorte/origen y destino. Los captures viejos en blanco no se reparan.
Si hay preservación con destino no vacío pero sigue faltando el jugador en
post-fight, esa parte requiere seguir los draws/lecturas del menú: todavía no
se ha demostrado en ejecución que ambas ausencias compartan toda la causa.

`ufc3-build` terminó con código 0 y generó `out/sacredfire`, versión
`ufc3-v0.1.2-58-g0ee0a1a-dirty`. `git diff --check` sin errores.
No se ejecutó el juego ni se hizo commit; el despachador realiza el commit.
El submódulo fsr-vulkan ya estaba modificado al comenzar; no se hicieron cambios
propios allí. No se tocó código online, vk_pipeline_cache ni shader_recompiler.
