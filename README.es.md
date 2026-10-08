<p align="center">
  <img src="docs/icon.png" width="96" alt="Icono de VitaIRC">
</p>

<h1 align="center">VitaIRC</h1>

<p align="center">
  Cliente IRC nativo y ligero para PlayStation Vita<br>
  con <b>traducción gratis</b>, <b>subida de imágenes</b>, <b>emoji y reacciones</b>, visor de imágenes y soporte de bouncers.
</p>

<p align="center">
  <img src="docs/tour.gif" width="640" alt="Recorrido por VitaIRC">
</p>

<p align="center"><i>English: <a href="README.md">README.md</a></i></p>

---

VitaIRC es un cliente IRC homebrew escrito en C para consolas con HENkaku/Ensō. Se conecta a los
servidores IRC directamente desde la Vita, sin PC ni servidores intermedios. Está hecho para apoyar
a la escena de la Vita y es de código abierto.

![Interfaz en español](screenshots/15_chat_es.png)

## Funciones

- Varios servidores a la vez, SSL/TLS y reconexión automática.
- **Identificación automática** (SASL, NickServ o PASS de servidor): si pusiste contraseña, se identifica
  sola y **recupera tu nick** si una sesión anterior lo dejó ocupado.
- Canales, privados, lista de usuarios con prefijos y estado ausente.
- IRCv3 (`server-time`, `multi-prefix`, `away-notify`, `batch`, `message-tags` y **`draft/chathistory`**):
  con un bouncer como soju recibes lo que te perdiste al conectar, y **al subir más allá del inicio
  se cargan mensajes anteriores** desde el servidor.
- **Reacciones** (`+draft/react`): toca un mensaje → *Reaccionar...*; se muestran debajo del mensaje.
- **Emoji** con una fuente monocroma (Noto Emoji) donde la del sistema no llega.
- **Separadores de fecha** (*Hoy*, *Ayer*, *jue 08/10/2026*) entre días.
- **Completar nicks**: escribe `@` y el comienzo de un nick, confirma, y VitaIRC lo completa.
- **Silenciar** un canal (sin sonido ni avisos).
- **Cola sin conexión**: lo que escribes desconectado se muestra atenuado y se envía al reconectar.
- **Aviso de versión nueva** (consulta GitHub Releases una vez al día).
- **Colores, negritas y subrayado de IRC**, **palabras de alerta**, avisos y **sonido** en privados y menciones.
- **Ignorar usuarios** (con comodines) y **protección contra flood** de privados.
- **Herramientas de operador** desde la lista de usuarios: op, voz, kick y ban.
- `/away`, invitaciones con confirmación, explorador de canales (`/list`).
- **Buscar en el canal** y **saltar a la última mención**.
- **Redes predefinidas**: Libera.Chat, OFTC, EFnet, Rizon, DALnet, hackint, IRCnet, Undernet, QuakeNet.
- **Historial guardado** en la tarjeta de memoria: al reabrir vuelven tus canales y sus mensajes.
- **Respuestas rápidas** (botón volver) y **borradores**.
- **Toca un mensaje** para responder, abrir un privado, traducirlo, ignorar o abrir sus enlaces.
- **Traducción gratis con Google** (sin cuenta) o **con ChatGPT** (tu API key), de mensajes recibidos
  y propios, con idioma por canal y caché en la tarjeta.
- **Sube imágenes** con vista previa o **saca una foto con la cámara** de la Vita: **Litterbox** (sin
  cuenta), **ImgBB**, **Imgur** o **tu propio soju** (FILEHOST). *Mis subidas recientes* guarda los enlaces.
- **Visor de imágenes** integrado (PNG/JPG con zoom); el resto de enlaces se abre en el navegador.
- Interfaz en **español e inglés**, controles físicos y pantalla táctil.

## Instalación

1. Descarga `VitaIRC.vpk` desde [Releases](../../releases).
2. Cópialo a la Vita (VitaShell por FTP o USB) e instálalo.
3. Abre **VitaIRC**. La primera vez se conecta a Libera.Chat y entra a `#vitasdk`.

## Controles

| Botón | Acción |
|---|---|
| ✕ (○ en consolas japonesas) | Escribir mensaje o comando |
| ○ | Respuestas rápidas, o volver al final si estás leyendo arriba |
| △ / START | Menú |
| □ | Activar/desactivar la traducción de recibidos en el canal |
| L / R, ← / → | Cambiar de ventana |
| ↑ / ↓, deslizar | Desplazar |
| SELECT | Servidores |
| Tocar un mensaje | Responder, privado, reaccionar, traducir, ignorar, abrir enlaces |

Escribe `/help` para ver todos los comandos (`/join`, `/msg`, `/away`, `/ignore`, `/kick`, `/ban`...).

## Configuración rápida

- **Traducción**: funciona sin configurar nada (Google). Para ChatGPT: △ → Ajustes → *Traductor*, y
  pon tu API key de [platform.openai.com](https://platform.openai.com/api-keys).
- **Imágenes**: △ → Ajustes → *Servicio de imágenes*. Litterbox no necesita nada; ImgBB necesita una
  API key gratis de [api.imgbb.com](https://api.imgbb.com/); Imgur un Client-ID de
  [api.imgur.com/oauth2/addclient](https://api.imgur.com/oauth2/addclient) (*uso anónimo*); soju
  FILEHOST, un soju 0.8+ con `file-upload` ([guía](docs/soju-filehost.md)).
- **Cuenta de Libera.Chat**: en Servidores → Libera.Chat, completa Nick, Cuenta y Contraseña, con
  autenticación **SASL PLAIN** o **Automática**. Al conectar verás "Autenticación SASL correcta".
- **Bouncer (soju/ZNC)**: agrégalo como un servidor más. soju: SASL con cuenta `usuario/red@vita`;
  ZNC: PASS de servidor `usuario/red:contraseña`. Así no pierdes mensajes aunque la Vita esté apagada.
  Con soju, VitaIRC pide el historial por `chathistory` (necesita `message-store db` en soju).

Todo se guarda en `ux0:data/VitaIRC/` (`config.ini`, `logs/`, `photos/`...). Las claves y contraseñas
se guardan en texto plano.

## Actualizar

Instala el VPK nuevo encima del anterior. Solo se reemplaza la app (`ux0:app/VIRC00001/`); tus
servidores, canales, ajustes e historial en `ux0:data/VitaIRC/` se conservan.

## Limitaciones

La Vita suspende las apps al apagar la pantalla con POWER o al salir de ellas, así que VitaIRC no
puede quedarse conectado en segundo plano. Con *"Evitar suspensión conectado"* no se duerme sola
mientras está abierta, y al volver de la suspensión se reconecta sola. Para no perder nada, usa un bouncer.

## Compilar

```sh
vdpm install libvita2d freetype curl-mbedtls mbedtls zlib libpng
export VITASDK=/usr/local/vitasdk
./build.sh        # deja dist/VitaIRC.vpk
```

Más detalles (estructura del proyecto, traducciones, capturas con Vita3K) en el [README en inglés](README.md).

## Licencia

[MIT](LICENSE).
