#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <stdlib.h>

namespace Relogio {
  constexpr time_t MIN_VALID_TIME = 1704067200;
  static bool configurado = false;
  static bool avisado = false;
  static unsigned long ultimaTentativa = 0;

  static void iniciar() {
    setenv("TZ", "BRT3", 1);
    tzset();
  }

  static bool valido() { return time(nullptr) >= MIN_VALID_TIME; }

  static bool ler(struct tm& local) {
    time_t agora = time(nullptr);
    return agora >= MIN_VALID_TIME && localtime_r(&agora, &local) != nullptr;
  }

  static int32_t dia(const struct tm& t) {
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
  }

  static void atualizar() {
    unsigned long agora = millis();
    if (WiFi.status() == WL_CONNECTED &&
        (!configurado || (!valido() && agora - ultimaTentativa >= 60000UL))) {
      configTzTime("BRT3", "pool.ntp.org", "time.google.com");
      configurado = true;
      ultimaTentativa = agora;
      Serial.println("[NTP] Aguardando sincronizacao...");
    }
    struct tm local;
    if (!avisado && ler(local)) {
      char texto[32];
      strftime(texto, sizeof(texto), "%d/%m/%Y %H:%M:%S", &local);
      Serial.printf("[NTP] Relogio sincronizado: %s (Brasilia)\n", texto);
      avisado = true;
    }
  }
}
