#include "LoRaWAN_Radioenge.h"
#include <Arduino.h>
#include <SoftwareSerial.h>

// Mantive o seu construtor original (incluindo o terceiro parâmetro)
SoftwareSerial SerialCommand(7, 6, false);
LoRaWAN_Radioenge LoRa(&SerialCommand);

// Configurações de envio
const uint8_t TOTAL_MSGS = 20;
const unsigned long SEND_INTERVAL_MS = 30UL * 1000UL; // 30 segundos

// Piscar pino original
const unsigned long LED_TOGGLE_MS = 5UL * 1000UL; // 5 segundos
const uint8_t LED_PIN = 2;

unsigned long lastSendMillis = 0;
unsigned long lastLedMillis = 0;
uint8_t sentCount = 0;
bool ledState = false;

// Timeouts e tentativas
const unsigned long MODULE_RESPONSE_TIMEOUT_MS = 8000UL; // tempo para esperar resposta do módulo após um send
const uint8_t MAX_SEND_ATTEMPTS = 3;

// Drena possíveis linhas pendentes do módulo por até timeout_ms (imprime no Serial).
void drainModule(unsigned long timeout_ms) {
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    while (SerialCommand.available() > 0) {
      String L = SerialCommand.readStringUntil('\n');
      L.trim();
      if (L.length() == 0) continue;
      Serial.print(F("[MOD] "));
      Serial.println(L);
    }
    delay(10);
  }
}

// Espera por uma resposta do módulo que indique SUCESSO ou ERRO para o envio.
// Retorna true se encontrou um indicador de sucesso, false se encontrou erro ou timeout.
bool waitForSendResult(unsigned long timeout_ms) {
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    while (SerialCommand.available() > 0) {
      String line = SerialCommand.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      Serial.print(F("[MOD] "));
      Serial.println(line);

      String up = line;
      up.toUpperCase();

      // Indicadores de erro
      if (up.indexOf("ERRO") >= 0 ||
          up.indexOf("ERROR") >= 0 ||
          up.indexOf("AT_ERROR") >= 0 ||
          up.indexOf("AT_TX_FAIL") >= 0 ||
          up.indexOf("TX_FAIL") >= 0) {
        Serial.println(F("=> Detectado erro pelo módulo."));
        return false;
      }

      // Indicadores de sucesso
      if (up.indexOf("AT_OK") >= 0 ||
          // cuidado: "OK" aparece em muitas mensagens; mantemos como indicador complementar
          up.indexOf("OK") >= 0 ||
          up.indexOf("AT_TX_DONE") >= 0 ||
          up.indexOf("AT_TX_OK") >= 0 ||
          up.indexOf("TX_OK") >= 0) {
        Serial.println(F("=> Detectado sucesso pelo módulo."));
        return true;
      }

      // Outras linhas são apenas logs; continuamos aguardando até timeout.
    }
    delay(20);
  }

  // Timeout sem mensagens claras -> considerar falha (decisão conservadora)
  Serial.println(F("=> Timeout aguardando resposta do módulo (assumindo falha nesta tentativa)."));
  return false;
}

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

  while (attempt < maxJoinAttempts && !joined) {
    Serial.print(F("Tentativa de join #"));
    Serial.println(attempt + 1);
    joined = LoRa.JoinNetwork(OTAA, TTN);
    if (!joined) {
      Serial.println(F("Join falhou - tentando novamente em 3s"));
      unsigned long waitStart = millis();
      while (millis() - waitStart < 3000UL) {
        // drena linhas do módulo enquanto espera (evita acumular)
        drainModule(50);
        delay(10);
      }
    }
    attempt++;
  }

  if (joined) {
    Serial.println(F("Join realizado com sucesso."));
  } else {
    Serial.println(F("Não foi possível realizar o join. Verifique configurações e rede."));
    // Segue o sketch para permitir testes locais.
  }

  // Habilita confirmed uplinks e configura 3 retransmissões no módulo
  LoRa.CFM(true);
  LoRa.NBTRIALS(3);
  // Drena possíveis respostas após configurar
  drainModule(200);

  // Mantém pino configurado como antes
  LoRa.pinMode(LED_PIN, RADIOENGE_OUTPUT_PUSH_PULL);

  // Inicializa timers para envio e toggle (dispara primeiro envio imediatamente)
  lastSendMillis = 0; // 0 -> enviar imediatamente na primeira passagem do loop
  lastLedMillis = millis();
}

void loop() {
  unsigned long now = millis();

  // --- Envio de mensagens (TOTAL_MSGS mensagens, intervalo definido) ---
  if (sentCount < TOTAL_MSGS) {
    if (lastSendMillis == 0 || (now - lastSendMillis >= SEND_INTERVAL_MS)) {
      // Monta payload
      char payload[64];
      snprintf(payload, sizeof(payload), "mensagem-%02u", sentCount + 1);

      Serial.print(F("Preparando envio ["));
      Serial.print(sentCount + 1);
      Serial.print(F("/"));
      Serial.print(TOTAL_MSGS);
      Serial.print(F("]: "));
      Serial.println(payload);

      // Drena restos antigos do módulo para começarmos com buffer limpo
      drainModule(100);

      bool overallSuccess = false;
      for (uint8_t attempt = 1; attempt <= MAX_SEND_ATTEMPTS; ++attempt) {
        Serial.print(F("Tentativa de envio "));
        Serial.print(attempt);
        Serial.print(F(" de "));
        Serial.println(MAX_SEND_ATTEMPTS);

        // envia o payload (SendString usa AT+SEND na biblioteca original)
        bool cmdOk = LoRa.SendString(payload, 1);

        if (!cmdOk) {
          Serial.println(F("Erro ao montar/enviar comando AT localmente (SendString retornou false)."));
          // aguarda e tenta novamente
          drainModule(100);
          continue;
        }

        // aguardamos resposta do módulo indicando sucesso / erro
        bool attemptResult = waitForSendResult(MODULE_RESPONSE_TIMEOUT_MS);

        if (attemptResult) {
          Serial.println(F("Envio confirmado pelo módulo (ou resposta de sucesso detectada)."));
          overallSuccess = true;
          // Drena possíveis linhas restantes
          drainModule(100);
          break;
        } else {
          Serial.println(F("Falha nesta tentativa de envio. Será tentado novamente (se houver tentativas)."));
          // pequena pausa antes de nova tentativa para dar tempo ao módulo se recuperar
          delay(200);
        }
      } // for attempts

      if (overallSuccess) {
        Serial.print(F("Enviado com sucesso ["));
        Serial.print(sentCount + 1);
        Serial.print(F("/"));
        Serial.print(TOTAL_MSGS);
        Serial.println(F("]."));
      } else {
        Serial.print(F("Todas as tentativas falharam para a mensagem ["));
        Serial.print(sentCount + 1);
        Serial.println(F("]. Prosseguindo para a próxima mensagem."));
      }

      // Considera a mensagem 'tratada' (sucesso ou falha após 3 tentativas) -> incrementa contador
      sentCount++;

      // Atualiza lastSendMillis
      lastSendMillis = now;
    }
  } else {
    // Opcional: quando terminar os envios, print e não enviar mais.
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
