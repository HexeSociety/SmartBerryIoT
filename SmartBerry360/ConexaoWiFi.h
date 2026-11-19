#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "Configuracao.h"

namespace ConexaoWiFi {
  static bool conectado = false;
  static unsigned long ultimaTentativa = 0;

  static void iniciar() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(SMARTBERRY_WIFI_SSID, SMARTBERRY_WIFI_PASSWORD);
    ultimaTentativa = millis();
    Serial.println("[WiFi] Conectando...");
  }

  static void atualizar() {
    bool agoraConectado = WiFi.status() == WL_CONNECTED;
    if (agoraConectado) {
      if (!conectado) {
        Serial.print("[WiFi] Conectado. IP: ");
        Serial.println(WiFi.localIP());
      }
      conectado = true;
      return;
    }
    if (conectado) {
      conectado = false;
      ultimaTentativa = millis();
      Serial.println("[WiFi] Desconectado. Controle local ativo.");
    }
    if (millis() - ultimaTentativa >= Configuracao::WIFI_RETRY_MS) {
      ultimaTentativa = millis();
      WiFi.reconnect();
      Serial.println("[WiFi] Tentando reconectar...");
    }
  }
}
