/*
 * Dispensador de medicamentos IoT — ESP32
 * -----------------------------------------------------------
 * Implementa la lógica de Etapas 1-4 descrita en el documento
 * "Descripción de función básica de aplicativo y dispositivo":
 *
 *  Etapa 1: espera la hora programada -> marca 0s
 *  Etapa 2: sensor detecta medicamento dispensado -> marca 1s,
 *           se envía evento "dispensed"
 *  Etapa 3: si a los 30s (alert_threshold) sigue en bandeja ->
 *           evento "alert" (buzzer + LED)
 *  Etapa 4: cuando el sensor deja de detectar el medicamento ->
 *           evento "taken" con el tiempo transcurrido.
 *           Si se excede max_wait_seconds -> evento "timeout".
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
const char* WIFI_SSID     = "TU_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD";

// URL de tu servicio en Render, sin slash final
const char* API_BASE_URL  = "https://tu-servicio.onrender.com";
const char* DEVICE_API_KEY = "PEGAR_AQUI_LA_device_api_key_DE_device_config";

// Pines
const int PIN_SERVO   = 13;
const int PIN_IR      = 34;   // sensor infrarrojo (o de contacto)
const int PIN_BUZZER  = 25;
const int PIN_LED     = 26;

const bool IR_ACTIVE_LOW = true; // ajustar según el sensor usado

// Servidor NTP (Colombia = UTC-5, sin horario de verano)
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -5 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

// ---------------- ESTADO GLOBAL ----------------
Servo dispensadorServo;

struct Schedule {
  String id;
  int hour, minute, second;
};

Schedule schedules[10];
int scheduleCount = 0;

int alertThresholdSeconds = 30;   // Etapa 3 (config viene del backend)
int maxWaitSeconds = 120;         // límite demo (real: 4-6h), config del backend

unsigned long lastSchedulePoll = 0;
const unsigned long SCHEDULE_POLL_INTERVAL_MS = 30000; // cada 30s

// Máquina de estados
enum EstadoDispensador { IDLE, DISPENSANDO, ESPERANDO_RETIRO };
EstadoDispensador estado = IDLE;

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
  if (millis() - lastSchedulePoll > SCHEDULE_POLL_INTERVAL_MS) {
    obtenerHorariosYConfig();
    lastSchedulePoll = millis();
  }

  switch (estado) {
    case IDLE:
      revisarSiTocaDispensar();
      break;

    case DISPENSANDO:
      manejarDispensacion();
      break;

    case ESPERANDO_RETIRO:
      manejarEsperaRetiro();
      break;
  }

  delay(100);
}

// =================================================================
void conectarWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado: " + WiFi.localIP().toString());
}

// -----------------------------------------------------------------
// GET /api/esp32/schedules -> horarios activos + configuración
// -----------------------------------------------------------------
void obtenerHorariosYConfig() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(API_BASE_URL) + "/api/esp32/schedules");
  http.addHeader("x-api-key", DEVICE_API_KEY);

  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, http.getString());

    scheduleCount = 0;
    for (JsonObject s : doc["schedules"].as<JsonArray>()) {
      if (scheduleCount >= 10) break;
      schedules[scheduleCount].id     = s["id"].as<String>();
      schedules[scheduleCount].hour   = s["hour"];
      schedules[scheduleCount].minute = s["minute"];
      schedules[scheduleCount].second = s["second"];
      scheduleCount++;
    }

    alertThresholdSeconds = doc["config"]["alert_threshold_seconds"] | 30;
    maxWaitSeconds = doc["config"]["max_wait_seconds"] | 120;

    Serial.printf("Horarios sincronizados: %d (alerta=%ds, max=%ds)\n",
                  scheduleCount, alertThresholdSeconds, maxWaitSeconds);
  } else {
    Serial.printf("Error obteniendo horarios: HTTP %d\n", code);
  }
  http.end();
}

// -----------------------------------------------------------------
// POST /api/esp32/event -> reporta dispensed / alert / taken / timeout
// -----------------------------------------------------------------
void enviarEvento(const char* tipo, String scheduleId, long delaySeconds) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(API_BASE_URL) + "/api/esp32/event");
  http.addHeader("x-api-key", DEVICE_API_KEY);
  http.addHeader("Content-Type", "application/json");

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

// -----------------------------------------------------------------
// Etapa 1: revisa si la hora actual coincide con algún horario activo
// -----------------------------------------------------------------
void revisarSiTocaDispensar() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  for (int i = 0; i < scheduleCount; i++) {
    bool coincide = timeinfo.tm_hour == schedules[i].hour &&
                    timeinfo.tm_min  == schedules[i].minute &&
                    timeinfo.tm_sec  == schedules[i].second;

    // evita disparar más de una vez dentro del mismo minuto
    if (coincide && ultimoMinutoDisparado != timeinfo.tm_min) {
      ultimoMinutoDisparado = timeinfo.tm_min;
      scheduleActivoId = schedules[i].id;
      iniciarDispensacion();
      break;
    }
  }
}

void iniciarDispensacion() {
  Serial.println("Marca 0s: iniciando dispensación");
  dispensadorServo.write(90); // abre el mecanismo
  estado = DISPENSANDO;
  marcaTiempoInicio = millis(); // referencia temporal para timeout de espera de detección
}

// -----------------------------------------------------------------
// Etapa 2: espera a que el sensor detecte el medicamento en la bandeja
// -----------------------------------------------------------------
void manejarDispensacion() {
  bool detectado = leerSensorIR();

  if (detectado) {
    dispensadorServo.write(0); // cierra el mecanismo
    Serial.println("Marca 1s: medicamento detectado -> evento 'dispensed'");
    marcaTiempoInicio = millis(); // reinicia la marca de tiempo de retiro
    alertaEnviada = false;
    enviarEvento("dispensed", scheduleActivoId, -1);
    estado = ESPERANDO_RETIRO;
  } else if (millis() - marcaTiempoInicio > 10000) {
    // el mecanismo no logró dispensar en 10s: aborta y vuelve a IDLE
    Serial.println("No se detectó dispensación, volviendo a IDLE");
    dispensadorServo.write(0);
    estado = IDLE;
  }
}

// -----------------------------------------------------------------
// Etapas 3 y 4: espera a que el paciente retire el medicamento
// -----------------------------------------------------------------
void manejarEsperaRetiro() {
  unsigned long transcurridoSeg = (millis() - marcaTiempoInicio) / 1000;
  bool presente = leerSensorIR();

  if (!presente) {
    // Opción 1 u Opción 2 del documento: se retiró el medicamento
    Serial.printf("Medicamento retirado. Tiempo transcurrido: %lus -> evento 'taken'\n", transcurridoSeg);
    enviarEvento("taken", scheduleActivoId, transcurridoSeg);
    apagarAlerta();
    estado = IDLE;
    return;
  }

  if (!alertaEnviada && transcurridoSeg >= (unsigned long)alertThresholdSeconds) {
    Serial.println("Marca 30s: medicamento sigue en bandeja -> evento 'alert'");
    enviarEvento("alert", scheduleActivoId, transcurridoSeg);
    activarAlerta();
    alertaEnviada = true;
  }

  if (transcurridoSeg >= (unsigned long)maxWaitSeconds) {
    Serial.println("Límite máximo alcanzado -> evento 'timeout'");
    enviarEvento("timeout", scheduleActivoId, transcurridoSeg);
    apagarAlerta();
    estado = IDLE;
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
