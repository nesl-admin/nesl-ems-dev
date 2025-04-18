/*
   -------------------------------------------------------------------
   EmonESP Serial to Emoncms gateway
   -------------------------------------------------------------------
   Adaptation of Chris Howells OpenEVSE ESP Wifi
   by Trystan Lea, Glyn Hudson, OpenEnergyMonitor

   Modified to use with the CircuitSetup.us Split Phase Energy Meter by jdeglavina
   Modified to use with EMS Workshop by dmendonca

   All adaptation GNU General Public License as below.

   -------------------------------------------------------------------

   This file is part of OpenEnergyMonitor.org project.
   EmonESP is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.
   EmonESP is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with EmonESP; see the file COPYING.  If not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

#include "mqtt_client.h"
#include <TimeLib.h>
#include <WiFiMulti.h>
#include <data_model.h>
#include <config.h>

WiFiClient transportClient;                 // the network client for MQTT (also works with EthernetLarge)
PubSubClient mqttclient(transportClient);   // the MQTT client

unsigned long mqtt_interval_ts = 0;
static char mqtt_data[128] = "";
static int mqtt_connection_error_count = 0;
String topic_device;          // device topic (publish/subscribe under here)
String topic_cmd;             // command topic (for 'southbound' commands)

void generateTopics() {
  //the top-level device topic string, eg: NESLE8D3ECAE3D98
  topic_device = MQTT_TOPIC;
  topic_device.concat("/");
  topic_device.concat(getDeviceID());
  topic_device.concat("/");

  //the command topic we subscribe to, eg: NESLE8D3ECAE3D98/cmd
  topic_cmd = topic_device;
  topic_cmd.concat("cmd");
}

// -------------------------------------------------------------------
// MQTT Connect
// Called only when MQTT server field is populated
// -------------------------------------------------------------------
boolean mqtt_connect()
{
  Serial.printf("MQTT Connecting...timeout in:%d\r\n", transportClient.getTimeout());

  if (transportClient.connect(MQTT_SERVER, 1883) != 1) //8883 for TLS
  {
     Serial.println("MQTT connect timeout.");
     return (0);
  }

  //transportClient.setTimeout(60);//(MQTT_TIMEOUT);
  mqttclient.setSocketTimeout(6);//MQTT_TIMEOUT);
  mqttclient.setBufferSize(MAX_DATA_LEN + 200);
  mqttclient.setKeepAlive(180);

  if (strcmp(MQTT_USER, "") == 0) {
    //allows for anonymous connection
    mqttclient.connect(getDeviceID()); // Attempt to connect
  } else {
    mqttclient.connect(getDeviceID(), MQTT_USER, MQTT_PW); // Attempt to connect
  }

  if (mqttclient.state() == 0) {
    Serial.printf("MQTT connected: %s\r\n", MQTT_SERVER);
    
    //subscribe to command topic
    if (!mqttclient.subscribe(topic_cmd.c_str())) {
      delay(250);
      if (!mqttclient.subscribe(topic_cmd.c_str())) {
        delay(500);
        if (!mqttclient.subscribe(topic_cmd.c_str())) {
          Serial.printf("MQTT: FAILED TO SUBSCRIBE TO COMMAND TOPIC: %s\r\n", topic_cmd.c_str());
          return false;
        }
      }
    }
    Serial.printf("MQTT: SUBSCRIBED TO COMMAND TOPIC: %s\r\n", topic_cmd.c_str());
    //  mqttclient.publish(getDeviceTopic().c_str(), "connected"); // Once connected, publish an announcement..
  } else {
    Serial.println("MQTT failed: ");
    Serial.println(mqttclient.state());
    return (0);
  }
  return (1);
}

//construct a comma-sep colon-delim string for the publish func
void mqtt_publish_meter() {
  char buf[32];
  sprintf(buf,"temp:%2.1f,humid:%2.2f", (float)inputRegisters[0], (float)inputRegisters[1]);
  mqtt_publish_comma_sep_colon_delim("meter", buf);
}

//pull apart a comma-sep colon-delim name:value string and publish the name:value pairs under 'subtopic'
void mqtt_publish_comma_sep_colon_delim(const char* subtopic, const char * data) {
    String topicBuf;
    char buf[256];
    Serial.printf("MQTT publish: size:%d chars", strlen(data));
    do {
      int pos = strcspn(data, ":");
      strncpy(buf, data, pos);
      buf[pos] = 0;
      String st(subtopic);
      topicBuf = topic_device;
      topicBuf.concat(st+"/");
      topicBuf.concat(buf);
      //topic_ptr[pos] = 0;
      data += pos;
      if (*data++ == 0) {
        break;
      }

      pos = strcspn(data, ",");
      strncpy(mqtt_data, data, pos);
      mqtt_data[pos] = 0;
      data += pos;

      if (!mqttclient.publish(topicBuf.c_str(), mqtt_data)) {
       Serial.println("MQTT publish: failed");
      }
#ifdef ENABLE_DEBUG_MQTT
      Serial.printf("topic: %s, data: %s\n", topicBuf.c_str(), mqtt_data);
#endif
    } while (*data++ != 0);
}

// Subscriber callback
//
// We're subscribed to the following topics:
// <top>/<device_id>/cmd
//
// 
void subscriber_callback(char* topic, uint8_t* payload, unsigned int length) {
  //sanity
  if (length > 254) {
    Serial.printf("MQTT CALLBACK: not handled: payload len overrun:%d\n", length);
    return;
  }
  if (strcmp(topic, topic_cmd.c_str()) == 0) {
    char payload_buf[length+1] = {0};
    strncpy(payload_buf, (char*)payload, length);
    payload_buf[length] = '\0'; //ensure null-termination
    Serial.printf("\n***MQTT CALLBACK: topic '%s', payload '%s'\n", topic, payload_buf);
    if (strstr(payload_buf, "report") == 0) {
      //trigger a data-model dump
      return;
    }
    if (strstr((char*)payload_buf, "meter") == 0) {
      //control the meter
      return;
    }
    if (strstr((char*)payload_buf, "bms") == 0) {
      //BMS command
      return;
    }
    if (strstr((char*)payload_buf, "inverter") == 0) {
      //inverter command
      return;
    }
    // add new commands here
  }
}

void setup_mqtt_client() {
  generateTopics();
  mqttclient.setCallback(subscriber_callback);
  if (!mqtt_connect()) {
    delay(250);
    if (!mqtt_connect()) {
      delay(500);
      if (!mqtt_connect()) {
        Serial.println("MQTT: FAILED TO CONNECT");
        return;
      }
    }
  }
  mqtt_interval_ts = now();
}

void loop_mqtt() {

    if ((millis() - mqtt_interval_ts > MQTT_PUBLISH_INTERVAL)) {
      bool mqtt_connected = mqttclient.connected();
      if (!mqtt_connected) {
        mqtt_connected = mqtt_connect();
      }
      //mqtt_publish(input);
      if (mqtt_connected) {
        mqtt_publish_meter();
      }
      mqtt_interval_ts = millis();
    }
    mqttclient.loop();
}

void mqtt_restart()
{
  if (mqttclient.connected()) {
    mqttclient.disconnect();
  }
}

boolean mqtt_connected()
{
  return mqttclient.connected();
}

void mqtt_publish_door_opened() {
  char buf[32] = {0};
  sprintf(buf,"%s/door", topic_device);
  mqttclient.publish(buf, "open", 0);
}

void mqtt_publish_door_closed() {
  char buf[32] = {0};
  sprintf(buf,"%s/door", topic_device);
  mqttclient.publish(buf, "closed", 0);
}