# Online: referencias y diferencias verificadas

Revisión: 2026-10-14. Esta investigación responde a la petición de comparar ports
existentes antes de seguir probando cambios del login. Los clones de referencia
están en `~/Projects/refs/`; no se incorporó su código al runtime.

## Referencias principales

| Proyecto / revisión inspeccionada | Código útil | Aplicación a UFC |
| --- | --- | --- |
| [droogie/bbhost](https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771) | `src/hle/net.cpp`, `src/hle/net_p2p.h`, `src/net/stun.cpp`, `src/hle/http.cpp`, `docs/online.md` | ABI de sockets PS4, puertos virtuales P2P, colas por puerto, STUN, hole punching y relay; HTTP asíncrono. Su backend Bloodborne no sustituye Blaze. |
| [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc/tree/21f8497082e73a2c3a21c87c69e08a236fb8f217) | `gpu/bbnet/bbnet_glue.cpp`, `gpu/shadps4/core/libraries/network/`, integración en CMake | Ejemplo de integrar la red de shadp2p y bbbridge en un port, incluyendo Windows. NP Matching y su servidor son específicos de Bloodborne. |
| [buchacho/BF4BlazeEmulator](https://github.com/buchacho/BF4BlazeEmulator/tree/51952cdd9d496cc64340c40c14a1ccec46c98915) | `Components_Client/Util.py`, `Accounts.py`, `Authentication.py` | Configuración preAuth, sesión tras OAuth y notificaciones UserSessions. Los componentes y campos deben contrastarse con UFC. |
| [PocketRelay/Server](https://github.com/PocketRelay/Server/tree/41a227016756bbcec9a275ee4c7c5bebbfdd9bda) | Modelos y rutas Authentication, UserSessions, GameManager | Arquitectura de un servidor privado Blaze para ME3. Usa un esquema anterior; no es un backend UFC listo para usar. |
| [Aim4kill/BlazeSDK](https://github.com/Aim4kill/BlazeSDK/tree/4bab6e20c3baf2edcd4565e1655c97b57d8e737b) | Tipos generados Blaze2/Blaze3 | Referencia de etiquetas, tipos y comandos. Su LoginResponse Blaze3 difiere del reflejado por UFC. |

Los README de los ports describen online funcional. Aquí se verificó su código;
no se ejecutaron partidas de esos proyectos para certificar esas afirmaciones.
No se encontró en la búsqueda realizada un backend público específico de UFC 3;
eso no demuestra que no exista.

## Huecos comprobados en el runtime actual

1. **P2P:** `runtime_net.c` convierte el tipo 6 en UDP y descarta `vport` al
   convertir sockaddr. En bbhost varias direcciones virtuales comparten un socket
   UDP físico, y un lector distribuye paquetes a colas por puerto virtual. La
   cabecera incluye puertos de origen/destino de 8 o 16 bits y un comid opcional.
   Crear dos UDP independientes no reproduce ese comportamiento.
2. **Internet/NAT:** bbhost usa el mismo socket para STUN y tráfico P2P, descubre
   la dirección pública, realiza hole punching y dispone de relay. UFC todavía
   no implementa esa cadena. Un eco UDP en localhost no la valida.
3. **HTTP:** bbhost entrega eventos IN|OUT con detalle 0 tras completar el worker;
   devuelve 0 al iniciar un envío asíncrono. No se encontró allí una implementación
   equivalente del callback de redirección que necesita el OAuth de UFC. Copiar
   su contrato HTTP sin comprobar ProtoHttp de UFC no resolvería ese requisito.
4. **Blaze:** el servidor UFC todavía responde éxito vacío a comandos desconocidos
   y no establece una sesión Authentication ni implementa matchmaking. Las rutas
   de BF4 y PocketRelay muestran que recibir un código OAuth es solo un paso.

## Diferencias que impiden copiar paquetes de BF4/Blaze3

La reflexión del ejecutable UFC identifica `Authentication::LoginResponse` con
`ANON`, `SESS`, `SPAM`, `UNDR`. `UserLoginInfo` tiene `1CON`, `BUID`, `FRST`,
`GEO`, `KEY`, `LLOG`, `MAIL`, `PDTL`, `UID`.

BF4 Accounts devuelve además `NTOS` y usa `FRSC`; su operación es componente
0x23/comando 0x0a. Todavía no se verificó que UFC use esa operación. BlazeSDK
Blaze3 tiene otro LoginResponse, con campos como `LDHT`, `PLST` y `SKEY`.
Reutilizar cualquiera de esos payloads sin adaptar el esquema no está justificado.

`blazeServerClientId` aparece en la configuración preAuth de BF4 y en la reflexión
de UFC. La referencia UFC inspeccionada en 0xeb0b3d registra el nombre de un campo;
**no prueba que lo lea del mapa CONF ni que su ausencia cause el fallo**. El
servidor actual ya envía `CLID=UFC2018_PS4_SERVER` y los registros muestran una
petición OAuth con ese client_id. No se añadió una clave especulativa.

## Diagnóstico y pruebas actuales

- El juego sigue sin enviar un login Authentication después del código de servidor.
  La captura de inicio, Quick Fight y Fighternet muestra el mismo error genérico;
  es compatible con un fallo compartido de autenticación, pero la pantalla por sí
  sola no identifica la causa.
- El código HTTP asíncrono experimental llegó a provocar un error de Location:
  la cabecera cruda sí contenía Location, pero la caché interna de ProtoHttp estaba
  vacía. La última variante entrega el callback al reintentar SendRequest; pasó
  el harness HTTP, pero aún no se validó dentro del juego.
- Pasaron pruebas de framing HTTP fragmentado, parser de cabeceras, errno por
  hilo y eco UDP común. Los ensayos Windows ejecutados bajo Wine comprueban el
  binario PE/Winsock en esa capa; no prueban Windows real ni partidas UFC.
- No hay una partida UFC P2P verificada en Linux/Linux, Linux/Windows o
  Windows/Windows. No hay commits, pushes ni una versión online publicada.

## Orden para continuar cuando se retome la implementación

1. Confirmar el contrato HTTP de UFC frente a implementaciones de referencia y
   validar una única corrección del callback/caché con el probe existente. Exigir
   evidencia: OAuth completo y una solicitud Blaze de login identificable.
2. Identificar en UFC componente/comando y esquema de esa solicitud, implementar
   la respuesta y notificaciones correspondientes usando BF4/PocketRelay como
   referencias de flujo. Vincular código, token, identidad y sesión del mismo
   cliente; no asignar identidades nuevas en cada tokeninfo.
3. Adaptar la arquitectura de sockets P2P compartidos de bbhost/shadp2p. Probar
   tipo 6, dos puertos virtuales en un puerto físico, aislamiento de colas,
   direcciones devueltas y readiness antes de intentar una pelea.
4. Implementar matchmaking mínimo host/invitado y verificar una pelea en LAN.
   Después validar Windows real y finalmente NAT/relay entre redes distintas.

No trasladar las APIs NP Matching específicas de Bloodborne al backend EA.
Conservar atribución y revisar las licencias de cualquier código que finalmente
se incorpore; bbhost está publicado bajo GPLv3.
