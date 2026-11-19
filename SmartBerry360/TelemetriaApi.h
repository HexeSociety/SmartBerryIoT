#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>
#include "iot_secrets.h"

namespace TelemetriaApi {
    static constexpr unsigned long INTERVALO = 15000UL;
    static unsigned long ultimoEnvio = 0;
    static bool primeiroEnvio = true;

    struct Leitura {
        float temperatura;
        float umidade;
    };

    static QueueHandle_t fila = nullptr;

    // Somente esta tarefa trabalha com HTTP; não acessa LCD nem relés.
    static void tarefaHttp(void*) {
        Leitura leitura;
        for (;;) {
            if (xQueueReceive(fila, &leitura, portMAX_DELAY) != pdTRUE) continue;
            if (WiFi.status() != WL_CONNECTED) continue;

            WiFiClient client;
            HTTPClient http;
            String url = String(SMARTBERRY_API_URL) + "/api/sensores/esp32/" + SMARTBERRY_ESP32_ID;
            if (!http.begin(client, url)) {
                Serial.println("[API] Falha ao iniciar HTTP.");
                continue;
            }
            http.setConnectTimeout(2500);
            http.setTimeout(2500);
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-IoT-Key", SMARTBERRY_IOT_API_KEY);

            String json = "{\"temperatura\":";
            json += String(leitura.temperatura, 1);
            json += ",\"umidade\":";
            json += String(leitura.umidade, 1);
            json += "}";

            int status = http.POST(json);
            Serial.printf("[API] HTTP %d | Temp: %.1f C | Umidade: %.1f %%\n",
                          status, leitura.temperatura, leitura.umidade);
            if (status < 0) {
                Serial.println("[API] " + http.errorToString(status));
            }
            http.end();
        }
    }

    static void iniciar() {
        if (fila != nullptr) return;
        fila = xQueueCreate(1, sizeof(Leitura));
        if (fila == nullptr) {
            Serial.println("[API] Falha ao criar fila.");
            return;
        }
        if (xTaskCreate(tarefaHttp, "telemetria", 8192, nullptr, 1, nullptr) != pdPASS) {
            vQueueDelete(fila);
            fila = nullptr;
            Serial.println("[API] Falha ao criar tarefa HTTP.");
        }
    }

    // Chamar a partir do loop, somente com dados recém-lidos do DHT.
    static void enviarSeNecessario(float temperatura, float umidade) {
        if (!fila || WiFi.status() != WL_CONNECTED) return;
        if (!isfinite(temperatura) || !isfinite(umidade) || umidade < 0.0f || umidade > 100.0f) return;
        unsigned long agora = millis();
        if (!primeiroEnvio && agora - ultimoEnvio < INTERVALO) return;
        Leitura leitura{temperatura, umidade};
        if (xQueueOverwrite(fila, &leitura) == pdPASS) {
            ultimoEnvio = agora;
            primeiroEnvio = false;
        }
    }
}
