# Mesh_Modem_Web_v3_Batt — nodo LoRa compatible con la red Meshtastic®, con saltos, chat web y batería

**ELECTGPL** · ESP32-S3 + SX1262 · Arduino · única biblioteca externa: [RadioLib](https://github.com/jgromes/RadioLib)

Mesh_Modem_Web_v3_Batt (v3.4) es la v3 completa más cuatro cambios:

* Lectura de la batería con el fuel gauge **MAX17048** de la placa: SOC, tensión y tasa de carga.
* Interfaz web en **tema oscuro fijo**, independiente del tema del sistema.
* **LEDs de estado:** alimentación en el rojo y mensajes sin leer en el verde.
* **Ecos:** confirmación de qué nodos repitieron cada mensaje propio (ACK implícito).

Todo lo demás (retransmisión, chat, historial y comandos) es idéntico a la v3. Mesh_Modem_Web v3 es la evolución de [MeshAT](../mesh_at_modem/README.md) y de Mesh_Modem_Web v2. La diferencia central de la v3 es que **retransmite**: deja de ser un nodo que solo origina y recibe, y pasa a ser un eslabón de la malla que extiende su alcance, con las mismas reglas que el firmware oficial (sección «Retransmisión»).

Igual que la v2, usa el modem LoRa por comandos AT y le suma tres cosas:

* Conexión a la red WiFi de la casa.
* Un chat web estilo IRC con panel de configuración.
* Un portal cautivo para cargar las credenciales WiFi.

La UART y la web son dos ventanas al mismo modem. Todo lo que se hace desde la web aparece en la terminal serie, y lo que se hace por AT aparece en la web.

```
            ┌────────────── Mesh_Modem_Web (ESP32-S3) ───────────────┐
 Terminal ──┤ UART ─┐                                                │
 serie      │       ├──► intérprete AT único ──► radio SX1262 ───────┼──► malla Meshtastic®
 Navegador ─┤ HTTP ─┘          │                     │               │
 (casa)     │                  └──── eventos (RX, TX, nodos) ◄───────┤
            └────────────────────────────────────────────────────────┘
```

---

## Batería (MAX17048)

El MAX17048 está en el bus I2C (SDA = GPIO8, SCL = GPIO9, dirección `0x36`), el mismo que usa el ADXL343 en el AlarmSensor. Se lee cada 10 s y se leen tres registros:

| Registro | Dato | Escala |
|---|---|---|
| `0x02` VCELL | Tensión de la celda | 78,125 µV/LSB |
| `0x04` SOC | Estado de carga | 1/256 %/LSB |
| `0x16` CRATE | Tasa de carga (+) o descarga (−) | 0,208 %/h por LSB, con signo |

El SOC lo calcula el algoritmo ModelGauge del propio chip a partir de la tensión, sin resistencia de shunt. Recién después de algunos minutos de funcionamiento se vuelve confiable, y puede marcar algo por encima de 100 % con la celda a fondo: el firmware lo limita a 100 % al mostrarlo.

* **Terminal:** `AT+BATT?` responde, por ejemplo, `+BATT:76.4%,3.987V,+2.3%/h,CARGANDO,USB`. `ATI` incluye la misma línea. Si el chip no responde en el bus, la respuesta es `+BATT:NO_DETECTADO`.
* **Web:** la cabecera muestra un ícono de batería con el porcentaje y la tensión, más la fuente: «USB», «USB ▲ cargando» o «a batería». El color cambia según el nivel: turquesa sobre 40 %, ámbar entre 15 y 40 %, rojo debajo de 15 %. Si se pasa el mouse por encima, aparece la tasa en %/h.
* **Aviso:** al bajar del 15 %, aparece un aviso en el chat.

Sin celda conectada, el BQ24075 hace pulsos de detección en el pin BAT, así que la tensión y el SOC que informe el MAX17048 en esa condición no son confiables.

## Ecos: saber si tu mensaje se propagó

Cuando otro nodo repite un mensaje tuyo, tu modem escucha el eco. Llega con tu `from`, el mismo `id` y, en `relay_node`, el último byte del nodo que lo repitió. El firmware oficial usa esto como ACK implícito. Mesh_Modem_Web lo muestra, así sabés si tu mensaje salió de tu zona sin depender de que alguien conteste.

* **Terminal:** `+ECO:1a2b3c4d,relay=B8,SB01,-80,7.5,2`. Los campos son el id del mensaje, el byte del repetidor, su nombre corto (si se conoce un nodo cuyo NodeNum termina en ese byte), RSSI, SNR y el `hop_limit` del eco.
* **Web:** junto a tu mensaje aparece primero «esperando eco…». Cuando llegan ecos, pasa a «✓ repetido por N: …», con la lista de repetidores. Si a los 15 s no llegó ninguno, muestra «✗ sin eco: nadie a tu alcance lo repitió».

Para interpretar el `hop_limit` del eco, compáralo con el valor con que salió tu mensaje (`AT+HOP`):

* **`AT+HOP − 1`:** lo repitió un vecino directo.
* **Un valor menor:** lo escuchaste después de dos o más repeticiones. La web lo marca como «2.º salto».

Ten en cuenta también:

* Los firmwares anteriores a 2.6 no completan `relay_node` y envían 0. Todos esos repetidores se cuentan como uno solo, «un nodo con firmware anterior a 2.6».
* Que no aparezcan ecos no prueba que el mensaje no se propagó: solo escuchás a los repetidores que están a tu alcance. Que aparezcan sí prueba que salió.
* Se siguen los últimos 8 mensajes enviados. Los ecos se muestran en vivo y no se guardan en el historial.

## LEDs

| LED | Estado | Significado |
|---|---|---|
| Rojo (GPIO47) | Fijo | Alimentado por USB (cargando o con la celda llena) |
| Rojo (GPIO47) | Destello corto cada 3 s | Funcionando a batería |
| Verde (GPIO48) | 3 destellos rápidos | Llegó un mensaje de texto |
| Verde (GPIO48) | Destello cada 2 s | Hay mensajes sin leer |

Los mensajes se marcan como leídos de dos maneras:

* cuando la web está abierta y la pestaña está **visible**;
* cuando se usa la terminal serie (cualquier comando).

Una pestaña en segundo plano no los marca. `AT+LED=0` apaga los LEDs (por ejemplo, de noche) y `AT+LED=1` los vuelve a activar.

### Detección de USB

Hay dos métodos:

* **Con `/PGOOD` (recomendado).** El BQ24075 tiene la salida `/PGOOD`: es open-drain y va a nivel bajo cuando hay una entrada de alimentación válida. Si está cableada a un GPIO libre (1, 2, 6 o 7), se configura en `PIN_PGOOD` y la detección es instantánea. El firmware activa el pull-up interno.
* **Sin `/PGOOD` (`PIN_PGOOD = -1`, por defecto).** Se estima con la tasa del MAX17048. A batería la celda siempre se descarga, del orden de −2 a −5 %/h con el modem activo. Con USB, en cambio, carga o queda quieta. La histéresis está entre −1,5 y −0,8 %/h. Como el chip promedia la tasa, **el cambio de estado tarda unos minutos** en reflejarse.

`AT+BATT?` y la web informan la fuente detectada (`USB` o `BATERIA`).

## Retransmisión: formar parte de la malla

En Meshtastic los saltos los hace **quien recibe**, no quien transmite. Con la v2, tus mensajes llegaban lejos solo si había nodos oficiales que los repitieran, y vos no repetías los de nadie. Con la v3, tu nodo también repite. Si está en un lugar que une dos zonas, los nodos de un lado pasan a escuchar a los del otro. Y una red armada solo con estos modems ahora tiene saltos.

### Qué se repite

Un paquete se repite si cumple todo esto:

* CRC de LoRa correcto.
* No es un duplicado: el par `from` + `id` no está entre los últimos 32 vistos.
* No es para este nodo, ni lo originó este nodo.
* `hop_limit > 0` e `id ≠ 0`.
* `next_hop` vale 0 (sin preferencia) o el último byte de este NodeNum.

Para repetir **no hace falta descifrar**: se retransmite el paquete crudo. Por eso también se repiten canales privados y DMs de otros, que siguen cifrados. Es el modo `ALL` del firmware oficial. Basta con coincidir en la capa física: mismo preset y misma frecuencia.

Al repetir, el paquete cambia en tres campos y el resto queda igual:

* `hop_limit` se decrementa en 1.
* `relay_node` pasa a ser el último byte de este nodo.
* `next_hop` queda en 0.

`hop_start` no se toca, porque es la referencia para calcular cuántos saltos lleva el paquete.

### Cuándo se repite

No se repite enseguida. Se usa la misma ventana de contención que `RadioInterface::getTxDelayMsecWeighted()` del firmware oficial:

```
slot   = 2,5 · T_símbolo + 7,6 ms             (CAD + propagación, conmutación y MAC)
CW     = map(SNR, −20…10 dB → 3…8)
cliente: espera = 16·slot + random(0, 2^CW)·slot
router : espera = random(0, 2·CW)·slot
```

Un **SNR bajo** da una ventana corta: los nodos que escucharon débil, que suelen ser los más lejanos, repiten primero porque son los que más alcance agregan. En modo cliente, si durante la espera se escucha que otro nodo ya repitió el paquete, la retransmisión propia **se cancela**. Así una zona con muchos nodos no repite lo mismo diez veces.

Valores de referencia, verificados con la fórmula:

| Preset | slot | Cliente, SNR −15 dB | Cliente, SNR +10 dB | Router |
|---|---|---|---|---|
| MediumFast | 12,7 ms | 204 a 293 ms | 204 ms a 3,4 s | 0 a 191 ms |
| LongFast | 28,1 ms | 449 a 646 ms | 449 ms a 7,6 s | 0 a 421 ms |

### Modos

| `AT+RELAY` | Rol anunciado | Comportamiento | Cuándo usarlo |
|---|---|---|---|
| `0` | CLIENT_MUTE | No repite | Nodo de escritorio en una zona muy cubierta, o para ahorrar batería |
| `1` (por defecto) | CLIENT | Repite con espera según SNR y cancela si otro repite antes | Uso normal |
| `2` | ROUTER | Repite siempre, con prioridad, y nunca cancela | Solo un nodo bien ubicado: altura, buena antena, alimentación fija |

El rol se anuncia en el NodeInfo (campo `role` de `User`), así que la app de los demás muestra tu nodo con el rol correcto. Además se anuncia `is_unmessagable = true`: como este nodo no implementa DMs cifrados, la app no ofrece enviarle mensajes directos.

### Qué se gana y qué cuesta

Se gana cobertura y redundancia. Además, tus repeticiones funcionan como **ACK implícito** para quien envió el mensaje: el emisor original escucha que alguien lo repitió y sabe que salió de su zona.

El costo es tiempo de aire, energía y colisiones. Cada repetición es una transmisión más en el canal. En modo router se suma todo el tráfico de la malla. Por eso el modo router se reserva para nodos bien ubicados, como recomienda Meshtastic. Si la placa se alimenta a través del BQ24075, conviene tener la batería conectada: repetir significa transmitir con mucha más frecuencia.

Con `AT+RAW=1`, cada repetición se informa en la terminal como `+RELAY:!from,id,hop_limit`. `ATI`, la línea `+RELAY` y la cabecera de la web muestran el modo y los contadores de repetidos y cancelados.

## Puesta en marcha

1. En Arduino IDE, elegir *ESP32S3 Dev Module* e instalar RadioLib desde el Library Manager. El resto de las bibliotecas viene con el core ESP32 (validado con 3.3.0).
2. Editar el bloque `CONFIG` del sketch. Como mínimo, un **`NODE_ID` distinto en cada placa**.
3. Grabar. La primera vez no hay credenciales guardadas y el modem levanta solo el **portal cautivo**:
   * Conectarse con el teléfono o la PC a la red abierta `MeshModem-XXXX`.
   * Se abre la página de configuración (si no aparece sola, ir a `http://192.168.4.1`).
   * Elegir la red de la casa, escribir la contraseña y apretar «Guardar y conectar».
4. Volver a la red de la casa y abrir **`http://meshmodem.local`**. Si el sistema no resuelve mDNS, usar la IP que informa `ATI` en la terminal.

Para cambiar de red WiFi más adelante, hay que **mantener apretado BOOT durante 5 segundos** con el equipo ya encendido, o enviar `AT+PORTAL`. Si BOOT está apretado en el momento del arranque, el ESP32-S3 entra en modo de grabación, como siempre. También se pueden cargar las credenciales directo por la terminal con `AT+WIFI=ssid,clave`.

## La interfaz web

* **Chat:** muestra los mensajes del canal con hora, nombre corto del nodo (cada uno con su color), RSSI, SNR y saltos. Los mensajes propios salen con tu nombre corto.
* **Comandos AT desde el chat:** una línea que empieza con `/` se envía como comando. Por ejemplo, `/ATI` muestra la configuración en el mismo chat y `/AT+NODES` lista los nodos.
* **Nodos escuchados:** nombre corto y largo, NodeNum, RSSI, SNR y cuánto hace que se escuchó cada nodo.
* **Historial:** al abrir la página se cargan los últimos 300 mensajes guardados en el modem, con fecha y hora, y después sigue en vivo. Se puede descargar completo («Descargar») o borrar («Borrar historial»).
* **Configuración:** nombre, NodeNum, preset, región, frecuencia manual, modo de retransmisión, saltos y potencia. Además tiene tres botones:
  * «Aplicar»: traduce los cambios a los mismos comandos AT y los aplica en el momento.
  * «Guardar en flash»: equivale a `AT+SAVE`.
  * «Enviar NodeInfo»: equivale a `AT+NI`.

La web consulta al modem una vez por segundo y nunca toca la radio por su cuenta: todo pasa por el intérprete AT. Por eso las dos interfaces no pueden quedar desincronizadas.

## Comandos AT

Son los mismos de MeshAT, más los de WiFi y almacenamiento.

| Comando | Función |
|---|---|
| `AT` / `AT+HELP` | Prueba de comunicación / ayuda |
| `ATI` / `AT+INFO?` | **Toda la configuración y el estado**: nodo, nombres, preset, hash, banda, frecuencia, slot, saltos, potencia, RAW, batería, retransmisión con sus contadores, motivo del último arranque, uso del historial y WiFi |
| `AT+NODE=!aabbccdd` | NodeNum (32 bit, hex) |
| `AT+NAME=largo[,corto]` | Nombre largo (≤ 39) y corto (≤ 4) |
| `AT+PRESET=?` / `=LongFast` / `=6` | Preset de modem |
| `AT+BAND=915,928` | Bordes de la banda de la región, en MHz |
| `AT+FREQ=926.125` / `=0` | Frecuencia manual / slot automático |
| `AT+HOP=3` | Saltos máximos de lo que origina este nodo (0..7) |
| `AT+RELAY=0`/`1`/`2` / `AT+RELAY?` | Retransmisión: mudo, cliente o router / modo actual |
| `AT+PWR=20` | Potencia de TX en dBm (−9..22) |
| `AT+SEND=texto` | Texto broadcast (≤ 232 bytes) |
| `AT+NI` / `AT+NODES` | Enviar NodeInfo / listar nodos escuchados |
| `AT+BATT?` | Batería: SOC, tensión, tasa de carga, estado y fuente (USB/BATERIA) |
| `AT+LED=0` / `AT+LED=1` | LEDs apagados / activos |
| `AT+SAVE` | Guarda la configuración de radio en flash |
| `AT+WIFI=ssid,clave` / `AT+WIFI?` | Credenciales WiFi / estado |
| `AT+PORTAL` | Portal de configuración WiFi |
| `AT+LOG=20` / `AT+LOGCLR` | Últimas N entradas del historial / borrar historial |
| `AT+RAW=1` | Traza de todo lo recibido |
| `AT&F` / `AT+RST` | Valores del bloque `CONFIG` (borra lo guardado) / reinicio |

En la terminal, los comandos que llegan desde la web aparecen precedidos por `[web]`, seguidos de su respuesta:

```
[web] AT+SEND=hola desde el navegador
+TX:5c1e09a3
OK
+RX:!9e7563b8,!ffffffff,-87,9.5,1,te leo desde Ciudadela
```

## Persistencia

| Qué | Dónde | Cuándo se guarda |
|---|---|---|
| Credenciales WiFi | NVS | Al guardarlas en el portal o con `AT+WIFI=` |
| Configuración de radio | NVS | Solo con `AT+SAVE` («Guardar en flash») |
| Historial de mensajes | LittleFS (`/log.jsonl` y `/log.old`) | Automáticamente, con cada mensaje recibido o enviado y con cada arranque |
| Valores de fábrica | Bloque `CONFIG` del sketch | Al compilar; `AT&F` vuelve a ellos |

Los cambios se aplican en el momento y se pierden al reiniciar mientras no se guarden. Esto permite probar un preset o una potencia sin dejarlos fijos por accidente.

## Historial de mensajes

Cada mensaje recibido o enviado se agrega como una línea JSON al archivo `/log.jsonl`, en la partición de datos de la flash interna (LittleFS). **No hace falta memoria externa.** La partición por defecto de 4 MB reserva unos 1,5 MB para datos, y el historial usa como máximo dos archivos de 384 KB: el actual y uno rotado, `/log.old`. Eso equivale a varios miles de mensajes. Cuando el actual se llena, el más viejo se descarta.

Cada línea lleva:

* La hora real (`ts`, de NTP), si el modem tenía WiFi y hora al momento del mensaje.
* Los segundos desde el arranque (`up`), que la web muestra como `+1h15` cuando no hay hora real.

El desgaste de la flash no es un problema a este ritmo. LittleFS reparte las escrituras, y aun con mil mensajes por día se escriben unos 200 KB diarios, muy lejos de los ciclos de borrado del chip.

Cada arranque también queda registrado, con su motivo: encendido, brownout, watchdog, excepción, software, etc. Si alguna vez el modem se reinicia solo, el historial y `ATI` (línea `+BOOT`) dicen por qué. En la web, un reinicio ya no borra la pantalla: aparece un aviso y el historial sigue cargado.

## Cómo está armado

* **Un solo lazo y sin tareas extra.** La radio, la UART, el servidor HTTP, el DNS del portal y el botón se atienden en el mismo `loop()`. Así no hay acceso concurrente al SPI ni hace falta un mutex. La contracara es que, mientras dura una transmisión, la web queda en pausa: CAD más tiempo en aire, desde unos 100 ms en MediumFast hasta varios segundos en LongSlow.
* **Salida unificada.** Toda la salida pasa por un `Print` que escribe en la UART y, mientras se atiende una petición web, también en la respuesta HTTP. Así la terminal y la web ven exactamente las mismas respuestas.
* **Eventos en vivo y en flash.** Los eventos de la sesión (mensajes, NodeInfo, avisos) van a un anillo de 48 en RAM, que la web consulta con `/api/ev?since=N`. Además, los mensajes y los arranques se escriben en el historial de LittleFS, que es lo que carga la web al abrirse.
* **Hora real.** Con WiFi conectado, la hora se toma por NTP (UTC−3); sin WiFi, la web usa la hora del navegador.
* **Sin bibliotecas web externas.** Se usan `WebServer`, `DNSServer` y `ESPmDNS` del core. Las páginas están embebidas en el firmware como texto, sin archivos en LittleFS.

API HTTP, por si querés integrarlo con otra cosa:

| Ruta | Método | Devuelve |
|---|---|---|
| `/` | GET | Chat (o el portal, si está activo) |
| `/api/info` | GET | JSON con la configuración, el estado WiFi y los nodos |
| `/api/ev?since=N` | GET | JSON con los eventos desde el número N |
| `/api/at` | POST `cmd=...` | Texto con la respuesta del comando AT |
| `/api/log?n=N` | GET | JSON con las últimas N entradas del historial |
| `/log.jsonl` | GET | Descarga del historial completo (una línea JSON por evento) |

## Protocolo

La trama, el cifrado y el cálculo de frecuencia son idénticos a los de MeshAT, y están explicados en su README. Para mensajes propios, las limitaciones también son las mismas: solo broadcast en el canal primario por defecto y sin mensajes directos. A diferencia de las versiones anteriores, la v3 sí retransmite.

La configuración guardada con `AT+SAVE` en la v2 no se reutiliza: la estructura cambió, porque ahora incluye el modo de retransmisión. Al primer arranque la v3 toma los valores del bloque `CONFIG`; después se puede volver a guardar.

## Consideraciones

* **La web no tiene contraseña.** Cualquier equipo de tu red local puede chatear y cambiar la configuración. Está pensada para la red de la casa; no conviene exponerla a Internet con un reenvío de puertos.
* **El canal por defecto es público.** Lo que escribís en el chat lo lee cualquier nodo Meshtastic al alcance.
* **Tamaño.** El firmware ocupa alrededor del 89 % de la partición por defecto (1,25 MB). Si se agregan más funciones, conviene pasar a *Huge APP* o *8M with spiffs*.
* **Lo que no replica del firmware oficial:** el reemplazo de un paquete en cola por una copia con más saltos restantes, el ruteo por `next_hop` aprendido de los ACK (al repetir siempre se usa inundación) y los modos de retransmisión `LOCAL_ONLY`/`KNOWN_ONLY`. Para un nodo de este tipo son refinamientos, no requisitos. Para agregar funciones, como OTA, conviene pasar a un esquema de particiones más grande (*Huge APP* o *8M with spiffs*).

---

*Mesh_Modem_Web_v3_Batt es un proyecto independiente de ELECTGPL que habla la misma trama de aire para interoperar; no está afiliado ni avalado por el proyecto Meshtastic.*
*Meshtastic® is a registered trademark of Meshtastic LLC. Meshtastic software components are released under various licenses, see GitHub for details. No warranty is provided - use at your own risk.*
