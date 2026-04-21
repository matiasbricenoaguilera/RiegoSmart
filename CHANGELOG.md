# Changelog

## 2026-04-20
- **`index.html`**: reloj **hora local** a **1 Hz** (`#liveClock`, `setInterval` 1 s); **Última sync** solo al completar cada `fetchState` (~4 s a Supabase).

## 2026-04-17
- **Textos alineados con firmware (ventana lluvia ~6h)**: `index.html`, `schedule.html`, `glosario.html` — el panel ya no habla de “24h”; describe la misma lógica que `WEATHER_RAIN_FORECAST_HOURS` en la central (`RiegoSmart-ESP32OFICIAL`).
- **`index.html`**: envío único de **`utc_offset_sec`** a `system_state` desde la zona horaria del navegador (al primer `fetchState`).

## 2026-04-12
- **`graficos.html`**: el eje X usaba solo `HH:MM` y un `Set` que **fusionaba** varias filas en el mismo punto; lecturas nuevas (p. ej. tras reiniciar el satélite) podían **pisar** un índice viejo o no mostrarse a la derecha mientras el indicador “último dato hace Xm” sí era reciente. Ahora hay **una etiqueta por fila** (`dd/mm HH:mm:ss E#`), más filas traídas de Supabase y refresco del gráfico cada **20 s** (antes 2 min).
- **Zona horaria alineada con el sistema del usuario**: `index.html` y `schedule.html` envían `utc_offset_sec` a `system_state` (offset del navegador, misma hora que ves en el reloj del PC/móvil). La central ESP32 lo lee y aplica NTP con ese desfase. Requiere columna en Supabase: `ALTER TABLE system_state ADD COLUMN IF NOT EXISTS utc_offset_sec integer;` y firmware v13+.

## 2026-04-10
- Se agrega `login.html` con acceso para el dashboard cloud.
- Se agrega `auth.js` para proteger acceso a `index.html`, `schedule.html` y `graficos.html`.
- Se mantiene navegación por pestañas: Estado, Programación y Gráficos.
- Se deja el proyecto listo para despliegue automático en Netlify vía GitHub.
- Se corrige lógica de control cloud en `index.html`:
  - el botón ahora cambia a `APAGAR` cuando la bomba está activa.
  - la confirmación de comandos pendientes se valida con `p.active` (estado real de bomba) en vez de `p.manual`.
  - mejora de etiquetas de acción para evitar confusión en operación remota.
- Se agrega indicador en vivo en `graficos.html`:
  - muestra tiempo desde el último dato recibido en `sensor_data`
  - informa estación y humedad del último registro
  - cambia color cuando el dato está tardando más de lo esperado
- Migración de `schedule.html` a modo nube (sin llamadas HTTP locales):
  - deja de usar `http://192.168.1.85/api/*` para evitar error Mixed Content en Netlify
  - lectura/escritura de programación ahora se realiza en `system_state` vía Supabase
  - activación de programa y vista de sensores operan desde datos cloud
