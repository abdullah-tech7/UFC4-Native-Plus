# UFC-13: quinta pasada, 15 de octubre de 2026

Cambio propuesto para las transiciones de retratos; falta validación visual en
el juego. No se da por resuelto el jugador ausente en post-fight.

## Archivos y comparación offline

Log analizado: `logs/20261015_204532.log`, 3778050 bytes, SHA-256
`6df05ca0d96f8810276b21cd4f8fff924ebe0cf5ba21cbb8c146e0ad2b52bf79`.
Se decodificaron los 160 SPG presentes con `tools/ufc13_spg_report.py`, sin
modificar los originales. Los 40 registros de `cap_10002` y `cap_20000` están
en `UFC-13-SPG-20261015-204532.csv`. El formato sigue siendo cabecera de 12 bytes,
PNG alfa y JPEG RGB completos, de dimensiones coincidentes.

| Archivos, en ambos captures nuevos | Resultado de inspección de RGB y alfa |
| --- | --- |
| left_large / right_large | Peleador reconocible, pelo violeta, guantes y pantalón azul; el fix anterior sí conserva estos retratos |
| medium, wfa, belt medium, tott, win | Negro y alfa completamente cero |
| left_small / right_small | Estática de colores; algunos recortes/bandas transparentes |
| front / front_thumbs / fightcard | Fragmentos ajenos, bandas o estática; sin retrato utilizable |

Respecto de `UFC-13-SPG-20261015.csv`, RGB y alfa de medium y win tienen
exactamente los mismos SHA-256 que los captures anteriores. Los large nuevos
cambian ambos planos y ahora sí muestran al peleador. Los archivos con ruido
cambian de contenido; tener muchos píxeles no cero no demuestra un retrato.

## Dónde se pierde

1. **Large funciona al principio, pero no todas sus transiciones se sincronizan.**
   L4553–4566, tick 42515: RT 1024x2048 en `0x1033298000`, copia a 864x1360
   de 11 mips, lectura efectiva de esa textura, un paso intermedio en
   `0x107a2b0000`, y otro draw al RT 1024x2048 original. Después reaparecen
   rechazos al muestrear y escribir los mips. La traza `alias-rejected-blockdim`
   se imprimía **antes** de intentar preservar: una línea seguida de
   `alias-preserved` no es por sí sola un fallo. Además `alias-preserved`
   estaba deduplicada por dirección/tamaño, sin tick ni identidad de imagen;
   sus 16 líneas no cuentan todas las copias. `FindImage` podía devolver una
   coincidencia exacta anterior sin revisar el nuevo RT registrado, tanto en
   el memo rápido (`still_exact`) como en la búsqueda completa.
2. **Medium/win/tott pierden mip 0 antes de generar los demás.**
   L4659–4675, tick 42576, `0x1033860000`: draw al RT 1024x2048, petición sampled
   652x1080 de **un mip**, petición storage de dos mips, expansión y sampled
   con flags `0x40` (Registered, sin GpuModified). El filtro anterior exigía
   más de un mip, por lo que la primera lectura ya había subido memoria vieja.
   La copia posterior aparece recién al solicitar sampled de dos mips, cuando
   se está leyendo mip 1. No reconstruye el mip 1 que se generó desde el cero
   perdido. El export 652x1080 posterior (L4729, tick 42579) ya es negro y
   transparente: el fallo antecede a la codificación del archivo.
3. **Los tamaños chicos usan la misma transición, con otros RT acolchados.**
   Por ejemplo L4735–4758, tick 42636: 256x512 -> 252x400, misma dirección
   `0x103e920000`, pitch 256, RGBA8 tile 14. L5326–5345, tick 43433:
   512x512 -> 360x260, `0x103e420000`, pitch 512. No entraban en el filtro
   1024x2048 ni en la lista de tamaños. Se muestreaba una imagen independiente
   con bytes guest viejos, que luego alimentaba los mips y el export.

Los downloads 448x672 de L2308–2411 (ticks 34365–35314) son anteriores a estas
secuencias. Sus conteos no prueban que los retratos finales lleguen al export.
Tampoco un download sólo alfa implica corrupción: puede ser un plano auxiliar.

## Ruido de Saved Careers

La corrupción está presente al decodificar los planos fuera del juego, no sólo
al componer la bandera en Saved Careers. En `cap_10002/front.spg` se ven trozos
de piel, bloques claros/oscuros y ruido; en `cap_20000/front.spg`, bandas y
fragmentos ajenos. No es un simple intercambio de canales. Los pequeños muestran
estática con zonas/bandas que cambian entre captures, sin una cara reconocible.

El export de front declara explícitamente origen sampled **tile 14**, seguido
de storage **tile 8, tiled=false**, 360x260, pitch 368; luego download-host y
escritura guest (L5343–5346). El conteo alfa visible del download, **29851**,
coincide con el PNG de cap_10002/front; el segundo tiene **46567** tanto en
L11885 como en su PNG. Para left_small, el download L4758 y el PNG tienen
**93887** píxeles alfa no cero. No hay evidencia de un PNG/JPEG mal formado.

El patrón visual por sí solo no permite invertir un tiling a través del JPEG
con pérdida. La evidencia de bindings localiza una lectura de memoria vieja
tras perder el alias, antes del export lineal. No se justifica cambiar canales,
stride ni el tiler del export a partir de estos datos.

## Cambio

Sólo se modifica `gpu/shadps4/video_core/texture_cache/texture_cache.cpp`:

- La preservación se hace en `FindImage`, antes de elegir una coincidencia
  exacta. El memo de retratos depende también de la generación del registro,
  para detectar un RT que se acaba de registrar aunque el alias siga vivo.
- Se admite la primera lectura de **un mip**, antes de que el juego genere
  mip 1. Se conservan las comprobaciones de formato RGBA8, tile 14, array mode,
  alt tile, bank swizzle, pitch efectivo de mip 0, offset cero, macro tiling,
  una capa/muestra, fuente GPU-modified sin Dirty y uso RT.
- Se agregan únicamente estas parejas observadas: 256x512 -> 252x400;
  512x512 -> 328x280 o 360x260; 256x256 -> 188x160; 128x128 -> 120x100.
  Las cuatro dimensiones grandes conservan la fuente 1024x2048.
- Si existe la cadena compatible, se actualiza su mip 0; se reutiliza también
  una cadena con más mips que los solicitados. Así las vistas sampled y storage
  permanecen en una misma imagen. Si no existe, se inicializa antes de copiar.
  La fuente se retira una vez consumida; los binds posteriores conservan los
  mips generados. No se vuelven a copiar en cada lectura ni se pisan los mips
  1..N. Dos fuentes candidatas se consideran ambiguas y usan resolución normal.
- La copia sigue enteramente en GPU con `CopyImageMip0Region`; no agrega
  readbacks normales. `BB_T4_PRESERVE_ALIAS=0` desactiva la preservación.
  `alias-preserved` ahora incluye tick, IDs fuente/destino, mips pedidos/reales
  y reutilización, hasta 128 eventos. `view` incluye ID y binding para verificar
  qué imagen se usa realmente. Las probes de píxeles siguen siendo opt-in.

El argumento de layout de `UFC-13-FIX-ALIAS.md` también aplica a las parejas
chicas: macrotiles determinados por tile/bpp/muestras/alt tile, mismo pitch
alineado, slice 0, un microtile RGBA8 de 256 bytes sin tile split. Se comprueba
que mip 0 sea macro-tiled en ambos extremos; el tamaño del slice no cambia las
direcciones dentro de su rectángulo origen.

## Post-fight: lo que el log permite afirmar

Hay **2796 draws T4**, no se agotó el límite de 4096. El último es L12558,
tick 93332, un quad a 1024x1024. En la parte posterior del log se lee
`cap_20000/win.spg` (L14289–14291, y de nuevo L14794 y L15339), junto con
las imágenes NISPOSTFIGHTCELEB / NISPOSTFIGHTDECISION. El menú carga el SPG
vacío, pero cargarlo no demuestra qué draw lo presenta.

La traza de tercera pasada sólo admite targets de ancho 600–1024 y alto
1000–2048. Excluye los targets de escena 1600x900/1920x1080 y los targets
pequeños. Por eso **no hay draws atribuibles al jugador en ese menú**, y
no se puede concluir si se presenta un modelo vivo, un retrato o ambos.
La apariencia 3D del rival tampoco distingue esos casos. No se cambia la
ruta de escena basándose en esta ausencia de trazas. La atribución de esos
draws queda pendiente si regenerar los retratos no resuelve el síntoma.

## UNA prueba manual corta

Con el comando habitual, anteponer
`BB_T4_TRACE=1 BB_T4_TRACE_PIXELS=1 BB_SAVE_LOG=1`, crear **un solo peleador
nuevo**, guardar, revisar su tarjeta en Saved Careers y cerrar normalmente.
No hace falta jugar una pelea. Entregar el log y el cap nuevo. Decodificar con:

```sh
python tools/ufc13_spg_report.py user/temp0/createplayer > /tmp/ufc13-spg.csv
```

Comprobar visualmente front/fightcard y también medium/win: ya no deben ser
estática ni negro transparente. En el log debe preservarse 652x1080/600x1080
con `requested_mips=1` antes del primer storage de mip 1, y también los tamaños
chicos. `view image_id` debe coincidir con `dst_id`. Esta prueba valida la
producción y la tarjeta; no certifica el render del menú post-fight. Los SPG
viejos no se reparan retroactivamente.

## Validación realizada

`ufc3-build` final terminó con código 0 y produjo `out/sacredfire`, versión
`ufc3-v0.1.2-59-ga4c55b1-dirty`. Log de compilación local:
`/tmp/ufc13-build-verified.log`. `git diff --check` pasó. No se ejecutó el juego
ni pruebas largas. No se tocó online, `vk_pipeline_cache.*` ni
`shader_recompiler/**`; tampoco orig/user, índices git, rama o commits.
El submódulo `gpu/third_party/fsr-vulkan` ya figuraba modificado al empezar y
no se modificó en esta pasada. El despachador debe excluir ese cambio previo.
