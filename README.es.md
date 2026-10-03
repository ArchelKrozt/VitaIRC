<p align="center">
  <img src="docs/icon.png" width="96" alt="Icono de VitaIRC">
</p>

<h1 align="center">VitaIRC</h1>

<p align="center">
  Cliente IRC nativo y ligero para PlayStation Vita<br>
  con <b>traducción por ChatGPT</b>, <b>subida a Imgur</b>, visor de imágenes y soporte de bouncers.
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

- Varios servidores a la vez, SSL/TLS y reconexión automática. NickServ, **SASL** y PASS de servidor.
- Canales, privados, lista de usuarios con prefijos y estado ausente.
- IRCv3 (`server-time`, `multi-prefix`, `away-notify`), ideal para usar con bouncers.
- **Colores, negritas y subrayado de IRC**, **palabras de alerta**, avisos y **sonido** en privados y menciones.
- **Ignorar usuarios** (con comodines) y **protección contra flood** de privados.
- **Herramientas de operador** desde la lista de usuarios: op, voz, kick y ban.
- `/away`, invitaciones con confirmación, explorador de canales (`/list`).
- **Buscar en el canal** y **saltar a la última mención**.
- **Redes predefinidas**: Libera.Chat, OFTC, EFnet, Rizon, DALnet, hackint, IRCnet, Undernet, QuakeNet.
- **Historial guardado** en la tarjeta de memoria: al reabrir vuelven tus canales y sus mensajes.
- **Respuestas rápidas** (botón volver) y **borradores**.
- **Toca un mensaje** para responder, abrir un privado, traducirlo, ignorar o abrir sus enlaces.
- **Traducción con ChatGPT** de mensajes recibidos y propios, con idioma por canal, caché en la
  tarjeta y contador de uso de la API.
- **Imgur**: sube imágenes con vista previa o **saca una foto con la cámara** de la Vita.
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
| Tocar un mensaje | Responder, privado, traducir, ignorar, abrir enlaces |

Escribe `/help` para ver todos los comandos (`/join`, `/msg`, `/away`, `/ignore`, `/kick`, `/ban`...).

## Configuración rápida

- **ChatGPT**: crea una API key en [platform.openai.com](https://platform.openai.com/api-keys) y ponla en △ → Ajustes.
- **Imgur**: registra una app en [api.imgur.com/oauth2/addclient](https://api.imgur.com/oauth2/addclient)
  (*uso anónimo*) y copia el Client-ID en Ajustes.
- **Cuenta de Libera.Chat**: en Servidores → Libera.Chat, completa Nick, Cuenta y Contraseña, y elige
  autenticación **SASL PLAIN**.
- **Bouncer (soju/ZNC)**: agrégalo como un servidor más. soju: SASL con cuenta `usuario/red@vita`;
  ZNC: PASS de servidor `usuario/red:contraseña`. Así no pierdes mensajes aunque la Vita esté apagada.

Todo se guarda en `ux0:data/VitaIRC/` (`config.ini`, `logs/`, `photos/`...). Las claves y contraseñas
se guardan en texto plano.

## Limitaciones

La Vita suspende las apps al apagar la pantalla con POWER o al salir de ellas, así que VitaIRC no
puede quedarse conectado en segundo plano. Con *"Evitar suspensión conectado"* no se duerme sola
mientras está abierta, y al volver de la suspensión se reconecta sola. Para no perder nada, usa un bouncer.

## Compilar

```sh
vdpm install libvita2d curl-mbedtls mbedtls zlib libpng
export VITASDK=/usr/local/vitasdk
./build.sh        # deja dist/VitaIRC.vpk
```

Más detalles (estructura del proyecto, traducciones, capturas con Vita3K) en el [README en inglés](README.md).

## Licencia

[MIT](LICENSE).
