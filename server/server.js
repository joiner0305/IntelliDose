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

  // Registra "última vez visto" para el indicador en línea del dashboard.
  // No esperamos (await) para no frenar la respuesta al ESP32.
  supabase.from('device_config').update({ last_seen_at: new Date().toISOString() }).eq('id', 1).then(() => {});

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
  if (error || !data?.user) {
    console.error('requireUser: rechazado ->', error?.message || 'sin usuario en la respuesta', '| status:', error?.status);
    return res.status(401).json({ error: 'Token inválido' });
  }

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
    .select('id, hour, minute, second, repeat_seconds, label, created_at, last_cycle_at')
    .eq('active', true)
    .order('hour', { ascending: true });

  if (error) return res.status(500).json({ error: error.message });

  const schedules = data.map(s => ({
    ...s,
    created_at_epoch: Math.floor(new Date(s.created_at).getTime() / 1000),
    last_cycle_epoch: s.last_cycle_at ? Math.floor(new Date(s.last_cycle_at).getTime() / 1000) : null
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

// Chequeo súper liviano, pensado para consultarse cada 1s (a diferencia
// de /api/esp32/schedules que se consulta cada 30s). Si el dashboard
// activó "Probar ahora", responde true UNA sola vez y lo apaga de inmediato,
// para que un solo click dispare exactamente una vez.
app.get('/api/esp32/force-check', requireDeviceKey, async (req, res) => {
  if (!req.deviceConfig.force_trigger) return res.json({ trigger: false });

  await supabase.from('device_config').update({ force_trigger: false }).eq('id', 1);
  res.json({ trigger: true });
});

// El dashboard activa esta bandera; el ESP32 la recoge en su próximo
// chequeo de /api/esp32/force-check (máx. ~1s).
app.post('/api/schedules/force-trigger', requireUser, async (req, res) => {
  const { error } = await supabase.from('device_config').update({ force_trigger: true }).eq('id', 1);
  if (error) return res.status(500).json({ error: error.message });
  res.status(204).end();
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

  // Cada dispensación cuenta contra el límite del horario (si tiene uno).
  // Un horario de "una vez" (repeat_seconds null) siempre se desactiva
  // tras su primera dispensación; uno repetitivo solo si alcanzó
  // repeat_count, o sigue activo indefinidamente si no tiene límite.
  if (type === 'dispensed' && schedule_id) {
    const { data: sched } = await supabase
      .from('schedules')
      .select('repeat_seconds, repeat_count, times_fired')
      .eq('id', schedule_id)
      .single();

    if (sched) {
      const timesFired = (sched.times_fired || 0) + 1;
      const isOnce = sched.repeat_seconds == null;
      const reachedLimit = isOnce || (sched.repeat_count != null && timesFired >= sched.repeat_count);

      await supabase
        .from('schedules')
        .update({ times_fired: timesFired, active: !reachedLimit })
        .eq('id', schedule_id);
    }
  }

  // El ciclo termina con "taken" o "timeout": recién ahí el ESP32 empieza
  // a contar el siguiente intervalo (si es un horario repetitivo). Guardamos
  // el momento para que el dashboard muestre el mismo conteo que el dispositivo.
  if ((type === 'taken' || type === 'timeout') && schedule_id) {
    await supabase
      .from('schedules')
      .update({ last_cycle_at: new Date().toISOString() })
      .eq('id', schedule_id);
  }

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
  const { hour, minute, second = 0, label, repeat_seconds, repeat_count } = req.body;

  let payload = { label, created_by: req.user.id };

  if (repeat_seconds != null) {
    if (repeat_seconds <= 0) return res.status(400).json({ error: 'repeat_seconds debe ser mayor a 0' });
    if (repeat_count != null && repeat_count <= 0) return res.status(400).json({ error: 'repeat_count debe ser mayor a 0' });
    payload = { ...payload, repeat_seconds, repeat_count: repeat_count ?? null, hour: null, minute: null, second: null };
  } else {
    if (hour == null || minute == null) {
      return res.status(400).json({ error: 'hour y minute son requeridos (formato 24h), o envía repeat_seconds' });
    }
    payload = { ...payload, hour, minute, second, repeat_seconds: null, repeat_count: null };
  }

  const { data, error } = await supabase
    .from('schedules')
    .insert(payload)
    .select()
    .single();
  if (error) return res.status(500).json({ error: error.message });

  // Para que la PRIMERA pastilla caiga en ~1s (y no esperar los 30s del
  // poll normal), levantamos la bandera de chequeo rápido: el ESP32 la
  // recoge en su force-check de cada 1s, recarga horarios y dispensa.
  await supabase.from('device_config').update({ force_trigger: true }).eq('id', 1);

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

// Estado del ESP32: el dashboard lo consulta para el indicador en línea.
app.get('/api/device/status', requireUser, async (req, res) => {
  const { data, error } = await supabase
    .from('device_config')
    .select('last_seen_at')
    .eq('id', 1)
    .single();
  if (error) return res.status(500).json({ error: error.message });

  const lastSeen = data.last_seen_at ? new Date(data.last_seen_at).getTime() : null;
  const online = lastSeen != null && (Date.now() - lastSeen) < 40000; // 40s de gracia
  res.json({ online, last_seen_at: data.last_seen_at });
});

app.get('/health', (req, res) => res.json({ ok: true }));

// Cualquier ruta que no sea /api/* sirve el dashboard
app.get(/^(?!\/api).*/, (req, res) => {
  res.sendFile(path.join(__dirname, '..', 'public', 'index.html'));
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`Servidor escuchando en puerto ${PORT}`));
