#include <Arduino.h>
#include <Arduino_RouterBridge.h>
#include "trame.h"

// ===============================
// Configuration GPS
// ===============================

constexpr byte NMEA_MAX = 82;

char sentence[NMEA_MAX + 1];

Trame gps(nullptr);

// ===============================
// Configuration MQTT
// ===============================

BridgeTCPClient<> client(Bridge);

const char* broker = "broker.emqx.io";
const int port = 1883;

const char* clientId = "VQGPS01";

unsigned long lastPublish = 0;
const unsigned long publishInterval = 5000;

// ===============================
// Réception des phrases NMEA
// ===============================

bool getSentence() {

  static byte i = 0;

  while (Serial1.available()) {

    char c = Serial1.read();

    if (c == '\r') continue;

    if (c == '\n') {
      sentence[i] = '\0';
      i = 0;
      return true;
    }

    if (i < NMEA_MAX) {
      sentence[i++] = c;
    }
  }

  return false;
}

// ===============================
// Connexion MQTT
// ===============================

bool mqttConnect() {

  Serial.println("Connecting to MQTT broker...");

  if (client.connect(broker, port) < 0) {
    Serial.println("TCP connection failed!");
    return false;
  }

  uint8_t packet[] = {
    0x10, 0x13,
    0x00, 0x04, 'M', 'Q', 'T', 'T',
    0x04, 0x02,
    0x00, 0x3C,
    0x00, 0x07,
    'V', 'Q', 'G', 'P', 'S', '0', '1'
  };

  client.write(packet, sizeof(packet));

  unsigned long start = millis();

  while (client.available() < 4 &&
         millis() - start < 5000) {
    delay(10);
  }

  if (client.available() < 4) {
    Serial.println("No CONNACK received");
    client.stop();
    return false;
  }

  uint8_t response[4];

  for (int i = 0; i < 4; i++) {
    response[i] = client.read();
  }

  if (response[0] != 0x20 ||
      response[1] != 0x02 ||
      response[2] != 0x00 ||
      response[3] != 0x00) {

    Serial.println("MQTT connection rejected");
    client.stop();
    return false;
  }

  Serial.println("MQTT connected!");

  return true;
}

// ===============================
// Publication MQTT QoS 0
// ===============================

void mqttPublish(const char* topic, const char* message) {

  uint16_t topicLength = strlen(topic);
  uint16_t messageLength = strlen(message);

  uint16_t remainingLength =
      2 + topicLength + messageLength;

  if (remainingLength > 127) {
    Serial.println("MQTT message too long");
    return;
  }

  client.write((uint8_t)0x30);
  client.write((uint8_t)remainingLength);

  client.write((uint8_t)(topicLength >> 8));
  client.write((uint8_t)(topicLength & 0xFF));

  client.write((const uint8_t*)topic, topicLength);
  client.write((const uint8_t*)message, messageLength);
}

// ===============================
// Initialisation
// ===============================

void setup() {

  Serial.begin(9600);
  Serial1.begin(9600);

  if (!Bridge.begin()) {
    while (true) {}
  }

  Serial.println("GPS MQTT started");

  mqttConnect();
}

// ===============================
// Boucle principale
// ===============================

void loop() {

  if (getSentence()) {
    gps.setSentence(sentence);
  }

  if (gps.extrait() && gps.estValide()) {

    if (millis() - lastPublish >= publishInterval) {

      lastPublish = millis();

      if (!client.connected()) {
        if (!mqttConnect()) {
          return;
        }
      }

      char buffer[32];

      // Latitude
      snprintf(buffer, sizeof(buffer),
               "%.6f", gps.latitude);

      mqttPublish("ventuno/gps/latitude", buffer);

      // Longitude
      snprintf(buffer, sizeof(buffer),
               "%.6f", gps.longitude);

      mqttPublish("ventuno/gps/longitude", buffer);

      // Altitude
      snprintf(buffer, sizeof(buffer),
               "%.1f", gps.altitude);

      mqttPublish("ventuno/gps/altitude", buffer);

      // Satellites
      snprintf(buffer, sizeof(buffer),
               "%d", gps.numSat);

      mqttPublish("ventuno/gps/satellites", buffer);

      Serial.println("GPS data published!");
    }
  }
}