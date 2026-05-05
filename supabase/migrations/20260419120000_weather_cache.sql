-- Caché de clima rellenada por Edge Function (Meteochile + Open-Meteo); la ESP32 solo lee esta fila.
create table if not exists public.weather_cache (
  id text primary key default 'default',
  rain_mm double precision default 0,
  et0_mm double precision default 0,
  t_max double precision default 25,
  t_min double precision default 15,
  description text default 'Sin datos',
  source text default '',
  updated_at timestamptz not null default now(),
  updated_unix bigint not null default 0
);

comment on table public.weather_cache is 'Snapshot climático para RiegoSmart; actualizar vía supabase/functions/sync-weather';

alter table public.weather_cache enable row level security;

-- Lectura pública con anon (misma clave que el firmware).
drop policy if exists "weather_cache_select_anon" on public.weather_cache;
create policy "weather_cache_select_anon"
  on public.weather_cache for select
  to anon, authenticated
  using (true);

-- Inserción/actualización solo con service_role (Edge Function); el rol service_role bypass RLS por defecto.

insert into public.weather_cache (id, rain_mm, et0_mm, t_max, t_min, description, source, updated_unix)
values ('default', 0, 0, 25, 15, 'Sin sincronizar aún', 'init', 0)
on conflict (id) do nothing;
