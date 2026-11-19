#pragma once
#include "ComandosApi.h"
#include "Atuadores.h"
#include "Automacao.h"

namespace ControleRemoto {
  static bool executar(const ComandosApi::Comando& comando) {
    Serial.printf("[CMD] Processando comando: %s\n", comando.id);
    switch (comando.tipo) {
      case ComandosApi::IRRIGAR:
        return Atuadores::iniciarBomba(comando.valor, Atuadores::REMOTA);
      case ComandosApi::PARAR:
        Automacao::paradaManual();
        return true;
      case ComandosApi::VENT_LIGAR:
      case ComandosApi::VENT_DESLIGAR:
        if (Automacao::autoVent) {
          Serial.println("[CMD] Ventilacao automatica ativa; comando manual recusado.");
          return false;
        }
        Atuadores::setVent(comando.tipo == ComandosApi::VENT_LIGAR);
        return true;
      default:
        Serial.println("[CMD] Comando nao suportado.");
        return false;
    }
  }
}
