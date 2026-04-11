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
