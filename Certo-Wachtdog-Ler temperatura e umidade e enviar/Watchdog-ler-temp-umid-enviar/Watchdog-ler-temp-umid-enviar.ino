// Reduz o buffer interno do SoftwareSerial para economizar RAM
#define _SS_MAX_RX_BUFF 64

#include "LoRaWAN_Radioenge.h"
#include <Arduino.h>
#include <SoftwareSerial.h>
#include "DHT.h"
#include <LowPower.h> // biblioteca rocketscream/Low-Power

// --- Hardware / biblioteca ---
SoftwareSerial SerialCommand(7, 6, false); // RX, TX para o módulo Radioenge
LoRaWAN_Radioenge LoRa(&SerialCommand);

// DHT
#define DHTPIN 2      // DHT11 no pino digital 2
#define DHTTYPE DHT11 // DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- Configurações ---
const unsigned long SEND_INTERVAL_MS = 3600UL * 1000UL; // intervalo entre leituras/envios
const uint8_t MAX_SEND_RETRIES = 3;         // tentativas por leitura (inclui 1ª)
const unsigned long RETRY_DELAY_MS = 2000UL; // espera entre tentativas (ms)
const uint8_t MAX_JOIN_ATTEMPTS = 3;
const unsigned long MODULE_RESPONSE_TIMEOUT_MS = 6000UL; // tempo para aguardar resposta do módulo após envio (ms)

// ----------------- Função de baixo consumo simplificada -----------------
void lowPowerDelay(unsigned long ms) {
  // Usa ciclos de 8s repetidos + ciclos de 1s para o resto, e delay() para o remanescente <1s.
  while (ms >= 8000UL) {
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
    ms -= 8000UL;
  }
  while (ms >= 1000UL) {
    LowPower.powerDown(SLEEP_1S, ADC_OFF, BOD_OFF);
    ms -= 1000UL;
  }
  if (ms > 0) delay(ms);
}

// ----------------- Utilitários de comunicação com o módulo -----------------

// Drena possíveis linhas pendentes do módulo por até timeout_ms (imprime no Serial).
void drainModule(unsigned long timeout_ms) {
  SerialCommand.listen();
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

// Aguarda por uma resposta do módulo que indique SUCESSO ou ERRO para o envio.
// Retorna true se encontrou um indicador de sucesso; false se encontrou erro ou timeout.
// Nota: adicionada detecção de "AT_ACK_OK" / "ACK_OK" como sucesso.
bool waitForModuleResult(unsigned long timeout_ms) {
  SerialCommand.listen();
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

      // --- Sucesso (verificar primeiro) ---
      if (up.indexOf("AT_TX_DONE") >= 0 ||
          up.indexOf("AT_TX_OK") >= 0 ||
          up.indexOf("TX_OK") >= 0 ||
          up.indexOf("AT_ACK_OK") >= 0 ||   // ADIÇÃO: aceita ACK explícito do firmware
          up.indexOf("ACK_OK") >= 0 ||      // ADIÇÃO: padrão genérico com ACK
          up.indexOf("AT_OK") >= 0) {
        Serial.println(F("=> Indicador de SUCESSO detectado pelo módulo."));
        return true;
      }

      // --- Erro (se nenhuma das condições de sucesso foi satisfeita) ---
      if (up.indexOf("AT_ERROR") >= 0 ||
          up.indexOf("AT_TX_FAIL") >= 0 ||
          up.indexOf("TX_FAIL") >= 0 ||
          up.indexOf("ERROR") >= 0 ||
          up.indexOf("ERRO") >= 0) {
        Serial.println(F("=> Indicador de ERRO detectado pelo módulo."));
        return false;
      }

      // Outras linhas são logs; continuamos a esperar até timeout
    }
    delay(20);
  }

  // Timeout sem indicação explícita -> considerar falha nesta tentativa (decisão conservadora)
  Serial.println(F("=> Timeout aguardando resposta do módulo (assumindo falha nesta tentativa)."));
  return false;
}

// ----------------- Setup / Loop principal -----------------
void setup() {
  Serial.begin(115200);            // monitor serial
  SerialCommand.begin(9600);       // porta para o módulo Radioenge
  delay(100);

  // Garantir que o SoftwareSerial esteja em modo "listening" para receber respostas do módulo
  SerialCommand.listen();

  Serial.println();
  Serial.println(F("Inicializando..."));

  // Inicializa LoRa module
  LoRa.begin();
  // Evitar chamadas muito verbosas aqui; printParameters() pode gerar saída grande -> comentar se problema
  // LoRa.printParameters();

  // Tenta join na rede OTAA TTN (até MAX_JOIN_ATTEMPTS)
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

  // Habilita confirmação de uplink (+CFM) e define número de tentativas de retransmissão no módulo
  LoRa.CFM(true);
  LoRa.NBTRIALS(3);
  // Drena possíveis respostas após configurar
  drainModule(200);

  // Inicializa DHT
  dht.begin();
  Serial.println(F("DHT inicializado."));
}

void loop() {
  // 1) leitura do sensor
  float hum = dht.readHumidity();
  float tempC = dht.readTemperature(); // Celsius

  if (isnan(hum) || isnan(tempC)) {
    Serial.println(F("Falha ao ler DHT11. Indo dormir até próximo ciclo."));
    lowPowerDelay(SEND_INTERVAL_MS);
    return;
  }

  // 2) converter floats para string usando dtostrf (evita printf com float)
  char tempStr[6]; // suficiente para "-XX.X" + '\0'
  char humStr[6];
  dtostrf(tempC, 4, 1, tempStr);
  dtostrf(hum,   4, 1, humStr);

  // 3) montar payload pequeno (carry somente valores)
  char payload[40];
  // formato curto: {"t":25.3,"h":60.0}
  snprintf(payload, sizeof(payload), "{\"t\":%s,\"h\":%s}", tempStr, humStr);

  Serial.print(F("Leitura: T="));
  Serial.print(tempStr);
  Serial.print(F(" C, H="));
  Serial.println(humStr);

  // 4) tentativa de envio com retries (se falhar, tenta MAX_SEND_RETRIES e segue)
  bool sent = false;
  for (uint8_t attempt = 1; attempt <= MAX_SEND_RETRIES; ++attempt) {
    Serial.print(F("Enviando (tentativa "));
    Serial.print(attempt);
    Serial.print(F(") "));
    Serial.println(payload);

    // limpar buffer antes de enviar
    drainModule(50);

    // enviar via AT+SEND (SendString) — biblioteca original
    bool ok = LoRa.SendString(payload, 1); // retorna booleano indicando apenas envio do comando AT localmente
    if (!ok) {
      Serial.println(F("Envio do comando AT falhou localmente (SendString retornou false)."));
      // pequena espera antes de nova tentativa
      if (attempt < MAX_SEND_RETRIES) {
        delay(RETRY_DELAY_MS);
        continue;
      } else {
        // última tentativa já falhou localmente
        break;
      }
    }

    // Se o comando AT foi enviado, aguardamos pela resposta assíncrona do módulo
    bool attemptResult = waitForModuleResult(MODULE_RESPONSE_TIMEOUT_MS);

    if (attemptResult) {
      Serial.println(F("Envio confirmado pelo módulo."));
      sent = true;
      // drenar quaisquer mensagens residuais curtas
      drainModule(100);
      break;
    } else {
      Serial.println(F("Falha nesta tentativa de envio (segundo parsing do módulo)."));
      // pequena pausa antes da próxima tentativa
      if (attempt < MAX_SEND_RETRIES) delay(RETRY_DELAY_MS);
    }
  }

  if (!sent) {
    Serial.println(F("Falha definitiva nesta leitura (exauriu tentativas)."));
  }

  // 5) Entrar em modo baixo consumo para economizar RAM/CPU até o próximo envio
  Serial.println(F("Modo baixo consumo..."));
  lowPowerDelay(SEND_INTERVAL_MS);

  // Ao acordar, o loop recomeça automaticamente
}
