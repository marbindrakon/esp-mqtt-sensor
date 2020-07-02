#include <Arduino.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <stdlib.h>
#include <SHA256.h>
#include <version.h>
#include <FW_Signing.h>
#include<ESP8266httpUpdate.h>

DHTesp dht;

#ifndef GIT_VERSION
#define GIT_VERSION "v0.0.1-nogit"
#endif
// Callback function header
void callback(char* topic, byte* payload, unsigned int length);

WiFiClient wclient;
PubSubClient client(wclient);

const int dhtpin = 12;
const int waterpin = 2;
long lastSentMillis = 0;
bool unconfigured = 1;

SHA256 configHash;

char configHashResult[65];

struct Config {
  bool water_enabled;
  uint16_t mqtt_port;
  char hostname[255];
  char wifi_ssid[33];
  char wifi_password[33];
  char mqtt_broker[255];
  char ntp_server[64];
  char data_topic[255];
  char status_topic[255];
  char command_topic[255];
};

struct Data {
  bool water_state;
  float temperature;
  float humidity;
};

struct Status {
  char config_hash[65];
  char fw_version[32];
  bool water_enabled;
  char message[33];
  uint32_t chipid;
};

Config config;

void string2hexString(char* input, char* output)
{
    int loop;
    int i; 
    
    i=0;
    loop=0;
    
    while(input[loop] != '\0')
    {
        sprintf((char*)(output+i),"%02X", input[loop]);
        loop+=1;
        i+=2;
    }
    //insert NULL at the end of the output string
    output[i++] = '\0';
}

bool loadConfig() {
  Serial.println("Loading config from /config.json");
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);
  configFile.close();
  configHash.update(buf.get(), size);
  char hashBuf[65];
  configHash.finalize(hashBuf, 65);
  string2hexString(hashBuf, configHashResult);
  StaticJsonDocument<200> config_dict;
  auto error = deserializeJson(config_dict, buf.get());
  if (error) {
    Serial.println("Failed to parse config file");
    return false;
  }
  
  config.water_enabled = config_dict["water_enabled"];
  config.mqtt_port = config_dict["mqtt_port"];
  char *tempHostnameBuf = (char *) malloc(sizeof(config.hostname));
  strlcpy(tempHostnameBuf, config_dict["hostname"], sizeof(config.hostname));
  Serial.println("Moved hostname to temporary buffer");
  if (strcmp(tempHostnameBuf, "UNCONFIGURED") == 0) {
    Serial.println("Hostname UNCONFIGURED, generating temporary hostname");
    String tempHostname = "ESP8266Client-";
    tempHostname += String(random(0xffff), HEX);
    tempHostname.toCharArray(tempHostnameBuf, sizeof(config.hostname));
    Serial.print("Hostname: ");
    Serial.println(tempHostnameBuf);
    strlcpy(config.hostname, tempHostnameBuf, sizeof(config.hostname));
    free(tempHostnameBuf);
  } else {
    strlcpy(config.hostname, config_dict["hostname"], sizeof(config.hostname));
    unconfigured = false;
  }
  strlcpy(config.wifi_ssid, config_dict["wifi_ssid"], sizeof(config.wifi_ssid));
  strlcpy(config.wifi_password, config_dict["wifi_password"], sizeof(config.wifi_password));
  strlcpy(config.mqtt_broker, config_dict["mqtt_broker"], sizeof(config.mqtt_broker));
  strlcpy(config.ntp_server, config_dict["ntp_server"], sizeof(config.mqtt_broker));
  strlcpy(config.data_topic, config_dict["data_topic"], sizeof(config.data_topic));
  strlcpy(config.command_topic, config_dict["command_topic"], sizeof(config.command_topic));
  strlcpy(config.status_topic, config_dict["status_topic"], sizeof(config.status_topic));
  Serial.println("Loaded config!");
  return true;
}

void write_config(Config new_config) {
  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return;
  }
  StaticJsonDocument<200> config_json;
  config_json["wifi_ssid"] = new_config.wifi_ssid;
  config_json["wifi_password"] = new_config.wifi_password;
  config_json["mqtt_broker"] = new_config.mqtt_broker;
  config_json["ntp_server"] = new_config.ntp_server;
  config_json["data_topic"] = new_config.data_topic;
  config_json["command_topic"] = new_config.command_topic;
  config_json["status_topic"] = new_config.status_topic;

  serializeJson(config_json, configFile);
  configFile.close();
  LittleFS.end();
  client.disconnect();
  ESP.reset();
}

// Callback function header
void callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, config.command_topic) != 0) {
    return;
  }

  StaticJsonDocument<64> input_json;
  DeserializationError err = deserializeJson(input_json, payload);
  if (err) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(err.c_str());
    return;
  }
  const char* command = input_json["command"];
  uint32_t chip_id = (uint32_t)input_json["chip_id"];
  if (chip_id != ESP.getChipId()) {
    Serial.println("Ignoring command for wrong chip ID");
    return;
  }
  if (strcmp(command, "reset") == 0){
    Serial.println("Resetting from command");
    client.disconnect();
    LittleFS.end();
    ESP.reset();
    return;
  }
  if (strcmp(command, "get_config") == 0) {
    Serial.println("Updatng configuration");
    Config newConfig;
    if (input_json["config"].isNull()){
      Serial.println("No new confiugration found");
      return;
    }
    strlcpy(newConfig.wifi_ssid, input_json["config"]["wifi_ssid"], sizeof(newConfig.wifi_ssid));
    strlcpy(newConfig.wifi_password, input_json["config"]["wifi_password"], sizeof(newConfig.wifi_password));
    strlcpy(newConfig.mqtt_broker, input_json["config"]["mqtt_broker"], sizeof(newConfig.mqtt_broker));
    strlcpy(newConfig.ntp_server, input_json["config"]["ntp_server"], sizeof(newConfig.mqtt_broker));
    strlcpy(newConfig.data_topic, input_json["config"]["data_topic"], sizeof(newConfig.data_topic));
    strlcpy(newConfig.command_topic, input_json["config"]["command_topic"], sizeof(newConfig.command_topic));
    strlcpy(newConfig.status_topic, input_json["config"]["status_topic"], sizeof(newConfig.status_topic));
    write_config(newConfig);
  }
  if (strcmp(command, "get_firmware") == 0) {
    Serial.println("OTA Update Requested");
    File updateFile = LittleFS.open("update", "w");
    updateFile.write((const char*)input_json["update_uri"]);
    updateFile.close();
    delay(500);
    LittleFS.end();
    delay(500);
    ESP.reset();
  }
  Serial.print("Invalid command: ");
  Serial.println(command);      
  return;
}

void ota_on_progress(int cur, int total){
  Serial.printf("Update process at %d of %d bytes...\n", cur, total);
}

void ota_on_start(){
  Serial.println("Starting update...");
}

void ota_on_finish(){
  Serial.println("Update complete!");
}

void ota_on_error(int error){
  Serial.printf("Error[%u]: ", error);
}

void do_ota_update(){
    ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
    ESPhttpUpdate.onStart(ota_on_start);
    ESPhttpUpdate.onEnd(ota_on_finish);
    ESPhttpUpdate.onProgress(ota_on_progress);
    ESPhttpUpdate.onError(ota_on_error);
    Serial.println("Beginning OTA FW Update from update file");
    Serial.print("OTA URI: ");
    char updateUri[255];
    File updateFile = LittleFS.open("update", "r");
    updateFile.readBytes(updateUri, updateFile.size());
    updateFile.close();
    Serial.println(updateUri);
    LittleFS.remove("update");
    LittleFS.end();
    t_httpUpdate_return ret = ESPhttpUpdate.update(wclient, updateUri);
    switch(ret)
    {
      case HTTP_UPDATE_FAILED:
        Serial.printf("[OTA]: HTTP_UPDATE_FAILD Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
        Serial.println();
        break;

      case HTTP_UPDATE_NO_UPDATES:
        Serial.println("[OTA]: HTTP_UPDATE_NO_UPDATES");
        break;

      case HTTP_UPDATE_OK:
        Serial.println("[OTA]: HTTP_UPDATE_OK");
        break;
    }
    delay(10000);
    ESP.reset();
}

bool publish_status() {
  if (millis() > lastSentMillis + 5000){
    Status status;
    strlcpy(status.config_hash, configHashResult, 65);
    status.water_enabled = config.water_enabled;
    status.chipid = ESP.getChipId();
    strlcpy(status.fw_version, GIT_VERSION, sizeof(status.fw_version));
    if (!unconfigured) {
      strlcpy(status.message, "alive", 65);
    } else {
      strlcpy(status.message, "unconfigured", 65);
    }
    StaticJsonDocument<200> statusJson;
    statusJson["config_hash"] = status.config_hash;
    statusJson["water_enabled"] = status.water_enabled;
    statusJson["message"] = status.message;
    statusJson["chip_id"] = status.chipid;
    statusJson["fw_version"] = status.fw_version;
    char *statusJsonBuf = (char*) malloc(measureJson(statusJson) * sizeof(char) + 1);
    serializeJson(statusJson, statusJsonBuf, measureJson(statusJson) + 1);
    //Serial.println(config.status_topic);
    if (!client.beginPublish(config.status_topic, measureJson(statusJson), true)){
      Serial.println("Failed to start status publish");
    }
    client.print(statusJsonBuf);
    if (!client.endPublish()){
      Serial.println("Failed to end status publish");
    }
    //Serial.println(statusJsonBuf);
    free(statusJsonBuf);
    return 1;
  } else {
    return 0;
  }
}

bool publish_data() {
  if (unconfigured) {
    return 0;
  }
  if (millis() > lastSentMillis + 5000){
    Data data;
    if (config.water_enabled) {
      data.water_state = !digitalRead(waterpin);
    } else {
      data.water_state = false;
    }
    data.temperature = dht.getTemperature();
    data.humidity = dht.getHumidity();
  
    StaticJsonDocument<85> dataJson;
    dataJson["water_state"] = data.water_state;
    dataJson["temperature"] = data.temperature;
    dataJson["humidity"] = data.humidity;
    char *dataJsonBuf = (char*) malloc(measureJson(dataJson) * sizeof(char) + 1);
    serializeJson(dataJson, dataJsonBuf, measureJson(dataJson) + 1);
    if (!client.beginPublish(config.data_topic, measureJson(dataJson), false)) {
      Serial.println("Failed to start data publish");
    }
    client.print(dataJsonBuf);
    if (!client.endPublish()){
      Serial.println("Failed to end data publish");
    }
    //Serial.println(dataJsonBuf);
    free(dataJsonBuf);
    return 1;
  } else {
    return 0;
  }
}

void setup(void) {
  Serial.begin(115200);
  Serial.print("Booting FW ");
  Serial.println(GIT_VERSION);
  Serial.print("FW Signing Key: ");
  Serial.println(signing_pubkey);
  Serial.println("Mounting FS...");

  if (!LittleFS.begin()) {
    Serial.println("Failed to mount file system");
    ESP.reset();
  }
  if (!loadConfig()) {
    Serial.println("Failed to load JSON config!");
    ESP.reset();
  }
  //
  // Setup WiFi
  //
  WiFi.softAPdisconnect (true);
  WiFi.mode(WIFI_STA);
  Serial.println(config.hostname);
  WiFi.hostname(config.hostname);
  WiFi.begin(config.wifi_ssid, config.wifi_password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(config.wifi_ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Hostname: ");
  Serial.println(config.hostname);

  //
  // Execute pending FW update
  //

  if (LittleFS.exists("update")) {
    Serial.println("Updating firmware...");
    do_ota_update(); // do_ota_update will reset the ESP
  }

  //
  // Setup sensor(s)
  //
  if (config.water_enabled == 1) {
    pinMode(waterpin, INPUT);
  }
  dht.setup(dhtpin, DHTesp::DHT22);
  //
  // Setup MQTT
  //
  client.setServer(config.mqtt_broker, config.mqtt_port);
  client.setCallback(callback);
}

void loop(void) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      Status willStatus;
      strlcpy(willStatus.config_hash, configHashResult, 65);
      willStatus.water_enabled = config.water_enabled;
      strlcpy(willStatus.message, "dead", 65);
      StaticJsonDocument<200> willStatusJson;
      willStatusJson["config_hash"] = willStatus.config_hash;
      willStatusJson["water_enabled"] = willStatus.water_enabled;
      willStatusJson["message"] = willStatus.message;
      char willStatusJsonBuf[200];
	    serializeJson(willStatusJson, willStatusJsonBuf);
      Serial.println("Connecting to MQTT server");
      //if (client.connect(config.hostname, config.status_topic, 1, true, willStatusJsonBuf))
      if (client.connect(config.hostname))
      {
        Serial.println(strcat("Connected to MQTT server at ", config.mqtt_broker));
        client.subscribe(config.command_topic);

      } else {
        Serial.println("Could not connect to MQTT server");   
      }
    }

  }
  int published = 0;
  if (publish_data()){
    published++;
  }
  if (publish_status()){
    published++;
  }
  if (published != 0){
    lastSentMillis = millis();
  }
  client.loop();  

}
