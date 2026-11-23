
#include <WiFi.h>
#include <HTTPClient.h>
#include <cJSON.h>

#if __has_include("SmartBerryApiConfig.h")
#include "SmartBerryApiConfig.h"
#else
#define SMARTBERRY_API_URL ""
#define SMARTBERRY_API_KEY ""
#endif

#ifndef SMARTBERRY_ESP32_ID
#define SMARTBERRY_ESP32_ID ""
#endif
#ifndef SMARTBERRY_WEB_PANEL_ENABLED
#define SMARTBERRY_WEB_PANEL_ENABLED 0
#endif
#include <WebServer.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

// ======================================
// CONFIGURACOES WIFI
// ======================================

const char* ssid = "SmartBerry";
const char* password = "teste123";

const unsigned long WIFI_TIMEOUT = 15000;
const unsigned long WIFI_RETRY = 15000;
const unsigned long DHT_INTERVAL = 2500;

// ======================================
// PINOS E RELES
// ======================================

#define PUMP_PIN 25
#define VENT_PIN 27
#define DHT_PIN 4
#define DHT_TYPE DHT22

// Bomba: ativa em HIGH
#define PUMP_ON HIGH
#define PUMP_OFF LOW

// Ventilacao: ativa em LOW
#define VENT_ON LOW
#define VENT_OFF HIGH

// ======================================
// COMPONENTES
// ======================================

DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x3F, 16, 2);
WebServer server(80);
Preferences prefs;

// ======================================
// VARIAVEIS
// ======================================

bool isPumpOn = false;
bool isVentOn = false;

bool wifiConnected = false;
bool serverStarted = false;
bool ntpConfigured = false;

float currentTemp = NAN;
float currentHumidity = NAN;
bool validSensorSample = false;
bool remotePumpTimed = false;
unsigned long remotePumpStart = 0;
unsigned long remotePumpDuration = 0;
unsigned long ultimaParadaSolicitada = 0;
bool remoteVentTimed = false;
unsigned long remoteVentStart = 0;
unsigned long remoteVentDuration = 0;
unsigned long lastTelemetry = 0;
unsigned long lastCommandPoll = 0;
unsigned long telemetryInterval = 5000;
bool telemetryChanged = true;
String pendingAck;
String pendingOutcome;
String apiDeviceId = SMARTBERRY_ESP32_ID;
unsigned long lastRegistrationAttempt = 0;

unsigned long lastDhtRead = 0;
unsigned long lastWifiAttempt = 0;

// Controle automatico da ventilacao
bool autoVent = false;
float maxTemp = 30.0;
const float HYSTERESIS = 1.0;

// Agendamento de irrigacao
bool scheduleEnabled = false;
uint8_t scheduleHour = 8;
uint8_t scheduleMinute = 0;
uint16_t scheduleSeconds = 15;

bool scheduledPumpRunning = false;
unsigned long scheduledPumpStart = 0;

int32_t lastRunDay = 0;

const time_t MIN_VALID_TIME = 1704067200;

// Declaracoes
void handleState();
void checkTemperatureControl();

// ======================================
// DISPLAY LCD
// ======================================

void updateLCD() {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Temp: ");

  if (isnan(currentTemp)) {
    lcd.print("--.-");
  } else {
    lcd.print(currentTemp, 1);
  }

  lcd.print((char)223);
  lcd.print("C");

  lcd.setCursor(0, 1);

  if (isVentOn) {
    lcd.print("Ventilando...");
  } else {
    lcd.print("Vent. Desligada");
  }
}

// ======================================
// CONTROLE DOS RELES
// ======================================

void setPump(bool on) {
  remotePumpTimed = false;
  telemetryChanged |= isPumpOn != on;
  isPumpOn = on;

  digitalWrite(
    PUMP_PIN,
    on ? PUMP_ON : PUMP_OFF
  );

  Serial.println(
    on ? "Bomba LIGADA" : "Bomba DESLIGADA"
  );
}

void setVent(bool on) {
  if (isVentOn == on) return;

  telemetryChanged = true;
  isVentOn = on;

  digitalWrite(
    VENT_PIN,
    on ? VENT_ON : VENT_OFF
  );

  updateLCD();

  Serial.println(
    on ? "Ventilacao LIGADA" :
         "Ventilacao DESLIGADA"
  );
}

// ======================================
// CONTROLE AUTOMATICO TEMPERATURA
// ======================================

void checkTemperatureControl() {
  if (!SMARTBERRY_WEB_PANEL_ENABLED || !autoVent || isnan(currentTemp)) {
    return;
  }

  if (!isVentOn && currentTemp >= maxTemp) {
    setVent(true);
  }

  else if (
    isVentOn &&
    currentTemp <= maxTemp - HYSTERESIS
  ) {
    setVent(false);
  }
}

// ======================================
// MEMORIA PERMANENTE
// ======================================

void loadSettings() {
  prefs.begin("smartberry", false);

  autoVent = prefs.getBool("autoVent", false);
  maxTemp = prefs.getFloat("maxTemp", 30.0);

  scheduleEnabled =
    prefs.getBool("enabled", false);

  scheduleHour =
    prefs.getUChar("hour", 8);

  scheduleMinute =
    prefs.getUChar("minute", 0);

  scheduleSeconds =
    prefs.getUShort("seconds", 15);

  lastRunDay =
    prefs.getInt("lastday", 0);

  if (
    !isfinite(maxTemp) ||
    maxTemp < 10 ||
    maxTemp > 60
  ) {
    maxTemp = 30.0;
  }

  if (
    scheduleHour > 23 ||
    scheduleMinute > 59 ||
    scheduleSeconds < 1 ||
    scheduleSeconds > 300
  ) {
    scheduleEnabled = false;
    scheduleHour = 8;
    scheduleMinute = 0;
    scheduleSeconds = 15;
  }
}

// ======================================
// RELOGIO
// ======================================

bool readClock(struct tm &local) {
  time_t now = time(nullptr);

  if (now < MIN_VALID_TIME) {
    return false;
  }

  return localtime_r(&now, &local) != nullptr;
}

int32_t dayKey(const struct tm &t) {
  return (t.tm_year + 1900) * 10000 +
         (t.tm_mon + 1) * 100 +
         t.tm_mday;
}

// Sincronizacao pelo navegador
void handleTime() {
  if (!server.hasArg("epoch")) {
    server.send(
      400, "application/json",
      "{\"error\":\"Horario ausente\"}"
    );
    return;
  }

  String value = server.arg("epoch");

  char* endPtr = nullptr;

  long long epoch =
    strtoll(value.c_str(), &endPtr, 10);

  if (
    value.length() == 0 ||
    *endPtr != '\0' ||
    epoch < 1704067200LL ||
    epoch > 4102444800LL
  ) {
    server.send(
      400, "application/json",
      "{\"error\":\"Horario invalido\"}"
    );
    return;
  }

  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;

  settimeofday(&tv, nullptr);

  struct tm local;

  if (readClock(local)) {
    Serial.printf(
      "Relogio sincronizado: %02d:%02d:%02d\n",
      local.tm_hour,
      local.tm_min,
      local.tm_sec
    );
  }

  server.send(
    200, "application/json",
    "{\"ok\":true}"
  );
}

// ======================================
// AGENDAMENTO DA BOMBA
// ======================================

void registrarParadaSolicitada() {
  ultimaParadaSolicitada = millis();
  struct tm local;
  if (scheduleEnabled && readClock(local)
      && local.tm_hour == scheduleHour && local.tm_min == scheduleMinute) {
    lastRunDay = dayKey(local);
    prefs.putInt("lastday", lastRunDay);
  }
}

void checkPumpSchedule() {
  if (!SMARTBERRY_WEB_PANEL_ENABLED) return;

  // Finaliza uma irrigacao em andamento
  if (scheduledPumpRunning) {

    unsigned long duration =
      (unsigned long)scheduleSeconds * 1000UL;

    if (
      millis() - scheduledPumpStart >= duration
    ) {
      scheduledPumpRunning = false;
      setPump(false);

      Serial.println(
        "Irrigacao agendada concluida."
      );
    }

    return;
  }

  if (!scheduleEnabled || isPumpOn) {
    return;
  }

  struct tm local;

  // Nao aciona sem relogio sincronizado
  if (!readClock(local)) {
    return;
  }

  int32_t today = dayKey(local);

  if (
    local.tm_hour == scheduleHour &&
    local.tm_min == scheduleMinute &&
    today != lastRunDay
  ) {

    // Marca a execucao do dia
    lastRunDay = today;

    prefs.putInt("lastday", lastRunDay);

    scheduledPumpRunning = true;
    scheduledPumpStart = millis();

    setPump(true);

    Serial.printf(
      "Irrigacao agendada iniciada %02d:%02d\n",
      local.tm_hour,
      local.tm_min
    );

    Serial.printf(
      "Duracao: %u segundos\n",
      scheduleSeconds
    );
  }
}

// ======================================
// API: AGENDAMENTO
// ======================================

void handleSchedule() {

  if (
    !server.hasArg("enabled") ||
    !server.hasArg("hour") ||
    !server.hasArg("minute") ||
    !server.hasArg("seconds")
  ) {
    server.send(
      400, "application/json",
      "{\"error\":\"Parametros ausentes\"}"
    );
    return;
  }

  String en = server.arg("enabled");
  String hh = server.arg("hour");
  String mm = server.arg("minute");
  String ss = server.arg("seconds");

  auto validDigits = [](const String &v) {
    if (v.length() == 0) return false;

    for (size_t i = 0; i < v.length(); i++) {
      if (!isDigit(v[i])) return false;
    }

    return true;
  };

  if (
    (en != "0" && en != "1") ||
    !validDigits(hh) ||
    !validDigits(mm) ||
    !validDigits(ss) ||
    hh.length() > 2 ||
    mm.length() > 2 ||
    ss.length() > 3
  ) {
    server.send(
      400, "application/json",
      "{\"error\":\"Valores invalidos\"}"
    );
    return;
  }

  int hour = hh.toInt();
  int minute = mm.toInt();
  int seconds = ss.toInt();

  if (
    hour > 23 ||
    minute > 59 ||
    seconds < 1 ||
    seconds > 300
  ) {
    server.send(
      400, "application/json",
      "{\"error\":\"Agendamento invalido\"}"
    );
    return;
  }

  bool enabled = (en == "1");

  bool changed =
    scheduleEnabled != enabled ||
    scheduleHour != hour ||
    scheduleMinute != minute;

  scheduleEnabled = enabled;
  scheduleHour = hour;
  scheduleMinute = minute;
  scheduleSeconds = seconds;

  // Permite novo teste ao mudar o horario
  if (changed) {
    lastRunDay = 0;
    prefs.putInt("lastday", 0);
  }

  prefs.putBool("enabled", scheduleEnabled);
  prefs.putUChar("hour", scheduleHour);
  prefs.putUChar("minute", scheduleMinute);
  prefs.putUShort("seconds", scheduleSeconds);

  if (!scheduleEnabled && scheduledPumpRunning) {
    scheduledPumpRunning = false;
    setPump(false);
  }

  Serial.printf(
    "Agendamento: %s - %02u:%02u - %u segundos\n",
    scheduleEnabled ? "ATIVO" : "INATIVO",
    scheduleHour,
    scheduleMinute,
    scheduleSeconds
  );

  handleState();
}

// ======================================
// INTERFACE WEB
// ======================================

void handleRoot() {

  static const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">

<head>
<meta charset="UTF-8">
<meta name="viewport"
      content="width=device-width,initial-scale=1">

<meta name="theme-color" content="#32a83c">

<title>SmartBerry360° | Minha Estufa</title>

<style>

:root {
  --green: #32a83c;
  --green-dark: #247f2e;
  --purple: #9224ca;
  --bg: #fbfaf5;
  --ink: #202421;
  --muted: #777d76;
  --border: #e9ece6;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  background: var(--bg);
  color: var(--ink);
  font-family: Arial, Helvetica, sans-serif;
  font-size: 15px;
}

header {
  position: relative;
  overflow: hidden;
  background: white;
  border-bottom: 1px solid var(--border);
}

.orb {
  position: absolute;
  border-radius: 50%;
  pointer-events: none;
}

.o1 {
  width: 180px;
  height: 180px;
  left: -80px;
  top: -120px;
  background: var(--purple);
}

.o2 {
  width: 170px;
  height: 170px;
  left: 20px;
  top: -132px;
  background: #58bc5b;
  opacity: .85;
}

.o3 {
  width: 160px;
  height: 160px;
  right: -85px;
  top: -95px;
  background: #61be61;
}

.o4 {
  width: 120px;
  height: 120px;
  right: 16px;
  top: -91px;
  background: var(--purple);
}

.header-inner {
  max-width: 1000px;
  margin: auto;
  position: relative;
  padding: 36px 24px 28px;
  display: flex;
  gap: 12px;
  align-items: center;
  justify-content: space-between;
}

.brand {
  display: flex;
  align-items: center;
  gap: 12px;
}

.logo {
  width: 45px;
  height: 45px;
  border: 1px solid #be81d9;
  border-radius: 50%;
  display: grid;
  place-items: center;
  color: var(--green);
  background: white;
  font-size: 26px;
}

.brand-name {
  font-size: 24px;
  font-weight: 800;
  color: var(--purple);
}

.brand-name span {
  color: var(--green);
}

.tag {
  font-size: 11px;
  color: var(--muted);
  margin-top: 3px;
}

.connection {
  border-radius: 20px;
  padding: 9px 13px;
  background: #ebf9ec;
  color: var(--green-dark);
  font-weight: bold;
  font-size: 12px;
}

.connection.off {
  background: #fff0eb;
  color: #a93b26;
}

main {
  max-width: 1000px;
  margin: auto;
  padding: 30px 24px 42px;
}

h1 {
  font-size: 27px;
  margin: 0 0 7px;
}

.lead {
  color: var(--muted);
  margin: 0 0 25px;
  line-height: 1.5;
  font-size: 14px;
}

.hero {
  background: var(--green);
  border-radius: 16px;
  padding: 21px 25px;
  color: white;
  margin-bottom: 20px;
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.hero strong {
  display: block;
  font-size: 18px;
  margin-bottom: 5px;
}

.hero small {
  opacity: .92;
  font-size: 12px;
}

.hero-mark {
  font-size: 32px;
}

.grid {
  display: grid;
  grid-template-columns: repeat(2,minmax(0,1fr));
  gap: 18px;
}

.card {
  background: white;
  border: 1px solid var(--border);
  border-radius: 16px;
  padding: 22px;
  box-shadow: 0 5px 18px #1c361008;
}

.card-title {
  font-weight: bold;
  font-size: 15px;
  margin: 0 0 17px;
}

.sensor {
  display: flex;
  align-items: center;
  gap: 15px;
}

.symbol {
  height: 57px;
  width: 57px;
  border-radius: 16px;
  background: #e6f9e8;
  display: grid;
  place-items: center;
  color: var(--green);
  font-size: 28px;
  flex-shrink: 0;
}

.symbol.purple {
  background: #f6eafa;
  color: var(--purple);
}

.sensor-label {
  font-size: 12px;
  color: var(--muted);
}

.reading {
  font-size: 36px;
  font-weight: 800;
  margin-top: 4px;
}

.unit {
  font-size: 17px;
  font-weight: normal;
  color: var(--muted);
}

.full {
  grid-column: 1 / -1;
}

.controls {
  display: grid;
  grid-template-columns: repeat(2,minmax(0,1fr));
  gap: 14px;
}

.control {
  border: 1px solid var(--border);
  padding: 17px;
  border-radius: 12px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.device {
  display: flex;
  align-items: center;
  gap: 12px;
}

.device .symbol {
  width: 42px;
  height: 42px;
  font-size: 21px;
  border-radius: 50%;
}

.device strong {
  font-size: 14px;
}

.state {
  color: var(--muted);
  font-size: 12px;
  margin-top: 4px;
}

button {
  background: var(--green);
  border: 0;
  color: white;
  padding: 12px 15px;
  min-width: 95px;
  border-radius: 9px;
  cursor: pointer;
  font-weight: bold;
}

button:hover {
  background: var(--green-dark);
}

button.active {
  background: var(--purple);
}

button:disabled {
  opacity: .5;
  cursor: not-allowed;
}

.info {
  margin-top: 18px;
  color: var(--muted);
  font-size: 12px;
  display: flex;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
}

.warning {
  display: none;
  margin-top: 14px;
  padding: 13px;
  background: #fff1e8;
  color: #99432e;
  border-radius: 10px;
  font-size: 13px;
}

.warning.show {
  display: block;
}

.settings,
.schedule-grid {
  display: flex;
  gap: 14px;
  align-items: end;
  flex-wrap: wrap;
}

.field {
  display: flex;
  gap: 7px;
  flex-direction: column;
  font-size: 12px;
  color: var(--muted);
}

.field input {
  padding: 11px 12px;
  border: 1px solid var(--border);
  border-radius: 9px;
  font-size: 16px;
  max-width: 155px;
  color: var(--ink);
}

.switchline {
  display: flex;
  gap: 9px;
  align-items: center;
  font-size: 14px;
}

.switchline input {
  accent-color: var(--green);
  width: 18px;
  height: 18px;
}

.hint {
  font-size: 12px;
  color: var(--muted);
  line-height: 1.5;
  margin: 12px 0 0;
}

.save-msg {
  font-size: 12px;
  color: var(--green-dark);
  margin-top: 10px;
}

.saved-setting {
  margin-top: 16px;
  border-radius: 12px;
  background: #ecf9ee;
  border: 1px solid #d7eed9;
  padding: 14px 17px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 7px;
}

.saved-setting strong {
  color: var(--green-dark);
  font-size: 20px;
}

.saved-setting small {
  display: block;
  color: var(--muted);
  font-size: 12px;
  margin-bottom: 5px;
}

.schedule-detail {
  margin-top: 12px;
  background: #f5f9f4;
  padding: 12px 14px;
  border-radius: 10px;
  font-size: 12px;
  color: var(--green-dark);
  line-height: 1.7;
}

.schedule-clock {
  font-size: 13px;
  color: var(--muted);
  margin-top: 12px;
}

@media(max-width:680px) {

  .header-inner {
    padding: 38px 17px 25px;
  }

  .brand-name {
    font-size: 20px;
  }

  main {
    padding: 22px 15px 32px;
  }

  h1 {
    font-size: 23px;
  }

  .grid,
  .controls {
    grid-template-columns: 1fr;
  }

  .full {
    grid-column: auto;
  }

  .card {
    padding: 19px;
  }

  .hero {
    padding: 18px;
  }

  .control {
    padding: 14px;
  }
}

</style>
</head>

<body>

<header>
  <div class="orb o1"></div>
  <div class="orb o2"></div>
  <div class="orb o3"></div>
  <div class="orb o4"></div>

  <div class="header-inner">

    <div class="brand">
      <div class="logo">♧</div>

      <div>
        <div class="brand-name">
          SmartBerry<span>360°</span>
        </div>

        <div class="tag">
          Gestão inteligente para sua estufa
        </div>
      </div>
    </div>

    <div class="connection off" id="connection">
      Conectando...
    </div>

  </div>
</header>

<main>

  <h1>Olá! Sua estufa está aqui 🌱</h1>

  <p class="lead">
    Monitore o ambiente e controle os
    equipamentos em tempo real.
  </p>

  <div class="hero">
    <div>
      <strong>Minha estufa</strong>
      <small>Monitoramento local • ESP32</small>
    </div>

    <div class="hero-mark">🌱</div>
  </div>

  <div class="grid">

    <!-- TEMPERATURA -->

    <section class="card">
      <h2 class="card-title">
        Temperatura em tempo real
      </h2>

      <div class="sensor">
        <div class="symbol">♨</div>

        <div>
          <div class="sensor-label">
            Temperatura atual
          </div>

          <div class="reading">
            <span id="temp">--</span>
            <span class="unit">°C</span>
          </div>
        </div>
      </div>
    </section>

    <!-- CONEXAO -->

    <section class="card">
      <h2 class="card-title">
        Conexão com a estufa
      </h2>

      <div class="sensor">
        <div class="symbol purple">◉</div>

        <div>
          <div class="sensor-label">
            Estado do dispositivo
          </div>

          <div id="deviceStatus"
               style="font-weight:bold;font-size:19px;margin-top:8px">
            Verificando...
          </div>
        </div>
      </div>
    </section>

    <!-- CONTROLES -->

    <section class="card full">

      <h2 class="card-title">
        Status e ações rápidas
      </h2>

      <div class="controls">

        <div class="control">
          <div class="device">
            <div class="symbol">💧</div>
            <div>
              <strong>Irrigação</strong>
              <div class="state" id="pumpState">
                Verificando...
              </div>
            </div>
          </div>

          <button id="btnPump"
                  onclick="toggleDevice('pump')"
                  disabled>
            Carregando
          </button>
        </div>

        <div class="control">
          <div class="device">
            <div class="symbol">🍃</div>
            <div>
              <strong>Ventilação</strong>
              <div class="state" id="ventState">
                Verificando...
              </div>
            </div>
          </div>

          <button id="btnVent"
                  onclick="toggleDevice('vent')"
                  disabled>
            Carregando
          </button>
        </div>

      </div>

      <div class="warning" id="warning">
        Não foi possível comunicar com o ESP32.
      </div>

    </section>

    <!-- TEMPERATURA MAXIMA -->

    <section class="card full">

      <h2 class="card-title">
        Temperatura máxima · Ventilação
      </h2>

      <div class="settings">

        <label class="switchline">
          <input id="autoVent" type="checkbox"
                 onchange="editingSettings=true">
          Controle automático
        </label>

        <label class="field">
          Ligar ventilação a partir de (°C)

          <input id="maxTemp"
                 type="number"
                 min="10"
                 max="60"
                 step="0.5"
                 value="30">
        </label>

        <button id="saveSettings"
                onclick="saveTemperatureSettings()">
          Salvar
        </button>

      </div>

      <div class="saved-setting">

        <div>
          <small>Temperatura máxima configurada</small>
          <strong id="savedTemp">30,0 °C</strong>
        </div>

        <div id="autoStatus">
          Modo manual
        </div>

      </div>

      <p class="hint">
        Ao salvar uma nova temperatura, o modo automático
        é ativado. A ventilação desliga 1°C abaixo do limite.
        Para voltar ao modo manual, desmarque o controle
        automático e clique em Salvar.
      </p>

      <div class="save-msg" id="saveMsg"></div>

    </section>

    <!-- AGENDAMENTO -->

    <section class="card full">

      <h2 class="card-title">
        ⏰ Agendamento da irrigação
      </h2>

      <div class="schedule-grid">

        <label class="switchline">
          <input type="checkbox"
                 id="scheduleEnabled">
          Ativar diariamente
        </label>

        <label class="field">
          Horário (Brasília)
          <input type="time"
                 id="scheduleTime"
                 value="08:00">
        </label>

        <label class="field">
          Duração (segundos)

          <input type="number"
                 id="scheduleSeconds"
                 min="1"
                 max="300"
                 value="15">
        </label>

        <button id="saveSchedule"
                onclick="savePumpSchedule()">
          Salvar agendamento
        </button>

      </div>

      <p class="hint">
        A bomba liga uma vez por dia no horário escolhido
        e desliga automaticamente após a duração definida.
        O horário é sincronizado pelo navegador ou pela internet.
      </p>

      <div class="schedule-clock"
           id="clockStatus">
        Verificando relógio...
      </div>

      <div class="schedule-detail"
           id="scheduleSummary">
        Carregando agendamento...
      </div>

      <div class="save-msg" id="scheduleMsg"></div>

    </section>

  </div>

  <div class="info">
    <span>SmartBerry360° • Tecnologia & Agricultura</span>
    <span>Atualização a cada 2,5 segundos</span>
  </div>

</main>

<script>

let busy = false;
let reading = false;
let editingTemp = false;
let editingSchedule = false;
let tempChanged = false;

const el = id => document.getElementById(id);

// ======================================
// ATUALIZAR INTERFACE
// ======================================

function updateUI(data) {

  el('temp').textContent =
    typeof data.temp === 'number'
      ? data.temp.toFixed(1).replace('.', ',')
      : '--';

  el('btnPump').textContent =
    data.pump ? 'Desligar' : 'Ligar';

  el('btnVent').textContent =
    data.vent ? 'Desligar' : 'Ligar';

  el('btnPump').classList.toggle(
    'active', data.pump
  );

  el('btnVent').classList.toggle(
    'active', data.vent
  );

  el('pumpState').textContent =
    data.scheduleRunning
      ? 'Ligada · Agendamento'
      : (data.pump ? 'Ligada' : 'Desligada');

  el('ventState').textContent =
    (data.vent ? 'Ligada' : 'Desligada') +
    (data.autoVent ? ' · Automática' : '');

  el('btnPump').disabled =
    busy || data.scheduleRunning;

  el('btnVent').disabled =
    busy || data.autoVent;

  // Temperatura configurada
  el('savedTemp').textContent =
    Number(data.maxTemp)
      .toFixed(1)
      .replace('.', ',') + ' °C';

  el('autoStatus').textContent =
    data.autoVent
      ? '● Automático ativo'
      : '○ Modo manual';

  if (!editingTemp) {
    el('maxTemp').value = data.maxTemp;
    el('autoVent').checked = data.autoVent;
  }

  // Agendamento
  if (!editingSchedule) {

    el('scheduleEnabled').checked =
      data.scheduleEnabled;

    el('scheduleTime').value =
      String(data.scheduleHour).padStart(2,'0') +
      ':' +
      String(data.scheduleMinute).padStart(2,'0');

    el('scheduleSeconds').value =
      data.scheduleSeconds;
  }

  el('clockStatus').textContent =
    data.clockValid
      ? 'Hora do ESP32 (Brasília): ' + data.localTime
      : 'Relógio não sincronizado. Abra o painel para sincronizar.';

  const scheduledTime =
    String(data.scheduleHour).padStart(2,'0') +
    ':' +
    String(data.scheduleMinute).padStart(2,'0');

  let summary = data.scheduleEnabled
    ? 'Agendamento ativo: diariamente às ' +
      scheduledTime + ', durante ' +
      data.scheduleSeconds + ' segundos.'
    : 'Agendamento desativado.';

  if (data.scheduleEnabled) {

    summary += data.clockValid
      ? ' Relógio sincronizado.'
      : ' Aguardando sincronização.';

    if (data.alreadyRanToday) {
      summary += ' Já executado hoje.';
    }
  }

  el('scheduleSummary').textContent = summary;

  el('connection').textContent = 'ESP32 online';
  el('connection').classList.remove('off');

  el('deviceStatus').textContent = 'Conectado';
  el('warning').classList.remove('show');
}

// ======================================
// ERRO DE COMUNICACAO
// ======================================

function showOffline() {
  el('connection').textContent = 'Sem comunicação';
  el('connection').classList.add('off');

  el('deviceStatus').textContent = 'Desconectado';
  el('warning').classList.add('show');

  el('btnPump').disabled = true;
  el('btnVent').disabled = true;
}

// ======================================
// LEITURA DOS DADOS
// ======================================

async function fetchData() {

  if (busy || reading) return;

  reading = true;

  try {

    const response = await fetch(
      '/api/state',
      { cache: 'no-store' }
    );

    if (!response.ok) throw Error();

    updateUI(await response.json());

  } catch (error) {
    showOffline();
  }

  reading = false;
}

// ======================================
// CONTROLE MANUAL
// ======================================

async function toggleDevice(device) {

  if (busy) return;

  busy = true;

  el('btnPump').disabled = true;
  el('btnVent').disabled = true;

  try {

    const response = await fetch(
      '/api/toggle?device=' +
      encodeURIComponent(device),
      { cache: 'no-store' }
    );

    if (!response.ok) throw Error();

    busy = false;
    updateUI(await response.json());

  } catch (error) {
    busy = false;
    showOffline();
  }
}

// ======================================
// SALVAR TEMPERATURA
// ======================================

async function saveTemperatureSettings() {

  const temp = Number(el('maxTemp').value);

  if (
    !Number.isFinite(temp) ||
    temp < 10 ||
    temp > 60
  ) {
    el('saveMsg').textContent =
      'Informe uma temperatura entre 10 e 60°C.';
    return;
  }

  // Alterar temperatura ativa o modo automatico
  const automatic =
    el('autoVent').checked || tempChanged;

  el('saveSettings').disabled = true;

  try {

    const body =
      'auto=' + (automatic ? '1' : '0') +
      '&maxTemp=' + encodeURIComponent(temp);

    const response = await fetch(
      '/api/settings',
      {
        method: 'POST',
        headers: {
          'Content-Type':
            'application/x-www-form-urlencoded'
        },
        body: body
      }
    );

    if (!response.ok) throw Error();

    editingTemp = false;
    tempChanged = false;

    updateUI(await response.json());

    el('saveMsg').textContent =
      'Temperatura atualizada com sucesso!';

  } catch (error) {

    el('saveMsg').textContent =
      'Erro ao salvar temperatura.';
  }

  el('saveSettings').disabled = false;
}

// ======================================
// SINCRONIZACAO DO RELOGIO
// ======================================

async function syncClock() {

  try {

    const epoch = Math.floor(
      Date.now() / 1000
    );

    const response = await fetch(
      '/api/time',
      {
        method: 'POST',
        headers: {
          'Content-Type':
            'application/x-www-form-urlencoded'
        },
        body: 'epoch=' + epoch
      }
    );

    return response.ok;

  } catch (error) {
    return false;
  }
}

// ======================================
// SALVAR AGENDAMENTO
// ======================================

async function savePumpSchedule() {

  const time = el('scheduleTime').value;
  const seconds = Number(
    el('scheduleSeconds').value
  );

  const enabled =
    el('scheduleEnabled').checked;

  if (
    !/^\d{2}:\d{2}$/.test(time) ||
    !Number.isInteger(seconds) ||
    seconds < 1 ||
    seconds > 300
  ) {
    el('scheduleMsg').textContent =
      'Informe horário válido e duração de 1 a 300 segundos.';
    return;
  }

  const clockOK = await syncClock();

  if (!clockOK) {
    el('scheduleMsg').textContent =
      'Não foi possível sincronizar o relógio.';
    return;
  }

  const [hour, minute] = time.split(':');

  const body =
    'enabled=' + (enabled ? '1' : '0') +
    '&hour=' + Number(hour) +
    '&minute=' + Number(minute) +
    '&seconds=' + seconds;

  el('saveSchedule').disabled = true;

  try {

    const response = await fetch(
      '/api/schedule',
      {
        method: 'POST',
        headers: {
          'Content-Type':
            'application/x-www-form-urlencoded'
        },
        body: body
      }
    );

    if (!response.ok) throw Error();

    editingSchedule = false;

    updateUI(await response.json());

    el('scheduleMsg').textContent =
      'Agendamento salvo com sucesso!';

  } catch (error) {

    el('scheduleMsg').textContent =
      'Erro ao salvar agendamento.';
  }

  el('saveSchedule').disabled = false;
}

// ======================================
// EVENTOS DO PAINEL
// ======================================

el('maxTemp').addEventListener('input', () => {
  editingTemp = true;
  tempChanged = true;
  el('autoVent').checked = true;
});

el('autoVent').addEventListener('change', () => {
  editingTemp = true;
  tempChanged = false;
});

['scheduleEnabled','scheduleTime','scheduleSeconds']
  .forEach(id => {
    el(id).addEventListener('change', () => {
      editingSchedule = true;
    });
  });

// Sincroniza ao abrir e periodicamente
syncClock().then(fetchData);

setInterval(fetchData, 2500);
setInterval(syncClock, 60000);

</script>

</body>
</html>
)rawliteral";

  server.send_P(
    200,
    "text/html; charset=utf-8",
    PAGE
  );
}

// ======================================
// API: ESTADO DO SISTEMA
// ======================================

void handleState() {

  String json = "{\"temp\":";

  if (isnan(currentTemp)) {
    json += "null";
  } else {
    json += String(currentTemp, 1);
  }

  json += ",\"pump\":";
  json += isPumpOn ? "true" : "false";

  json += ",\"vent\":";
  json += isVentOn ? "true" : "false";

  json += ",\"autoVent\":";
  json += autoVent ? "true" : "false";

  json += ",\"maxTemp\":";
  json += String(maxTemp, 1);

  json += ",\"scheduleEnabled\":";
  json += scheduleEnabled ? "true" : "false";

  json += ",\"scheduleHour\":";
  json += String(scheduleHour);

  json += ",\"scheduleMinute\":";
  json += String(scheduleMinute);

  json += ",\"scheduleSeconds\":";
  json += String(scheduleSeconds);

  json += ",\"scheduleRunning\":";
  json += scheduledPumpRunning ? "true" : "false";

  struct tm local;
  bool validClock = readClock(local);

  json += ",\"clockValid\":";
  json += validClock ? "true" : "false";

  char timeText[20] = "--:--:--";

  if (validClock) {
    strftime(
      timeText,
      sizeof(timeText),
      "%H:%M:%S",
      &local
    );
  }

  json += ",\"localTime\":\"";
  json += String(timeText);
  json += "\"";

  bool ranToday =
    validClock &&
    dayKey(local) == lastRunDay;

  json += ",\"alreadyRanToday\":";
  json += ranToday ? "true" : "false";

  json += "}";

  server.sendHeader(
    "Cache-Control",
    "no-store"
  );

  server.send(
    200,
    "application/json",
    json
  );
}

// ======================================
// API: CONTROLE MANUAL
// ======================================

void handleToggle() {

  if (!server.hasArg("device")) {
    server.send(
      400, "application/json",
      "{\"error\":\"Dispositivo ausente\"}"
    );
    return;
  }

  String device = server.arg("device");

  if (device == "pump") {

    if (scheduledPumpRunning) {
      server.send(
        409, "application/json",
        "{\"error\":\"Agendamento em andamento\"}"
      );
      return;
    }

    if (isPumpOn) registrarParadaSolicitada();
    setPump(!isPumpOn);

  } else if (device == "vent") {

    if (autoVent) {
      server.send(
        409, "application/json",
        "{\"error\":\"Modo automatico ativo\"}"
      );
      return;
    }

    setVent(!isVentOn);

  } else {

    server.send(
      400, "application/json",
      "{\"error\":\"Dispositivo invalido\"}"
    );
    return;
  }

  handleState();
}

// ======================================
// API: TEMPERATURA MAXIMA
// ======================================

void handleSettings() {

  if (
    !server.hasArg("maxTemp") ||
    !server.hasArg("auto")
  ) {
    server.send(
      400, "application/json",
      "{\"error\":\"Parametros ausentes\"}"
    );
    return;
  }

  String value = server.arg("maxTemp");
  String automatic = server.arg("auto");

  char* endPtr = nullptr;

  float temp = strtof(
    value.c_str(),
    &endPtr
  );

  if (
    value.length() == 0 ||
    *endPtr != '\0' ||
    !isfinite(temp) ||
    temp < 10 ||
    temp > 60
  ) {
    server.send(
      400, "application/json",
      "{\"error\":\"Temperatura invalida\"}"
    );
    return;
  }

  if (
    automatic != "0" &&
    automatic != "1"
  ) {
    server.send(
      400, "application/json",
      "{\"error\":\"Modo invalido\"}"
    );
    return;
  }

  maxTemp = temp;
  autoVent = (automatic == "1");

  prefs.putFloat("maxTemp", maxTemp);
  prefs.putBool("autoVent", autoVent);

  checkTemperatureControl();

  Serial.printf(
    "Temperatura maxima: %.1f C | Auto: %s\n",
    maxTemp,
    autoVent ? "SIM" : "NAO"
  );

  handleState();
}

// ======================================
// WIFI
// ======================================

void connectWiFi() {

  Serial.println();
  Serial.println("Conectando ao WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startTime = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startTime < WIFI_TIMEOUT
  ) {
    delay(500);
    Serial.print(".");
  }

  lastWifiAttempt = millis();

  if (WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;

    Serial.println();
    Serial.println("WiFi conectado!");

    Serial.print("Endereco IP: ");
    Serial.println(WiFi.localIP());

  } else {

    wifiConnected = false;

    Serial.println();
    Serial.println("WiFi indisponivel.");
    Serial.println("Sistema funcionando offline.");
  }
}

void startServer() {
  if (!SMARTBERRY_WEB_PANEL_ENABLED) return;

  if (serverStarted) return;

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.println("Iniciando servidor web...");

  server.begin();
  serverStarted = true;

  Serial.println("Servidor web iniciado!");

  Serial.print("Acesse: http://");
  Serial.println(WiFi.localIP());
}

void checkWiFi() {

  bool connected =
    WiFi.status() == WL_CONNECTED;

  if (connected) {

    if (!wifiConnected) {

      wifiConnected = true;

      Serial.println("WiFi reconectado!");

      Serial.print("Endereco IP: ");
      Serial.println(WiFi.localIP());
    }

    // NTP quando houver internet
    if (!ntpConfigured) {

      configTzTime(
        "BRT3",
        "pool.ntp.org",
        "time.google.com"
      );

      ntpConfigured = true;
    }

    startServer();
    return;
  }

  if (wifiConnected) {

    wifiConnected = false;

    Serial.println("WiFi desconectado.");

    lastWifiAttempt = millis();
  }

  if (
    millis() - lastWifiAttempt >= WIFI_RETRY
  ) {

    lastWifiAttempt = millis();

    Serial.println(
      "Tentando reconectar ao WiFi..."
    );

    WiFi.reconnect();
  }
}

// A rede é consultada com timeouts curtos para manter o painel e os relés responsivos.
bool prepararRequisicaoApi(HTTPClient &http, const String &path) {
  if (!http.begin(String(SMARTBERRY_API_URL) + path)) return false;
  http.setConnectTimeout(500);
  http.setTimeout(700);
  http.addHeader("X-SmartBerry-Key", SMARTBERRY_API_KEY);
  return true;
}

void cadastrarPlaca() {
  char code[19];
  snprintf(code, sizeof(code), "esp32-%012llx",
           static_cast<unsigned long long>(ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL));

  HTTPClient http;
  if (!prepararRequisicaoApi(http, String("/api/esp32/registro/") + code)) return;
  int status = http.PUT("");
  if (status == 200) {
    String body = http.getString();
    cJSON *device = cJSON_Parse(body.c_str());
    if (device != nullptr) {
      const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(device, "id"));
      if (id != nullptr) apiDeviceId = id;
      cJSON_Delete(device);
    }
  } else {
    Serial.printf("Falha no cadastro do ESP32: %d\n", status);
  }
  http.end();
}

void enviarTelemetria() {
  if (!validSensorSample) return;
  HTTPClient http;
  if (!prepararRequisicaoApi(http, String("/api/sensores/esp32/") + apiDeviceId)) return;
  char body[256];
  snprintf(body, sizeof(body),
    "{\"temperatura\":%.1f,\"umidade\":%.1f,\"irrigacaoLigada\":%s,\"ventilacaoLigada\":%s,"
    "\"irrigacaoAutomatica\":%s,\"ventilacaoAutomatica\":%s}",
    currentTemp, currentHumidity, isPumpOn ? "true" : "false", isVentOn ? "true" : "false",
    (SMARTBERRY_WEB_PANEL_ENABLED && scheduleEnabled) ? "true" : "false",
    (SMARTBERRY_WEB_PANEL_ENABLED && autoVent) ? "true" : "false");
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body);
  if (status == 201) telemetryChanged = false;
  else if (status == 404 && !SMARTBERRY_ESP32_ID[0]) apiDeviceId = "";
  else Serial.printf("Falha no envio da leitura: %d\n", status);
  http.end();
}

bool executarComando(const String &type, int value, bool apiScheduled) {
  if (type == "ACIONAR_IRRIGACAO") {
    if (isPumpOn || scheduledPumpRunning || value < 1 || value > 300) return false;
    if (apiScheduled && ultimaParadaSolicitada != 0
        && millis() - ultimaParadaSolicitada < 30000UL) return false;
    setPump(true);
    remotePumpStart = millis();
    remotePumpDuration = value * 1000UL;
    remotePumpTimed = true;
  } else if (type == "PARAR_IRRIGACAO") {
    if (scheduledPumpRunning) return false;
    registrarParadaSolicitada();
    setPump(false);
  } else if (type == "CONFIGURAR_IRRIGACAO_AUTOMATICA") {
    if (!SMARTBERRY_WEB_PANEL_ENABLED) return false;
    if (value != 0 && value != 1) return false;
    bool enabled = value == 1;
    if (scheduleEnabled != enabled && prefs.putBool("enabled", enabled) == 0) return false;
    scheduleEnabled = enabled;
    telemetryChanged = true;
    if (!scheduleEnabled && scheduledPumpRunning) {
      scheduledPumpRunning = false;
      setPump(false);
    }
  } else if (type == "ACIONAR_VENTILACAO_TEMPORIZADA") {
    if (isVentOn || (SMARTBERRY_WEB_PANEL_ENABLED && autoVent) || value < 1 || value > 300) return false;
    setVent(true);
    remoteVentStart = millis();
    remoteVentDuration = value * 1000UL;
    remoteVentTimed = true;
  } else if (type == "LIGAR_VENTILACAO" || type == "DESLIGAR_VENTILACAO") {
    if (SMARTBERRY_WEB_PANEL_ENABLED && autoVent) return false;
    setVent(type == "LIGAR_VENTILACAO");
  } else if (type == "ALTERAR_INTERVALO" && value > 0 && value <= 3600) {
    telemetryInterval = value * 1000UL;
  } else {
    return false;
  }
  return true;
}

void confirmarComando() {
  HTTPClient http;
  if (!prepararRequisicaoApi(http, "/api/comandos/" + pendingAck + "/" + pendingOutcome)) return;
  int status = http.PATCH("");
  http.end();
  if (status == 200 || status == 404) {
    prefs.remove("apiCommand");
    pendingAck = "";
  }
}

void consultarComando() {
  if (pendingAck.length() > 0) {
    confirmarComando();
    return;
  }
  HTTPClient http;
  if (!prepararRequisicaoApi(http, String("/api/comandos/esp32/") + apiDeviceId + "/proximo")) return;
  int status = http.GET();
  String body;
  if (status == 200) body = http.getString();
  http.end();
  if (status != 200) return;

  cJSON *command = cJSON_Parse(body.c_str());
  if (command == nullptr) return;
  const char *commandId = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "id"));
  const char *deviceId = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "esp32Id"));
  const char *commandType = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "tipo"));
  String id = commandId == nullptr ? "" : commandId;
  String type = commandType == nullptr ? "" : commandType;
  bool correctDevice = deviceId != nullptr && apiDeviceId == deviceId;
  const cJSON *number = cJSON_GetObjectItemCaseSensitive(command, "valor");
  int value = cJSON_IsNumber(number) ? number->valueint : -1;
  const cJSON *scheduledTime = cJSON_GetObjectItemCaseSensitive(command, "programadoPara");
  bool apiScheduled = scheduledTime != nullptr && !cJSON_IsNull(scheduledTime);
  cJSON_Delete(command);
  if (id.length() == 0 || !correctDevice) return;

  // Registra a tentativa antes de acionar. Uma reinicialização nesse intervalo
  // impede o reacionamento e comunica o resultado incerto como rejeitado.
  if (prefs.putString("apiCommand", id + "/rejeitado") == 0) return;
  pendingAck = id;
  bool done = executarComando(type, value, apiScheduled);
  pendingOutcome = done ? "executado" : "rejeitado";
  if (done) prefs.putString("apiCommand", id + "/executado");
  confirmarComando();
}

void sincronizarApi() {
  if (WiFi.status() != WL_CONNECTED || !SMARTBERRY_API_URL[0]
      || !SMARTBERRY_API_KEY[0]) return;
  unsigned long now = millis();
  if (apiDeviceId.isEmpty()) {
    if (lastRegistrationAttempt == 0 || now - lastRegistrationAttempt >= 5000) {
      lastRegistrationAttempt = now;
      cadastrarPlaca();
    }
    return;
  }
  if (now - lastTelemetry >= telemetryInterval
      || (telemetryChanged && now - lastTelemetry >= 1000
          && now - lastCommandPoll < 1000)) {
    lastTelemetry = now;
    enviarTelemetria();
  } else if (now - lastCommandPoll >= 1000) {
    lastCommandPoll = now;
    consultarComando();
  }
}

// ======================================
// SETUP
// ======================================

void setup() {

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Iniciando SmartBerry360...");

  // Horario de Brasilia (UTC-3)
  setenv("TZ", "BRT3", 1);
  tzset();

  // Carrega configuracoes salvas
  loadSettings();
  String savedCommand = prefs.getString("apiCommand", "");
  int separator = savedCommand.indexOf('/');
  if (separator > 0) {
    pendingAck = savedCommand.substring(0, separator);
    pendingOutcome = savedCommand.substring(separator + 1);
  }

  // Rele da bomba inicia desligado
  digitalWrite(PUMP_PIN, PUMP_OFF);
  digitalWrite(VENT_PIN, VENT_OFF);

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(VENT_PIN, OUTPUT);

  Serial.println("Reles configurados.");

  // Sensor
  dht.begin();
  Serial.println("DHT22 iniciado.");

  // LCD
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");

  Serial.println("LCD iniciado.");

  currentTemp = dht.readTemperature();

  updateLCD();
  checkTemperatureControl();

  // Registra rotas
  server.on(
    "/", HTTP_GET, handleRoot
  );

  server.on(
    "/api/state", HTTP_GET, handleState
  );

  server.on(
    "/api/toggle", HTTP_GET, handleToggle
  );

  server.on(
    "/api/settings", HTTP_POST, handleSettings
  );

  server.on(
    "/api/schedule", HTTP_POST, handleSchedule
  );

  server.on(
    "/api/time", HTTP_POST, handleTime
  );

  Serial.println("Rotas configuradas.");

  // WiFi antes do servidor
  connectWiFi();

  // Inicia apenas quando conectado
  startServer();

  Serial.println();
  Serial.println("SmartBerry inicializado!");
}

// ======================================
// LOOP PRINCIPAL
// ======================================

void loop() {

  checkWiFi();

  // Verifica agendamento mesmo sem WiFi
  if (remotePumpTimed && millis() - remotePumpStart >= remotePumpDuration) {
    setPump(false);
  }
  if (remoteVentTimed && millis() - remoteVentStart >= remoteVentDuration) {
    setVent(false);
  }
  checkPumpSchedule();

  if (
    WiFi.status() == WL_CONNECTED &&
    serverStarted
  ) {
    server.handleClient();
  }

  if (
    millis() - lastDhtRead >= DHT_INTERVAL
  ) {

    lastDhtRead = millis();

    float t = dht.readTemperature();
    currentHumidity = dht.readHumidity();
    validSensorSample = isfinite(t) && isfinite(currentHumidity);

    if (!isnan(t)) {

      if (
        isnan(currentTemp) ||
        t != currentTemp
      ) {
        currentTemp = t;
        updateLCD();
      }

    } else {

      Serial.println(
        "Falha na leitura do DHT22."
      );
    }

    checkTemperatureControl();
  }

  sincronizarApi();
  delay(1);
}