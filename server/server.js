require('dotenv').config();
const express = require('express');
const cors = require('cors');
const { createClient } = require('@supabase/supabase-js');

const path = require('path');

const app = express();
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '..', 'public')));

// Cliente con service_role: puede saltarse RLS. SOLO se usa en el backend.
const supabase = createClient(
  process.env.SUPABASE_URL,
  process.env.SUPABASE_SERVICE_ROLE_KEY
);

// -----------------------------------------------------------
// Middleware: autenticación del ESP32 (x-api-key contra device_config)
// -----------------------------------------------------------
async function requireDeviceKey(req, res, next) {
  const key = req.header('x-api-key');
  if (!key) return res.status(401).json({ error: 'Falta x-api-key' });

  const { data, error } = await supabase
    .from('device_config')
    .select('*')
    .eq('id', 1)
    .single();

  if (error || !data) return res.status(500).json({ error: 'No se pudo validar el dispositivo' });
  if (data.device_api_key !== key) return res.status(403).json({ error: 'API key inválida' });

  req.deviceConfig = data;
  next();
}

// -----------------------------------------------------------
// Middleware: autenticación del dashboard (JWT de Supabase Auth)
// -----------------------------------------------------------
async function requireUser(req, res, next) {
  const auth = req.header('authorization') || '';
  const token = auth.replace('Bearer ', '');
  if (!token) return res.status(401).json({ error: 'Falta token' });

  const { data, error } = await supabase.auth.getUser(token);
  if (error || !data?.user) return res.status(401).json({ error: 'Token inválido' });

  req.user = data.user;
  next();
}

// =============================================================
// RUTAS PARA EL ESP32
// =============================================================

// El ESP32 hace polling de esto cada X segundos para saber
// qué horarios activos hay y con qué configuración (Etapa 1).
app.get('/api/esp32/schedules', requireDeviceKey, async (req, res) => {
  const { data, error } = await supabase
    .from('schedules')
    .select('id, hour, minute, second, repeat_seconds, label, created_at')
    .eq('active', true)
    .order('hour', { ascending: true });

  if (error) return res.status(500).json({ error: error.message });

  const schedules = data.map(s => ({
    ...s,
    created_at_epoch: Math.floor(new Date(s.created_at).getTime() / 1000)
  }));

  res.json({
    schedules,
    config: {
      alert_threshold_seconds: req.deviceConfig.alert_threshold_seconds,
      max_wait_seconds: req.deviceConfig.max_wait_seconds,
      timezone_offset_minutes: req.deviceConfig.timezone_offset_minutes
    }
  });
});

// El ESP32 reporta cada evento de su máquina de estados
// (Etapas 2, 3 y 4 del documento).
app.post('/api/esp32/event', requireDeviceKey, async (req, res) => {
  const { type, schedule_id, delay_seconds } = req.body;

  const validTypes = ['dispensed', 'alert', 'taken', 'timeout'];
  if (!validTypes.includes(type)) {
    return res.status(400).json({ error: `type debe ser uno de: ${validTypes.join(', ')}` });
  }

  const { data, error } = await supabase
    .from('events')
    .insert({ type, schedule_id: schedule_id || null, delay_seconds: delay_seconds ?? null })
    .select()
    .single();

  if (error) return res.status(500).json({ error: error.message });

  // Supabase Realtime notifica automáticamente al dashboard
  // (suscripción directa a la tabla `events`, ver public/index.html).
  res.status(201).json({ ok: true, event: data });
});

// =============================================================
// RUTAS PARA EL DASHBOARD (requieren login)
// =============================================================

app.get('/api/schedules', requireUser, async (req, res) => {
  const { data, error } = await supabase
    .from('schedules')
    .select('*')
    .order('hour', { ascending: true });
  if (error) return res.status(500).json({ error: error.message });
  res.json(data);
});

app.post('/api/schedules', requireUser, async (req, res) => {
  const { hour, minute, second = 0, label, repeat_seconds } = req.body;

  let payload = { label, created_by: req.user.id };

  if (repeat_seconds != null) {
    if (repeat_seconds <= 0) return res.status(400).json({ error: 'repeat_seconds debe ser mayor a 0' });
    payload = { ...payload, repeat_seconds, hour: null, minute: null, second: null };
  } else {
    if (hour == null || minute == null) {
      return res.status(400).json({ error: 'hour y minute son requeridos (formato 24h), o envía repeat_seconds' });
    }
    payload = { ...payload, hour, minute, second, repeat_seconds: null };
  }

  const { data, error } = await supabase
    .from('schedules')
    .insert(payload)
    .select()
    .single();
  if (error) return res.status(500).json({ error: error.message });
  res.status(201).json(data);
});

app.delete('/api/schedules/:id', requireUser, async (req, res) => {
  const { error } = await supabase.from('schedules').delete().eq('id', req.params.id);
  if (error) return res.status(500).json({ error: error.message });
  res.status(204).end();
});

app.get('/api/events', requireUser, async (req, res) => {
  const { data, error } = await supabase
    .from('events')
    .select('*')
    .order('created_at', { ascending: false })
    .limit(100);
  if (error) return res.status(500).json({ error: error.message });
  res.json(data);
});

app.patch('/api/events/:id/read', requireUser, async (req, res) => {
  const { error } = await supabase.from('events').update({ is_read: true }).eq('id', req.params.id);
  if (error) return res.status(500).json({ error: error.message });
  res.status(204).end();
});

app.get('/health', (req, res) => res.json({ ok: true }));

// Cualquier ruta que no sea /api/* sirve el dashboard
app.get(/^(?!\/api).*/, (req, res) => {
  res.sendFile(path.join(__dirname, '..', 'public', 'index.html'));
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`Servidor escuchando en puerto ${PORT}`));
