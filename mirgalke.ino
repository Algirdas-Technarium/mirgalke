#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <MQTT.h>
#include <WiFiManager.h>

#include <NeoPixelBus.h>
#include <FastLED.h>

#include <ArduinoJson.h>
#include <ESP8266httpUpdate.h>

#define FRAMES_IN_BUFFER 60
#define BUFFER_SIZE 100 * 3
#define BROKER_HOST "mqtt.weasel.science"

char identifier[16];
char topic_buffer[64];
char topic_led_count[64];
char topic_identify[64];
char topic_alias[64];
char topic_ota[64];
char topic_wifi_reconfigure[64];

void setupTopics() {
  sprintf(topic_buffer, "/mirgalke/%s/buffer", identifier);
  sprintf(topic_led_count, "/mirgalke/%s/led-count", identifier);
  sprintf(topic_alias, "/mirgalke/%s/alias", identifier);
  sprintf(topic_ota, "/mirgalke/%s/ota", identifier);
  sprintf(topic_wifi_reconfigure, "/mirgalke/%s/wifi-reconfigure", identifier);
}

WiFiClient net;
MQTTClient client(BUFFER_SIZE + 100);

void unsubscribeAll() {
  client.unsubscribe(topic_buffer);
  client.unsubscribe(topic_led_count);
  client.unsubscribe(topic_alias);
  client.unsubscribe(topic_ota);
  client.unsubscribe(topic_wifi_reconfigure);
}

void subscribeAll() {
  client.subscribe(topic_buffer);
  client.subscribe(topic_led_count);
  client.subscribe(topic_alias);
  client.subscribe(topic_ota);
  client.subscribe(topic_wifi_reconfigure);
}

uint8_t led_buffer[FRAMES_IN_BUFFER][BUFFER_SIZE];

NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> *strip;
uint8_t led_count = 0;
bool led_structures_prepared = false;

bool setupLedStructures() {
  Serial.println("Setting up LED structures");
  
  if (led_count == 0) {
    return false;
  }

  if (led_structures_prepared) {
    delete strip;
  }

  strip = new NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod>(led_count, 26);

  strip->Begin();
  strip->Show();

  led_structures_prepared = true;
}

void connect() {
  Serial.print("checking wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }

  Serial.print("\nconnecting...");
  while (!client.connect(identifier)) {
    Serial.print(".");
    delay(1000);
  }

  Serial.println("\nconnected!");

  subscribeAll();
}

uint8_t buffer_write_key = 0;
uint8_t framesInBuffer = 0;

void messageReceived(MQTTClient *client, char topic[], char payload[], int payload_length) {
  if (strcmp(topic, topic_buffer) == 0 && led_structures_prepared) {
    if (framesInBuffer < 60) {
      //Serial.println("Topic is buffer");
  
      //deserializeJson(json_doc[buffer_write_key], payload);

      for (int i = 0; i < led_count * 3; i++) {
        led_buffer[buffer_write_key][i] = payload[i];
      }
  
      framesInBuffer++;
  
      if (++buffer_write_key >= FRAMES_IN_BUFFER) {
        buffer_write_key = 0;
      }

      maybeRenderFrame();
    } else {
      //Serial.println("Buffer full");
    }
  } else if (strcmp(topic, topic_led_count) == 0) {
    //Serial.println("Topic is led_count");
    led_count = atoi(payload);
    setupLedStructures();
  } else if (strcmp(topic, topic_alias) == 0) {
    Serial.printf("Alias assigned");
    unsubscribeAll();
    sprintf(identifier, "%s", payload);
    setupTopics();
    subscribeAll();
  } else if (strcmp(topic, topic_wifi_reconfigure) == 0) {
    Serial.printf("wifi reconf"); 
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
  }
}

void setup() {
  Serial.begin(115200);
  //WiFi.begin(ssid, pass);

  itoa(ESP.getChipId(), identifier, 16);
  
  // This topic will only be outgoing, and should always include the original identifier
  // This allows us to see which devices are assigned which aliases
  sprintf(topic_identify, "/mirgalke/%s/identify", identifier);
  
  setupTopics();

  WiFiManager wifiManager;
  wifiManager.autoConnect();

  // Note: Local domain names (e.g. "Computer.local" on OSX) are not supported by Arduino.
  // You need to set the IP address directly.
  //
  // MQTT brokers usually use port 8883 for secure connections.
  client.begin(BROKER_HOST, 1883, net);
  client.onMessageAdvanced(messageReceived);

  connect();
}

uint8_t buffer_read_key = 0;

uint32_t next_frame_time = 0;

void renderFrame() {
  if (led_structures_prepared) {
    if (framesInBuffer > 0) {
      //Serial.print("Will read from buffer index ");
      //Serial.println(buffer_read_key);
      
      for (uint8_t i = 0; i < led_count; i++) {
        //Serial.print("Setting pixel color for ");
        //Serial.println(i);
        //Serial.println(led_count);
        
        strip->SetPixelColor(i, RgbColor(led_buffer[buffer_read_key][i * 3], led_buffer[buffer_read_key][i * 3 + 1], led_buffer[buffer_read_key][i * 3 + 2]));
        //strip->SetPixelColor(i, red);
      }
    
      strip->Show();
  
      framesInBuffer--;
    
      if (++buffer_read_key >= FRAMES_IN_BUFFER) {
        buffer_read_key = 0;
      }
    } else {
      //Serial.println("Buffer empty");
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

    //Serial.println(next_frame_time - millis());
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
    connect();
  }

  maybeRenderFrame();

  maybeIdentify();
}
