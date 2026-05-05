/**
 * RiegoSmart: sincroniza Meteochile (EMA) + Open-Meteo y guarda en public.weather_cache.
 *
 * Secretos (Dashboard → Edge Functions → sync-weather → Secrets, o `supabase secrets set`):
 *   METEOCHILE_USER, METEOCHILE_TOKEN, METEOCHILE_CODIGO
 * Opcionales: SITE_LAT (default -33.04), SITE_LON (default -71.37),
 *   WEATHER_RAIN_FORECAST_HOURS (default 6): suma precipitación horaria Open-Meteo; rain_mm = max(EMA, suma).
 *
 * Programar: cada 15–30 min POST https://<ref>.supabase.co/functions/v1/sync-weather
 *   Header: Authorization: Bearer <SUPABASE_ANON_KEY> y apikey: igual
 */
import { serve } from "https://deno.land/std@0.177.0/http/server.ts";
import { createClient } from "https://esm.sh/@supabase/supabase-js@2.49.1";

const METEO_BASE =
  "https://climatologia.meteochile.gob.cl/application/servicios/getDatosRecientesEma/";

function extractPrecip(data: unknown): number | null {
  if (data === null || data === undefined) return null;
  if (typeof data === "number" && !Number.isNaN(data)) return data;
  if (typeof data !== "object") return null;
  const o = data as Record<string, unknown>;
  for (const [k, v] of Object.entries(o)) {
    const kl = k.toLowerCase();
    if (
      kl.includes("precip") || kl.includes("lluvia") ||
      kl.includes("aguacai") || kl.includes("agua_ca")
    ) {
      if (typeof v === "number") return v;
      const inner = extractPrecip(v);
      if (inner !== null) return inner;
    }
  }
  for (const v of Object.values(o)) {
    if (typeof v === "object" && v !== null) {
      const inner = extractPrecip(v);
      if (inner !== null) return inner;
    }
  }
  return null;
}

serve(async (req: Request) => {
  if (req.method !== "POST" && req.method !== "GET") {
    return new Response("Method Not Allowed", { status: 405 });
  }

  const supabaseUrl = Deno.env.get("SUPABASE_URL")!;
  const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;
  const user = Deno.env.get("METEOCHILE_USER") ?? "";
  const token = Deno.env.get("METEOCHILE_TOKEN") ?? "";
  const codigo = Deno.env.get("METEOCHILE_CODIGO") ?? "";
  const lat = parseFloat(Deno.env.get("SITE_LAT") ?? "-33.04");
  const lon = parseFloat(Deno.env.get("SITE_LON") ?? "-71.37");

  const supabase = createClient(supabaseUrl, serviceKey);

  let rainMm: number | null = null;
  let desc = "Sin datos";
  let source = "openmeteo";

  if (user && token && codigo) {
    const url =
      `${METEO_BASE}${encodeURIComponent(codigo)}?usuario=${
        encodeURIComponent(user)
      }&token=${encodeURIComponent(token)}`;
    try {
      const r = await fetch(url, {
        headers: {
          "Accept-Encoding": "identity",
          "User-Agent": "RiegoSmart-sync-weather/1",
        },
      });
      const text = await r.text();
      if (!r.ok) {
        console.error("Meteochile HTTP", r.status, text.slice(0, 200));
      } else {
        try {
          const j = JSON.parse(text) as Record<string, unknown>;
          const msg = String(j["mensaje"] ?? "");
          if (msg.includes("bloquead")) {
            console.warn("Meteochile bloqueado:", msg);
          } else {
            rainMm = extractPrecip(j);
            if (rainMm !== null) {
              source = "meteochile+openmeteo";
              desc = rainMm >= 5 ? "Lluvia (EMA DMC)" : "Clima EMA DMC";
            }
          }
        } catch {
          console.error("Meteochile no JSON", text.slice(0, 120));
        }
      }
    } catch (e) {
      console.error("Meteochile error", e);
    }
  }

  let et0 = 0;
  let tmax = 25;
  let tmin = 15;
  let hourlySum = 0;
  const rainH = Math.min(
    48,
    Math.max(
      1,
      parseInt(Deno.env.get("WEATHER_RAIN_FORECAST_HOURS") ?? "6", 10) || 6,
    ),
  );

  try {
    const om =
      `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}` +
      `&hourly=precipitation&forecast_hours=${rainH}` +
      "&daily=temperature_2m_max,temperature_2m_min,et0_fao_evapotranspiration" +
      "&forecast_days=1&timezone=America%2FSantiago";
    const r = await fetch(om);
    const j = await r.json() as {
      hourly?: { precipitation?: number[] };
      daily?: {
        et0_fao_evapotranspiration?: number[];
        temperature_2m_max?: number[];
        temperature_2m_min?: number[];
      };
    };
    const arr = j.hourly?.precipitation;
    if (Array.isArray(arr)) {
      for (let i = 0; i < Math.min(arr.length, rainH); i++) {
        hourlySum += Number(arr[i]) || 0;
      }
    }
    const d = j.daily;
    if (d) {
      et0 = d.et0_fao_evapotranspiration?.[0] ?? 0;
      tmax = d.temperature_2m_max?.[0] ?? 25;
      tmin = d.temperature_2m_min?.[0] ?? 15;
    }
  } catch (e) {
    console.error("Open-Meteo error", e);
  }

  const obsMm = rainMm !== null && rainMm !== undefined ? rainMm : 0;
  const mergedRain = Math.max(obsMm, hourlySum);
  rainMm = mergedRain;

  if (mergedRain === 0 && source === "openmeteo") {
    desc = "Sin datos";
  } else if (source === "openmeteo" && hourlySum > 0) {
    desc = `Modelo Open-Meteo (~${rainH}h)`;
  }

  const updatedUnix = Math.floor(Date.now() / 1000);

  const { error } = await supabase.from("weather_cache").upsert({
    id: "default",
    rain_mm: mergedRain,
    et0_mm: et0,
    t_max: tmax,
    t_min: tmin,
    description: desc,
    source,
    updated_unix: updatedUnix,
    updated_at: new Date().toISOString(),
  }, { onConflict: "id" });

  if (error) {
    console.error(error);
    return new Response(JSON.stringify({ ok: false, error: error.message }), {
      status: 500,
      headers: { "Content-Type": "application/json" },
    });
  }

  return new Response(
    JSON.stringify({
      ok: true,
      rain_mm: mergedRain,
      et0_mm: et0,
      t_max: tmax,
      t_min: tmin,
      source,
      updated_unix: updatedUnix,
    }),
    { headers: { "Content-Type": "application/json" } },
  );
});
