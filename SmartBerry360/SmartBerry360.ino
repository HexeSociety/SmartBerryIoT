#include <Arduino.h>
#include "Configuracao.h"
#include "ConexaoWiFi.h"
#include "Relogio.h"
#include "Sensores.h"
#include "Atuadores.h"
#include "Display.h"
#include "Automacao.h"
#include "TelemetriaApi.h"
#include "ComandosApi.h"
#include "ControleRemoto.h"
#include "Diagnostico.h"

void setup() {
  Atuadores::iniciar();
  Serial.begin(115200);
  Serial.println("\nIniciando SmartBerry360 (sem interface web)...");
  Relogio::iniciar();
  Automacao::iniciar();
  Sensores::iniciar();
  Display::iniciar();
  ConexaoWiFi::iniciar();
  TelemetriaApi::iniciar();
  ComandosApi::iniciar(ControleRemoto::executar);
  Serial.println("SmartBerry inicializado!");
  Serial.println("[LOCAL] Comandos USB: STATUS, AUTO OFF, AGENDA OFF, PARAR");
}

void loop() {
  Atuadores::atualizar();
  Diagnostico::atualizar();
  ConexaoWiFi::atualizar();
  Relogio::atualizar();
  ComandosApi::processar();
  Automacao::atualizarAgenda();
  if (Sensores::atualizar()) {
    Automacao::atualizarVent(Sensores::temperatura);
    TelemetriaApi::enviarSeNecessario(Sensores::temperatura, Sensores::umidade);
  }
  Display::atualizar(Sensores::temperatura, Atuadores::ventLigada);
  Atuadores::atualizar();
  delay(1);
}
