#ifndef PUMPCONTROLLER_H
#define PUMPCONTROLLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

// uint16_t set_time;
// uint16_t pre_set_time; // Thêm biến này

class CtVanPump
{
public:
    void setup();
    void update();

    void update_message(char *topic, uint8_t *payload, unsigned int length);
    void updateTelemertry();
    void updateSchedule();
    void saveLocalData();
    void exportLocalData();
    // void updateSensorData();
    // void updateGpioStatus();
    uint16_t set_time;

private:
    uint16_t pre_set_time; // Thêm biến này
};

#endif // DOSINGPUMPCONTROLLER_H
