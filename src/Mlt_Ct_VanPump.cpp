#include "Mlt_Ct_VanPump.h"
#include "Mlt_ESP32Paring.h"
// void CtVanPump::updateGpioStatus() {
//     gpioStatus.PUMP1 = digitalRead(PUMP1);
// }
void CtVanPump::setup()
{
    if (!SPIFFS.begin())
    {
        Serial.println("Error: SPIFFS Mount Failed");
    }
    exportLocalData();
}

void CtVanPump::update()
{
    // Placeholder for potential update tasks outside state machine
}

void CtVanPump::update_message(char *topic, uint8_t *payload, unsigned int length)
{
    Serial.println("On message FROM update_message");
    StaticJsonDocument<2048> doc;
    char json[length + 1];
    strncpy(json, (char *)payload, length);
    json[length] = '\0';
    // Serial.println("TOPIC: " + (String)topic);
    // Serial.println("Message: " + (String)json);
    DeserializationError error = deserializeJson(doc, json);

    if (error)
    {
        Serial.println("deserializeJson failed");
        Serial.println(error.f_str());
        return;
    }

    if (strstr((char *)payload, "set_state") != NULL)
    {

        if (doc["params"].containsKey("set_time"))
        {
            uint16_t value = doc["params"]["set_time"].as<uint16_t>();
            set_time = value;
            saveLocalData();
        }
    }
}

void CtVanPump::updateTelemertry()
{
    if (ESP32_ThingsboardService.mqttIsConnected())
    {
        static bool initialized = false;
        if (!initialized)
        {
            initialized = true;
            ESP32_ThingsboardService.sendTelemertry("set_time", set_time);
        }

        // Implement real-time data changes here
        // Check for changes and send updated values
        if (set_time != pre_set_time)
        {
            pre_set_time = set_time;
            ESP32_ThingsboardService.sendTelemertry("set_time", set_time);
        }

        // static unsigned long preUpdateGPIOTime = 0;
        // if (millis() - preUpdateGPIOTime >= 10)
        // {
        //     preUpdateGPIOTime = millis();
        //     updateGpioStatus();
        //     if (gpioStatus.PUMP1 != pre_gpioStatus.PUMP1)
        //     {
        //         pre_gpioStatus.PUMP1 = gpioStatus.PUMP1;
        //         ESP32_ThingsboardService.sendTelemertry("digital_out_5", gpioStatus.PUMP1);
        //     }
        // }
    }
}

void CtVanPump::updateSchedule()
{
    // Load schedule from file and apply actions
    // Add your schedule handling logic here
}

void CtVanPump::saveLocalData()
{
    DynamicJsonDocument data(512);
    data["set_time"] = set_time;

    String jsonString;
    serializeJson(data, jsonString);
    File file = SPIFFS.open("/dosing_pump_config.json", FILE_WRITE);
    if (!file)
    {
        Serial.println("Failed to open config file for writing");
        return;
    }
    file.print(jsonString);
    file.close();
    Serial.println("save data to local");
}

void CtVanPump::exportLocalData()
{
    File file = SPIFFS.open("/dosing_pump_config.json", FILE_READ);
    if (!file)
    {
        Serial.println("Failed to open config file for reading");
        return;
    }
    String jsonString = file.readString();
    file.close();

    DynamicJsonDocument data(1024);
    DeserializationError error = deserializeJson(data, jsonString);
    if (error)
    {
        Serial.println("Failed to deserialize JSON");
        return;
    }

    set_time = data["set_time"];
}
