#include <cstring>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>

// --------------------------- Configuracion ---------------------------
#define STATION_ID 1
#define SLEEP_TIME_MIN 30
// Misma red 2,4 GHz que la central (solo escaneo para leer el canal; no se usa contraseña).
static const char *WIFI_SSID_FOR_CHANNEL_SCAN = "MYM";
// Si no aparece el SSID en el escaneo, se usa este canal (p. ej. prueba en banco sin AP).
static const uint8_t ESPNOW_FALLBACK_CHANNEL = 11;
static const int SENSOR_PINS[3] = {34, 35, 32}; // ADC1

static const uint8_t ADC_SAMPLES = 21;
static const uint16_t ADC_GAP_US = 200;

static void sortU16(uint16_t *a, uint8_t n) {
  for (uint8_t i = 0; i < n - 1; i++)
    for (uint8_t j = i + 1; j < n; j++)
      if (a[j] < a[i]) {
        uint16_t t = a[i];
        a[i] = a[j];
        a[j] = t;
      }
}

/** Mediana de N lecturas — mismo criterio que calibrador_sensor (capacitivos, ruido). */
uint16_t readRawStable(int pin) {
  static uint16_t buf[ADC_SAMPLES];
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
    buf[i] = (uint16_t)analogRead(pin);
    delayMicroseconds(ADC_GAP_US);
  }
  sortU16(buf, ADC_SAMPLES);
  return buf[ADC_SAMPLES / 2];
}

// MAC STA de la central
uint8_t centralAddress[] = {0x1C, 0xC3, 0xAB, 0xC2, 0x64, 0xA8};

// Payload recomendado (coincide con la central)
struct __attribute__((packed)) SensorPacketV2 {
  uint8_t stationID;
  uint8_t sensorID;
  uint16_t rawValue;
  uint16_t batteryMv;
  uint32_t nonce;
};

SensorPacketV2 packet;
esp_now_peer_info_t peerInfo;
uint32_t txNonce = 0;

uint16_t readBatteryMv() {
  // Placeholder simple. Si luego agregas divisor a un pin ADC dedicado,
  // reemplazar por lectura real.
  return 0;
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  (void)mac_addr;
  Serial.printf("ESP-NOW TX: %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

/** Canal del AP donde está la central (misma banda 2,4 GHz). Elige el BSSID con mejor RSSI si hay varios. */
static uint8_t detectWifiChannelForEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(80);
  int n = WiFi.scanNetworks(false, false, false, 120);
  if (n <= 0) {
    Serial.printf("[ESP-NOW] Scan sin resultados, fallback canal %u\n", (unsigned)ESPNOW_FALLBACK_CHANNEL);
    return ESPNOW_FALLBACK_CHANNEL;
  }
  int best = -1, bestRssi = -999;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != WIFI_SSID_FOR_CHANNEL_SCAN) continue;
    int r = WiFi.RSSI(i);
    if (r > bestRssi) {
      bestRssi = r;
      best = i;
    }
  }
  if (best < 0) {
    WiFi.scanDelete();
    Serial.printf("[ESP-NOW] SSID \"%s\" no visto, fallback canal %u\n", WIFI_SSID_FOR_CHANNEL_SCAN,
                  (unsigned)ESPNOW_FALLBACK_CHANNEL);
    return ESPNOW_FALLBACK_CHANNEL;
  }
  uint8_t ch = (uint8_t)WiFi.channel(best);
  WiFi.scanDelete();
  if (ch < 1 || ch > 13) ch = ESPNOW_FALLBACK_CHANNEL;
  Serial.printf("[ESP-NOW] SSID \"%s\" canal=%u RSSI=%d\n", WIFI_SSID_FOR_CHANNEL_SCAN, (unsigned)ch, bestRssi);
  return ch;
}

void sendSensorData(uint8_t sensorId, uint16_t raw) {
  packet.stationID = STATION_ID;
  packet.sensorID = sensorId;
  packet.rawValue = raw;
  packet.batteryMv = readBatteryMv();
  packet.nonce = ++txNonce;

  for (uint8_t r = 0; r < 3; r++) {
    esp_err_t result = esp_now_send(centralAddress, (uint8_t *)&packet, sizeof(packet));
    if (result == ESP_OK) break;
    delay(25);
  }
}

void setupEspNow() {
  uint8_t ch = detectWifiChannelForEspNow();
  delay(50);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error inicializando ESP-NOW");
    return;
  }

  esp_now_register_send_cb(onDataSent);

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, centralAddress, 6);
  peerInfo.channel = ch;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Error al anadir peer");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Satelite iniciando...");

  analogReadResolution(12);
  for (uint8_t i = 0; i < 3; i++)
    analogSetPinAttenuation((uint8_t)SENSOR_PINS[i], ADC_11db);

  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);

  setupEspNow();
}

void loop() {
  Serial.println("--- Diagnostico Satelite ---");
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t raw = readRawStable(SENSOR_PINS[i]);
    Serial.printf("Sensor %u pin %d raw=%u\n", i + 1, SENSOR_PINS[i], raw);
    sendSensorData(i + 1, raw);
    delay(40);
  }

  Serial.printf("Entrando en deep sleep por %u min...\n", SLEEP_TIME_MIN);
  WiFi.disconnect(true);
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TIME_MIN * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}
