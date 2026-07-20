-- =========================================================
-- Esquema Supabase: Sistema de dispensación y monitoreo
-- remoto de medicamentos (IoT / ESP32)
-- =========================================================
-- Diseño: un único dispositivo físico (ESP32). Cualquier
-- usuario autenticado en la página ve y gestiona el mismo
-- dispositivo (según el documento: "no es aplicado a
-- múltiples" dispositivos).
-- =========================================================

create extension if not exists "pgcrypto";

-- ---------------------------------------------------------
-- Configuración del dispositivo (fila única / singleton)
-- ---------------------------------------------------------
create table if not exists device_config (
  id                     int primary key default 1,
  device_api_key         text not null default encode(gen_random_bytes(24), 'hex'),
  alert_threshold_seconds int not null default 30,   -- Etapa 3 del doc
  max_wait_seconds        int not null default 120,  -- límite demo (real: 4-6h)
  timezone_offset_minutes int not null default -300, -- Colombia UTC-5
  updated_at             timestamptz not null default now(),
  constraint single_row check (id = 1)
);

insert into device_config (id) values (1)
  on conflict (id) do nothing;

-- ---------------------------------------------------------
-- Horarios programados de dispensación (Parte 2 del app)
-- ---------------------------------------------------------
create table if not exists schedules (
  id         uuid primary key default gen_random_uuid(),
  created_by uuid references auth.users(id) on delete set null,
  hour       int not null check (hour between 0 and 23),
  minute     int not null check (minute between 0 and 59),
  second     int not null default 0 check (second between 0 and 59),
  label      text,                         -- ej: "Losartán - mañana"
  active     bool not null default true,
  created_at timestamptz not null default now()
);

-- ---------------------------------------------------------
-- Eventos / notificaciones generados por el ESP32
-- (Parte 3 del app: dispensada, alerta, retraso)
-- ---------------------------------------------------------
create table if not exists events (
  id            uuid primary key default gen_random_uuid(),
  schedule_id   uuid references schedules(id) on delete set null,
  type          text not null check (type in ('dispensed','alert','taken','timeout')),
  delay_seconds int,                       -- tiempo transcurrido (Opción 1 / Opción 2)
  is_read       bool not null default false,
  created_at    timestamptz not null default now()
);

create index if not exists events_created_at_idx on events (created_at desc);

-- ---------------------------------------------------------
-- Row Level Security
-- ---------------------------------------------------------
alter table device_config enable row level security;
alter table schedules      enable row level security;
alter table events         enable row level security;

-- device_config: solo el backend (service_role) lo toca.
-- Los usuarios autenticados pueden leerlo (para mostrar límites en la UI).
create policy "device_config: lectura autenticados"
  on device_config for select
  to authenticated
  using (true);

-- schedules: cualquier usuario autenticado puede ver/crear/eliminar
-- (dispositivo único y compartido). El ESP32 sólo lee vía backend.
create policy "schedules: select autenticados"
  on schedules for select to authenticated using (true);

create policy "schedules: insert autenticados"
  on schedules for insert to authenticated with check (true);

create policy "schedules: delete autenticados"
  on schedules for delete to authenticated using (true);

-- events: sólo lectura para el dashboard. La inserción la hace
-- el backend con la service_role key (el ESP32 nunca escribe directo).
create policy "events: select autenticados"
  on events for select to authenticated using (true);

create policy "events: update (marcar leído) autenticados"
  on events for update to authenticated using (true) with check (true);
