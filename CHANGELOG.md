# Changelog

## 2026-04-20 (central – ventana riego ±2 min y OLED 1 Hz)
- **`SCHED_EVAL_WINDOW_MIN`**: por defecto **2** (antes ±5 min): evaluación de riego en **T−2..T+2** respecto al horario programado; `nextEvalHHMM` y textos de `SIN_LECTURA` usan el mismo valor.
- **OLED**: refresco **1 s** en modo normal (`OLED_NORMAL_REFRESH_MS`) y en salvapantallas/reloj (`OLED_SLEEP_CLOCK_REFRESH_MS`) para ver el **segundo** de forma continua en la línea de hora.

## 2026-04-17 (central + sync-weather – lluvia para riego: ventana horaria 6 h)
- **Problema**: cancelación por **acumulado diario** Open-Meteo (hoy/mañana) podía saltar riegos por lluvia **muy lejana en el tiempo**.
- **`WEATHER_RAIN_FORECAST_HOURS`** (default **6**): la lluvia usada en `rainCancelMm` y reducción de mm sale de la **suma horaria** Open-Meteo en esa ventana.
- **Modo solo Open-Meteo**: una petición con `hourly=precipitation` + `daily` ET₀/T.
- **Meteochile / RedMeteo**: se conserva la observación de estación y se mezcla con **`max(observación, suma N h)`** vía `mergeStationRainWithOpenMeteoHourly`.
- **Edge Function `sync-weather`**: misma lógica; opcional `WEATHER_RAIN_FORECAST_HOURS` en secretos.
- **README**: documentado el define y el secreto.

## 2026-04-19 (Supabase + central – clima en caché `weather_cache`)
- **Migración** `supabase/migrations/20260419120000_weather_cache.sql`: tabla `public.weather_cache` (fila `id=default`), RLS con **SELECT** para `anon`/`authenticated`.
- **Edge Function** `supabase/functions/sync-weather`: obtiene precipitación Meteochile (`getDatosRecientesEma`) y ET₀/temps Open-Meteo; hace **upsert** en `weather_cache` con `updated_unix`.
- **`config.toml`**: `[functions.sync-weather]` con `verify_jwt = true`.
- **`central.ino`**: `WEATHER_USE_SUPABASE_CACHE`, `fetchWeatherFromSupabaseCache()` vía `supabaseGet`; `fetchWeather()` intenta caché primero y solo entonces Meteochile/RedMeteo/Open-Meteo.
- **README**: pasos `db push`, `secrets set`, `functions deploy` y programación periódica del POST a `sync-weather`.

## 2026-04-19 (central – clima: IncompleteInput Meteochile + RedMeteo 0 bytes)
- **`httpWeatherConfigure`**: timeouts 45s, **`Accept-Encoding: identity`**, `Connection: close`, User-Agent, **`HTTP/1.0`** para reducir cortes TLS y gzip raro en ESP32.
- **Meteochile**: `DynamicJsonDocument` **49152**; log **len + heap** si falla; mensaje explícito si **`IncompleteInput`** (JSON truncado por red/TLS).
- **RedMeteo**: **reintento** si cuerpo vacío con HTTP 200; **delay 300 ms** tras intento Meteochile antes de RedMeteo (~70 KB JSON).

## 2026-04-19 (central – clima: lectura completa HTTPS + sin HTTP/1.0)
- **Causa**: `getString()` **no completaba** cuerpos grandes → **IncompleteInput** con len~64k y JSON cortado; **HTTP/1.0** hacía **RedMeteo HTTP 200 con 0 bytes** (LiteSpeed).
- **`httpReadFullBody`**: si hay **Content-Length**, lee del **stream** hasta leer todos los bytes.
- **`httpWeatherConfigure`**: quitados **`useHTTP10`** y **`Connection: close`**; timeouts **60s / 25s**.
- **Meteochile**: `DynamicJsonDocument` tamaño según **payload** (hasta **131072**).

## 2026-04-19 (central – Meteochile DMC opcional)
- **`METEOCHILE_CODIGO_NACIONAL`**: **320041** (EMA indicada por el usuario; sustituye el ejemplo 330020).
- **`getDatosRecientesEma/{codigoNacional}`** ([Servicios climáticos DMC](https://climatologia.meteochile.gob.cl/application/documentacion/getDocumento/0)): si **`METEOCHILE_USER`** y **`METEOCHILE_TOKEN`** no están vacíos y **`METEOCHILE_CODIGO_NACIONAL` > 0**, se intenta **antes** que RedMeteo; ET₀/T siguen con Open-Meteo.
- Credenciales opcionales en NVS namespace **`meteoch`**: `user`, `token`, `cod` (sobrescriben las constantes).
- Codificación URL de usuario/token; detección de respuesta **bloqueada** por DMC; parseo flexible de precipitación en JSON.

## 2026-04-19 (central – fix RedMeteo + Open-Meteo JSON en ESP32)
- **RedMeteo**: `deserializeJson` desde **`http.getStream()`** con HTTPS y JSON ~70 KB dejaba **lista vacía** o datos incompletos; ahora se usa **`getString()`**, timeouts más largos, filtro en doc más grande y **fallback por texto** (scan de `lluviadiaria` para `REDMETEO_STATION_ID`) si el filtro sigue vacío.
- **Open-Meteo**: mismo cambio (**`getString()`** en lugar de stream) para evitar **`Error JSON Open-Meteo`**; logs muestran código de error ArduinoJson y longitud del cuerpo.
- Mensaje **ESP-NOW** acortado para reducir saturación del puerto serie.

## 2026-04-19 (central – fix informe clima "Sin datos")
- **`fetchWeather` antes de cmd/push/sched (CAPA 0)**: si no, el bucle HTTP casi nunca llegaba al clima y **`wx_fetched`** quedaba **false** → dashboard "Sin datos".
- **`wx_fetched = true` solo si** Open-Meteo completo o RedMeteo lluvia tuvieron éxito; reintento cada **60 s** si falla (`lastWxAttemptAt`).
- Si **falla un refresco** con datos ya válidos, se actualiza `wx_lastFetchAt` para no llamar HTTP en cada `loop`.
- `fetchOpenMeteoDailyEt0Temps` / `fetchWeatherOpenMeteoOnly` devuelven **bool**.

## 2026-04-19 (central – clima RedMeteo 3.0 + Open-Meteo)
- **RedMeteo**: lectura de **`https://redmeteo.cl/last-data.json`** (feed público del mapa v3.0). Filtro JSON para reducir RAM; **lluvia** = **`lluviadiaria`** (mm acumulados **hoy** en la estación, observación real).
- **`REDMETEO_STATION_ID`**: por defecto **`RMCL0022`** (Villa Alemana – Pob. Guillermo Marconi); cadena vacía = estación **más cercana** a `SITE_LAT` / `SITE_LON`.
- **Open-Meteo** (segunda petición): solo **ET₀** y **T max/min** diarios (`fetchOpenMeteoDailyEt0Temps`), no sustituye el pronóstico de lluvia cuando RedMeteo OK.
- **`WEATHER_USE_REDMETEO`**: `1` = modo mixto; `0` = solo Open-Meteo (`fetchWeatherOpenMeteoOnly`). Si RedMeteo falla → fallback Open-Meteo completo.
- Requiere **ArduinoJson** con `DeserializationOption::Filter` (6.15+ aprox.).

## 2026-04-18 (central – clima Open-Meteo: lluvia 24 h + modelo)
- **Lluvia para riego**: ya no se usa `max(precip_hoy, precip_mañana)` (diario); se usa la **suma de precipitación horaria de las próximas 24 h**, más alineada con “lluvia esperada en la ventana” y menos propensa a cancelar solo por el día siguiente.
- **`OPEN_METEO_MODEL`**: por defecto **`gfs_global`** (cambiable; cadena vacía = `best_match` del servidor). Otros valores documentados en Open-Meteo: p. ej. `icon_seamless`, `ecmwf_ifs025`.
- Buffer JSON de `fetchWeather` subido a **16384** por arrays `hourly`. Log `[WX]` muestra `24h=…`, daily hoy/mañana y modelo.

## 2026-04-17 (wifi_test – sketch solo WiFi)
- Nuevo sketch **`wifi_test/wifi_test.ino`**: conecta al mismo SSID/contraseña que la central, imprime **éxito** por Serial (115200), IP, canal y RSSI; timeout 30 s y reintento en `loop()` si cae el enlace.
- **`wifiStaBeginClean()`**: `persistent(false)`, `disconnect(true,true)`, `WIFI_OFF` → `WIFI_STA`, delays — reduce **`E wifi_init: Failed to deinit Wi-Fi (0x3001)`** y estado anómalo al conectar.

## 2026-04-17 (central – ventana de riego T-5..T+5 y SIN_LECTURA sin bloquear el dia)
- **Ventana de decision**: de **5 min antes** del `HH:MM` de la zona hasta **5 min despues** inclusive: `inEvalWindow = (minsToSched <= 5) || (minsFromSched <= 5)`.
- **SIN_LECTURA**: ya **no** asigna `lastRunDay`/`lastRunSched`; se marca `sinLecturaPending` y se **reintenta** cada segundo dentro de la ventana. Al **salir** de la ventana con pendiente, se **consume** el disparo con motivo `SIN_LECTURA` y texto de cierre de ventana.
- **Consumen disparo** (como antes): **HUM_OK**, **riegos** (`HUM_BAJA`), **LLUVIA_PREVISTA**; en todos se limpia `sinLecturaPending`.
- Campos nuevos en `ZoneDecision`: `wasInEvalWindow`, `sinLecturaPending`. Zona **deshabilitada** limpia pendiente y actualiza `wasInEvalWindow`. Log `[SCHED-DBG]` incluye `win=SI|NO`.

## 2026-04-17 (central + satélite – canal ESP-NOW alineado al AP 2,4 GHz)
- **Causa vista en campo**: router en **canal 1**, firmware fijo en **11** → la central no recibía paquetes (`Canal WiFi actual=1` vs emisor en 11).
- **Central** (`setupEspNow`): usa **`WiFi.channel()`** del STA (1–13) para `esp_wifi_set_channel`; si aún no es válido, **`ESPNOW_FALLBACK_CHANNEL`** (11). Log: `ESP-NOW RX listo en canal N`.
- **Satélite**: escaneo del SSID **`WIFI_SSID_FOR_CHANNEL_SCAN`** (por defecto `MYM`, misma red que la central); elige el BSSID con mejor **RSSI** y usa ese **canal** para ESP-NOW y `peerInfo.channel`. Si no hay SSID o el scan falla → fallback 11. Ajustar el literal del SSID en `satelite.ino` si cambias el nombre del WiFi.

## 2026-04-12 (central – fix “nunca recibido” con satélite sí activo)
- **Causa**: si `assignedStationID` quedaba **0 o fuera de 1..5** en Flash (corrupción o clave vieja), la expresión `cfg.stationID - 1` en **uint8_t** desbordaba y el firmware leía **fuera** de `lastStationSeenAt[]` → `sensor_age_s` pasaba a **65535** (“nunca recibido”) aunque el satélite 1 enviara datos.
- **Fix**: `stationIdxFromSchedule()` acota siempre a **0..4**; `effectiveLastRxMs()` usa el máximo entre `lastStationSeenAt` y las tres sondas; `loadSchedules()` **corrige y persiste** `stationID` inválido (log `[SCHED] WARN`).
- `predictiveCheck` y `pushStatusToCloud` usan el índice seguro para humedad y edad del sensor.

## 2026-04-12 (central – limpiar `reason` SIN_LECTURA obsoleto)
- **`decisions[].reason`**: si en la ventana de riego no había humedad fresca se guardaba `SIN_LECTURA`; al volver a tener `humFresh` **fuera** de esa ventana el motivo no se actualizaba y el dashboard seguía mostrando “sin lectura” aunque `hum_valid` ya fuera verdadero. Ahora se borra `SIN_LECTURA` cuando la humedad vuelve a ser válida.

## 2026-04-12 (central – sensor_data solo con muestra ESP-NOW nueva)
- **`sensor_data` (gráfico)**: ya no se inserta fila por el recálculo periódico de humedad. Solo la **estación que acaba de recibir un paquete** marca `sensorDataPushPending[st]` en `applySensorSample`; el POST a Supabase envía **únicamente esas estaciones** tras el intervalo `SENSOR_PUSH_INTERVAL_MS`. El gráfico refleja **ciclos de medición reales**, no repeticiones del último valor.

## 2026-04-12 (central – lecturas frescas por sonda y avisos de fallo)
- Solo entran en el **promedio de humedad** de una estación las sondas cuyo último paquete tiene antigüedad ≤ **`SENSOR_SAMPLE_MAX_AGE_MS`** (40 min por defecto, mayor que un ciclo típico de deep sleep de 30 min en el satélite). Cada ~**5 s** se **vuelve a calcular** la media para descartar datos obsoletos sin esperar un paquete nuevo.
- **`moisturePctValid[]`**: si ninguna sonda aporta lectura fresca, la humedad no es válida (`hum=0` en JSON, panel y OLED muestran `--` donde corresponde). El **riego automático** en ventana horaria **no arranca** si `hum_valid` es falso (`reason`: `SIN_LECTURA`).
- **OLED**: si hay al menos una sonda con dato fresco en una estación y otra no (nunca recibió o excedió la antigüedad), aviso `! E# S# sin dato` u `obsoleto`.
- **Supabase `decisions`**: `hum_valid` y array **`sensor_age_s`** (segundos desde último paquete por sonda, `65535` = nunca). **`/api/sensors/list`**: por sonda `fresh` y `age_s`.
- **Serial** en recepción: `valid=si|no`.

## 2026-04-12 (central – calibración raw por sensor)
- **`central.ino`**: cada estación (satélite) tiene **3 sensores**; la calibración ya no es un solo par seco/mojado por zona sino **`calDry[estación][0..2]`** y **`calWet[estación][0..2]`** (raw **seco = máximo**, **mojado = mínimo**; validación `seco > mojado > 0`).
- **Preferences** (`riego`): claves **`d{estación}_{sensor}`** y **`w{estación}_{sensor}`** (ej. `d0_0` … `w4_2`). **Migración**: si no existe clave por sensor, se reutiliza el legado **`dry{i}` / `wet{i}`** para los tres sensores de esa estación.
- **`applySensorSample`**: el % de humedad de la estación es la **media aritmética** de los `rawToPercent` de cada sonda que ya haya enviado al menos una lectura (índice `sensorSeen`).
- **`/cal`**: formulario con 5 bloques × 3 sensores; POST con campos `d{i}_{j}` / `w{i}_{j}`.
- **`GET /api/sensors/list`**: cada elemento tiene **`sensors`**: array de 3 objetos con `sensorID`, `min` (mojado), `max` (seco), `raw` (antes un solo `sensorID`/`min`/`max`/`raw` por estación).

## 2026-04-12 (satélite – lectura ADC alineada con calibrador)
- **`satelite.ino`**: mediana de **21** muestras + `analogReadResolution(12)` + **ADC_11db** (igual criterio que `calibrador_sensor`). La central **no** lee analógico: solo recibe `raw` por ESP-NOW; estabilizar en el nodo que tiene el sensor.

## 2026-04-12 (v14 – OLED 128×64 I2C + sleep)
- **Pantalla** SSD1306 128×64 por I2C: **SDA = GPIO 21**, **SCL = GPIO 22**, 3.3 V y GND. Librería **U8g2** (olikraus), constructor **1 página** (`NONAME_1_HW_I2C`) para **poca RAM**.
- **Modo normal** (~cada 2 s): hora/fecha, WiFi, estado de riego (`Zx ON (Auto|Local|Nube)` o todas OFF), próximo evento según `decisions[]` del programa activo, humedad de la estación más seca entre zonas habilitadas, línea de **aviso** si falla WiFi/NTP/hora o sensor de zona habilitada sin dato o >15 min sin muestra (tras 2 min de arranque).
- **Sleep OLED** tras **120 s** sin bomba ON y sin aviso crítico: solo **hora grande** que **rebota**, fecha pequeña abajo fija; **refresco cada 60 s** (y un paso de rebote por refresco). Cualquier **encendido de zona** o **aviso** vuelve al modo normal.
- **Origen del riego**: `ZoneState.source` — `ZSRC_AUTO` (horario), `ZSRC_LOCAL` (panel web `/api`), `ZSRC_CLOUD` (`remote_commands`). `ENABLE_OLED` en 0 desactiva todo el bloque OLED si compilás sin pantalla.
- **OLED**: modo normal con `u8g2_font_5x7_tf` y líneas cada 8 px para que la última fila (aviso) entre en 64 px de alto; fecha en sleep también 5×7.

## 2026-04-12 (v16 – sensor_data al gráfico en ~30–60 s)
- **Problema**: las lecturas llegaban por ESP-NOW a la RAM, pero `sensor_data` solo se insertaba cada **30 min** (`SENSOR_PUSH_INTERVAL_MS`), así que el gráfico / historial no reflejaba reinicios del satélite ni cambios recientes.
- **Cambio**: intervalo de POST a `sensor_data` = **30 s** (alineado con `pushStatusToCloud`). Log `[CLOUD] sensor_data E# hum=.. OK|FAIL`.

## 2026-04-12 (v15 – ESP-NOW antes que el satélite duerma)
- **Problema**: `setupEspNow()` solo corría tras **9 s** de arranque; el satélite manda 3 paquetes en los primeros segundos y entra en **deep sleep 30 min** → la central no recibía nada y `hum` quedaba en 0.
- **Fix**: registrar ESP-NOW en `maintainConnectivity()` en cuanto `WiFi.status() == WL_CONNECTED`, sin esperar `STARTUP_STAGGER_MS`.

## 2026-04-12 (v13 – Hora local = la de tu PC / navegador)
- **Nueva columna Supabase** `system_state.utc_offset_sec` (entero): segundos al **este** de UTC para la hora local, calculado en el dashboard como `-Math.round(new Date().getTimezoneOffset() * 60)` (incluye horario de verano vigente en el navegador).
- **Firmware**: si existe `utc_offset_sec` en el GET de programación, se aplica `configTime(sec, 0, ntp…)` y se persiste en Preferences (`tm/utc_os`) para el próximo arranque. Sin valor guardado en flash ni en nube, se mantiene el fallback Chile (`configTzTime CLT4CLST3…`).
- **Dashboard** (`schedule.html`, `index.html`): al cargar la página o al guardar programa / activar vigente se envía el offset del navegador. **CloudDashboard** `main.js`: al guardar programa activo también.
- **SQL** (ejecutar una vez en Supabase): `ALTER TABLE system_state ADD COLUMN IF NOT EXISTS utc_offset_sec integer;`

## 2026-04-12 (v12 – Riego a la hora programada: pull inicial + JSON + lluvia)
- **Bug**: la primera sincronización de `pumps`/`programs` desde Supabase solo ocurría cuando `now - lastSchedPullAt >= 5 min`. Con `lastSchedPullAt = 0` eso equivale a **esperar 5 minutos de uptime** antes del primer `pullScheduleFromCloud`. Hasta entonces `predictiveCheck` usaba solo horarios en Flash (`loadSchedules`), a menudo desactualizados respecto al dashboard → la bomba no coincidía con la hora guardada en la nube.
- **Fix**: primer pull en cuanto el loop cloud está activo (`lastSchedPullAt == 0`), luego cada 5 minutos como antes.
- **Bug**: `DynamicJsonDocument` de 4096 bytes en `pullScheduleFromCloud` era insuficiente para 5 zonas × 3 programas → `deserializeJson` fallaba (NoMemory), se logueaba JSON inválido y **nunca** se aplicaban horarios desde Supabase. Buffer aumentado a **14336** bytes; errores de deserialización ahora imprimen `c_str()` y tamaño del payload.
- **Bug**: si `rainCancelMm` era **0** (o faltaba y quedaba 0), la condición `wx_rain24h >= cfg.rainCancelMm` era casi siempre cierta y el riego se cancelaba como “lluvia prevista”. Ahora solo se cancela si `rainCancelMm > 0` y la lluvia prevista supera ese umbral (misma lógica en el texto de estado).
- Logs extra: `pumps` null, zona sin `programs`, y advertencias más claras.

## 2026-04-12 (v11 – Fix real: pumps eliminado del push del ESP32)
- **Bug confirmado**: el StrReplace de v10 no eliminó correctamente el array `pumps` de `pushStatusToCloud`. La línea `JsonArray pumps = doc.createNestedArray("pumps")` y el loop que llenaba los objetos sin `programs` permanecieron. Resultado: Supabase seguía recibiendo `pumps:[{active,remaining_s...}]` cada 30s, reemplazando la columna entera y borrando los programas guardados.
- **Fix aplicado**: reescritura completa y limpia de `pushStatusToCloud`. El PATCH ahora contiene ÚNICAMENTE `active_program`, `decisions` y `weather`. El array `pumps` fue eliminado definitivamente. Se verificó con grep que no existe ningún `createNestedArray("pumps")` en la función.
- **Regla de oro documentada en el código**: `decisions` = solo ESP32 escribe; `pumps` = solo dashboard escribe.

## 2026-04-12 (v10 – Fix definitivo separación config/estado operativo)
- **Raíz del problema**: Supabase reemplaza la columna `pumps` completa en cada PATCH. Aunque el ESP32 no enviara `programs`, al enviar `pumps:[{active,remaining_s...}]` sin programas, borraba la configuración guardada por el dashboard.
- **Solución arquitectónica**: separación estricta de columnas:
  - `decisions` (columna) → **solo escribe el ESP32**. Ahora incluye `active`, `manual`, `remaining_s`, `total_s`, `sensor_age_s`, `enabled` además de los campos agronómicos.
  - `pumps` (columna) → **solo escribe el dashboard**. El ESP32 nunca la toca.
- **Firmware `central.ino`**: `pushStatusToCloud()` ya no incluye el array `pumps` en ningún momento. Todos los campos operativos se añadieron a cada objeto del array `decisions`.
- **Dashboard `index.html`**: `renderPumps()` ahora recibe solo `decisions[]` (un array). Lee `d.active`, `d.remaining_s`, `d.total_s`, `d.manual` del mismo objeto decision. La referencia a `lastPumps` queda reservada solo para compatibilidad futura.

## 2026-04-12 (v9 – Fix crítico: programación se pierde al guardar)
- **Bug raíz**: `pushStatusToCloud()` incluía el array `programs` en cada PATCH cada 30s. Si el usuario guardaba cambios en el dashboard y el ESP32 aún no había hecho `pullScheduleFromCloud` (intervalo 5 min), sobreescribía los cambios con los valores viejos en memoria. Además, los nuevos campos (`enabled`, `rainCancelMm`, `maxDecisionMin`) no estaban en el push, borrándolos en cada ciclo.
- **Fix firmware**: eliminado el bloque `programs` de `pushStatusToCloud()`. El ESP32 solo sube estado operativo (`active`, `remaining_s`, `manual`). Los programas son datos de configuración que únicamente escribe el dashboard.
- **Fix `schedule.html`**: `saveAll()` ahora hace un fetch fresco de Supabase antes de guardar (no usa el caché `systemState` que puede ser obsoleto), evitando race conditions con datos concurrentes.
- Reducido el tamaño del `static DynamicJsonDocument` en `pushStatusToCloud` de 5120 a 3072 bytes (ya no lleva programs).

## 2026-04-12 (v8 – Parámetros por zona, heap estable, historial de riegos)

### Firmware (`central.ino`)
- **`PumpSchedule` ampliado con 3 nuevos campos por zona**:
  - `rainCancelMm` (float, default 5.0): mm de lluvia prevista que cancela el riego. Antes era una constante global; ahora cada zona puede tener su propio umbral (ej. jardín=2mm, huerto=8mm).
  - `maxDecisionMin` (uint8, default 10): tope de minutos de riego calculado. Antes constante global `MAX_DECISION_MIN`.
  - `enabled` (bool, default true): deshabilita completamente una zona sin tocar su umbral ni horario.
- **`predictiveCheck`**: salta zonas con `enabled=false` (apaga si estaba encendida), usa `cfg.rainCancelMm` y `cfg.maxDecisionMin` en lugar de constantes.
- **`pullScheduleFromCloud`**: parsea los nuevos campos desde Supabase. `saveSchedules()` solo se llama si hubo **algún cambio real**, protegiendo los ~100.000 ciclos de escritura NOR de la Flash.
- **`pushStatusToCloud`**: ahora incluye `sensor_age_s` (segundos desde último paquete ESP-NOW de esa estación, 0xFFFF si nunca se recibió) y `enabled` en cada zona.
- **Heap estable — docs JSON estáticos**: `processCloudCommands`, `pullScheduleFromCloud` y `pushStatusToCloud` usan `static DynamicJsonDocument` con `.clear()` en lugar de crear/destruir el documento en cada llamada. El bloque de heap se asigna una sola vez, eliminando la fragmentación acumulativa.
- **`pushIrrigationLog()`**: nueva función que hace POST a la tabla `irrigation_log` en Supabase al completar una auditoría (+45 min) o al cancelar por lluvia. Registra zona, duración, litros, humedad inicio/fin, variación, razón, lluvia mm y ET₀.
- **`lastStationSeenAt[ZONE_COUNT]`**: nuevo array que registra el `millis()` de la última recepción por estación. Alimentado desde `applySensorSample`.

### Dashboard (`schedule.html`)
- **Zona activa / deshabilitada**: nuevo checkbox "Zona activa" en cada card. La tarjeta se atenúa visualmente cuando está deshabilitada.
- **Lluvia cancela (mm)**: nuevo campo en parámetros agronómicos.
- **Máx. riego (min)**: nuevo campo en parámetros agronómicos.
- Los nuevos campos se guardan correctamente en Supabase (`saveAll()`).

### Dashboard (`index.html`)
- **Badge "Sin señal"**: si `sensor_age_s > 3600` o nunca se ha recibido, aparece una alerta roja en la tarjeta de la zona.
- **Badge "Zona deshabilitada"**: si `enabled=false`, la tarjeta se atenúa y muestra un indicador gris.
- **Sección "Historial de Riegos"**: tabla debajo del panel de control que consulta `irrigation_log` (últimos 15 registros) con fecha, zona, estado, duración, litros, humedad, variación y lluvia. Se refresca cada 60 segundos.

### Supabase — SQL requerido
```sql
-- Ejecutar una vez en el SQL Editor de Supabase:
CREATE TABLE IF NOT EXISTS irrigation_log (
  id            BIGSERIAL PRIMARY KEY,
  created_at    TIMESTAMPTZ DEFAULT NOW(),
  zone_id       INT NOT NULL,
  duration_mins INT,
  liters        FLOAT,
  hum_start     INT,
  hum_end       INT,
  delta_hum     INT,
  reason        TEXT,
  cancelled     BOOLEAN DEFAULT FALSE,
  rain_mm       FLOAT,
  et0_mm        FLOAT
);
ALTER TABLE irrigation_log ENABLE ROW LEVEL SECURITY;
CREATE POLICY "anon read" ON irrigation_log FOR SELECT USING (true);
CREATE POLICY "anon insert" ON irrigation_log FOR INSERT WITH CHECK (true);
```

## 2026-04-12 (v7 – Fix comandos remotos: apagado desde dashboard bloqueado)
- **Bug corregido**: al usar el scheduler escalonado (`cloudStep`), los comandos remotos solo se procesaban cada ~60s. El dashboard esperaba 30s → mostraba "Sin confirmación" y era imposible apagar la bomba.
- **Rediseño del loop cloud por capas de prioridad**:
  - **Capa 1 – Comandos** (`processCloudCommands`): cada **10 segundos**. Garantiza respuesta al usuario en ≤10s.
  - **Capa 2 – Status push** (`pushStatusToCloud`): urgente (bomba apagada) o cada 30s.
  - **Capa 3 – Programación** (`pullScheduleFromCloud`): cada 5 minutos.
  - **Capa 4 – Clima** (`fetchWeather`): primera vez +15s tras boot, luego cada 6h.
- Se eliminan las variables `cloudStep` / `lastCloudStepAt` y se introducen `lastCmdPollAt` / `lastSchedPullAt` con temporizadores independientes.

## 2026-04-11 (v6 – Auditoría de riego + límite MAX_DECISION_MIN=10)
- **Auditoría post-riego completa** (a los +45 minutos de finalizado el riego):
  - Calcula **litros entregados**: `flowRate × auditMins / 60`
  - Registra **humedad al inicio** y **variación de humedad** (`humAfter - humAtStart`)
  - Log `[AUDIT] Z#: X.XL | hum Y%→Z% (+N%)` en Serial Monitor
  - Resultado visible en dashboard bajo cada zona
- **Nuevos campos en `ZoneDecision`**: `humAtStart`, `auditMins`, `auditLiters`, `auditDeltaHum`, `auditResult`
- **`pushStatusToCloud`**: incluye objeto `audit` en cada decisión (solo si hay auditoría disponible)
- **Dashboard `index.html`**: panel "Auditoría último riego" con grid de 4 métricas por zona
- **`MAX_DECISION_MIN` reducido a 10 minutos** (antes 30)
- **Fix countdown timer**: evita reinicio cada 4s, solo resincroniza si desvío > 12s

## 2026-04-10 (v4 – Fix definitivo ejecución programada)
- **Reescritura completa de `predictiveCheck`** con lógica directa de ventana temporal:
  - Eliminada toda la lógica de `runDueMs`, `runDueArmed`, `forceEval`, `lastEvalDay/Sched` para ejecución.
  - Nuevo principio: si `minsFromSched` está en el rango `[0, 5]` (0 a 5 minutos después del horario), se evalúa humedad y se enciende si `hum < threshold`. Sin timers internos, sin dependencias NTP complejas.
  - Condición de no-repetición: `lastRunDay == dayStamp && lastRunSched == sched` — se resetea automáticamente si cambia el horario o al día siguiente.
  - Sobrevive reinicios: si el ESP32 reinicia dentro de la ventana de 5 min, igual ejecuta.
- **`pullScheduleFromCloud` simplificado**: al detectar cambio de horario ya no usa `forceEval` sino que resetea `lastRunDay/Sched`, lo que desbloquea la ejecución para el nuevo horario.
- **Log `[SCHED-DBG]` cada 30s**: muestra hora actual, horario, `minsFrom`, `minsTo`, humedad, umbral, si ya corrió hoy, y si la zona está encendida. Facilita diagnóstico en terreno.
- **`getLocalTime` con timeout 200ms** (era 20ms) para reducir falsos negativos en momentos de carga.
- **Migración a Open-Meteo**: reemplaza OpenWeatherMap. Sin API key, gratis sin límite, modelos ECMWF. ET₀ Penman-Monteith FAO-56 entregado directamente por la API (más preciso que Hargreaves-Samani). Coordenadas actualizadas a Villa Alemana (-33.04°S / -71.37°O). Se eliminó `computeRa()`. Log: `[WX] Villa Alemana | hoy=Xmm mañana=Xmm ET0=X.XXmm/d`.
- **Fix timezone Chile**: `CLT3` (UTC-3, incorrecto) → `CLT4CLST3` (UTC-4 invierno / UTC-3 verano, correcto para Chile continental). Migrado a `configTzTime` que es más fiable que `setenv+configTime` por separado. Se añade `time.cloudflare.com` como tercer servidor NTP. Al sincronizar imprime `[NTP] Hora local sincronizada: YYYY-MM-DD HH:MM:SS CLT/CLST`.
- **Dashboard mejorado (`index.html` + `pushStatusToCloud`):**
  - Firmware ahora envía `hum` (live desde sensor), `thr` (umbral), `sched_hhmm` (horario), `mins_to_sched` (minutos al próximo riego), `remaining_s` y `total_s` (tiempo restante/total de riego activo).
  - Cards del dashboard rediseñadas: barra de humedad con marcador de umbral visual, timer regresivo cuando la bomba está regando (cuenta local entre polls de Supabase), indicador del próximo horario programado con cuenta regresiva, estado claro con razón de decisión.

## 2026-04-10
- Se crea proyecto nuevo `RiegoSmart-ESP32` para firmware de la ESP32 central (Arduino IDE).
- Se implementa control de 5 zonas/MOSFET en pines `13, 12, 14, 27, 26` con arranque seguro en OFF.
- Se agrega sistema de seguridad de corte automatico al superar 15 minutos por zona.
- Se incorpora dashboard local liviano por IP con login (`matias` / `406263`), control manual y calibracion de humedad.
- Se integra receptor ESP-NOW en canal 11 para datos de satelites y conversion de RAW a porcentaje.
- Se integra Supabase con tablas reales:
  - lectura de `remote_commands` pendientes (`is_processed=false`)
  - marcado de comandos procesados (`is_processed=true`)
  - actualizacion de `system_state` (id=1) con estado de bombas
  - insercion de humedad por estacion en `sensor_data`
- Se incluye prioridad de control local sobre nube por ventana temporal tras accion manual.
- Se agrega compatibilidad de callback ESP-NOW para versiones Arduino ESP32 2.x y 3.x.
- Mejora mayor de estabilidad del firmware:
  - arranque escalonado de servicios para reducir picos de consumo al boot
  - conexion WiFi no bloqueante con reintentos periodicos
  - telemetria de salud por serial (`HEALTH`) para diagnostico en terreno
  - timeouts HTTP para evitar bloqueos por red inestable
  - reconexion cloud condicionada a estado real de WiFi
- Se corrige integracion Supabase contra tablas reales:
  - `remote_commands` (lectura de pendientes + marcado `is_processed=true`)
  - `system_state` (PATCH de estado de bombas en `id=1`)
  - `sensor_data` (insercion por estacion/humedad)
- Fix de estabilidad de red (assert `Invalid mbox`):
  - se inicia WiFi/LwIP antes de levantar `WebServer`
  - se elimina cambio redundante de modo WiFi al inicializar ESP-NOW
- Perfil `ultra low-power boot` para reducir brownout:
  - CPU fijada a 80 MHz en arranque
  - potencia TX WiFi reducida (`WIFI_POWER_8_5dBm`)
  - modo sleep WiFi habilitado
  - ESP-NOW diferido a 9 segundos de boot
  - trafico cloud diferido a 15 segundos de boot
- Modo recuperacion ante brownout persistente:
  - workaround de brownout habilitable (`USE_BROWNOUT_WORKAROUND`)
  - pines de bombas parten en alta impedancia y se arman mas tarde en OFF
  - armado progresivo de pines de salida para evitar picos al inicio
- Fix de control manual de MOSFET:
  - el comando manual ON/OFF arma pines inmediatamente si aun no estaban activos
  - cada accion fuerza `pinMode(OUTPUT)` antes de `digitalWrite`
  - se agregan trazas seriales para confirmar solicitudes manuales por zona
- Remapeo de pines de salida para mayor estabilidad en ESP32 basica:
  - MOSFET 1..5 ahora en `GPIO25, GPIO26, GPIO27, GPIO32, GPIO33`
  - se eliminan `GPIO12` y `GPIO14` del control de bombas para evitar conflictos de arranque
- Optimizacion de panel local (latencia ON/OFF):
  - control manual via API ligera (`/api/on`, `/api/off`) sin recargar pagina completa
  - endpoint `/api/state` para refresco incremental de estado/humedad
  - actualizacion de UI local por `fetch` cada 2 segundos
- Mejora adicional de respuesta manual:
  - control API migra a `POST` para comandos ON/OFF
  - respuesta devuelve el estado de la zona afectada
  - UI actualiza solo la fila de la zona tocada (sin refresco global)
  - botones ON/OFF se bloquean brevemente durante envio para evitar doble toque
- Politica de seguridad electrica por simultaneidad:
  - se limita a una sola bomba encendida a la vez
  - al encender una zona, las demas se apagan automaticamente
- Integracion completa con satelites ESP-NOW:
  - central ahora acepta payload legado (12 bytes) y payload V2 compacto
  - conversion de muestras por `stationID/sensorID` con promedio por estacion
  - trazas de diagnostico por muestra recibida (raw/promedio/humedad/bateria)
  - se imprime `MAC STA` de la central al arranque para configurar peers ESP-NOW sin errores
- Se agrega firmware satelite nuevo en `satelite/satelite.ino`:
  - canal fijo 11
  - callback de envio ESP-NOW
  - envio de payload V2 con `nonce`
  - deep sleep cada 30 minutos
- Ajuste de frecuencia de telemetria a nube:
  - `sensor_data` solo se sube cuando hay dato nuevo de satelite (`sensorDataDirty`)
  - ventana minima de subida: 30 minutos
  - evita inserciones cada 30 segundos sin cambios reales
- Implementacion completa de programacion en central (6 puntos):
  - APIs para `schedule.html`: `/api/schedule`, `/api/schedule/save`, `/api/program/select`, `/api/time`, `/api/sensors/list`, `/api/sensors/add`, `/api/sensors/remove`
  - configuracion persistente de 3 programas x 5 bombas en `Preferences`
  - decision por umbral de humedad 5 minutos antes de la hora programada
  - ejecucion a la hora programada solo si la decision indica riego
  - auditoria automatica a +45 minutos con resultado por zona
  - publicacion de decisiones por zona en `system_state.decisions`
- Restauracion de `central/central.ino` como gateway (separado del satelite):
  - recupera control local, control cloud, ESP-NOW y seguridad de riego
  - mantiene soporte de payload satelite legado y V2
  - agrega sincronizacion de programacion cloud/local via `system_state.pumps[].programs`
- Mejora de visibilidad de decisiones en dashboard:
  - la central publica estado anticipado por zona antes de la ventana de evaluación (`PEND_EVAL`)
  - se informa cuenta regresiva aproximada para evaluación de umbral (`Eval en Xm`)
- Ajuste de evaluación para programación tardía:
  - si una zona se programa con menos de 5 minutos de anticipación, la central realiza evaluación inmediata
  - evita el caso donde no había decisión por haber pasado la ventana `-5m` al guardar
- Fix de reevaluación por cambio de horario:
  - cada zona guarda la última hora programada evaluada/ejecutada
  - si cambias la hora en el mismo día, la central vuelve a evaluar para ese nuevo horario
- Robustez de programación y visibilidad:
  - timezone Chile aplicada con regla DST (`TZ`) para evitar desfases de evaluación
  - la central fuerza evaluación inmediata al detectar cambios de programación en nube
  - se publican `next_eval_hhmm` y `next_eval_min` por zona en `system_state.decisions`
- Fallback de ejecución en hora programada:
  - si no hubo evaluación previa del horario, la central evalúa en caliente al minuto exacto
  - evita perder riegos por fallas de sincronización en la ventana `-5m`
- Tolerancia de ejecución programada:
  - la central ahora ejecuta en ventana de 0 a 2 minutos tras la hora objetivo
  - evita perder arranques por desfasajes de segundo/tick/NTP
- Ajuste robusto de ejecución diferida:
  - se amplía la ventana de ejecución a 0..30 minutos tras la hora programada
  - evita que una decisión `SI` se pierda por saltos de sincronización o polling
  - no se sobreescribe estado `PEND_EVAL` cuando ya existe decisión activa para el horario actual
- Fix crítico de evaluación tardía:
  - la evaluación tardía ahora aplica en ventana de 0..30 minutos post-horario (no solo minuto exacto)
  - corrige casos donde una zona evaluaba `SI` pero no ejecutaba si se pasaba por 1 minuto
- Motor de ejecución robusto por timestamp:
  - las decisiones `SI` ahora generan un `runDueEpoch` por zona
  - la ejecución deja de depender de coincidencia de minuto exacto
  - mejora tolerancia a desfase de reloj/tick y evita pérdidas de encendido en horas cercanas
- Fix de ejecución programada cercana:
  - corregido cálculo de `runDueEpoch` para horarios cercanos (ya no queda en cero)
  - resuelve caso donde se evaluaba `SI` pero nunca aparecía `[SCHED-RUN]`
- Respaldo de ejecución por decisión:
  - si existe decisión positiva para el horario actual, ejecuta en ventana 0..30m aunque falle `runDueEpoch`
  - prioriza fiabilidad de encendido programado sobre dependencia exclusiva de timestamp interno
- Motor de ejecución migrado a temporizador local:
  - `runDueEpoch` reemplazado por `runDueMs` + `runDueArmed`
  - la ejecución ya no depende de conversiones de tiempo NTP para disparar la bomba
  - reduce falsos negativos en encendido programado cercano
