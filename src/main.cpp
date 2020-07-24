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
#include <CertStoreBearSSL.h>
#include<ESP8266httpUpdate.h>
#include <ESP8266HTTPClient.h>
#include <time.h>

DHTesp dht;

#ifndef GIT_VERSION
#define GIT_VERSION "v0.0.1-nogit"
#endif

// MQTT callback function header
void callback(char* topic, byte* payload, unsigned int length);

WiFiClientSecure bear;
WiFiClient wclient;
PubSubClient client;

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
//  char sensor_name[33];
//  char zone[33];
  bool mqtt_tls;
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
//  char sensor_name[33];
//  char zone[33];
  char command_topic[255];
  uint32_t chipid;
};

Config config;

BearSSL::CertStore certStore;

// Set time via NTP, as required for x.509 validation
void setClock() {
  configTime(3 * 3600, 0, config.ntp_server);

  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("");
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
}


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
  char hashBuf[64];
  configHash.finalize(hashBuf, 64);
  string2hexString(hashBuf, configHashResult);
  StaticJsonDocument<512> config_dict;
  auto error = deserializeJson(config_dict, buf.get());
  if (error) {
    Serial.println("Failed to parse config file");
    return false;
  }
  
  config.water_enabled = config_dict["water_enabled"];
  config.mqtt_port = config_dict["mqtt_port"];
  if (config_dict["mqtt_tls"].isNull()){
    config.mqtt_tls = false;
  } else {
    config.mqtt_tls = config_dict["mqtt_tls"];
  }
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
  //strlcpy(config.sensor_name, config_dict["sensor_name"], sizeof(config.sensor_name));
  //strlcpy(config.zone, config_dict["zone"], sizeof(config.zone));
  Serial.println("Loaded config!");
  return true;
}

bool startsWith(const char *pre, const char *str)
{
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? false : memcmp(pre, str, lenpre) == 0;
}

void write_config() {
  File updateSourceFile = LittleFS.open("/config-update", "r");
  char updateSource[255] = {0};
  updateSourceFile.readBytes(updateSource, 255);
  updateSourceFile.close();
  LittleFS.remove("/config-update");
  Serial.println("Beginning OTA Config Update from URI");
  Serial.print("OTA URI: ");
  Serial.println(updateSource);
  HTTPClient http;
  if (startsWith("https", updateSource)){
    Serial.println("Using BearSSL");
    http.begin(bear, updateSource);
  } else {
    Serial.println("Using WifiClient");
    http.begin(wclient, updateSource);
  }
  delay(500);
  int respCode = http.GET();
  if (respCode < 0){
    Serial.println("HTTP client error");
    Serial.printf("[HTTPS] GET... failed, error: %s\n", http.errorToString(respCode).c_str());
    return;
  } else {
    File configFile = LittleFS.open("/config.json", "w+");
    if (!configFile) {
      Serial.println("Failed to open config file");
      return;
    }
    http.writeToStream(&configFile);
    delay(500);
    configFile.seek(0);
    while (configFile.available()) {
      Serial.write(configFile.read());
    }
    delay(500);
    configFile.close();
    ESP.restart();
  }
}

// Callback function header
void callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, config.command_topic) != 0) {
    return;
  }

  StaticJsonDocument<512> input_json;
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
    ESP.restart();
    return;
  }
  if (strcmp(command, "get_config") == 0) {
    Serial.println("Updatng configuration");
    if (input_json["config_uri"].isNull()){
      Serial.println("No new confiugration found");
      return;
    }
    File configUpdateFile = LittleFS.open("/config-update", "w");
    char configUri[255] = {0};
    strlcpy(configUri, input_json["config_uri"], 255);
    configUpdateFile.write(configUri, 255);
    delay(500);
    configUpdateFile.close();
    delay(500);
    LittleFS.end();
    delay(1000);
    ESP.restart();
  }
  if (strcmp(command, "get_firmware") == 0) {
    Serial.println("OTA Update Requested");
    File updateFile = LittleFS.open("update", "w");
    char updateUri[255] = {0};
    strlcpy(updateUri, input_json["update_uri"], 255);
    updateFile.write(updateUri, 255);
    delay(500);
    updateFile.close();
    delay(500);
    LittleFS.end();
    delay(1000);
    ESP.restart();
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
    File updateFile = LittleFS.open("update", "r");
    char updateUri[255] = {0};
    updateFile.readBytes(updateUri, 255);
    updateFile.close();
    Serial.print("OTA URI: ");
    Serial.println(updateUri);
    LittleFS.remove("update");
    t_httpUpdate_return ret = HTTP_UPDATE_FAILED;
    if (startsWith("https", updateUri)) {
      Serial.println("Using TLS client for update");
      yield();
      ret = ESPhttpUpdate.update(bear, updateUri);
    } else {
      Serial.println("Using HTTP client for update");
      yield();
      ret = ESPhttpUpdate.update(wclient, updateUri);
    }
    LittleFS.end();
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
    ESP.restart();
}

bool publish_status() {
  if (millis() > lastSentMillis + 5000){
    Status status;
    strlcpy(status.config_hash, configHashResult, 65);
    status.water_enabled = config.water_enabled;
    status.chipid = ESP.getChipId();
    strlcpy(status.command_topic, config.command_topic, sizeof(status.command_topic));
//    status.sensor_name = config.sensor_name;
//    status.zone = config.zone;
    strlcpy(status.fw_version, GIT_VERSION, sizeof(status.fw_version));
    if (!unconfigured) {
      strlcpy(status.message, "alive", 65);
    } else {
      strlcpy(status.message, "unconfigured", 65);
    }
    StaticJsonDocument<255> statusJson;
    statusJson["config_hash"] = status.config_hash;
    statusJson["water_enabled"] = status.water_enabled;
    statusJson["message"] = status.message;
    statusJson["chip_id"] = status.chipid;
    statusJson["fw_version"] = status.fw_version;
    statusJson["command_topic"] = status.command_topic;
//    statusJson["sensor_name"] = status.sensor_name;
//    statusJson["zone"] = status.zone;
    char *statusJsonBuf = (char*) malloc(measureJson(statusJson) * sizeof(char) + 1);
    serializeJson(statusJson, statusJsonBuf, measureJson(statusJson) + 1);
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
  Serial.println("FW Signing Key: ");
  Serial.println(signing_pubkey);
  Serial.println("Mounting FS...");

  if (!LittleFS.begin()) {
    Serial.println("Failed to mount file system");
    ESP.restart();
  }
  if (!loadConfig()) {
    Serial.println("Failed to load JSON config!");
    ESP.restart();
  }

  //
  // Setup WiFi
  //
  WiFi.softAPdisconnect (true);
  WiFi.mode(WIFI_STA);
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
  // Setup BearSSL
  //
  setClock();
  int numCerts = certStore.initCertStore(LittleFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
  Serial.printf("Number of CA certs read: %d\n", numCerts);
  if (numCerts == 0) {
    Serial.printf("No certs found.\n");
  }
  bear.setCertStore(&certStore);

  //
  // Execute pending update
  //
  if (LittleFS.exists("update")) {
    Serial.println("Updating firmware...");
    do_ota_update(); // do_ota_update will reset the ESP
  }
  if (LittleFS.exists("config-update")) {
    Serial.println("Updating config...");
    write_config();
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
  if (config.mqtt_tls) {
  client.setClient(bear);
  } else {
  client.setClient(wclient);
  }
  client.setBufferSize(512);
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
