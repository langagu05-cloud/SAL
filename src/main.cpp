#include <Arduino.h>
#include <driver/i2s.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ============================================================================
// CONFIGURAÇÃO BLUETOOTH (BLE)
// ============================================================================
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_AUDIO_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHARACTERISTIC_TEXT_UUID  "8b365922-358b-42ea-92b0-8f61536b1d4c"

BLEServer* pServer = NULL;
BLECharacteristic* pAudioCharacteristic = NULL;
BLECharacteristic* pTextCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// --- Pinos (ESP32 WROOM + INMP441) ---
#define MIC_PORT  I2S_NUM_0
#define MIC_SCK   14
#define MIC_WS    15
#define MIC_SD    32

#define PIN_LED       2

// --- Áudio ---
#define MIC_SAMPLE_RATE   8000
#define ACTUAL_REC_SEC    3     
#define WAV_HEADER_SIZE   44
#define PCM_BYTES         (MIC_SAMPLE_RATE * ACTUAL_REC_SEC * 2)
#define AUDIO_BUF_SIZE    (WAV_HEADER_SIZE + PCM_BYTES) // Exatamente 48044 bytes
#define MIC_GAIN_SHIFT    12
#define VAD_THRESHOLD     300   // Alta sensibilidade para captar a fala facilmente

uint8_t* audioBuf = NULL;

static inline size_t minz(size_t a, size_t b) { return a < b ? a : b; }

// ============================================================================
// CALLBACKS DO BLUETOOTH
// ============================================================================
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("\n[BLE] Telemóvel/PC conectado ao Sálvio com sucesso!");
    digitalWrite(PIN_LED, HIGH);
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("\n[BLE] Dispositivo desconectado. A reiniciar advertising...");
    digitalWrite(PIN_LED, LOW);
  }
};

class MyTextCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      Serial.println("==================================================");
      Serial.printf("👴💬 SÁLVIO DIZ: \"%s\"\n", rxValue.c_str());
      Serial.println("==================================================");
    }
  }
};

// ============================================================================
// MICROFONE (INMP441)
// ============================================================================
void setupMic() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = MIC_SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;

  i2s_pin_config_t pins = {
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD
  };
  i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
}

size_t micRead(int16_t* out, size_t maxSamples) {
  static int32_t raw[256];
  size_t total = 0;
  while (total < maxSamples) {
    size_t want = minz(maxSamples - total, 256);
    size_t br = 0;
    i2s_read(MIC_PORT, raw, want * sizeof(int32_t), &br, pdMS_TO_TICKS(200));
    size_t n = br / sizeof(int32_t);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      int32_t s = raw[i] >> MIC_GAIN_SHIFT;
      if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
      out[total + i] = (int16_t)s;
    }
    total += n;
  }
  return total;
}

void micFlush(uint32_t ms) {
  static int16_t tmp[256];
  uint32_t t0 = millis();
  while (millis() - t0 < ms) micRead(tmp, 256);
}

static uint32_t meanAbs(const int16_t* s, size_t n) {
  if (!n) return 0;
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i++) acc += (s[i] < 0) ? -s[i] : s[i];
  return acc / n;
}

bool listenAndRecord() {
  int16_t* pcm = (int16_t*)(audioBuf + 44);
  const size_t totalSamples = PCM_BYTES / 2;
  const size_t BLOCK = 512;

  micFlush(200);
  size_t pos = 0; int loud = 0;

  while (loud < 3) {
    if (!deviceConnected) return false;
    size_t n = micRead(pcm + pos, BLOCK);
    if (n == 0) continue;
    uint32_t level = meanAbs(pcm + pos, n);
    if (level > VAD_THRESHOLD) { loud++; } else { loud = 0; }
  }

  Serial.println("\n[MIC] Voz detetada! A gravar áudio...");
  digitalWrite(PIN_LED, LOW); // Pisca LED para indicar gravação
  
  pos = 0;
  while (pos < totalSamples) {
    size_t n = micRead(pcm + pos, minz(BLOCK, totalSamples - pos));
    if (n == 0) break;
    pos += n;
  }
  if (pos < totalSamples) memset(pcm + pos, 0, (totalSamples - pos) * 2);

  digitalWrite(PIN_LED, HIGH); // Volta a acender fixo quando termina
  return true;
}

void buildWavHeader(uint8_t* h, uint32_t pcmBytes, uint32_t rate) {
  uint32_t fSize = 36 + pcmBytes;
  uint32_t byteRate = rate * 2;
  const uint8_t hdr[44] = {
    'R','I','F','F', (uint8_t)fSize, (uint8_t)(fSize>>8), (uint8_t)(fSize>>16), (uint8_t)(fSize>>24),
    'W','A','V','E','f','m','t',' ', 16, 0, 0, 0, 1, 0, 1, 0,
    (uint8_t)rate, (uint8_t)(rate>>8), (uint8_t)(rate>>16), (uint8_t)(rate>>24),
    (uint8_t)byteRate, (uint8_t)(byteRate>>8), (uint8_t)(byteRate>>16), (uint8_t)(byteRate>>24),
    2, 0, 16, 0, 'd','a','t','a',
    (uint8_t)pcmBytes, (uint8_t)(pcmBytes>>8), (uint8_t)(pcmBytes>>16), (uint8_t)(pcmBytes>>24)
  };
  memcpy(h, hdr, 44);
}

// ============================================================================
// SETUP E LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== SÁLVIO - MODO BLE ROBUSTO ===");

  audioBuf = (uint8_t*)malloc(AUDIO_BUF_SIZE);
  if (!audioBuf) { Serial.println("[ERRO] Memória RAM insuficiente!"); return; }

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  setupMic();

  // Inicializa BLE
  BLEDevice::init("Salvio-Assistente");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // CARACTERÍSTICA DE ÁUDIO COM O DESCRITOR 2902 PARA NOTIFICAÇÕES (Corrigido!)
  pAudioCharacteristic = pService->createCharacteristic(
                         CHARACTERISTIC_AUDIO_UUID,
                         BLECharacteristic::PROPERTY_READ |
                         BLECharacteristic::PROPERTY_NOTIFY
                       );
  pAudioCharacteristic->addDescriptor(new BLE2902());

  // CARACTERÍSTICA DE TEXTO PARA RECEBER A RESPOSTA (Corrigido!)
  pTextCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_TEXT_UUID,
                        BLECharacteristic::PROPERTY_WRITE
                      );
  pTextCharacteristic->setCallbacks(new MyTextCallbacks());

  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); 
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("--- Sálvio BLE pronto! À espera de conexão... ---");
}

void loop() {
  // Gestão de reconexão BLE
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); 
    pServer->startAdvertising(); 
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected) {
    if (listenAndRecord()) {
      buildWavHeader(audioBuf, PCM_BYTES, MIC_SAMPLE_RATE);
      Serial.println("[BLE] Áudio pronto. A transmitir pacotes via BLE...");
      
      size_t totalSize = AUDIO_BUF_SIZE;
      size_t offset = 0;
      const size_t chunkSize = 180; // Otimizado para estabilidade máxima no BLE do Windows

      while (offset < totalSize) {
        size_t len = minz(chunkSize, totalSize - offset);
        pAudioCharacteristic->setValue(audioBuf + offset, len);
        pAudioCharacteristic->notify();
        offset += len;
        delay(20); // Pausa para garantir que o buffer BLE processa sem erros
      }
      
      Serial.println("[BLE] Transmissão de áudio concluída! À espera do retorno da IA...");
      delay(2000); 
    }
  } else {
    delay(500);
  }
}