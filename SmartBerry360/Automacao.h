#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "Configuracao.h"
#include "Atuadores.h"
#include "Relogio.h"

namespace Automacao {
  static Preferences prefs;
  static bool memoriaOk = false;
  static bool autoVent = false, agendaAtiva = false;
  static float maxTemp = 30.0f;
  static uint8_t hora = 8, minuto = 0;
  static uint16_t segundos = 15;
  static int32_t ultimoDia = 0;

  static void iniciar() {
    // Mesmo namespace e chaves da versao web: preserva configuracoes NVS.
    memoriaOk = prefs.begin("smartberry", false);
    if (!memoriaOk) {
      Serial.println("[AUTO] NVS indisponivel; automacao desativada.");
      return;
    }
    autoVent = prefs.getBool("autoVent", false);
    maxTemp = prefs.getFloat("maxTemp", 30.0f);
    agendaAtiva = prefs.getBool("enabled", false);
    hora = prefs.getUChar("hour", 8);
    minuto = prefs.getUChar("minute", 0);
    segundos = prefs.getUShort("seconds", 15);
    ultimoDia = prefs.getInt("lastday", 0);
    if (!isfinite(maxTemp) || maxTemp < 10 || maxTemp > 60) maxTemp = 30.0f;
    if (hora > 23 || minuto > 59 || segundos < 1 || segundos > Configuracao::DURACAO_MAXIMA) {
      agendaAtiva = false; hora = 8; minuto = 0; segundos = 15;
    }
    Serial.printf("[AUTO] Vent: %s (%.1f C) | Agenda: %s %02u:%02u, %u s\n",
      autoVent ? "ON" : "OFF", maxTemp, agendaAtiva ? "ON" : "OFF",
      static_cast<unsigned>(hora), static_cast<unsigned>(minuto), static_cast<unsigned>(segundos));
  }

  static void atualizarVent(float temperatura) {
    if (!autoVent || !isfinite(temperatura)) return;
    if (!Atuadores::ventLigada && temperatura >= maxTemp) Atuadores::setVent(true);
    else if (Atuadores::ventLigada && temperatura <= maxTemp - Configuracao::HISTERESE) Atuadores::setVent(false);
  }

  static void atualizarAgenda() {
    if (!memoriaOk || !agendaAtiva || Atuadores::bombaLigada) return;
    struct tm local;
    if (!Relogio::ler(local)) return;
    int32_t hoje = Relogio::dia(local);
    if (local.tm_hour != hora || local.tm_min != minuto || hoje == ultimoDia) return;
    // Reserva o dia antes de ligar; se a persistencia falhar, nao aciona.
    if (prefs.putInt("lastday", hoje) != sizeof(int32_t)) {
      ultimoDia = hoje;
      Serial.println("[AUTO] Falha ao reservar agendamento; bomba nao acionada.");
      return;
    }
    ultimoDia = hoje;
    Atuadores::iniciarBomba(segundos, Atuadores::AGENDADA);
  }

  static bool desligarVentAuto() {
    autoVent = false;
    return memoriaOk && prefs.putBool("autoVent", false) != 0;
  }

  static bool desligarAgenda() {
    agendaAtiva = false;
    if (Atuadores::origem == Atuadores::AGENDADA) Atuadores::pararBomba();
    return memoriaOk && prefs.putBool("enabled", false) != 0;
  }

  // Interrompe a bomba sem deixar a agenda reaciona-la no mesmo minuto.
  static void paradaManual() {
    Atuadores::pararBomba();
    struct tm local;
    if (agendaAtiva && Relogio::ler(local) && local.tm_hour == hora && local.tm_min == minuto) {
      ultimoDia = Relogio::dia(local);
      if (memoriaOk && prefs.putInt("lastday", ultimoDia) != sizeof(int32_t))
        Serial.println("[AUTO] Falha ao gravar cancelamento da agenda.");
    }
  }
}
