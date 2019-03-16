#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <MQTT.h>
#include <WiFiManager.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <ESP8266httpUpdate.h>
#include <EEPROM.h>

#define FRAMES_IN_BUFFER 60
#define BUFFER_SIZE 100 * 3
#define BROKER_HOST "mqtt.weasel.science"

char identifier[16];
char topic_buffer[64];
char topic_identify[64];
char topic_alias[64];
char topic_max_brightness[64];
char topic_ota[64];
char topic_wifi_reconfigure[64];

void setupTopics() {
  sprintf(topic_buffer, "/mirgalke/%s/buffer", identifier);
  sprintf(topic_alias, "/mirgalke/%s/alias", identifier);
  sprintf(topic_max_brightness, "/mirgalke/%s/max-brightness", identifier);
  sprintf(topic_ota, "/mirgalke/%s/ota", identifier);
  sprintf(topic_wifi_reconfigure, "/mirgalke/%s/wifi-reconfigure", identifier);
}

WiFiClient net;
MQTTClient client(BUFFER_SIZE + 100);

void unsubscribeAll() {
  client.unsubscribe(topic_buffer);
  client.unsubscribe(topic_alias);
  client.unsubscribe(topic_max_brightness);
  client.unsubscribe(topic_ota);
  client.unsubscribe(topic_wifi_reconfigure);
}

void subscribeAll() {
  client.subscribe(topic_buffer);
  client.subscribe(topic_alias);
  client.subscribe(topic_max_brightness);
  client.subscribe(topic_ota);
  client.subscribe(topic_wifi_reconfigure);
}

uint8_t led_buffer[FRAMES_IN_BUFFER][BUFFER_SIZE];
uint8_t led_count = 0;
CRGB leds[100];

char mqtt_host[128];

void connect() {  
  Serial.print("checking wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }

  uint8_t connection_attempts = 0;

  Serial.print("\nconnecting...");
  while (!client.connect(identifier)) {
    // Wait for 30 minutes before clearing all config
    if (++connection_attempts >= 60 * 30) {
      WiFi.disconnect(true);
      delay(1000);
      ESP.reset();
    }
    
    Serial.print(".");
    delay(1000);
  }

  Serial.println("\nconnected!");

  subscribeAll();
}

uint8_t buffer_write_key = 0;
uint8_t framesInBuffer = 0;

void messageReceived(MQTTClient *client, char topic[], char payload[], int payload_length) {
  if (strcmp(topic, topic_buffer) == 0) {
    if (framesInBuffer < 60) {
      led_count = payload_length / 3;
      
      for (int i = 0; i < payload_length; i++) {
        led_buffer[buffer_write_key][i] = payload[i];
      }
  
      framesInBuffer++;
  
      if (++buffer_write_key >= FRAMES_IN_BUFFER) {
        buffer_write_key = 0;
      }

      maybeRenderFrame();
    }
  } else if (strcmp(topic, topic_alias) == 0) {
    unsubscribeAll();
    sprintf(identifier, "%s", payload);
    setupTopics();
    subscribeAll();
  } else if (strcmp(topic, topic_wifi_reconfigure) == 0) {
    WiFi.disconnect(true);
    ESP.reset();
  } else if (strcmp(topic, topic_ota) == 0) {
    DynamicJsonDocument doc(300);
    deserializeJson(doc, payload);
    const char* ota_host = doc["host"];
    int ota_port = doc["port"];
    const char* ota_path = doc["path"];
    WiFiClient updateClient;
    ESPhttpUpdate.update(updateClient, ota_host, ota_port, ota_path);
  } else if (strcmp(topic, topic_max_brightness) == 0) {
    FastLED.setBrightness(atoi(payload));
  }
}

bool wifiManagerConfigChanged = false;
void saveConfigCallback() {
  wifiManagerConfigChanged = true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("");

  EEPROM.begin(512);
  EEPROM.get(0, mqtt_host);
  EEPROM.end();

  Serial.print("Retrieved mqtt host from eeprom: ");
  Serial.println(mqtt_host);
  
  FastLED.addLeds<WS2812B, 3, GRB>(leds, 100).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(100);
  FastLED.show();  

  itoa(ESP.getChipId(), identifier, 16);
  
  // This topic will only be outgoing, and should always include the original identifier
  // This allows us to see which devices are assigned which aliases
  sprintf(topic_identify, "/mirgalke/%s/identify", identifier);
  
  setupTopics();

  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", "mqtt.weasel.science", 128);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.autoConnect();

  if (wifiManagerConfigChanged) {
    strcpy(mqtt_host, custom_mqtt_server.getValue());

    EEPROM.begin(512);
    EEPROM.put(0, mqtt_host); 
    EEPROM.commit();
    EEPROM.end();       

    Serial.print("Stored mqtt host to eeprom: ");
    Serial.println(mqtt_host);
  }

  client.begin(mqtt_host, 1883, net);
  client.onMessageAdvanced(messageReceived);

  //connect();
  // This delay may prevent double mqtt connections
  //delay(1000);
}

uint8_t buffer_read_key = 0;

uint32_t next_frame_time = 0;

void renderFrame() {
  if (framesInBuffer > 0) {
    for (uint8_t i = 0; i < led_count; i++) {
      //strip->SetPixelColor(i, RgbColor(led_buffer[buffer_read_key][i * 3], led_buffer[buffer_read_key][i * 3 + 1], led_buffer[buffer_read_key][i * 3 + 2]));
      leds[i].r = led_buffer[buffer_read_key][i * 3];
      leds[i].g = led_buffer[buffer_read_key][i * 3 + 1];
      leds[i].b = led_buffer[buffer_read_key][i * 3 + 2];
    }
  
    //strip->Show();
    FastLED.show();

    framesInBuffer--;
  
    if (++buffer_read_key >= FRAMES_IN_BUFFER) {
      buffer_read_key = 0;
    }
  }
}

uint8_t compensation[30] = {
  0, 0, 0, 0, 0,
  1, 1, 1, 2, 2,
  2, 2, 3, 3, 3,
  4, 4, 4, 5, 5,
  5, 5, 5, 6, 7,
  8, 9, 10, 11, 12
};

void maybeRenderFrame() {
  if (millis() >= next_frame_time) {
    renderFrame();

    int8_t buffer_offset = framesInBuffer - 30;

    uint16_t time_offset = 0;

    if (buffer_offset > 0) {
      time_offset = 16 - compensation[abs(buffer_offset) - 1];      
    }

    if (buffer_offset == 0) {
      time_offset = 16;
    }

    if (buffer_offset < 0) {
      time_offset = 16 + compensation[abs(buffer_offset) - 1];
    }
     
    next_frame_time = millis() + time_offset;
  }
}

uint32_t lastIdentified = 0;
void maybeIdentify() {
  if (millis() - lastIdentified > 15000) {
    client.publish(topic_identify, identifier);
    lastIdentified = millis();    
  }
}

void loop() {
  client.loop();
  // delay(10);  // <- fixes some issues with WiFi stability

  if (!client.connected()) {
    Serial.println("Trying to connect in loop");
    connect();
  }

  maybeRenderFrame();

  maybeIdentify();
}
