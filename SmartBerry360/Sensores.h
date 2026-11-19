#pragma once
#include <Arduino.h>
#include <DHT.h>
#include <math.h>
#include "Configuracao.h"

namespace Sensores {
  // Mantem o modo DHT22 que funcionou na maquete. Conferir calibracao fisica.
  static DHT dht(Configuracao::PINO_DHT, DHT22);
  static float temperatura = NAN;
  static float umidade = NAN;
  static unsigned long ultimaLeitura = 0;

  static void iniciar() {
    dht.begin();
    ultimaLeitura = millis();
    Serial.println("[DHT] Sensor iniciado (modo DHT22).");
  }

  static bool atualizar() {
    unsigned long agora = millis();
    if (agora - ultimaLeitura < Configuracao::LEITURA_MS) return false;
    ultimaLeitura = agora;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    temperatura = isfinite(t) ? t : NAN;
    umidade = (isfinite(h) && h >= 0 && h <= 100) ? h : NAN;
    Serial.printf("Temperatura: %.1f C | Umidade: %.1f %%\n", temperatura, umidade);
    if (!isfinite(temperatura) || !isfinite(umidade)) Serial.println("[DHT] Leitura invalida.");
    return true;
  }
}
