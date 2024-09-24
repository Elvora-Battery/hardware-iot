#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include <HardwareSerial.h>

// Deklarasi serial untuk PZEM-017
HardwareSerial PZEM(2); // UART2 menggunakan TX2 (GPIO17) dan RX2 (GPIO16)
TinyGPSPlus gps;
SoftwareSerial ss(4, 15); // SoftwareSerial untuk GPS

// Konfigurasi SIM800L
#define TINY_GSM_MODEM_SIM800
#define MODEM_RST 13
#define MODEM_PWRKEY 12
#define MODEM_POWER_ON 14
#define MODEM_TX 27  // TX pada SIM800L dihubungkan ke RX pada ESP32
#define MODEM_RX 26  // RX pada SIM800L dihubungkan ke TX pada ESP32
#define SerialMon Serial
#define SerialAT Serial1

// Inisialisasi pin untuk sensor LM35
const int lm35Pin = 34;

// GPRS credentials (APN, username, password)
const char apn[] = "Telkomsel"; // Ganti dengan APN provider Anda
const char gprsUser[] = "wap";    // Username GPRS (jika ada)
const char gprsPass[] = "wap123";    // Password GPRS (jika ada)

// Variabel SIM800L dan MQTT
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient;
PubSubClient client(gsmClient);

// Deklarasi array untuk menyimpan 10 kode unik token
String tokens[10] = {
  "TOKEN1234", "TOKEN5678", "TOKEN9012", "TOKEN3456", "TOKEN7890",
  "TOKEN1122", "TOKEN3344", "TOKEN5566", "TOKEN7788", "TOKEN9900"
};

String tokenIn = "";
String totalRemainingTimeFormated;
String coordinateBattery;
String latitude = "";
String longitude = "";

unsigned long tokenExpireTime[10] = {0};
unsigned long tokenStartTime[10] = {0};
int ElvoraActive[10] = {0};
int tokenUsed[10] = {0};
unsigned long validDuration = 30UL * 24UL * 60UL * 60UL * 1000UL; // 30 hari dalam milidetik
int relayUtama = 0;

float voltage = 0.0;
float current = 0.0;
float power = 0.0;
float energy = 0.0;
float suhu = 0.00;

float batteryPercentage = 0.0;
float previousVoltage = 0.0;
bool isCharging = false;

// MQTT Server dan topic
const char* mqtt_server = "ee.unsoed.ac.id";
const char* pubBatteryStatus = "FourDayTeam/output/BatteryStatus";
const char* pubBatteryConditions = "FourDayTeam/output/BatteryConditions";
const char* subToken = "FourDayTeam/input/token";
const char* mqtt_client_name = "ESP32_client";

unsigned long lastSendTime = 0; // Waktu terakhir mengirim data
const unsigned long sendInterval = 5000; // Interval untuk mengirim data (2 detik)

// Fungsi koneksi ke GPRS menggunakan SIM800L
void connectToGPRS() {
  SerialMon.println("Initializing modem...");
  modem.restart();
  String modemInfo = modem.getModemInfo();
  SerialMon.print("Modem: ");
  SerialMon.println(modemInfo);

  SerialMon.print("Connecting to APN: ");
  SerialMon.println(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println("Failed to connect to GPRS");
    while (true);
  }
  SerialMon.println("GPRS connected");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String topicIn = String(topic);
  if (topicIn.equals(subToken)) {
    String messageIn = "";
    for (unsigned int i = 0; i < length; i++) {
      messageIn += (char)payload[i];
    }
    messageIn.trim();
    tokenIn = messageIn;
  }

  Serial.print("Message received on topic: ");
  Serial.print(topic);
  Serial.print(" with message: ");
  Serial.println(tokenIn);

  bool tokenFound = false;
  unsigned long lastExpireTime = 0;
  int lastTokenIndex = -1;

  for (int i = 0; i < 10; i++) {
    if (ElvoraActive[i] == 1) {
      if (tokenExpireTime[i] > lastExpireTime) {
        lastExpireTime = tokenExpireTime[i];  // Simpan waktu kedaluwarsa terakhir
        lastTokenIndex = i;
      }
    }
  }

  // Jika tidak ada token yang aktif, mulai dari waktu sekarang
  if (lastExpireTime == 0) {
    lastExpireTime = millis();
  }

  for (int i = 0; i < 10; i++) {
    if (tokenIn == tokens[i]) {
      tokenFound = true;
      tokens[i] = "";
      tokenStartTime[i] = lastExpireTime;
      tokenExpireTime[i] = tokenStartTime[i] + validDuration;
      ElvoraActive[i] = 1;
      tokenUsed[i] = 1;
      break;
    }
  }
  // Menampilkan hasil pengecekan
  if (tokenFound) {
    Serial.println("Token valid! Token telah diaktifkan dengan penambahan 30 hari.");
  } else {
    Serial.println("Token tidak valid atau sudah digunakan.");
  }
}

void sendTokenStatus() {
  // Cari token yang aktif
  String activeToken = "";
  unsigned long currentTime = millis();
  for (int i = 0; i < 10; i++) {
    if (ElvoraActive[i] == 1 && currentTime > tokenStartTime[i] && currentTime < tokenExpireTime[i]) {
      activeToken = tokens[i];
      break;
    }
  }

  DynamicJsonDocument jsonDoc(256);
  jsonDoc["token_data"] = activeToken;
  jsonDoc["remainingTime"] = totalRemainingTimeFormated;
  jsonDoc["relayStatus"] = relayUtama;
  jsonDoc["latitude"] = latitude;
  jsonDoc["longitude"] = longitude;
  jsonDoc["Voltage"] = voltage;
  jsonDoc["Current"] = current;
  jsonDoc["PowerNow"] = power;
  jsonDoc["PowerUsed"] = energy;
  jsonDoc["Temperature"] = suhu;
  String BatteryConditions;
  serializeJson(jsonDoc, BatteryConditions);
  client.publish(pubBatteryConditions, BatteryConditions.c_str());
}

String formatTime(unsigned long timeMillis) {
  unsigned long totalSeconds = timeMillis / 1000;
  unsigned long days = totalSeconds / (24 * 3600);
  unsigned long hours = (totalSeconds % (24 * 3600)) / 3600;
  return String(days) + " hari " + String(hours) + " jam";
}

// Fungsi koneksi MQTT
void mqttConnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect(mqtt_client_name)) {
      Serial.println("connected");
      client.subscribe(subToken);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void requestData() {
  PZEM.write(0xB0); // Perintah baca tegangan
  if (PZEM.available() > 0) {
    voltage = PZEM.read() / 100.0;
  }
  PZEM.write(0xB1); // Perintah baca arus
  if (PZEM.available() > 0) {
    current = PZEM.read() / 100.0;
  }
  PZEM.write(0xB2); // Perintah baca daya
  if (PZEM.available() > 0) {
    power = PZEM.read();
  }
  PZEM.write(0xB3); // Perintah baca energi
  if (PZEM.available() > 0) {
    energy = PZEM.read();
  }
}

float calculateBatteryPercentage(float voltage) {
  if (voltage >= 84.0) {
    return 100.0;
  } else if (voltage <= 60.0) {
    return 0.0;
  } else {
    return (voltage - 60.0) / (84.0 - 60.0) * 100.0;
  }
}

bool checkChargingStatus() {
  if (current > 0 && voltage > previousVoltage) {
    return true;
  } else {
    return false;
  }
}

float readTemperature() {
  // Baca nilai analog dari sensor LM35
  int rawValue = analogRead(lm35Pin);
  float voltage = rawValue * (3.3 / 4095.0);
  float temperatureC = voltage * 100.0; // LM35 memberikan 10mV/°C
  return temperatureC;
}

void setup() {
  ss.begin(9600);
  PZEM.begin(9600, SERIAL_8N1, 16, 17); // Inisialisasi PZEM-017
  Serial.begin(115200);

  // Inisialisasi SIM800L
  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  
  // Koneksi ke GPRS
  connectToGPRS();
  
  // Inisialisasi MQTT
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  pinMode(lm35Pin, INPUT);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  previousVoltage = voltage;
  requestData();  // Baca data dari PZEM-017
  batteryPercentage = calculateBatteryPercentage(voltage);
  isCharging = checkChargingStatus();

  if (gps.location.isValid()) {
    latitude = String(gps.location.lat(), 6);
    longitude = String(gps.location.lng(), 6);
    coordinateBattery = latitude + "," + longitude;
  }

  unsigned long totalRemainingTime = 0;
  unsigned long currentTime = millis();
  for (int i = 0; i < 10; i++) {
    if (ElvoraActive[i] == 1 && currentTime > tokenStartTime[i] && currentTime < tokenExpireTime[i]) {
      totalRemainingTime += tokenExpireTime[i] - currentTime;
    }
  }

  totalRemainingTimeFormated = formatTime(totalRemainingTime);
  relayUtama = (totalRemainingTime > 0) ? 1 : 0;
  suhu = readTemperature();

  // Mengirim status token setiap 2 detik
  if (currentTime - lastSendTime >= sendInterval) {
    sendTokenStatus();
    Serial.print("Tegangan: "); Serial.println(voltage);
    Serial.print("Arus: "); Serial.println(current);
    Serial.print("Daya: "); Serial.println(power);
    Serial.print("Energi Total: "); Serial.println(energy);
    Serial.print("Persentase Baterai: "); Serial.println(batteryPercentage);
    Serial.print("Status: "); Serial.println(isCharging ? "Sedang diisi (Charging)" : "Tidak diisi (Not Charging)");
    lastSendTime = currentTime; // Perbarui waktu terakhir mengirim
  }
}
