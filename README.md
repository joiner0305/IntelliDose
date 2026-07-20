# Dispensador IoT — Render + Supabase + ESP32

Sistema de dispensación y monitoreo remoto de medicamentos.
Arquitectura de 3 piezas:

```
ESP32  <--HTTP-->  Backend (Render/Express)  <--service_role-->  Supabase (Auth + Postgres + Realtime)
                              ^
                              | JWT (Supabase Auth)
                    Dashboard web (public/index.html)
```

El ESP32 **nunca habla directo con Supabase** — todo pasa por tu API en
Render, que es quien valida al dispositivo (`x-api-key`) y a los usuarios
(JWT de Supabase Auth). El dashboard sí habla directo con Supabase para
Auth y para las notificaciones en tiempo real (Realtime), pero usa la
`anon key` (segura de exponer) y las políticas RLS del `schema.sql`.

## 1. Supabase

1. Crea un proyecto en https://supabase.com.
2. Ve a **SQL Editor** y ejecuta `supabase/schema.sql` completo.
3. En **Authentication → Providers**, deja habilitado Email (registro/login
   por correo, tal como pide la Parte 1 del documento funcional).
4. En **Project Settings → API** copia:
   - `Project URL` → `SUPABASE_URL`
   - `anon public key` → para `public/index.html`
   - `service_role key` → `SUPABASE_SERVICE_ROLE_KEY` (solo backend, nunca la subas a git ni al frontend)
5. En **Table Editor → device_config**, copia el valor de `device_api_key`
   generado automáticamente — lo necesita el ESP32.

## 2. Backend (Render)

```bash
cd server
cp .env.example .env   # completa con tus valores de Supabase
npm install
npm run dev             # prueba local en http://localhost:3000
```

Despliegue en Render:
1. Sube esta carpeta a un repo de GitHub.
2. En Render: **New → Web Service**, apunta al repo, root directory `server/`.
3. Build command: `npm install` · Start command: `npm start`.
4. Agrega las variables de entorno (`SUPABASE_URL`, `SUPABASE_SERVICE_ROLE_KEY`)
   en **Environment**.
5. Plan free: el servicio "duerme" tras inactividad — la primera petición
   tras dormir tarda ~30-50s. El ESP32 hace polling cada 30s, así que
   conviene mantenerlo despierto con un ping externo (ej. cron-job.org
   golpeando `/health` cada 10 min) si el letargo afecta la demo.

## 3. Dashboard (public/index.html)

El backend ya sirve `public/` como archivos estáticos, así que un único
servicio de Render expone tanto la API como el dashboard — no necesitas
un hosting aparte.

Antes de desplegar, edita las tres constantes al inicio del `<script>`:
```js
const SUPABASE_URL = "...";
const SUPABASE_ANON_KEY = "...";
const API_BASE_URL = "https://tu-servicio.onrender.com"; // la misma URL del paso 2
```
El logo va en `public/assets/logo.png` (y el favicon en `assets/favicon.png`) —
solo reemplaza esos archivos si cambias de marca.

## 4. ESP32 (esp32/dispensador.ino)

1. Instala en Arduino IDE: `ArduinoJson`, `ESP32Servo`.
2. Completa al inicio del archivo:
   - `WIFI_SSID` / `WIFI_PASSWORD`
   - `API_BASE_URL` (tu servicio en Render)
   - `DEVICE_API_KEY` (el `device_api_key` de `device_config`)
   - Pines reales de tu prototipo (servo, sensor IR, buzzer, LED)
3. Sube el sketch. El ESP32:
   - Sincroniza hora por NTP (UTC-5, Colombia).
   - Cada 30s consulta `/api/esp32/schedules`.
   - Al llegar la hora programada, acciona el servo y espera detección
     del sensor (Etapa 1→2), reporta `dispensed`.
   - Si a los 30s (configurable) el medicamento sigue en la bandeja,
     reporta `alert` y activa buzzer/LED (Etapa 3).
   - Cuando el sensor deja de detectarlo, reporta `taken` con el tiempo
     transcurrido (Etapa 4). Si se excede el máximo (2 min en el demo,
     configurable en `device_config.max_wait_seconds`), reporta `timeout`.

## Subir a GitHub

```bash
cd dispensador-iot
git init
git add .
git commit -m "Dispensador IoT: backend, dashboard y firmware ESP32"
git branch -M main
git remote add origin https://github.com/TU_USUARIO/TU_REPO.git
git push -u origin main
```

El `.gitignore` ya excluye `node_modules/` y `.env` — tus llaves de Supabase
nunca se suben. En Render, apunta el **Web Service** a este repo con
**Root Directory: `server`** y agrega las variables de entorno ahí mismo
(paso 2 arriba).

## Notas de diseño

- **Un solo dispositivo compartido**: según el documento, todas las
  cuentas ven y controlan el mismo ESP32 — por eso las políticas RLS
  no filtran por usuario en `schedules`/`events`.
- **Notificaciones "push"**: se logran con Supabase Realtime (el
  dashboard se suscribe a `INSERT` en `events`), sin necesidad de un
  servicio de push separado. Si más adelante quieres notificaciones
  reales al celular (fuera del navegador), el siguiente paso natural
  es Firebase Cloud Messaging desde el backend.
- **API vs SMS**: se implementó API/HTTP tal como el documento lo
  planteaba como la opción más simple.
