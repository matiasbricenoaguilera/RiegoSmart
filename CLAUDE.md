# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Proyecto

RiegoSmart es un sistema de riego automatizado IoT con tres capas:

1. **Firmware ESP32** (Arduino C++) — `central/central.ino` y `satelite/satelite.ino`
2. **Dashboard web** (HTML/JS estático) — `index.html`, `schedule.html`, `graficos.html`, `glosario.html`
3. **Backend Supabase** — tablas PostgreSQL + Edge Function `sync-weather`

## Comandos frecuentes

### Firmware (Arduino IDE)
- Abrir `central/central.ino` o `satelite/satelite.ino` en Arduino IDE.
- Placa: **ESP32 Dev Module** (o equivalente).
- Velocidad serie: **115200** (logs y depuración).
- No hay Makefile ni CLI de build; la compilación y el flash se hacen desde el IDE.

### Backend Supabase (CLI)
```bash
# Vincular proyecto (una vez)
supabase link --project-ref ontyquiklrmebtzcvjyn

# Aplicar migración de weather_cache
supabase db push

# Configurar credenciales de Meteochile para la Edge Function
supabase secrets set METEOCHILE_USER='...' METEOCHILE_TOKEN='...' METEOCHILE_CODIGO='320041'

# Desplegar Edge Function
supabase functions deploy sync-weather
```

## Arquitectura

### Flujo de datos
```
Satélites (ESP-NOW) ──► Central ESP32 ──► Supabase REST API ──► Dashboard web
                                │
                      Edge Function sync-weather
                      (Meteochile DMC + RedMeteo + Open-Meteo)
                                │
                         weather_cache (tabla)
```

### Central (`central/central.ino`)
- Gateway principal: 5 zonas de riego controladas por MOSFET en pines `25, 26, 27, 32, 33` (HIGH = ON).
- Recibe paquetes ESP-NOW de satélites; promedia humedad por zona y la publica en `sensor_data`.
- Ciclo principal (`loop()`): procesa comandos remotos → push de estado → evaluación de horario → clima → ESP-NOW.
- **Supabase tables** consumidas:
  - `remote_commands` — comandos enviados desde el dashboard (encendido/apagado de zona).
  - `system_state` (id=1) — estado publicado por la central (humedad, bomba encendida, clima, etc.).
  - `sensor_data` — historial de lecturas de humedad (gráficos).
  - `weather_cache` — snapshot climático actualizado por la Edge Function.
- NVS (Preferences) almacena calibración de sensores, programas de riego y credenciales Meteochile sin recompilar.

### Satélite (`satelite/satelite.ino`)
- Nodo con deep sleep (`SLEEP_TIME_MIN = 30`): lee hasta 3 sensores capacitivos (ADC1), envía `SensorPacketV2` por ESP-NOW y vuelve a dormir.
- Detecta el canal WiFi del AP escaneando el SSID `WIFI_SSID_FOR_CHANNEL_SCAN` para alinear el canal ESP-NOW con la central.
- MAC de la central codificada en `centralAddress[]` — actualizar si se reemplaza el hardware.

### Dashboard web
- Páginas estáticas sin framework: cada HTML incluye `auth.js` (guard de localStorage) y el SDK de Supabase desde CDN.
- `index.html` — estado en vivo y control manual de zonas.
- `schedule.html` — 3 programas × 5 zonas; guarda en `system_state` vía Supabase y opcionalmente en la IP local de la ESP32.
- `graficos.html` — Chart.js 4 con eje temporal; 2 estaciones (E1/E2) como series independientes; hasta 240 puntos por estación, ventana 6/12/24/48 h o últimas N filas globales.
- `glosario.html` — referencia de parámetros agrícolas (Kc, ET₀, etc.).
- Auth: cliente-lado con `localStorage.setItem("rs_auth","1")`; credenciales hardcodeadas (usuario `matias`).

### Edge Function `sync-weather`
- No existe en el repo como archivo TypeScript visible (se deployó desde otro directorio); solo hay configuración en `supabase/config.toml` y la migración de la tabla en `supabase/migrations/`.
- Consulta Meteochile DMC → RedMeteo → Open-Meteo y hace upsert en `weather_cache` (fila única `id='default'`).

## Parámetros clave del firmware

| Define / constante | Descripción | Valor por defecto |
|---|---|---|
| `WEATHER_USE_SUPABASE_CACHE` | 1 = leer `weather_cache`; 0 = llamar APIs directas | 1 |
| `WEATHER_CACHE_MAX_AGE_SEC` | Máx. antigüedad aceptada de la caché climática | 4 h |
| `WEATHER_RAIN_FORECAST_HOURS` | Ventana horaria Open-Meteo para lluvia de decisión | 6 |
| `WEATHER_USE_REDMETEO` | 1 = RedMeteo + Open-Meteo; 0 = solo Open-Meteo | 1 |
| `SCHED_EVAL_WINDOW_MIN` | Minutos ± del horario programado en que se evalúa riego | 2 |
| `ENABLE_OLED` | 1 = pantalla OLED SDA=21 SCL=22 (U8g2) | 1 |
| `OLED_NORMAL_REFRESH_MS` | Intervalo refresco OLED normal | 1000 ms |
| `ESPNOW_FALLBACK_CHANNEL` | Canal ESP-NOW si WiFi aún no tiene canal válido | 11 |
| `MAX_WATERING_MS` | Corte de seguridad máximo por zona | 15 min |
| `SENSOR_PUSH_INTERVAL_MS` | Frecuencia de push de humedad a `sensor_data` | 30 s |

## Librerías Arduino requeridas

- `ArduinoJson` v7+
- `U8g2` (display OLED, solo si `ENABLE_OLED=1`)
- Nativas ESP32: `WiFi`, `WebServer`, `HTTPClient`, `esp_now`, `Preferences`

## Supabase project

- **Project ref**: `ontyquiklrmebtzcvjyn`
- **URL**: `https://ontyquiklrmebtzcvjyn.supabase.co`
- La `SUPABASE_ANON_KEY` está hardcodeada en `central.ino` y en cada HTML — es la clave pública `anon`.
- RLS en `weather_cache`: SELECT público para `anon`; escritura solo via `service_role` (Edge Function).
