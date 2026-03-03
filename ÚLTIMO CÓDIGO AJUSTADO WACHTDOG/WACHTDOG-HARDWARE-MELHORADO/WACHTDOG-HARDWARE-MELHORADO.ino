// Sketch enxuto: tenta até MAX_SEND_RETRIES; ao SUCESSO ou após 3 tentativas -> dormir SEND_INTERVAL_MS
// Mantém loop de JOIN no setup (restaurado)

#define _SS_MAX_RX_BUFF 128

#include "LoRaWAN_Radioenge.h"
#include <Arduino.h>
#include <SoftwareSerial.h>
#include "DHT.h"
#include <LowPower.h>
#include <avr/wdt.h>

SoftwareSerial SerialCommand(7, 6, false); // RX, TX para o módulo Radioenge
LoRaWAN_Radioenge LoRa(&SerialCommand);

#define DHTPIN 2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Parâmetros principais
const unsigned long SEND_INTERVAL_MS = 3600UL * 1000UL; // 1 h
const uint8_t MAX_SEND_RETRIES = 3;
const unsigned long RETRY_DELAY_MS = 2000UL;
const unsigned long MODULE_RESPONSE_TIMEOUT_MS = 15000UL; // aguarda resposta
const unsigned long GRACE_AFTER_TIMEOUT_MS = 10000UL;     // janela adicional se houve timeout

// JOIN
const uint8_t MAX_JOIN_ATTEMPTS = 3;

// Watchdog
#define WATCHDOG_TIMEOUT WDTO_8S
static bool watchdogActive = false;
void enableWatchdog() { wdt_enable(WATCHDOG_TIMEOUT); wdt_reset(); watchdogActive = true; }
void disableWatchdog() { wdt_disable(); watchdogActive = false; }
inline void petWatchdog() { if (watchdogActive) wdt_reset(); }

// Função de baixo consumo — modificada para NÃO acordar por Serial durante o sono
void lowPowerDelay(unsigned long ms) {
  // desabilita WDT antes do sono longo
  disableWatchdog();

  // ENCERRA o SoftwareSerial para evitar que atividade no pino RX acorde a MCU
  // (SoftwareSerial usa pin-change interrupts; end() remove handlers)
  SerialCommand.end();

  // Dorme em blocos grandes (8s / 1s) enquanto possível
  while (ms >= 8000UL) { LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF); ms -= 8000UL; }
  while (ms >= 1000UL) { LowPower.powerDown(SLEEP_1S, ADC_OFF, BOD_OFF); ms -= 1000UL; }

  // Para o restante (<1000 ms) usamos pequenos ciclos de powerDown para NÃO ficar em delay ativo
  while (ms > 0) {
    if (ms >= 120) {
      LowPower.powerDown(SLEEP_120MS, ADC_OFF, BOD_OFF);
      ms -= 120;
    } else if (ms >= 60) {
      LowPower.powerDown(SLEEP_60MS, ADC_OFF, BOD_OFF);
      ms -= 60;
    } else if (ms >= 30) {
      LowPower.powerDown(SLEEP_30MS, ADC_OFF, BOD_OFF);
      ms -= 30;
    } else {
      // último degrau fino
      LowPower.powerDown(SLEEP_15MS, ADC_OFF, BOD_OFF);
      if (ms > 15) ms -= 15; else ms = 0;
    }
  }

  // Ao acordar, reativa o SoftwareSerial para receber respostas do módulo novamente
  SerialCommand.begin(9600);
  SerialCommand.listen();

  // NOTA: o WDT será reativado no início do loop (enableWatchdog()), conforme seu fluxo original
}

// Drena mensagens do módulo por até timeout_ms (imprime no Serial)
void drainModule(unsigned long timeout_ms) {
  SerialCommand.listen();
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    petWatchdog();
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

// waitForModuleResult: 1 = sucesso; -1 = erro; 0 = timeout (nenhuma indicação)
int waitForModuleResult(unsigned long timeout_ms) {
  SerialCommand.listen();
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    petWatchdog();
    while (SerialCommand.available() > 0) {
      String line = SerialCommand.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      Serial.print(F("[MOD] "));
      Serial.println(line);
      String up = line; up.toUpperCase();
      // sucesso
      if (up.indexOf("AT_TX_DONE") >= 0 || up.indexOf("AT_TX_OK") >= 0 ||
          up.indexOf("TX_OK") >= 0 || up.indexOf("AT_ACK_OK") >= 0 ||
          up.indexOf("ACK_OK") >= 0 || up.indexOf("AT_OK") >= 0) {
        Serial.println(F("=> Sucesso detectado."));
        return 1;
      }
      // erro
      if (up.indexOf("AT_ERROR") >= 0 || up.indexOf("AT_TX_FAIL") >= 0 ||
          up.indexOf("TX_FAIL") >= 0 || up.indexOf("ERROR") >= 0 ||
          up.indexOf("ERRO") >= 0) {
        Serial.println(F("=> Erro detectado."));
        return -1;
      }
    }
    delay(20);
  }
  Serial.println(F("=> Timeout aguardando resposta do módulo."));
  return 0;
}

// Poll curto para capturar resposta tardia (janela de GRACE). Retorna true se encontrar sucesso.
bool pollForLateSuccess(unsigned long grace_ms) {
  SerialCommand.listen();
  unsigned long start = millis();
  while (millis() - start < grace_ms) {
    petWatchdog();
    while (SerialCommand.available() > 0) {
      String line = SerialCommand.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      Serial.print(F("[MOD - LATE] "));
      Serial.println(line);
      String up = line; up.toUpperCase();
      if (up.indexOf("AT_TX_DONE") >= 0 || up.indexOf("AT_TX_OK") >= 0 ||
          up.indexOf("TX_OK") >= 0 || up.indexOf("AT_ACK_OK") >= 0 ||
          up.indexOf("ACK_OK") >= 0 || up.indexOf("AT_OK") >= 0) {
        Serial.println(F("=> (late) Sucesso detectado."));
        return true;
      }
      if (up.indexOf("AT_ERROR") >= 0 || up.indexOf("AT_TX_FAIL") >= 0 ||
          up.indexOf("TX_FAIL") >= 0 || up.indexOf("ERROR") >= 0 ||
          up.indexOf("ERRO") >= 0) {
        Serial.println(F("=> (late) Erro detectado."));
        return false;
      }
    }
    delay(50);
  }
  return false;
}

// Envia payload com até MAX_SEND_RETRIES; retorna true se algum envio for confirmado, false se todas falharem.
bool sendPayload(const char* payload) {
  for (uint8_t attempt = 1; attempt <= MAX_SEND_RETRIES; ++attempt) {
    petWatchdog();
    Serial.print(F("Enviando (tentativa "));
    Serial.print(attempt);
    Serial.print(F(") "));
    Serial.println(payload);

    drainModule(50);

    bool ok = LoRa.SendString(payload, 1);
    if (!ok) {
      Serial.println(F("SendString retornou false (falha local ao enviar comando)."));
      if (attempt < MAX_SEND_RETRIES) {
        unsigned long end = millis() + RETRY_DELAY_MS;
        while (millis() < end) { petWatchdog(); delay(10); }
        continue;
      } else {
        // última tentativa local falhou => considera falha definitiva
        return false;
      }
    }

    int res = waitForModuleResult(MODULE_RESPONSE_TIMEOUT_MS);

    if (res == 1) {
      // sucesso explícito
      Serial.println(F("Envio confirmado pelo módulo (sucesso)."));
      drainModule(100);
      return true;
    } else if (res == -1) {
      // erro explícito do módulo -> tenta novamente (se houver tentativas)
      Serial.println(F("Módulo retornou erro nesta tentativa."));
      if (attempt < MAX_SEND_RETRIES) {
        unsigned long end = millis() + RETRY_DELAY_MS;
        while (millis() < end) { petWatchdog(); delay(10); }
        continue;
      } else {
        return false;
      }
    } else {
      // timeout primário -> aguarda janela de grace antes de decidir reenviar
      Serial.println(F("Timeout primário: aguardando janela adicional (grace) para resposta tardia..."));
      bool late = pollForLateSuccess(GRACE_AFTER_TIMEOUT_MS);
      if (late) {
        Serial.println(F("Resposta tardia indicou sucesso -> não reenviaremos."));
        drainModule(50);
        return true;
      } else {
        Serial.println(F("Nenhuma resposta tardia -> tentativa considerada falha."));
        if (attempt < MAX_SEND_RETRIES) {
          unsigned long end = millis() + RETRY_DELAY_MS;
          while (millis() < end) { petWatchdog(); delay(10); }
          continue;
        } else {
          return false;
        }
      }
    }
  }
  // não deveria chegar aqui, mas por segurança:
  return false;
}

// ---------- Setup e loop ----------
void setup() {
  Serial.begin(115200);
  SerialCommand.begin(9600);
  delay(100);
  SerialCommand.listen();

  Serial.println();
  Serial.println(F("Inicializando..."));

  LoRa.begin();

  // --- Loop de JOIN (restaurado) ---
  uint8_t joinAttempt = 0;
  bool joined = false;
  while (joinAttempt < MAX_JOIN_ATTEMPTS && !joined) {
    Serial.print(F("Tentativa JOIN #"));
    Serial.println(joinAttempt + 1);
    joined = LoRa.JoinNetwork(OTAA, TTN);
    if (!joined) {
      Serial.println(F("Join falhou, aguardando 2s..."));
      delay(2000);
    }
    joinAttempt++;
  }
  if (joined) {
    Serial.println(F("Join OK"));
    delay(500); // pequeno delay para estabilizar módulo antes de primeiro envio
  } else {
    Serial.println(F("Sem join. Continuação local (envios podem falhar)."));
  }
  // --- fim loop de JOIN ---

  LoRa.CFM(true);
  LoRa.NBTRIALS(3);
  drainModule(200);

  dht.begin();
  Serial.println(F("DHT inicializado."));
}

void loop() {
  // Ativa WDT para proteger contra travamentos durante o ciclo ativo
  enableWatchdog();

  // 1) leitura do sensor
  float hum = dht.readHumidity();
  float tempC = dht.readTemperature();

  if (isnan(hum) || isnan(tempC)) {
    Serial.println(F("Falha ao ler DHT11. Não envia neste ciclo. Indo dormir."));
    // desativa WDT e dorme uma vez; NÃO reenvia nem reinicia
    lowPowerDelay(SEND_INTERVAL_MS);
    return;
  }

  // 2) formata payload
  char tempStr[6], humStr[6];
  dtostrf(tempC, 4, 1, tempStr);
  dtostrf(hum,   4, 1, humStr);
  char payload[64];
  snprintf(payload, sizeof(payload), "{\"t\":%s,\"h\":%s}", tempStr, humStr);

  Serial.print(F("Leitura: T="));
  Serial.print(tempStr);
  Serial.print(F(" C, H="));
  Serial.println(humStr);

  // 3) tenta enviar até MAX_SEND_RETRIES (com grace window)
  bool sent = sendPayload(payload);

  if (sent) {
    Serial.println(F("Envio concluído (sucesso). Indo dormir até próximo ciclo."));
  } else {
    Serial.println(F("Todas as tentativas falharam. Não reenviaremos neste ciclo. Indo dormir."));
  }

  // Antes de dormir, drena e desliga WDT dentro de lowPowerDelay
  drainModule(100);
  lowPowerDelay(SEND_INTERVAL_MS);

  // Ao acordar, o loop reinicia e re-habilita WDT no começo
}
