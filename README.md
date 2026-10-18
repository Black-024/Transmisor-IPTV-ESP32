
 IPTV Remote ESP32
 Control remoto IPTV con pantalla tactil: eliges el canal en el ESP32 y
 todos los navegadores conectados lo reproducen al instante.
==============================================================================


1. QUE ES
---------
Un ESP32 con pantalla TFT tactil que lee una lista M3U guardada en su
memoria flash, te deja navegar categorias y canales con el dedo, y publica el
canal elegido por WebSocket a cualquier navegador abierto en
http://iptv.local (o en la IP que muestre la pantalla).

IMPORTANTE - alcance real del proyecto:
  * El ESP32 NO retransmite ni decodifica video. Solo envia al navegador el
    nombre, grupo y URL del canal.
  * El navegador descarga y reproduce el stream directamente desde Internet
    (con hls.js, o el reproductor nativo si el navegador soporta HLS).
  * Por eso el dispositivo que reproduce necesita Internet, y el stream debe
    permitir ser cargado desde un navegador (CORS).
  * No hay soporte DRM ni de protocolos distintos de HLS (.m3u8).


2. FUNCIONAMIENTO
-----------------
Arranque (main.cpp)
  1. Inicializa pantalla y tactil (calibracion si hace falta).
  2. Monta LittleFS y construye un indice de /playlist.m3u.
  3. Lanza dos tareas FreeRTOS: UI (core 1) y red (core 0).

Indice M3U (m3u_index.*)
  La lista NO se carga en RAM: se guarda solo (offset en archivo + id de
  grupo) por canal, unos 5 bytes. El nombre y la URL se leen del archivo bajo
  demanda. Limites: 8000 canales y 120 grupos (ver config.h).

Pantalla (ui.cpp)
  * Wi-Fi: usa credenciales guardadas en NVS; si fallan o no existen, escanea
    redes, las muestra en una lista y abre un teclado en pantalla.
  * Menu Categorias -> Canales. Toque para elegir, arrastre para desplazar,
    flechas laterales para paginar, boton "Atras" arriba a la derecha.
  * Al tocar un canal se envia el evento a la tarea de red. Si se tocan
    varios seguidos, solo cuenta el ultimo.

Red (net.cpp)
  * Servidor HTTP en el puerto 80 que sirve la web desde /www en LittleFS.
  * WebSocket en /ws: es SOLO de salida (ESP32 -> navegadores). Lo que envie
    un cliente se ignora, asi que el canal solo se cambia desde la pantalla.
  * GET /api/now devuelve el ultimo canal publicado (JSON).
  * Anuncia el nombre mDNS "iptv" (http://iptv.local).
  * Un navegador que entra tarde recibe automaticamente el canal actual.

Web (data/www/index.html)
  Visor de pantalla completa. Al abrirla hay que pulsar "Activar reproductor"
  una vez (los navegadores exigen un gesto del usuario para reproducir con
  audio). Se reconecta sola al WebSocket si se corta.


3. REQUISITOS
-------------
Hardware
  * ESP32-WROOM (placa "esp32dev"), con flash de 4 MB.
  * Pantalla tactil, para las pruebas del proyecto se uso y se recomienda una TFT 480x320 con controlador ST7796 y tactil resistivo XPT2046
    (comparte el bus SPI con la pantalla). Si no cuenta con esta pantalla especifica debera alterar el codigo para adaptarlo a sus componentes
  * Cable USB para cargar el firmware.
  * Encoder rotativo y botones: OPCIONALES (desactivados por defecto).

Conexiones (pines tal como estan en platformio.ini)
  TFT_MISO 12 | TFT_MOSI 13 | TFT_SCLK 14 | TFT_CS 15 | TFT_DC 2
  TFT_RST  -1 (sin pin) | TFT_BL 27 (backlight, activo en HIGH)
  TOUCH_CS 33
  Encoder (solo si ENCODER_ENABLED=1): A=32, B=33, OK=25, ATRAS=26
  OJO: A=32/B=33 chocan con TOUCH_CS=33. Si activas el encoder, cambia
  esos pines en config.h.

Red
  * Wi-Fi de 2,4 GHz (el ESP32 no ve redes de 5 GHz). No soporta WPA2-Enterprise.
  * El ESP32 y los navegadores deben estar en la misma red.
  * Los navegadores necesitan acceso a Internet (reproducir streams y cargar
    hls.js desde cdn.jsdelivr.net).

Software
  * Visual Studio Code + extension PlatformIO IDE, o PlatformIO Core (CLI).
  * Python 3 (lo instala PlatformIO).


4. DEPENDENCIAS
---------------
Se descargan solas la primera vez que compilas (ver platformio.ini).

  Plataforma : espressif32 ~6.9.0 (framework Arduino)
  Librerias  : ESP32Async/AsyncTCP        ^3.3.2
               ESP32Async/ESPAsyncWebServer ^3.7.0
               bblanchon/ArduinoJson       ^7.2.0
               bodmer/TFT_eSPI             ^2.5.43
               madhephaestus/ESP32Encoder  ^0.11.7
  Navegador  : hls.js 1.5.17 (por CDN, no ocupa flash del ESP32)

La pantalla se configura por build_flags en platformio.ini; no hay que
tocar User_Setup.h de TFT_eSPI.


5. PUESTA EN MARCHA
-------------------
1) Clonar y abrir
     git clone https://github.com/TU_USUARIO/IPTV-Remote-ESP32.git
     cd IPTV-Remote-ESP32
   Abre la carpeta en VS Code con PlatformIO.

2) Poner tu lista de canales
   Reemplaza data/playlist.m3u por tu lista M3U. Debe llamarse exactamente
   playlist.m3u. El archivo incluido es solo un ejemplo sin URLs.
   Formato esperado: lineas #EXTINF con group-title="..." y, en la linea
   siguiente, la URL del stream.

3) Compilar y cargar el firmware
     pio run -t upload
   (o en VS Code: PlatformIO > esp32dev > General > Upload)

4) Cargar los archivos de LittleFS (web + playlist)
     pio run -t uploadfs
   (o PlatformIO > esp32dev > Platform > Upload Filesystem Image)
   Hay que repetirlo cada vez que cambies data/playlist.m3u o la web.
   El firmware y el sistema de archivos se cargan por separado.

5) Abrir el monitor serie
     pio device monitor
   Velocidad 115200. Veras el estado del arranque y la IP.

6) Primer arranque
   * Aparece "Mantene la pantalla tocada para recalibrar": la primera vez
     se calibra sola; toca el centro de cada cruz y manten hasta que se
     ponga verde. La calibracion se guarda. Para repetirla, manten la
     pantalla tocada durante ese mensaje en el arranque.
   * Elige tu red Wi-Fi y escribe la clave con el teclado tactil.
     Alternativa por Serial: escribe  wifi NOMBRE_RED|CLAVE  y Enter
     (red abierta: wifi NOMBRE_RED| ). El comando  status  muestra el estado.
   * Al conectar, la pantalla muestra la IP.

7) Reproducir
   En cualquier dispositivo de la misma red abre http://iptv.local (o la IP),
   pulsa "Activar reproductor" y elige canales desde la pantalla del ESP32.
   Si iptv.local no resuelve (Windows/Android antiguos), usa la IP.


6. CONFIGURACION
----------------
src/config.h
  ENCODER_ENABLED  0/1 activa encoder y botones fisicos
  TOUCH_Z_MIN      presion minima para detectar toque
  DRAG_PX          movimiento minimo para considerarlo arrastre
  PLAYLIST_PATH    ruta de la lista en LittleFS
  MAX_CHANNELS / MAX_GROUPS   limites del indice
  HOSTNAME         nombre mDNS (http://<HOSTNAME>.local)

src/wifi_secrets.h
  Credenciales Wi-Fi de respaldo compiladas en el firmware (opcional).
  Se prueban despues de las guardadas en NVS y no se guardan en NVS.
  Dejalas vacias antes de subir el proyecto a GitHub.

platformio.ini
  Pines y parametros de la pantalla (build_flags).
  Si los colores salen invertidos agrega:  -DTFT_INVERSION_ON
  Si rojo y azul estan cambiados agrega:   -DTFT_RGB_ORDER=TFT_BGR
  Si tu pantalla requiere el puerto HSPI agrega: -DUSE_HSPI_PORT

Datos guardados en la flash (NVS): Wi-Fi (espacio "wifi") y calibracion del
tactil (espacio "touch"). El firmware nunca los borra por su cuenta.


7. ESTRUCTURA DEL PROYECTO
--------------------------
  platformio.ini          configuracion de compilacion y pines
  src/main.cpp            arranque y tareas
  src/ui.cpp, ui.h        pantalla, tactil, Wi-Fi, menus
  src/net.cpp, net.h      servidor HTTP, WebSocket, mDNS
  src/m3u_index.cpp, .h   indice de la lista M3U
  src/events.h            cola y eventos entre tareas
  src/config.h            constantes ajustables
  src/wifi_secrets.h      credenciales de respaldo (vacias)
  data/playlist.m3u       lista de ejemplo
  data/www/index.html     visor web


8. PROBLEMAS FRECUENTES
-----------------------
  "Falta /playlist.m3u"   No se encuentra el archivo m3u necesario, pon un archivo llamado "playlist.m3u" en la subcarpeta data
  No ve mi Wi-Fi          Debe ser de 2,4 GHz.
  Pantalla en blanco      Revisa pines y TFT_BL; prueba los flags de la
                          seccion 6.
  Tactil desplazado       Recalibra manteniendo la pantalla tocada al arrancar.
  Canal en negro/error    El servidor del canal puede estar caido, tener
                          geobloqueo o no enviar cabeceras CORS. Prueba otro
                          canal o un navegador con HLS nativo (Safari).
  Pantalla web no carga   Sin Internet en el navegador, hls.js no se descarga.


9. NOTAS LEGALES Y DE USO
-------------------------
  * No se incluye ninguna lista de canales real. Usa solo contenido que
    tengas derecho a ver.
  * Este proyecto no hospeda ni distribuye contenido: solo reenvia al
    navegador la URL que tu mismo cargas en la lista.
  * Falta elegir una licencia: agrega un archivo LICENSE (por ejemplo MIT)
    antes de publicar si quieres permitir su reutilizacion.
