#include "LoRaWAN_Radioenge.h"
#include <Arduino.h>
#include <SoftwareSerial.h>

SoftwareSerial SerialCommand(7, 6, false);
LoRaWAN_Radioenge LoRa(&SerialCommand);

// Configurações de envio
const uint8_t TOTAL_MSGS = 30;
const unsigned long SEND_INTERVAL_MS = 20UL * 1000UL; // 20 segundos

// Piscar pino original
const unsigned long LED_TOGGLE_MS = 5UL * 1000UL; // 5 segundos
const uint8_t LED_PIN = 2;

unsigned long lastSendMillis = 0;
unsigned long lastLedMillis = 0;
uint8_t sentCount = 0;
bool ledState = false;

void setup() {
  Serial.begin(9600);
  SerialCommand.begin(9600);

  Serial.println(F("Iniciando Radioenge..."));
  LoRa.begin();
  LoRa.printParameters();

  // Tentar juntar na rede (até 3 tentativas)
  const uint8_t maxJoinAttempts = 3;
  uint8_t attempt = 0;
  bool joined = false;

  while(attempt < maxJoinAttempts && !joined) {
    Serial.print(F("Tentativa de join #"));
    Serial.println(attempt + 1);
    joined = LoRa.JoinNetwork(OTAA, TTN);
    if(!joined) {
      Serial.println(F("Join falhou - tentando novamente em 3s"));
      delay(3000);
    }
    attempt++;
  }

  if(joined) {
    Serial.println(F("Join realizado com sucesso."));
  } else {
    Serial.println(F("Não foi possível realizar o join. Verifique configurações e rede."));
    // Decide-se aqui não bloquear; ainda continuamos o sketch para poder testar comportamento local.
  }

  // Mantenha pino configurado como antes
  LoRa.pinMode(LED_PIN, RADIOENGE_OUTPUT_PUSH_PULL);

  // Inicializa timers para envio e toggle (dispara primeiro envio imediatamente)
  lastSendMillis = 0; // 0 -> enviar imediatamente na primeira passagem do loop
  lastLedMillis = millis();
}

void loop() {
  unsigned long now = millis();

  // --- Envio de mensagens (10 mensagens, uma a cada 10s) ---
  if (sentCount < TOTAL_MSGS) {
    if (lastSendMillis == 0 || (now - lastSendMillis >= SEND_INTERVAL_MS)) {
      // Monta payload
      char payload[64];
      snprintf(payload, sizeof(payload), "mensagem-%02u", sentCount + 1);

      // Envia
      bool ok = LoRa.SendString(payload, 1);
      if (ok) {
        Serial.print(F("Enviado ["));
        Serial.print(sentCount + 1);
        Serial.print(F("/"));
        Serial.print(TOTAL_MSGS);
        Serial.print(F("]: "));
        Serial.println(payload);
      } else {
        Serial.print(F("Falha no envio ["));
        Serial.print(sentCount + 1);
        Serial.println(F("]"));
      }

      sentCount++;
      lastSendMillis = now;
    }
  } else {
    // Opcional: quando terminar os envios, print e não enviar mais.
    // Uncomment se quiser uma mensagem única notificando término:
    // static bool finishedPrinted = false;
    // if(!finishedPrinted){ Serial.println(F("Envios concluídos.")); finishedPrinted = true; }
  }

  // --- Toggle pino 2 a cada 5s (comportamento original preservado) ---
  if (now - lastLedMillis >= LED_TOGGLE_MS) {
    ledState = !ledState;
    LoRa.digitalWrite(LED_PIN, ledState ? 1 : 0);
    Serial.print(F("LED_PIN "));
    Serial.print(LED_PIN);
    Serial.print(F(" -> "));
    Serial.println(ledState ? "ON" : "OFF");
    lastLedMillis = now;
  }

  // Pequena pausa para reduzir uso de CPU (não atrapalha timers)
  delay(10);
}
