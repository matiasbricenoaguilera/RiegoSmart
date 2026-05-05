# RiegoSmart-ESP32

Firmware base para la ESP32 central de riego.

## Incluye
- Control de 5 bombas con MOSFET (activo en HIGH): pines `25, 26, 27, 32, 33`.
- Arranque seguro en OFF.
- Corte automatico de seguridad a 15 minutos maximo por zona.
- Dashboard local liviano por IP con login.
- Receptor ESP-NOW canal 11 (solo recepcion).
- Conversion de humedad RAW a porcentaje con calibracion por zona.
- Sincronizacion base con Supabase.

## Estructura
- `central/central.ino`: sketch principal para Arduino IDE (gateway).
- `satelite/satelite.ino`: sketch satelite ESP-NOW con deep sleep.
- `CHANGELOG.md`: historial de cambios.

## Librerias Arduino necesarias
- `ArduinoJson` (v7 o compatible)
- Librerias nativas ESP32: `WiFi`, `WebServer`, `HTTPClient`, `esp_now`, `Preferences`

## Horario de riego y OLED (central)
- **`SCHED_EVAL_WINDOW_MIN`**: minutos antes y después del `HH:MM` en que se evalúa el riego automático (por defecto **2**; se puede redefinir antes de incluir el sketch).
- **OLED**: refresco por defecto **1 s** (`OLED_NORMAL_REFRESH_MS`, `OLED_SLEEP_CLOCK_REFRESH_MS`).

## Pasos de uso
1. Abrir `central/central.ino` en Arduino IDE.
2. Configurar:
   - `WIFI_SSID`, `WIFI_PASS`
   - `SUPABASE_ANON_KEY`
   - Endpoints Supabase (`SUPABASE_*_ENDPOINT`) segun tus tablas reales.
3. Compilar para tu ESP32 y subir.
4. Abrir Serial Monitor a `115200` para ver IP local y logs.

## Clima: caché en Supabase (Meteochile + Open-Meteo en servidor)

La central puede leer el clima desde la tabla **`weather_cache`** (actualizada por una Edge Function), en lugar de llamar directamente a Meteochile/RedMeteo en cada refresco. Así se reduce carga y fallos TLS en la ESP32.

### Firmware (`central.ino`)
- **`WEATHER_USE_SUPABASE_CACHE`**: `1` (por defecto) usa la caché; ponlo en **`0`** para desactivarla y volver solo a APIs directas.
- **`WEATHER_CACHE_MAX_AGE_SEC`**: antigüedad máxima aceptada de la fila (por defecto 4 h). Si NTP no está sincronizado, no se usa la caché.
- **`WEATHER_RAIN_FORECAST_HOURS`** (por defecto **6**): horas de pronóstico Open-Meteo que se suman para decidir cancelación por lluvia (ya no se usa el máximo de acumulados diarios hoy/mañana). Con Meteochile/RedMeteo, el valor de decisión es **max(observación estación, suma horaria modelo)**.

### Backend (Supabase CLI)
1. Vincular proyecto (si aún no): `supabase link --project-ref <tu-project-ref>`.
2. Aplicar migración: `supabase db push` (o ejecutar el SQL de `supabase/migrations/20260419120000_weather_cache.sql` en el SQL Editor).
3. Secretos de la función (servidor Meteochile + código EMA):
   ```bash
   supabase secrets set METEOCHILE_USER='...' METEOCHILE_TOKEN='...' METEOCHILE_CODIGO='320041'
   ```
   Opcionales: `SITE_LAT`, `SITE_LON`; **`WEATHER_RAIN_FORECAST_HOURS`** (entero 1–48, default 6) alinea la caché con el firmware.
4. Desplegar: `supabase functions deploy sync-weather`.

### Actualizar la caché de forma periódica
La función expone `POST https://<project-ref>.supabase.co/functions/v1/sync-weather`. Con **`verify_jwt = true`**, incluye cabeceras típicas de Supabase:

- `Authorization: Bearer <SUPABASE_ANON_KEY>`
- `apikey: <SUPABASE_ANON_KEY>`

Programa una llamada cada 1–3 h (cron en un VPS, GitHub Actions, **pg_cron** + `http`, etc.). Sin ejecuciones periódicas la fila queda vacía o caduca y la ESP32 usará el fallback a APIs directas.

## Notas de interoperabilidad ESP-NOW
- Canal fijo recomendado: `11` en central y satelites.
- La central acepta payload legado de 12 bytes (`stationID,sensorID,rawValue`) y payload V2 compacto.
- Cada satelite puede enviar hasta 3 sensores por estacion; la central promedia y publica humedad por zona.
