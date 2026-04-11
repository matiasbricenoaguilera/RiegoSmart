# Changelog

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
