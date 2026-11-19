#pragma once
#include <Arduino.h>
#include "Configuracao.h"

namespace Atuadores {
  enum OrigemBomba : uint8_t { NENHUMA, AGENDADA, REMOTA };
  static bool bombaLigada = false;
  static bool ventLigada = false;
  static OrigemBomba origem = NENHUMA;
  static unsigned long inicio = 0, duracao = 0;

  static void iniciar() {
    digitalWrite(Configuracao::PINO_BOMBA, Configuracao::BOMBA_OFF);
    digitalWrite(Configuracao::PINO_VENT, Configuracao::VENT_OFF);
    pinMode(Configuracao::PINO_BOMBA, OUTPUT);
    pinMode(Configuracao::PINO_VENT, OUTPUT);
  }

  static void pararBomba() {
    digitalWrite(Configuracao::PINO_BOMBA, Configuracao::BOMBA_OFF);
    if (bombaLigada) Serial.println("Bomba DESLIGADA");
    bombaLigada = false;
    origem = NENHUMA;
    duracao = 0;
  }

  static bool iniciarBomba(int segundos, OrigemBomba modo) {
    if (bombaLigada || modo == NENHUMA || segundos < 1 ||
        segundos > Configuracao::DURACAO_MAXIMA) return false;
    origem = modo;
    duracao = static_cast<unsigned long>(segundos) * 1000UL;
    inicio = millis();
    bombaLigada = true;
    digitalWrite(Configuracao::PINO_BOMBA, Configuracao::BOMBA_ON);
    Serial.printf("Bomba LIGADA | %s | %d segundos\n", modo == REMOTA ? "remota" : "agendada", segundos);
    return true;
  }

  static void setVent(bool ligada) {
    if (ventLigada == ligada) return;
    ventLigada = ligada;
    digitalWrite(Configuracao::PINO_VENT, ligada ? Configuracao::VENT_ON : Configuracao::VENT_OFF);
    Serial.println(ligada ? "Ventilacao LIGADA" : "Ventilacao DESLIGADA");
  }

  static void atualizar() {
    if (bombaLigada && millis() - inicio >= duracao) {
      OrigemBomba anterior = origem;
      pararBomba();
      Serial.println(anterior == REMOTA ? "[CMD] Irrigacao remota concluida." : "[AUTO] Irrigacao agendada concluida.");
    }
  }
}
