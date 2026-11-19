#pragma once
#include <Arduino.h>
#include <string.h>
#include "Atuadores.h"
#include "Automacao.h"
#include "Relogio.h"

namespace Diagnostico {
  // Console USB local de manutencao, sem servidor web ou comandos de ligar bomba.
  static char linha[32] = {};
  static size_t n = 0;
  static bool descartando = false;

  static void executar() {
    if (!strcmp(linha, "AUTO OFF")) {
      bool gravou = Automacao::desligarVentAuto();
      Atuadores::setVent(false);
      Serial.println(gravou ? "[LOCAL] Ventilacao automatica desativada." : "[LOCAL] Desativada nesta sessao; falha NVS.");
    } else if (!strcmp(linha, "AGENDA OFF")) {
      Serial.println(Automacao::desligarAgenda() ? "[LOCAL] Agenda desativada." : "[LOCAL] Agenda desativada nesta sessao; falha NVS.");
    } else if (!strcmp(linha, "PARAR")) {
      Automacao::paradaManual();
      Serial.println("[LOCAL] Parada solicitada.");
    } else if (!strcmp(linha, "STATUS")) {
      struct tm local;
      char texto[32] = "nao sincronizado";
      if (Relogio::ler(local)) strftime(texto, sizeof(texto), "%d/%m/%Y %H:%M:%S", &local);
      Serial.printf("[LOCAL] Hora: %s | Bomba: %s | Vent: %s | Auto: %s | Agenda: %s\n",
          texto, Atuadores::bombaLigada ? "ON" : "OFF", Atuadores::ventLigada ? "ON" : "OFF",
          Automacao::autoVent ? "ON" : "OFF", Automacao::agendaAtiva ? "ON" : "OFF");
    } else if (n > 0) {
      Serial.println("[LOCAL] Comandos: STATUS, AUTO OFF, AGENDA OFF, PARAR");
    }
  }

  static void atualizar() {
    // Limita o trabalho por iteracao para nao bloquear os temporizadores.
    for (int i = 0; i < 64 && Serial.available(); ++i) {
      char c = static_cast<char>(Serial.read());
      if (c == '\r' || c == '\n') {
        if (!descartando && n) { linha[n] = '\0'; executar(); }
        n = 0; descartando = false;
      } else if (!descartando) {
        if (n < sizeof(linha) - 1) linha[n++] = c;
        else { n = 0; descartando = true; }
      }
    }
  }
}
