# UFC-2: corrupción del heap con write windows

Estado: allocator corrupto confirmado; productor del write **no identificado**. Corrupción y
rendimiento **no resueltos**. Se entrega diagnóstico optativo (`BB_WRITE_WINDOW_DIAG`), no un
fix de coherencia ni una habilitación por defecto. No se ejecutó el juego.

Hallazgo válido principal: con `BB_WRITE_STEP=0` el gate de `DecideWriteStep` impide abrir
windows; con los defaults actuales todo write fault sobre una página GPU paga un readback, y
recuperar ese rendimiento queda pendiente de resolver la coherencia de las windows.

Orden de lectura: la tercera pasada corrige a las dos anteriores donde se contradicen. La
sección «Una prueba manual» de la primera pasada y la «Prueba A/B» de la segunda quedan
**reemplazadas** por la única prueba de la tercera.

## Tercera pasada (16 oct): grados de certeza, flags seguros, validación Windows

### Grados de certeza

| Afirmación | Grado | Base |
| --- | --- | --- |
| La free list del allocator de clases tiene una cabeza inválida (`-1`, `0x64`) en `+0x1262a2a` | Hecho | Disassembly + registros de 3 logs y transcripción Reddit |
| Con `BB_WRITE_STEP=0` no se abre ninguna window aunque `BB_WRITE_WINDOW=1` | Hecho de código | `DecideWriteStep` retorna antes de `RequestWindow` |
| Con window abierta, writes CPU sobre bytes GPU-owned se pierden al cerrar y un readback posterior los pisa | Hecho de código, no reproducido | Lectura de `CloseWindows`/`ProcessLateWrites`/`DownloadMemory` |
| Los faults en `+0x1262a2a` aparecen en corridas con windows abriéndose | Correlación; en el log del propio fault sólo se vio actividad en Reddit (transcripción) | ver tabla de reportes |
| Los faults de HeuZ salen de corridas con windows activas | Inferido del default 0.1.2 (STEP on); actividad vista sólo en otro log de la misma sesión | ver tabla de reportes |
| Las windows producen el write que corrompe la free list | **Hipótesis** | ningún evento liga una window con el nodo dañado |
| El crash del amigo (0.1.3) tiene la misma causa | **Sin atribuir** | otro sitio, otro recorrido, windows probablemente inertes |

Qué haría falta para pasar de hipótesis a causa (y aun así con límites):
- un evento `readback-overwrite` (o `upload-skip`) cuyo rango cubra exactamente el slot dañado
  (`[r14+0x18]`, o el campo «next» del nodo que fue cabeza antes), con el payload igual al
  valor malo (`-1`/`0x64`) y ocurrido **antes** del fault y **después** de la última escritura
  legítima conocida del allocator en ese slot;
- que la window del evento sea de la misma asignación: la página puede haberse liberado y
  reutilizado/remapeado entre el snapshot y el fault (comparar edad/ID de window con la vida
  del bloque);
- descartar productores que el diagnóstico no ve: writes GPU directos in-place, readbacks de
  imágenes/labels, fences tardías (`Late label guarded`);
- tener presente que el diagnóstico cambia el timing: si no crashea con él, no prueba nada.

La entrada «windows confirmadas como productor si `head page window id=...`» de la segunda pasada
era incorrecta: que la página del head figure en el historial de windows es sólo correlación
(la página pudo tener una window por cualquier otro bloque que comparte esos 4 KiB).

### `BB_WRITE_LOG`: no recomendarlo, en ningún modo

Código actual (`gpu/shim/bbport_write_log.cpp`, `bbgpu_dump_guest_writes`): con
`Mode() != 0`, es decir `BB_WRITE_LOG=1` **y también `=2`**, el dump del fault hace
`memcpy` directo desde `rax - 0x30 .. rax + 0x60` sin validar la dirección. En `+0x1262a2a`
`rax` no es un puntero del allocator (en Reddit `rax=1`): el handler puede fallar dentro del
handler. Además el dump busca el patrón de otro bug (`0x0000005300000000`, valor `0x53`).
Por eso **se retira** la sugerencia de la segunda pasada de un ticket con `BB_WRITE_LOG=2`
para fences: queda como idea sólo después de que el dump valide la dirección (no se hace en
esta pasada). `BB_WRITE_WINDOW_DIAG` en cambio lee `[r14+0x18]` con
`runtime_memory_read_backing`, que consulta la tabla de VMAs y devuelve ceros si no está
mapeado; no sigue punteros inválidos (sí toma el read lock de esa tabla dentro del handler).

### Validación Windows de este código (sin paquete)

Sólo **comprobación de sintaxis** con el toolchain Windows real; no es un build, no enlaza, no
hace LTO, no genera `.exe` ni paquete. Script: `/tmp/ufc2-winsyntax/run.sh`.

- Toma con `ninja -t commands sacredfire-game` los comandos de compilación del build Windows ya
  configurado en el checkout principal (`/home/myzk/Projects/ufc3-native/out/windows/build`,
  llvm-mingw `20261006-ucrt`, `x86_64-w64-mingw32-clang++ -std=gnu++23 -O2 -flto=thin`, mismos
  `-D`/`-I` de CMake) para `page_manager.cpp`, `buffer_cache/buffer_cache.cpp` y
  `shim/bbport_write_log.cpp`.
- Reescribe con `sed` las rutas `ufc3-native/gpu/` → worktree UFC-2, quita `-MD -MT -MF` y
  cambia `-o *.obj` por `-fsyntax-only` (no escribe nada en el build principal).
- Corre bajo el lock global de `ufc3-build` (`$XDG_RUNTIME_DIR/ufc3-build.lock`, `flock`).
- Resultado: las 3 unidades **ok**; único warning `argument unused during compilation: '-c'`.
- Límites: los headers generados (`out/windows/build/include`) y el sysroot son los del checkout
  principal (rama `ufc3-online`); no se verificó el enlace, ni el resto de las unidades, ni el
  comportamiento en Windows. No se modificaron defaults ni scripts.

### La única prueba sugerida (cuando exista paquete Windows con este diagnóstico)

Hoy no hay paquete Windows que lo contenga: no pedir nada al usuario hasta que exista.
Cuando exista, **una sola** corrida:

```
BB_WRITE_STEP=1 BB_WRITE_WINDOW=1 BB_WRITE_WINDOW_DIAG=1
```

en el entorno antes de abrir el launcher (el launcher respeta las variables gestionadas que ya
existen); Fight Now con peleadores de fábrica, pelea completa, post-pelea, volver y cargar la
segunda; hasta 20 min o el primer crash. Sin `BB_WRITE_LOG`, sin `BB_READBACKS=2`.

Es un **diagnóstico combinado** STEP+windows (las windows no pueden abrirse sin STEP): no atribuye
FPS ni separa la contribución de STEP de la de windows, y STEP arrastra la regresión conocida de
pieles negras en peleadores creados (por eso fábrica). Lo que se busca en el log: banner sin
`inactive`, líneas `Write windows: N/s`, y si crashea, `Write window audit fault` más los
eventos `readback-overwrite`/`upload-skip` para cruzar contra los criterios de arriba. Un crash
sin esos eventos no descarta windows; una corrida sin crash no valida nada.

## Segunda pasada (16 oct): qué builds abrían windows de verdad

### Hechos (código y tags)

- `BufferCache::DecideWriteStep` retorna `false` cuando `BB_WRITE_STEP=0` (o con twins, o
  `assume_locks`) **antes** de mirar `WriteWindowsEnabled()` y antes de `RequestWindow()`. Con
  step apagado no se abre ninguna window: `BB_WRITE_WINDOW=1` sola es inerte.
- El banner `CPU writes beside GPU data open their page...` lo imprime `WriteWindowsEnabled()`
  en el primer write fault aunque step esté apagado (el handler lo consulta antes de llamar a
  DecideWriteStep). El banner **no** prueba windows activas; la prueba es la línea de stats
  `Write windows: N/s opened instead of a readback` (contador `window_opens`, que sólo sube
  después del gate). Este commit hace que el banner diga `inactive: BB_WRITE_STEP=0 opens none`.
- El código de windows/step/readback (`page_manager.cpp`, `buffer_cache.cpp`, `liverpool.cpp`)
  es igual en `ufc3-v0.1.2`, `ufc3-v0.1.3`, `ufc3-v0.1.4` y `ufc3-online` (8de5098), salvo el
  diagnóstico de este ticket y el CMask de PS4 Pro. Lo que cambia entre tags es el entorno.

La tabla describe los **defaults de los scripts** (`git grep` por tag). El usuario puede
sobreescribirlos: el launcher Windows respeta `BB_WRITE_WINDOW`/`BB_WRITE_STEP` si ya están en el
entorno, y `run.sh` usa `${VAR:-default}`. Que una corrida concreta abriera windows sólo lo
prueba su log: la línea de stats `Write windows: N/s` (se imprime sólo si `window_opens` > 0).

| Build | Default launcher Windows | Default run.sh | Con esos defaults, ¿windows abren? |
| --- | --- | --- | --- |
| 0.1.2 | WINDOW=1, STEP sin fijar (código: on) | WINDOW=1 | Sí |
| 0.1.3 | WINDOW=1, STEP=0 | WINDOW=1, STEP=0 | No |
| 0.1.4 / ufc3-online | WINDOW=0, STEP=0 | WINDOW=1, STEP=0 | No |
| esta rama (desde 4b644a8) | WINDOW=0, STEP=0 | WINDOW=0, STEP=0 | No |

- Consecuencia **para corridas con defaults**: el hotfix 0.1.4 (`BB_WRITE_WINDOW=0` en Windows)
  no cambió el comportamiento respecto de 0.1.3. Con los defaults actuales ni Linux ni Windows
  abren windows ni hacen single-step: todo write fault sobre una página GPU paga un readback.
  Una corrida con `BB_WRITE_STEP=1` en el entorno sí las abre en cualquiera de esos builds.

### Hechos (reportes) cruzados con la tabla

| Reporte | Build | Actividad de windows | Fuente | Fault |
| --- | --- | --- | --- | --- |
| HeuZ (RTX 4090), 3 faults | 0.1.2 | Vista 545–671/s sólo en 142452 (crash `0xC0000409`, no el fault); en 134426/153603/154238 no quedó anotada | logs de `/tmp/ufc3z` (ya no están; no se re-verificó) | +0x1262a2a, head `-1` |
| Reddit (RTX 3060) | 0.1.2 | Vista: 470–806/s | transcripción del Jefe, no el log original | +0x1262a2a, head `0x64`, carga 2.ª pelea |
| Tester AMD (RX 9070) | parches sobre 0.1.2 | Desconocida | README de parches, sin log | free-list next `0x64` |
| Amigo (RX 7800 XT), 2 logs | 0.1.3 | **Inferida** inactiva | extracto: «STEP apagado»; no transcribe líneas `Write windows:` | +0x4c4b3 / +0x4c90d en SimThread |

Lectura con esos límites: los dos reportes 0.1.2 con log tienen actividad de windows vista
(Reddit en el log del fault; HeuZ en otro log de la misma sesión) y fault en +0x1262a2a. Esto es correlación, no causa (ver grados de certeza arriba). El caso del amigo
**no se atribuye**: es otro sitio (vecino del allocator), otro recorrido (Career + Create Fighter,
crash al firmar contrato) y la inactividad de windows sale del extracto y del default de 0.1.3,
no de un log completo revisado aquí. Ese log trae además cientos de `Late label guarded: EOS
fence ... dropped`, lo que mantiene viva la hipótesis alternativa de escrituras tardías de
fences/labels/readbacks sobre memoria ya liberada (mismo patrón que el bug de 2026-10-01 en
`docs/parallel_gpu.md`). El comentario del launcher 0.1.4 («con `BB_WRITE_WINDOW=0` el usuario
RX 7800 XT dejó de crashear») es anecdótico: con STEP=0 ese cambio no altera el camino de
windows, así que no confirma ni descarta nada.

### Defecto de coherencia de las windows (hecho por lectura de código, no reproducido en juego)

Sea R = bytes de la página en `gpu_modified_ranges` (VRAM es la copia buena, guest es vieja).

1. DecideWriteStep sólo verifica que **el primer** write esté a más de 16 bytes de R. Con la
   window abierta, cualquier thread escribe cualquier byte de la página sin fault, incluidos
   los de R (por ejemplo un bloque que el GPU escribió, se liberó y el allocator reutiliza como
   nodo: el «next» de la free list cae en R).
2. Al cerrar, las palabras que tocan R se descartan (`stepped_write_conflicts`): VRAM conserva
   el valor viejo y R sigue marcado GPU-owned.
3. El siguiente readback que cubra esa página (`DownloadMemory`, ventana de 512 KiB alrededor de
   cualquier fault) copia R de VRAM al guest con `TryWriteBacking`: **pisa el write del CPU**.
   Un readback durante la window abierta hace lo mismo de inmediato.
4. Un write del CPU que deja el mismo valor que tenía la copia vieja del guest no aparece en el
   diff del snapshot: es indetectable, y el readback posterior también lo pisa.

El camino normal (readback antes del write) y el single-step (cada instrucción chequea su
dirección contra R) no tienen el punto 1; el step conserva una versión estrecha (otros threads
durante la instrucción abierta). Este mecanismo explica que el valor leído como «next» sea un
dato GPU (`-1`, `0x64`) y que falle al reciclar memoria (fin de pelea, carga de la siguiente);
**sigue siendo hipótesis**: no se correlacionó el nodo dañado con R.

Carreras secundarias (hechos de código, sin evidencia de que ocurran ni de que corrompan):
- `OpenWindow` cambia protección sin el lock de página de los watchers; si un watcher (p. ej.
  read watcher con `BB_READBACKS=2`) reprotege una página con window abierta, `OpenWindow`
  devuelve «ya abierta» sin reabrirla: el write reintenta y vuelve a fallar hasta el próximo
  submit (spin, no corrupción).
- `CloseWindows` suelta `windows_mutex` antes de reproteger: otro submitter (Gfx/Asc) puede
  seguir sin que los diffs del primero estén encolados para su submit.

### Por qué no hay fix en esta pasada

No existe una corrección mínima demostrable sin juego:
- Subir las palabras en conflicto y quitarlas de R arregla sólo los writes con valor distinto
  (punto 4 queda), necesita uploads con granularidad de byte y el orden entre writes GPU
  registrados después de abrir la window y el write CPU (si no, destruye resultados GPU
  válidos): es un rediseño del ownership, no un parche.
- Una prueba sintética de BufferCache requiere Vulkan/arenas; un modelo aparte sólo repetiría
  la descripción de arriba. No se agregaron tests.
- Serializar Open/Close con los locks de página arregla el spin, no la pérdida de writes.

Recuperar rendimiento sin windows: el camino coherente es el single-step (`BB_WRITE_STEP=1`,
windows 0), apagado por las pieles negras de peleadores creados; no se midió en esta pasada.

### Prueba A/B (retirada en la tercera pasada)

Proponía dos corridas (A defaults, B `BB_WRITE_STEP=1 BB_WRITE_WINDOW=1 BB_WRITE_WINDOW_DIAG=1`)
y leía «head page window id» como confirmación del productor. Retirada: eso es correlación, B
mezcla STEP y windows (no sirve para atribuir FPS) y no hay paquete Windows con el diagnóstico.
Ver «La única prueba sugerida» arriba.

### Siguiente ticket implementable (condicional)

Sólo si una corrida diagnóstica cumple los criterios de causa de la tercera pasada: «Windows
coherentes: reconciliar R antes de dar acceso». Al abrir una window, descargar sólo los rangos
GPU de esa página (readback queue, esperando su write tick) y restarlos de
`gpu_modified_ranges`, de modo que la página quede CPU-owned y el diff de cierre sea completo;
medir FPS por separado. Si el fault aparece sin windows activas (actividad comprobada en log):
ticket aparte sobre fences/labels tardíos (`Late label guarded`); su herramienta no puede ser
`BB_WRITE_LOG` mientras el dump lea `rax` sin validar.

## Resolución del fault

El worktree no traía `out/guest-image.bin`. Se leyó, sin modificarlo, el del checkout
principal: `/home/myzk/Projects/ufc3-native/out/guest-image.bin` (48.465.196 bytes).
Los offsets coinciden con instrucciones y retornos del stack reportado. Comando reproducible:

```sh
objdump -D -b binary -m i386:x86-64 --start-address=0x12629c0 \
  --stop-address=0x1262ae0 out/guest-image.bin
objdump -D -b binary -m i386:x86-64 --start-address=0x7b410 \
  --stop-address=0x7b510 out/guest-image.bin
```

La función que empieza en `+0x12629c0` redondea el tamaño/alineación solicitado,
selecciona una clase de tamaño en unidades de 32 bytes y extrae un nodo de una lista:

```asm
+0x1262a17 mov r14, [r15+rsi*8+0x18] ; bucket/slab de la clase
+0x1262a1c mov r13, [r14+0x18]       ; cabeza de la free list
+0x1262a20 test r13, r13
+0x1262a23 je   +0x1262a54          ; camino de lista vacía
+0x1262a25 inc  word ptr [r14+0x20] ; contador de asignaciones
+0x1262a2a mov  rax, [r13]          ; FAULT: leer el siguiente nodo
+0x1262a2e mov  [r14+0x18], rax     ; pop de la lista
```

La dirección del fault `FFFFFFFFFFFFFFFF` identifica `r13=-1`. El allocator intenta
seguir una cabeza inválida: confirma corrupción de sus metadatos, aunque no identifica
quién la escribió. Puede provenir de un enlace corrupto de un nodo extraído anteriormente.
Los logs no incluyen r13/r14/r15; no permiten localizar el bucket ni el bloque anterior.
No hay símbolos que permitan dar un nombre C++ exacto.

`+0x7b495` es el retorno de `call [rax+0x38]` en `+0x7b492`. La función
`+0x7b410` recorre allocators y comprueba si devolvieron un puntero no nulo. Es el
dispatcher de asignación que llama al allocator de clases, coherente con el fault.

Los siguientes frames también se resolvieron por sus call sites, sin atribuirles
nombres de símbolos inexistentes:

| Retorno | Instrucción previa y función observable |
| --- | --- |
| +0x11bd0b | `call [r11+0x18]` en +0x11bd07; selecciona allocator según categoría y devuelve puntero/categoría |
| +0xf3f7e | `call [r11+0x10]` en +0xf3f7a; calcula tamaño como stride redondeado a 16 por cantidad y guarda el puntero en objeto+0xa0 |
| +0x116610 | `call +0xf3dc0` en +0x11660b; inicializa el objeto que solicita esa asignación |

El contexto apunta a una asignación de almacenamiento de un objeto/buffer del
engine. Los frames de jobs más profundos varían y no tienen símbolos; no permiten
identificar el productor del write que corrompió el allocator.

Los registros disponibles muestran distintas clases: `rsi=15`, `8` y `1` en los
tres faults. No es una sola petición de tamaño que siempre falle.

## Evidencia original y límites

Se leyó `~/ufcCOntext/EXTERNAL-REPORTS.md` y los seis logs indicados, directamente
desde `/tmp/ufc3z/logs/`. Hay discrepancias con el resumen del ticket:

| Log | Resultado real |
| --- | --- |
| 134426 | Fault en +0x1262a2a, General Job 1, exit 3 |
| 152704 | Sin guest fault; exit 0 |
| 153603 | Fault en +0x1262a2a, Replay Job 3, exit 3 |
| 154238 | Fault en +0x1262a2a, Replay Job 1, exit 3 |
| 141026 | Exit 0 |
| 142452 | Exit 3221226505, precedido por 545 y 671 windows/s |

Los tres faults comparten los primeros cuatro retornos (`7b495`, `11bd0b`, `f3f7e`,
`116610`) y parte del camino de jobs, pero difieren en los frames intermedios.
La última estadística de 134426 incluye 5014 conflictos de stepped writes; la de
152704 incluye 3133 y esa corrida termina limpia. Los otros dos faults tienen cero
conflictos en su última estadística. Ese contador mezcla single steps y windows:
no demuestra por sí solo corrupción ni permite descartar daño anterior.
`0xC0000409` sin subcódigo/dump tampoco prueba por sí solo stack cookie corrupta.

Se revisó `0017-*.patch`: cambia la invalidación de texturas cuando un write GPU
alcanza su interior. No controla la protección, snapshots ni el ownership de los
bytes de una write window. No se aplicó.

## Qué hace el código y causa más probable

1. `BufferCache::DecideWriteStep` comprueba sólo la dirección del fault inicial,
   con margen de 16 bytes, contra los rangos GPU-modified. Para windows no limita
   cuánto podrá escribir después esa instrucción ni otros threads.
2. `PageManager::Impl::OpenWindow` toma un snapshot y concede ReadWrite a toda la
   página de 4 KiB, incluso cuando había read watchers. Cualquier thread puede
   leer bytes GPU obsoletos y escribir bytes GPU-owned sin un nuevo fault.
3. En SubmitGfx/SubmitAsc, `CloseWindows` reprotege, compara en palabras de 4 bytes
   y encola los cambios en `NoteSteppedWrite`.
4. `BufferCache::ProcessLateWrites` descarta cada palabra que toca cualquier rango
   GPU-modified. Incluso bytes CPU adyacentes dentro de esa palabra pueden perder
   su upload. El snapshot no permite distinguir un write CPU de un write host/GPU.
5. El diff sólo hace `runtime.UpdateBuffer` hacia VRAM: no escribe guest. En cambio,
   `BufferCache::DownloadMemory` sí copia a guest mediante TryWriteBacking, usando
   los rangos GPU-modified exactos, sin reconciliación con las windows (salvo el
   experimento separado de twins). Un write CPU invisible dentro de esos rangos
   puede ser sustituido por datos viejos de VRAM en un readback posterior.

La hipótesis principal es pérdida de coherencia/ownership cuando la ventana permite
escrituras CPU posteriores en bytes todavía considerados GPU-owned, por ejemplo
al reutilizar una asignación como nodo libre. También se pierde la barrera de
lectura de esos bytes. **Compartir página con un buffer GPU no basta**: un buffer
readback no sobrescribe automáticamente toda la página ni sus metadatos vecinos.
Falta correlacionar la dirección dañada con el rango GPU y el write concreto.

Hay además una carrera real de cierre: `CloseWindows` retira la lista bajo
`windows_mutex`, suelta ese mutex y luego reprotege y toma el snapshot final. Un
fault concurrente puede abrir otra window para la misma página mientras se cierra
la anterior, habilitando escrituras durante la captura o recibiendo después la
reprotección de la window anterior. OpenWindow ni siquiera usa el lock de página
que emplean los watchers. Submitters concurrentes también pueden solapar cierres.
La reprotección por sí sola no garantiza el comentario «none is lost» del código.

No se aplicó un fix especulativo: subir palabras en conflicto puede destruir
resultados válidos del GPU; ignorarlas puede perder writes CPU; serializar sólo
OpenWindow/CloseWindows no resuelve lectores de datos obsoletos, watchers ni writes
por el backing mapping. Un fix general requiere ownership/rangos de descarte
confirmados, o reconciliar con GPU antes de permitir esos accesos. No se recomienda
reactivar windows por defecto con esta evidencia.

## Parche de diagnóstico

`BB_WRITE_WINDOW_DIAG=1`, apagado por defecto, conserva hasta 1024 snapshots de
windows activas/en cierre/recientes (aprox. 4 MiB de datos, más estructuras):

- `reopen-during-close`: apertura observada mientras otra window de esa página
  todavía está cerrándose.
- `upload-skip`: palabra cuyo upload se descarta por conflicto GPU, correlacionada
  con la window más reciente retenida de esa página.
- `readback-overwrite`: buffer readback cuyo payload difiere de un byte guest que
  cambió desde el snapshot. Se registra antes de escribir al backing.
- Cada evento incluye página, ID/edad en aperturas, fase, opener/caller, thread,
  bytes distintos, qword del snapshot, qword guest y payload con sus direcciones.
- En un fault se imprimen RIP y r12–r15. Para +0x1262a2a también se lee mediante
  backing la cabeza `[r14+0x18]` y se busca su snapshot reciente. La consulta del
  historial usa try_lock para no esperar un audit interrumpido.

Máximo 64 líneas por clase de evento. `opener` es un offset guest o una dirección
host con el bit alto de HostSite; `caller` es un offset guest. Baseline/guest son
qwords alineados a 8 bytes; payload empieza en `payload_at` y contiene hasta 8 bytes
de ese rango, cero extendidos. No deben compararse sin atender a las direcciones.
En el dump del fault las fases numéricas son 0=open, 1=closing, 2=closed.

Archivos: `page_manager.cpp/.h` implementan el historial; `buffer_cache.cpp` añade
los dos puntos de observación; `gpu/shim/bbport_write_log.cpp` conecta el reporte
de fault existente. No cambia decisiones de upload/readback/protección ni defaults.

Limitaciones: un cambio de snapshot puede ser CPU, GPU o host; una página histórica
puede haber sido reutilizada/remapeada. Los eventos son candidatos, no prueba de
ownership ni de que causaron el fault. Las fases son observaciones concurrentes,
no un reloj global del pipeline. No se interceptan escrituras directas GPU en
memoria in-place ni readbacks de imágenes/labels. El historial y los reportes son
acotados; su ausencia no descarta el bug y el coste del diagnóstico puede cambiar
el timing de la carrera.

## Una prueba manual (retirada en la tercera pasada)

Se conserva como registro; **no usar**: mezcla `BB_READBACKS=2` con el diagnóstico y fue
reemplazada por la única prueba de la tercera pasada.

Ejecutar una sola sesión con el build que contiene este parche:

```sh
BB_WRITE_WINDOW=1 BB_WRITE_STEP=1 BB_READBACKS=2 BB_WRITE_WINDOW_DIAG=1 ./run.sh
```

En Windows, establecer esas mismas cuatro variables en el entorno del proceso que
ejecuta el juego antes de abrirlo. Mantener el resto de opciones de la reproducción
original y comprobar en el log el banner `Write window audit: on` y actividad
`Write windows:`. Jugar una pelea completa, ver la repetición/postpelea y comenzar
la segunda; continuar hasta 20 minutos o el primer crash. Entregar el log completo,
incluyendo `Write window audit` y el reporte del fault. No activar BB_WRITE_LOG
(ni `=1` ni `=2`): su dump supone otro bug y lee directamente un bloque desde rax.

Objetivo: correlacionar el bucket/nodo con los eventos de overwrite/skip y determinar
si se observa reapertura durante cierre. Una sesión sin crash no valida un fix.
Queda pendiente esa evidencia para diseñar y validar la corrección de coherencia.

## Compilación

`ufc3-build` terminó con exit 0: biblioteca GPU (PGO off, LTO ON) y
`out/sacredfire` generados. Hubo warnings de políticas CMake y de ODR en los tipos
FidelityFX; no hubo errores de compilación. `git diff --check` pasó.

El primer intento esperó el lock global y falló antes de compilar porque build.sh
está versionado con modo 0644 y el wrapper lo ejecuta directamente. Se habilitó
temporalmente su permiso de ejecución para repetir `ufc3-build` y se restauró 0644
al terminar. También se revirtió únicamente el parche FSR que build.sh aplica
automáticamente al submódulo, para dejar los cambios del ticket aislados.

No se ejecutó el binario, el juego ni pruebas largas. No se validó el build Windows;
la instrumentación usa el acceso a registros multiplataforma ya existente.

### Compilación (segunda pasada)

Cambio de código: sólo el texto del banner de `BB_WRITE_WINDOW` en `page_manager.cpp`
(`getenv`/`printf` portables); no cambia gates, defaults ni decisiones.

- Linux: `ufc3-build` → exit 0, `out/sacredfire` generado (PGO off, LTO ON).
- Windows (segunda pasada): **no verificado**; resuelto en la tercera pasada sólo a nivel de
  sintaxis (ver «Validación Windows»). No hay wrapper Windows con lock; `scripts/build-windows.sh
  --runtime` reconstruiría toda la biblioteca en el worktree. Se preparó una comprobación de
  una sola unidad sin reconstruir (`ninja -t commands sacredfire-game` del build Windows del
  checkout principal, filtrando `page_manager.cpp`, con rutas `gpu/` apuntando al worktree y
  `-fsyntax-only` en lugar de `-o`), pero el entorno del agente no permitió ejecutarla.
- `build.sh` volvió a aplicar el parche FSR al submódulo `gpu/third_party/fsr-vulkan`; queda
  modificado en el worktree y no se commitea (el agente no pudo revertirlo).
