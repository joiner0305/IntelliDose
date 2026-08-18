/*
 * Dispensador de medicamentos IoT — ESP32
 * -----------------------------------------------------------
 * Implementa la lógica de Etapas 1-4 descrita en el documento
 * "Descripción de función básica de aplicativo y dispositivo":
 *
 *  Etapa 1: espera la hora programada (o el botón "Probar ahora") -> marca 0s
 *  Aviso:   servo a 90° suelta la pastilla + buzzer y LED encendidos 3s;
 *           al terminar, el servo vuelve a 0° y se registra "dispensed"
 *           (la pastilla ya está disponible).
 *  Etapa 3: espera a que el sensor detecte la MANO de la persona
 *           acercándose. Si a los 30s nadie se acerca -> evento "alert"
 *           (buzzer + LED). Si pasa max_wait_seconds -> evento "timeout".
 *  Etapa 4: con la mano ya detectada, cuando el sensor deja de detectarla
 *           significa que la persona se llevó la pastilla -> evento "taken".
 *
 * NOTA: el sensor detecta la MANO de la persona, no la pastilla.
 *
 * Soporta dos tipos de horario:
 *  · Hora fija (hour/minute/second): dispara una vez al día a esa hora.
 *  · Repetitivo (repeat_seconds): dispara cada N segundos de forma
 *    indefinida, útil para probar el prototipo sin esperar al reloj.
 *
 * Comunicación: HTTP/REST contra el backend en Render (más simple
 * que SMS, según lo definido en el documento).
 *
 * Librerías necesarias (Arduino Library Manager):
 *   - ArduinoJson
 *   - ESP32Servo
 * -----------------------------------------------------------
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <time.h>

// ---------------- CONFIGURACIÓN ----------------
const char* WIFI_SSID     = "ALEJANDRO7104";
const char* WIFI_PASSWORD = "AlejoWTFgamer312";

// URL de tu servicio en Render, sin slash final
const char* API_BASE_URL  = "https://intellidose.onrender.com";
const char* DEVICE_API_KEY = "4ef8ee9ac1abb42f1e01ee6c9189ec00a5dc0a085b8b66fb";

// Pines
const int PIN_SERVO   = 4;
const int PIN_IR      = 14;   // sensor infrarrojo (o de contacto)
const int PIN_BUZZER  = 26;
const int PIN_LED     = 25;

const bool IR_ACTIVE_LOW = true; // ajustar según el sensor usado

// Servidor NTP (Colombia = UTC-5, sin horario de verano)
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -5 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

// ---------------- ESTADO GLOBAL ----------------
Servo dispensadorServo;

struct Schedule {
  String id;
  bool repeating;
  int hour, minute, second;      // solo si !repeating
  long repeatSeconds;            // solo si repeating
  long createdAtEpoch;           // ancla para calcular el próximo disparo
  long lastCycleEpoch;           // fin del último ciclo (retiro/timeout), -1 si aún no hay ninguno
};

Schedule schedules[10];
int scheduleCount = 0;

// Resultado del force-check: dos banderas independientes.
//  · trigger:         "Probar ahora" -> dispensar YA, sin horario.
//  · schedulesDirty:  se creó/borró un horario -> solo refrescar la lista;
//                      NUNCA debe forzar una dispensación por sí sola.
// (Declarado aquí, junto a Schedule, porque el auto-generador de
// prototipos de Arduino inserta las firmas de función ANTES de este
// punto en el archivo si se declara más abajo, y el tipo no existiría
// todavía -> error de compilación "EstadoForceCheck no declarado".)
struct EstadoForceCheck {
  bool trigger;
  bool schedulesDirty;
};

// Próximo disparo (epoch) por cada horario repetitivo, indexado por id.
// Se recalcula cada vez que ese id "no se ha visto antes" o tras disparar.
String repeatIds[10];
long repeatNextFire[10];
int repeatTrackedCount = 0;

int alertThresholdSeconds = 30;   // Etapa 3 (config viene del backend)
int maxWaitSeconds = 120;         // límite demo (real: 4-6h), config del backend

unsigned long lastSchedulePoll = 0;
const unsigned long SCHEDULE_POLL_INTERVAL_MS = 30000; // cada 30s

unsigned long lastForceCheck = 0;
const unsigned long FORCE_CHECK_INTERVAL_MS = 2000; // cada 2s: "Probar ahora" del dashboard

// Máquina de estados
enum EstadoDispensador { IDLE, AVISANDO, ESPERANDO_MANO, ESPERANDO_RETIRO };
EstadoDispensador estado = IDLE;

const unsigned long DURACION_AVISO_MS = 3000; // buzzer + LED al dispensar (3s)

// --- Alarma intermitente (aplica a TODAS las pastillas) ---
const unsigned long ALARMA_INTERMITENTE_MS = 5000; // duración de cada alarma pulsante
const int MAX_ALERTAS_PRIMERA = 3; // primeras 3 alertas de la 1ª pastilla llevan mensaje especial
const char* MSG_PRIMERA = "¡El paciente no ha empezado tratamiento!";

bool esPrimeraPastilla = false;       // ¿el ciclo actual es la primera pastilla?
int alertasEnviadas = 0;              // cuántas alertas van en este ciclo de espera
unsigned long marcaProximaAlerta = 0; // cuándo toca la siguiente alarma (cada 30s)
bool enAlarmaIntermitente = false;    // ¿estamos en los 5s de alarma pulsante?
unsigned long marcaInicioAlarma = 0;  // inicio de la alarma pulsante actual
unsigned long ultimoParpadeoBuzzer = 0; // para el patrón bip-bip
bool buzzerEncendidoParpadeo = false;

String scheduleActivoId = "";
unsigned long marcaTiempoInicio = 0; // millis() en el momento de detección (Etapa 2)
bool alertaEnviada = false;
int ultimoMinutoDisparado = -1; // evita disparar la misma hora dos veces

// =================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_IR, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED, LOW);

  dispensadorServo.attach(PIN_SERVO);
  dispensadorServo.write(0); // posición cerrada

  conectarWiFi();
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  obtenerHorariosYConfig();
}

// =================================================================
void loop() {
  // Si el WiFi se cayó (ej. tras una prueba), reconectar sin bloquear
  // el resto del programa: el servo y la máquina de estados siguen.
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long ultimoIntentoWifi = 0;
    if (millis() - ultimoIntentoWifi > 5000) {
      ultimoIntentoWifi = millis();
      Serial.println("WiFi desconectado, reintentando...");
      WiFi.reconnect();
    }
  }

  if (millis() - lastSchedulePoll > SCHEDULE_POLL_INTERVAL_MS) {
    obtenerHorariosYConfig();
    lastSchedulePoll = millis();
  }

  if (estado == IDLE && millis() - lastForceCheck > FORCE_CHECK_INTERVAL_MS) {
    lastForceCheck = millis();
    EstadoForceCheck fc = revisarDisparoForzado();

    if (fc.trigger) {
      // "Probar ahora": ÚNICO caso donde se dispensa sin pertenecer a
      // ningún horario, de inmediato.
      delay(200);
      scheduleActivoId = "";
      iniciarDispensacion();
    } else if (fc.schedulesDirty) {
      // Se creó o borró un horario: solo refrescamos la lista. NO se
      // dispensa aquí — revisarSiTocaDispensar() (más abajo, en el mismo
      // ciclo de loop()) es quien decide si el nuevo horario debe caer
      // ya (repetitivo sin ciclo previo) o si debe esperar su hora/intervalo.
      obtenerHorariosYConfig();
      lastSchedulePoll = millis();
    }
  }

  // Durante un ciclo en curso, el force-check sirve para enterarse rápido
  // si el usuario borró el horario activo (para cancelar el ciclo en ~2s
  // en vez de esperar el poll lento de 30s). Aquí NUNCA debe dispensarse
  // nada: solo importa refrescar la lista para detectar la cancelación.
  if (estado != IDLE && millis() - lastForceCheck > FORCE_CHECK_INTERVAL_MS) {
    lastForceCheck = millis();
    EstadoForceCheck fc = revisarDisparoForzado();
    if (fc.trigger || fc.schedulesDirty) {
      obtenerHorariosYConfig(); // esto detecta si el horario activo ya no existe y cancela
      lastSchedulePoll = millis();
    }
  }

  // Salvaguarda: si un ciclo queda "atascado" (por un fallo de red a mitad,
  // etc.) más del doble del tiempo máximo de espera, se fuerza el regreso a
  // IDLE y se apaga todo, para que la alarma nunca quede sonando sin fin.
  if (estado != IDLE && (millis() - marcaTiempoInicio) > ((unsigned long)maxWaitSeconds * 2000UL + 20000UL)) {
    Serial.println("Salvaguarda: ciclo atascado demasiado tiempo, reiniciando a IDLE.");
    apagarAlerta();
    enAlarmaIntermitente = false;
    dispensadorServo.write(0);
    estado = IDLE;
  }

  switch (estado) {
    case IDLE:
      revisarSiTocaDispensar();
      break;

    case AVISANDO:
      manejarAviso();
      break;

    case ESPERANDO_MANO:
      manejarEsperaMano();
      break;

    case ESPERANDO_RETIRO:
      manejarEsperaRetiro();
      break;
  }

  delay(100);
}

// =================================================================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);            // modo estación (cliente)
  WiFi.setAutoReconnect(true);   // reconecta solo si se cae
  WiFi.persistent(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado: " + WiFi.localIP().toString());

  // Confirmación visual: el LED parpadea 3 veces al conectar.
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, HIGH);
    delay(200);
    digitalWrite(PIN_LED, LOW);
    delay(200);
  }
}

// -----------------------------------------------------------------
// GET /api/esp32/schedules -> horarios activos + configuración
// -----------------------------------------------------------------
void obtenerHorariosYConfig() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(API_BASE_URL) + "/api/esp32/schedules");
  http.addHeader("x-api-key", DEVICE_API_KEY);
  http.addHeader("Connection", "close");
  http.setTimeout(5000);

  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<3072> doc;
    deserializeJson(doc, http.getString());

    scheduleCount = 0;
    for (JsonObject s : doc["schedules"].as<JsonArray>()) {
      if (scheduleCount >= 10) break;
      schedules[scheduleCount].id = s["id"].as<String>();

      if (!s["repeat_seconds"].isNull()) {
        schedules[scheduleCount].repeating = true;
        schedules[scheduleCount].repeatSeconds = s["repeat_seconds"];
        schedules[scheduleCount].createdAtEpoch = s["created_at_epoch"];
        schedules[scheduleCount].lastCycleEpoch = s["last_cycle_epoch"].isNull() ? -1 : (long)s["last_cycle_epoch"];
      } else {
        schedules[scheduleCount].repeating = false;
        schedules[scheduleCount].hour   = s["hour"];
        schedules[scheduleCount].minute = s["minute"];
        schedules[scheduleCount].second = s["second"];
      }
      scheduleCount++;
    }

    alertThresholdSeconds = doc["config"]["alert_threshold_seconds"] | 30;
    maxWaitSeconds = doc["config"]["max_wait_seconds"] | 120;

    Serial.printf("Horarios sincronizados: %d (alerta=%ds, max=%ds)\n",
                  scheduleCount, alertThresholdSeconds, maxWaitSeconds);

    // Re-ancla el próximo disparo de cada horario repetitivo YA rastreado
    // a la hora que confirma el SERVIDOR (last_cycle_epoch), no a la hora
    // local en que el ESP32 creyó que terminó el ciclo. Sin esto, el
    // siguiente ciclo podía arrancar un poco antes de tiempo: el ESP32
    // calculaba su reloj interno en el instante de mandar "taken"/"timeout",
    // pero el servidor graba last_cycle_at unos milisegundos-segundos
    // después (latencia del POST), así que su ancla siempre queda un poco
    // más tarde que la del ESP32. Al resincronizar aquí, el ESP32 adopta
    // siempre la ancla "oficial" del servidor, que es la misma que usa el
    // dashboard para mostrar la cuenta regresiva.
    for (int i = 0; i < scheduleCount; i++) {
      if (!schedules[i].repeating || schedules[i].lastCycleEpoch <= 0) continue;
      for (int j = 0; j < repeatTrackedCount; j++) {
        if (repeatIds[j] == schedules[i].id) {
          repeatNextFire[j] = schedules[i].lastCycleEpoch + schedules[i].repeatSeconds;
          break;
        }
      }
    }

    // Si estamos a mitad de un ciclo y el horario activo ya no existe
    // (se borró desde el dashboard), cancelamos el ciclo limpiamente:
    // apagamos alarma, cerramos el servo y volvemos a IDLE.
    if (estado != IDLE && scheduleActivoId.length() > 0) {
      bool sigueExistiendo = false;
      for (int i = 0; i < scheduleCount; i++) {
        if (schedules[i].id == scheduleActivoId) { sigueExistiendo = true; break; }
      }
      if (!sigueExistiendo) {
        Serial.println("El horario activo fue eliminado: cancelando ciclo en curso.");
        detenerAlarmaIntermitente();
        dispensadorServo.write(0);
        estado = IDLE;
      }
    }
  } else {
    Serial.printf("Error obteniendo horarios: HTTP %d\n", code);
  }
  http.end();
}

// -----------------------------------------------------------------
// POST /api/esp32/event -> reporta dispensed / alert / taken / timeout
// -----------------------------------------------------------------
// -----------------------------------------------------------------
// GET /api/esp32/force-check -> ¿el dashboard pidió una prueba manual?
// Endpoint liviano pensado para consultarse cada 1s.
// -----------------------------------------------------------------
EstadoForceCheck revisarDisparoForzado() {
  EstadoForceCheck resultado = { false, false };
  if (WiFi.status() != WL_CONNECTED) return resultado;

  HTTPClient http;
  http.begin(String(API_BASE_URL) + "/api/esp32/force-check");
  http.addHeader("x-api-key", DEVICE_API_KEY);
  http.addHeader("Connection", "close"); // cierra la conexión al terminar (no acumula sockets)
  http.setTimeout(4000); // no esperar más de 4s; evita que el loop se cuelgue

  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<128> doc;
    deserializeJson(doc, http.getString());
    resultado.trigger = doc["trigger"] | false;
    resultado.schedulesDirty = doc["schedules_dirty"] | false;
  }
  http.end();
  return resultado;
}

void enviarEvento(const char* tipo, String scheduleId, long delaySeconds) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(API_BASE_URL) + "/api/esp32/event");
  http.addHeader("x-api-key", DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(5000);

  StaticJsonDocument<256> doc;
  doc["type"] = tipo;
  if (scheduleId.length() > 0) doc["schedule_id"] = scheduleId;
  if (delaySeconds >= 0) doc["delay_seconds"] = delaySeconds;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  Serial.printf("Evento '%s' enviado -> HTTP %d\n", tipo, code);
  http.end();
}

// Igual que enviarEvento pero con un mensaje personalizado (para las
// alertas especiales de la primera pastilla).
void enviarEventoConMensaje(const char* tipo, String scheduleId, const char* mensaje) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(API_BASE_URL) + "/api/esp32/event");
  http.addHeader("x-api-key", DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.setTimeout(5000);

  StaticJsonDocument<384> doc;
  doc["type"] = tipo;
  if (scheduleId.length() > 0) doc["schedule_id"] = scheduleId;
  doc["message"] = mensaje;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  Serial.printf("Evento '%s' (con mensaje) enviado -> HTTP %d\n", tipo, code);
  http.end();
}

// Revisa si algún horario toca disparar en este instante (por ejemplo,
// la primera pastilla inmediata de un horario recién creado). Si sí,
// lo arranca y devuelve true. Se usa tras un force-trigger.
bool dispararHorarioPendiente() {
  time_t ahora = time(nullptr);
  struct tm timeinfo;
  bool horaOk = getLocalTime(&timeinfo);

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].repeating) {
      if (tocaDisparoRepetitivo(schedules[i], ahora)) {
        scheduleActivoId = schedules[i].id;
        iniciarDispensacion();
        return true;
      }
    } else if (horaOk) {
      bool coincide = timeinfo.tm_hour == schedules[i].hour &&
                      timeinfo.tm_min  == schedules[i].minute &&
                      timeinfo.tm_sec  == schedules[i].second;
      if (coincide && ultimoMinutoDisparado != timeinfo.tm_min) {
        ultimoMinutoDisparado = timeinfo.tm_min;
        scheduleActivoId = schedules[i].id;
        iniciarDispensacion();
        return true;
      }
    }
  }
  return false;
}

// -----------------------------------------------------------------
// Etapa 1: revisa si toca dispensar, ya sea por hora fija o por
// intervalo de repetición.
// -----------------------------------------------------------------
void revisarSiTocaDispensar() {
  time_t ahora = time(nullptr);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].repeating) {
      if (tocaDisparoRepetitivo(schedules[i], ahora)) {
        scheduleActivoId = schedules[i].id;
        iniciarDispensacion();
        return;
      }
    } else {
      bool coincide = timeinfo.tm_hour == schedules[i].hour &&
                      timeinfo.tm_min  == schedules[i].minute &&
                      timeinfo.tm_sec  == schedules[i].second;

      // evita disparar más de una vez dentro del mismo minuto
      if (coincide && ultimoMinutoDisparado != timeinfo.tm_min) {
        ultimoMinutoDisparado = timeinfo.tm_min;
        scheduleActivoId = schedules[i].id;
        iniciarDispensacion();
        return;
      }
    }
  }
}

// Revisa si toca disparar un horario repetitivo.
//  · Primera vez (nunca ha dispensado, sin ciclo previo): dispara de
//    inmediato al ver el horario por primera vez.
//  · Siguientes: dispara cuando pasan repeatSeconds contados DESDE el
//    retiro de la pastilla anterior (registrarProximoDisparo lo fija).
// El conteo del próximo disparo nunca corre mientras la pastilla
// anterior sigue en la bandeja.
bool tocaDisparoRepetitivo(Schedule &s, time_t ahora) {
  int idx = -1;
  for (int i = 0; i < repeatTrackedCount; i++) {
    if (repeatIds[i] == s.id) { idx = i; break; }
  }
  if (idx == -1 && repeatTrackedCount < 10) {
    idx = repeatTrackedCount++;
    repeatIds[idx] = s.id;
    if (s.lastCycleEpoch > 0) {
      // Ya hubo al menos un ciclo (ej: el ESP32 se reinició): el próximo
      // disparo se cuenta desde el último retiro registrado.
      repeatNextFire[idx] = s.lastCycleEpoch + s.repeatSeconds;
    } else {
      // Nunca ha dispensado: la PRIMERA pastilla cae de inmediato.
      repeatNextFire[idx] = ahora;
    }
  }
  if (idx == -1) return false; // sin espacio para rastrear (no debería pasar, máx 10)

  return ahora >= repeatNextFire[idx];
}

// Se llama al terminar un ciclo (pastilla retirada o timeout). Si el
// horario activo era repetitivo, reinicia su cuenta desde este momento.
void registrarProximoDisparo(String scheduleId, time_t desde) {
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].id == scheduleId && schedules[i].repeating) {
      for (int j = 0; j < repeatTrackedCount; j++) {
        if (repeatIds[j] == scheduleId) {
          repeatNextFire[j] = desde + schedules[i].repeatSeconds;
          return;
        }
      }
      if (repeatTrackedCount < 10) {
        repeatIds[repeatTrackedCount] = scheduleId;
        repeatNextFire[repeatTrackedCount] = desde + schedules[i].repeatSeconds;
        repeatTrackedCount++;
      }
      return;
    }
  }
}

// Determina si el horario activo es repetitivo y está en su PRIMERA
// pastilla (nunca ha completado un ciclo: sin last_cycle previo y sin
// retiro registrado localmente en esta sesión).
bool esHorarioEnPrimeraPastilla(String id) {
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].id == id) {
      if (!schedules[i].repeating) return false;         // los de "una vez" no aplican
      return schedules[i].lastCycleEpoch <= 0;           // aún no ha habido ningún ciclo
    }
  }
  return false;
}

void iniciarDispensacion() {
  Serial.println("Dispensando: servo a 90°, buzzer + LED por 3s");

  // ¿Este ciclo es la primera pastilla? (para el mensaje especial en las alertas)
  esPrimeraPastilla = esHorarioEnPrimeraPastilla(scheduleActivoId);
  alertasEnviadas = 0;
  enAlarmaIntermitente = false;

  dispensadorServo.write(90);   // abre el mecanismo (suelta la pastilla)
  activarAlerta();               // buzzer + LED encendidos
  estado = AVISANDO;
  marcaTiempoInicio = millis();
}

// -----------------------------------------------------------------
// Aviso de dispensación: buzzer + LED suenan 3s mientras el servo
// está en 90° (suelta la pastilla). Al terminar, el servo vuelve a 0°,
// se registra el evento "dispensed" (la pastilla YA está disponible),
// y se pasa a esperar que la mano de la persona se acerque a tomarla.
// -----------------------------------------------------------------
void manejarAviso() {
  if (millis() - marcaTiempoInicio >= DURACION_AVISO_MS) {
    apagarAlerta();              // apaga buzzer + LED
    dispensadorServo.write(0);   // cierra el mecanismo
    delay(150);                  // deja que el servo termine antes de usar la red
    Serial.println("Pastilla dispensada -> evento 'dispensed'. Esperando la mano de la persona.");
    enviarEvento("dispensed", scheduleActivoId, -1);
    marcaTiempoInicio = millis();
    alertaEnviada = false;
    // La primera alarma intermitente toca a los 30s (para toda pastilla).
    marcaProximaAlerta = millis() + (unsigned long)alertThresholdSeconds * 1000;
    estado = ESPERANDO_MANO;
  }
}

// -----------------------------------------------------------------
// Etapa 3: espera a que el sensor detecte la MANO de la persona.
// Cada 30s (alertThreshold) sin tomar la pastilla, suena una alarma
// intermitente de 5s (bip-bip) y luego para. Se repite cada 30s de
// forma indefinida hasta que la persona tome la pastilla o se alcance
// el tiempo máximo (timeout).
//
// La primera pastilla del ciclo, además, en sus 3 primeras alertas
// envía el mensaje especial "¡El paciente no ha empezado tratamiento!".
// -----------------------------------------------------------------
void manejarEsperaMano() {
  unsigned long transcurridoSeg = (millis() - marcaTiempoInicio) / 1000;
  bool manoDetectada = leerSensorIR();

  if (manoDetectada) {
    Serial.println("Mano detectada: la persona está tomando la pastilla.");
    detenerAlarmaIntermitente();
    estado = ESPERANDO_RETIRO;
    return;
  }

  // ¿Toca iniciar una nueva alarma intermitente? (cada 30s)
  if (!enAlarmaIntermitente && millis() >= marcaProximaAlerta) {
    enAlarmaIntermitente = true;
    marcaInicioAlarma = millis();
    Serial.printf("Alerta #%d: alarma intermitente 5s (pastilla sin tomar)\n", alertasEnviadas + 1);
  }

  if (enAlarmaIntermitente) {
    unsigned long enAlarma = millis() - marcaInicioAlarma;

    if (enAlarma < ALARMA_INTERMITENTE_MS) {
      // Patrón bip-bip: alterna buzzer + LED cada 250ms
      if (millis() - ultimoParpadeoBuzzer >= 250) {
        ultimoParpadeoBuzzer = millis();
        buzzerEncendidoParpadeo = !buzzerEncendidoParpadeo;
        digitalWrite(PIN_BUZZER, buzzerEncendidoParpadeo ? HIGH : LOW);
        digitalWrite(PIN_LED, buzzerEncendidoParpadeo ? HIGH : LOW);
      }
    } else {
      // Terminaron los 5s: apaga la alarma y envía la notificación.
      digitalWrite(PIN_BUZZER, LOW);
      digitalWrite(PIN_LED, LOW);
      enAlarmaIntermitente = false;
      alertasEnviadas++;

      // Primera pastilla del ciclo, primeras 3 alertas: mensaje especial.
      if (esPrimeraPastilla && alertasEnviadas <= MAX_ALERTAS_PRIMERA) {
        Serial.println("-> notificación: ¡El paciente no ha empezado tratamiento!");
        enviarEventoConMensaje("alert", scheduleActivoId, MSG_PRIMERA);
      } else {
        Serial.println("-> evento 'alert'");
        enviarEvento("alert", scheduleActivoId, transcurridoSeg);
      }

      // La siguiente alarma toca en otros 30s.
      marcaProximaAlerta = millis() + (unsigned long)alertThresholdSeconds * 1000;
    }
  }

  // Si pasa el tiempo máximo sin que nadie tome la pastilla -> timeout.
  if (transcurridoSeg >= (unsigned long)maxWaitSeconds) {
    Serial.println("Límite máximo alcanzado sin tomar la pastilla -> evento 'timeout'");
    enviarEvento("timeout", scheduleActivoId, transcurridoSeg);
    registrarProximoDisparo(scheduleActivoId, time(nullptr)); // estimado local, por si el refresco de abajo falla
    detenerAlarmaIntermitente();
    estado = IDLE;
    // Recién aquí el servidor ya guardó last_cycle_at (enviarEvento espera
    // la respuesta del POST). Refrescamos ya mismo -sin esperar el poll de
    // 30s- para reemplazar el estimado local por la ancla oficial del
    // servidor y que el siguiente disparo caiga exactamente donde debe.
    obtenerHorariosYConfig();
    lastSchedulePoll = millis();
  }
}

// Apaga la alarma intermitente y limpia sus banderas.
void detenerAlarmaIntermitente() {
  enAlarmaIntermitente = false;
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED, LOW);
}

// -----------------------------------------------------------------
// Etapa 4: la mano ya está en el sensor. Cuando se retira (el sensor
// deja de detectarla), significa que la persona se llevó la pastilla.
// -----------------------------------------------------------------
void manejarEsperaRetiro() {
  bool manoPresente = leerSensorIR();

  if (!manoPresente) {
    // La mano se fue con la pastilla.
    Serial.println("La mano se retiró: pastilla tomada -> evento 'taken'");
    enviarEvento("taken", scheduleActivoId, -1);
    registrarProximoDisparo(scheduleActivoId, time(nullptr)); // estimado local, por si el refresco de abajo falla
    apagarAlerta();
    estado = IDLE;
    // Igual que en el timeout: refrescamos ya mismo para adoptar la ancla
    // oficial del servidor (last_cycle_epoch) en vez de quedarnos con la
    // hora local del ESP32, que siempre queda un poco antes.
    obtenerHorariosYConfig();
    lastSchedulePoll = millis();
  }
}

// -----------------------------------------------------------------
bool leerSensorIR() {
  int val = digitalRead(PIN_IR);
  return IR_ACTIVE_LOW ? (val == LOW) : (val == HIGH);
}

void activarAlerta() {
  digitalWrite(PIN_LED, HIGH);
  digitalWrite(PIN_BUZZER, HIGH);
}

void apagarAlerta() {
  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_BUZZER, LOW);
}
