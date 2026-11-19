#pragma once
#include <Arduino.h>
#include "iot_secrets.h"

#if !defined(SMARTBERRY_WIFI_SSID) || !defined(SMARTBERRY_WIFI_PASSWORD)
#error "Adicione SMARTBERRY_WIFI_SSID e SMARTBERRY_WIFI_PASSWORD ao iot_secrets.h"
#endif

namespace Configuracao {
  constexpr uint8_t PINO_BOMBA = 25;
  constexpr uint8_t PINO_VENT = 27;
  constexpr uint8_t PINO_DHT = 4;
  constexpr uint8_t BOMBA_ON = HIGH, BOMBA_OFF = LOW;
  constexpr uint8_t VENT_ON = LOW, VENT_OFF = HIGH;
  constexpr unsigned long LEITURA_MS = 2500;
  constexpr unsigned long WIFI_RETRY_MS = 15000;
  constexpr int DURACAO_MAXIMA = 300;
  constexpr float HISTERESE = 1.0f;
}
