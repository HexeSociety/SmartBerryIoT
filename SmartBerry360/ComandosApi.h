#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "iot_secrets.h"

namespace ComandosApi {
    enum Tipo : uint8_t { IRRIGAR, PARAR, VENT_LIGAR, VENT_DESLIGAR, NAO_SUPORTADO };
    struct Comando {
        char id[25];
        Tipo tipo;
        int valor;
        time_t expiracao;
    };
    using Executor = bool (*)(const Comando&);
    static QueueHandle_t filaComando = nullptr;
    static QueueHandle_t filaResultado = nullptr;
    static Executor executarLocal = nullptr;
    static constexpr time_t MIN_RELOGIO = 1704067200;
    static constexpr uint32_t INTERVALO = 3000;

    static bool idValido(const char* id) {
        if (!id || strlen(id) != 24) return false;
        for (size_t i = 0; i < 24; ++i) {
            char c = id[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }

    static bool bissexto(int ano) {
        return (ano % 4 == 0 && ano % 100 != 0) || ano % 400 == 0;
    }

    // Instant do Java: ISO-8601 UTC, com ou sem fracoes de segundo.
    // Conversao independente do fuso local (BRT3).
    static time_t lerExpiracao(const char* s) {
        if (!s || strlen(s) < 20 || strlen(s) > 30) return 0;
        for (int i = 0; i < 19; ++i) {
            char sep = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' : 0;
            if (sep ? s[i] != sep : (s[i] < '0' || s[i] > '9')) return 0;
        }
        int y, m, d, h, min, sec;
        if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &m, &d, &h, &min, &sec) != 6) return 0;
        const char* fim = s + 19;
        if (*fim == '.') {
            ++fim;
            if (*fim < '0' || *fim > '9') return 0;
            while (*fim >= '0' && *fim <= '9') ++fim;
        }
        if (*fim != 'Z' || fim[1] != '\0') return 0;
        if (y < 2024 || y > 2099 || m < 1 || m > 12 || h > 23 || min > 59 || sec > 59) return 0;
        const int meses[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int limite = meses[m-1] + (m == 2 && bissexto(y));
        if (d < 1 || d > limite) return 0;
        int64_t dias = 0;
        for (int ano = 1970; ano < y; ++ano) dias += bissexto(ano) ? 366 : 365;
        for (int mes = 1; mes < m; ++mes) dias += meses[mes-1] + (mes == 2 && bissexto(y));
        dias += d - 1;
        int64_t epoch = dias * 86400LL + h * 3600LL + min * 60LL + sec;
        time_t t = static_cast<time_t>(epoch);
        return static_cast<int64_t>(t) == epoch ? t : 0;
    }

    static void configurar(HTTPClient& http) {
        http.setConnectTimeout(5000);
        http.setTimeout(10000);
        http.addHeader("X-IoT-Key", SMARTBERRY_IOT_API_KEY);
    }

    // Confirma apenas o primeiro comando. A API devolve a fila ordenada.
    static bool consultar(Comando& c) {
        WiFiClient client;
        HTTPClient http;
        String url = String(SMARTBERRY_API_URL) + "/api/comandos/pendentes";
        if (!http.begin(client, url)) return false;
        configurar(http);
        int status = http.GET();
        if (status != 200) {
            Serial.printf("[CMD] GET HTTP %d\n", status);
            http.end();
            return false;
        }
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (deserializeJson(doc, body) || !doc.is<JsonArray>()) {
            Serial.println("[CMD] Resposta JSON invalida.");
            return false;
        }
        JsonArray arr = doc.as<JsonArray>();
        if (arr.size() == 0) return false;
        JsonObject obj = arr[0].as<JsonObject>();
        const char* id = obj["id"] | "";
        const char* destino = obj["esp32Id"] | "";
        const char* estado = obj["status"] | "";
        const char* tipo = obj["tipo"] | "";
        if (!idValido(id) || strcmp(destino, SMARTBERRY_ESP32_ID) != 0 || strcmp(estado, "PENDENTE") != 0) {
            Serial.println("[CMD] Identidade ou estado invalido; comando ignorado.");
            return false;
        }
        memset(&c, 0, sizeof(c));
        memcpy(c.id, id, 24);
        c.tipo = NAO_SUPORTADO;
        if (!strcmp(tipo, "ACIONAR_IRRIGACAO")) c.tipo = IRRIGAR;
        else if (!strcmp(tipo, "PARAR_IRRIGACAO")) c.tipo = PARAR;
        else if (!strcmp(tipo, "LIGAR_VENTILACAO")) c.tipo = VENT_LIGAR;
        else if (!strcmp(tipo, "DESLIGAR_VENTILACAO")) c.tipo = VENT_DESLIGAR;
        c.valor = obj["valor"].is<int>() ? obj["valor"].as<int>() : 0;
        c.expiracao = lerExpiracao(obj["dataExpiracao"] | "");
        return true;
    }

    static int confirmar(const char* id, char resultado) {
        WiFiClient client;
        HTTPClient http;
        String url = String(SMARTBERRY_API_URL) + "/api/comandos/" + id +
                     (resultado == 'E' ? "/executado" : "/falhou");
        if (!http.begin(client, url)) return -1;
        configurar(http);
        int status = http.PATCH(String(""));
        http.end();
        Serial.printf("[CMD] PATCH %s HTTP %d\n", id, status);
        return status;
    }

    // Um unico registro persistente, pois processamos um comando por vez.
    // I = reservado, E = executado, F = falhou. Reservar ANTES de atuar
    // impede reexecucao apos reset, mesmo se a confirmacao se perder.
    static bool salvar(Preferences& p, const char* id, char estado) {
        String registro = String(id) + ":" + estado;
        return p.putString("ultimo", registro) == registro.length();
    }

    static void tarefaHttp(void*) {
        Preferences p;
        if (!p.begin("sb-cmd", false)) {
            Serial.println("[CMD] NVS indisponivel; controle remoto desativado.");
            vTaskDelete(nullptr);
            return;
        }
        String ultimo = p.getString("ultimo", "");
        char ultimoId[25] = {};
        char ultimoEstado = 0;
        bool confirmarPendente = false;
        if (ultimo.length() == 26 && ultimo[24] == ':') {
            memcpy(ultimoId, ultimo.c_str(), 24);
            if (idValido(ultimoId) && (ultimo[25] == 'I' || ultimo[25] == 'E' || ultimo[25] == 'F')) {
                ultimoEstado = ultimo[25] == 'I' ? 'F' : ultimo[25];
                confirmarPendente = true;
            }
        }
        for (;;) {
            if (WiFi.status() == WL_CONNECTED) {
                if (confirmarPendente) {
                    int status = confirmar(ultimoId, ultimoEstado);
                    if (status == 200 || status == 404 || status == 409) {
                        confirmarPendente = false;
                    }
                } else if (time(nullptr) >= MIN_RELOGIO) {
                    Comando c{};
                    if (consultar(c)) {
                        if (ultimoEstado && !strcmp(c.id, ultimoId)) {
                            // Nunca aciona de novo; somente repete a confirmacao.
                            confirmarPendente = true;
                        } else if (!salvar(p, c.id, 'I')) {
                            Serial.println("[CMD] Falha ao reservar comando; nao executado.");
                        } else {
                            memcpy(ultimoId, c.id, sizeof(ultimoId));
                            bool ok = false;
                            if (xQueueSend(filaComando, &c, portMAX_DELAY) == pdTRUE) {
                                xQueueReceive(filaResultado, &ok, portMAX_DELAY);
                            }
                            ultimoEstado = ok ? 'E' : 'F';
                            if (!salvar(p, ultimoId, ultimoEstado)) {
                                Serial.println("[CMD] Falha ao persistir resultado; reserva mantida se possivel.");
                            }
                            confirmarPendente = true;
                            int status = confirmar(ultimoId, ultimoEstado);
                            if (status == 200 || status == 404 || status == 409) confirmarPendente = false;
                        }
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(INTERVALO));
        }
    }

    static void iniciar(Executor executor) {
        if (filaComando || !executor) return;
        executarLocal = executor;
        filaComando = xQueueCreate(1, sizeof(Comando));
        filaResultado = xQueueCreate(1, sizeof(bool));
        if (filaComando && filaResultado &&
            xTaskCreate(tarefaHttp, "comandosApi", 10240, nullptr, 1, nullptr) == pdPASS) {
            Serial.println("[CMD] Modulo iniciado.");
            return;
        }
        if (filaComando) vQueueDelete(filaComando);
        if (filaResultado) vQueueDelete(filaResultado);
        filaComando = filaResultado = nullptr;
        Serial.println("[CMD] Falha ao iniciar modulo.");
    }

    // Somente o loop principal altera reles, temporizador e LCD.
    static void processar() {
        if (!filaComando || !filaResultado) return;
        Comando c{};
        if (xQueueReceive(filaComando, &c, 0) != pdTRUE) return;
        time_t agora = time(nullptr);
        bool ok = false;
        if (agora >= MIN_RELOGIO && c.expiracao > agora) {
            ok = executarLocal(c);
        } else {
            Serial.println("[CMD] Comando vencido ou relogio invalido.");
        }
        xQueueSend(filaResultado, &ok, 0);
    }
}
