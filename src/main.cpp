
#include "Arduino.h"
#include "SPIFFS.h"
#include "ArduinoJson.h"

#include "Mlt_Ct_VanPump.h"
#include "Mlt_DIO.h"
#include "Mlt_ESP32Paring.h"
#include "Mlt_ESP32OTA.h"
#include "Mlt_Schedule.h"
#include "Mlt_ESP32Ntp.h"
#include "Mlt_DS1307.h"
// Digital Output Indices
#define DO_PUMP      0 // digital_out_1
#define DO_VALVE_1   1 // digital_out_2
#define DO_VALVE_2   2 // digital_out_3
#define DO_VALVE_3   3 // digital_out_4
#define DO_VALVE_4   4 // digital_out_5
#define DO_VALVE_5   5 // digital_out_6
#define DO_VALVE_6   6 // digital_out_7
#define DO_VALVE_7   7 // digital_out_8
#define NUM_DO       8 // Total number of Digital Outputs

/** Choose use DS1307 RTC or not */
#define USE_DS1307 true
#define DO_OFF_DELAY 3000
MLT_DIOClass MLT_DIO;
CtVanPump ctVanPump;

bool manual_override_pump = false;   // Manual override flag for pump
bool manual_override_valves[NUM_DO]; // Manual override flags for each valve
bool schedule_running = false;  // Flag to track if a schedule is running


void mqttCallback(char *topic, uint8_t *payload, unsigned int length);
void updateIOStateToThingsboard();
void checkAndExecSchedule();
void updateThingsboardByEvent();
void connectionStatusLedLoop();
void logic();
void executeSchedule(int i_schedule, TSchedule &m_schedule);
String getCurrentTimeString();
String getCurrentDayOfWeekString();
bool timeElapsed(uint32_t& last_time, uint32_t interval);
void resetManualOverrideFlags();
void clearManualOverrideFlags();
void turnOffAllDOs();
unsigned long do0_on_time = 0;  // Thời gian bật của DO_0
bool do0_is_on = false;         // Trạng thái của DO_0
unsigned long do0_off_time = 0; // Thời gian tắt của DO_0
bool do0_just_off = false;      // Trạng thái vừa tắt của DO_0
int timepump = 0;
bool timeElapsed(uint32_t& last_time, uint32_t interval) {
    if (millis() - last_time >= interval) {
        last_time = millis();
        return true;
    }
    return false;
}

void setup()
{
    /** Init Serial and SPIFFS */
    Serial.begin(115200);
    ctVanPump.setup();

    if (!SPIFFS.begin())
    {
        Serial.println("Error: SPIFFS Mount Failed");
        return;
    }
    /** Init Digital Input and Output */
    if (!MLT_DIO.diBegin(NUM_DI, DI_0, DI_1, DI_2, DI_3))
    {
        Serial.println("Error: diBegin failed");
    };
    if (!MLT_DIO.doBegin(NUM_DO, DO_0, DO_1, DO_2, DO_3, DO_4, DO_5, DO_6, DO_7))
    {
        Serial.println("Error: doBegin failed");
    };
    /** Init Paring */
    Mlt_ESP32Paring.init(SMART_CONF_BUTTON, SMART_CONF_LED, MQTT_SERVER, MQTT_PORT, mqttCallback);
    if (SMART_CONF_BUTTON_IS_PULL_UP)
    {
        Mlt_ESP32Paring.configBtnMode(BTN_PULL_UP);
    }
    else
    {
        Mlt_ESP32Paring.configBtnMode(BTN_PULL_DOWN);
    }
    Mlt_ESP32Paring.setCheckWifiConnectionInterval(CHECK_WIFI_CONNETION_INTERVAL_SEC * 1000);
    Mlt_ESP32Paring.setCheckMqttConnectionInterval(CHECK_MQTT_CONNETION_INTERVAL_SEC * 1000);
    String device_token = ESP32_ThingsboardService.getDeviceTokent();
    /** Init Thingsboard OTA */
    Mlt_ESP32OTA.init(OTA_SERVER, OTA_PORT, device_token);
    /** Init Schedule */
    Mlt_Schedule.loadScheduleFromFilePagination();
    Mlt_Schedule.setDeviceToken(device_token);
    /** Init Ntp */
    Mlt_ESP32Ntp.setCheckInitSuccessInterval(CHECK_INIT_NTPTIME_INTERVAL_SEC * 1000);
    Mlt_ESP32Ntp.init();
#if (USE_DS1307 == true)
    /** Init DS1307
     * And If NTP time init success, set time to DS1307
     */
    Mlt_DS1307.init();
    struct tm time_info;
    if (Mlt_ESP32Ntp.getInitSuccess() && Mlt_ESP32Ntp.getNtpTime(&time_info))
    {
        Mlt_DS1307.setTime(time_info);
    }
#endif
    /** Connection status led */
    pinMode(LED_CONNECTION_PIN, OUTPUT);
}

void loop()
{
    /** Paring */
    Mlt_ESP32Paring.loop();
    if (ESP32_ThingsboardService.getEventConnected())
    {
        updateIOStateToThingsboard();
        ESP32_ThingsboardService.removeEventConncted();
    }

    /** Digital Input/Output */
    MLT_DIO.loop();
    if (MLT_DIO.diChanged())
    {
        bool di_data[NUM_DI];
        MLT_DIO.getAllDISate(di_data, NUM_DI);
        String msg = "DIs changed: ";
        for (uint8_t i = 0; i < NUM_DI; i++)
        {
            msg += "DI_" + String(i + 1) + ": " + String(di_data[i]) + ", ";
        }
        Serial.println(msg);
        MLT_DIO.clearFlagDiChanged();
        if (ESP32_ThingsboardService.mqttIsConnected())
        {
            updateThingsboardByEvent();
        }
    }
    ctVanPump.updateTelemertry();

    /** NTP and DS1307 */
    Mlt_ESP32Ntp.loop();
    
#if (USE_DS1307 == true)
    /** Check if DS1307 is initialized and retry if necessary */
    static uint32_t last_check_ds1307 = 0;
    if (!Mlt_DS1307.getInitSuccess() && timeElapsed(last_check_ds1307, CHECK_INIT_DS1307_INTERVAL_SEC * 1000))
    {
        Serial.println("DS1307 is not initialized yet, trying to initialize again.");
        Mlt_DS1307.init();
        struct tm time_info;
        if (Mlt_DS1307.getInitSuccess() && Mlt_ESP32Ntp.getNtpTime(&time_info))
        {
            Mlt_DS1307.setTime(time_info);
        }
    }

    /** Sync NTP time with DS1307 at regular intervals */
    static uint32_t last_sync_ntp_to_ds1307 = millis();
    if (Mlt_DS1307.getInitSuccess() && timeElapsed(last_sync_ntp_to_ds1307, SYNC_NTP_TO_DS1307_INTERVAL_SEC * 1000))
    {
        Serial.println("Syncing NTP time to DS1307.");
        struct tm time_info;
        if (Mlt_ESP32Ntp.getNtpTime(&time_info))
        {
            Mlt_DS1307.setTime(time_info);
        }
    }
#endif

    /** Schedule check and execute every CHECK_AND_EXEC_SCHEDULE_INTERVAL_SEC */
    static uint32_t last_check_schedule = millis();
    if (timeElapsed(last_check_schedule, CHECK_AND_EXEC_SCHEDULE_INTERVAL_SEC * 1000))
    {
        checkAndExecSchedule();
    }

    /** Check and set Connection status LED */
    // connectionStatusLedLoop();

    /** Logic control */
    logic();

    /** Request to server to update schedule */
    static uint32_t last_sync_schedule_to_server = millis();
    if (timeElapsed(last_sync_schedule_to_server, SYNC_SCHEDULE_INTERVAL_MINUTE * 60 * 1000))
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            Mlt_Schedule.syncFromDevicetoServerPagination();
        }
    }
}

void setDOState(bool* do_data, int index, bool state) {
    do_data[index] = state;
    MLT_DIO.writeAllDO(do_data, NUM_DO);
}

void logic()
{
    // Define the interval for logging (in milliseconds)
    const unsigned long LOG_INTERVAL = 1000; // 1000 ms = 1 second

    // Static variable to store the last time logs were printed
    static unsigned long last_log_time = 0;

    // Get the current time
    unsigned long current_time = millis();

    // Update timepump based on set_time (assuming set_time is in minutes)
    timepump = ctVanPump.set_time * 60000;

    // Retrieve the current state of all Digital Outputs (DOs)
    bool do_data[NUM_DO];
    MLT_DIO.getAllDOState(do_data, NUM_DO);

    // Determine if any valve (DO2 to DO8) is open
    bool any_valve_open = false;
    for (uint8_t i = DO_VALVE_1; i < NUM_DO; i++) // DO2 to DO8
    {
        if (do_data[i])
        {
            any_valve_open = true;
            break;
        }
    }

    // Check if it's time to print the logs
    if (current_time - last_log_time >= LOG_INTERVAL)
    {
        // Update the last log time
        last_log_time = current_time;

        // Log the state of each valve and any_valve_open
        Serial.println("=== Logic Function ===");
        Serial.println("Pump (DO1) state: " + String(do_data[DO_PUMP] ? "ON" : "OFF"));
        for (uint8_t i = DO_VALVE_1; i < NUM_DO; i++) {
            Serial.println("Valve " + String(i) + " (DO" + String(i + 1) + ") state: " + String(do_data[i] ? "ON" : "OFF"));
        }
        Serial.println("any_valve_open: " + String(any_valve_open ? "Yes" : "No"));
        Serial.println("manual_override_pump: " + String(manual_override_pump ? "ACTIVE" : "INACTIVE"));
        for (uint8_t i = DO_VALVE_1; i < NUM_DO; i++) {
            Serial.println("manual_override_valves[" + String(i) + "]: " + String(manual_override_valves[i] ? "ACTIVE" : "INACTIVE"));
        }
        Serial.println("======================");
    }

    // **Manual Override Flags are now managed exclusively via MQTT callbacks**
    // Proceed with the rest of the logic

    // Check if pump (DO1) can be turned on
    if (do_data[DO_PUMP] && !do0_is_on)
    {
        if (any_valve_open)  // Only turn on the pump if any valve is open
        {
            do0_on_time = millis(); // Record the time when pump is turned on
            do0_is_on = true;       // Mark pump as on
            Serial.println("Pump turned on.");
        }
        else
        {
            // If no valve is open, do not allow the pump to turn on
            do_data[DO_PUMP] = false;                 // Ensure pump stays off
            MLT_DIO.writeAllDO(do_data, NUM_DO);      // Update the DO states
            Serial.println("Pump cannot be turned on without valves open");
            ESP32_ThingsboardService.sendTelemertry("digital_out_1", false);
        }
    }

    // If the pump is on and no valves are open, turn off the pump immediately
    if (do0_is_on && !any_valve_open)
    {
        do_data[DO_PUMP] = false;                  // Turn off the pump (DO1)
        MLT_DIO.writeAllDO(do_data, NUM_DO);       // Write the new DO states
        do0_is_on = false;                         // Mark pump as off
        manual_override_pump = true;               // Set manual override for pump
        ESP32_ThingsboardService.sendTelemertry("digital_out_1", false);
        Serial.println("Pump turned off because no valves are open. Manual override set.");
    }

    // If the pump is on and has been running for the set time, turn it off
    if (do0_is_on && (millis() - do0_on_time >= timepump))
    {
        do_data[DO_PUMP] = false;                  // Turn off the pump (DO1)
        MLT_DIO.writeAllDO(do_data, NUM_DO);       // Write the new DO states
        do0_is_on = false;                         // Mark pump as off
        do0_off_time = millis();                   // Record the time pump was turned off
        do0_just_off = true;                       // Mark pump as just turned off
        manual_override_pump = true;               // Set manual override for pump
        ESP32_ThingsboardService.sendTelemertry("digital_out_1", false);
        Serial.println("Pump turned off after running for set time. Manual override set.");
    }

    // After the pump is off, turn off all valves (DO2 to DO8) after 3 seconds
    if (do0_just_off && (millis() - do0_off_time >= DO_OFF_DELAY))
    {
        for (uint8_t i = DO_VALVE_1; i < NUM_DO; i++)
        {
            do_data[i] = false; // Turn off all valves (DO2 to DO8)
            String key = "digital_out_" + String(i + 1);
            ESP32_ThingsboardService.sendTelemertry(key, false);
        }
        MLT_DIO.writeAllDO(do_data, NUM_DO);       // Write the new DO states
        do0_just_off = false;                      // Reset the just-off state for the pump
        Serial.println("Valves turned off after pump shutdown.");
    }
}


void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
    ctVanPump.update_message(topic, payload, length);
    Serial.println("On message");
    StaticJsonDocument<1024> doc;
    char json[length + 1];
    strncpy(json, (char *)payload, length);
    json[length] = '\0';
    Serial.println("TOPIC: " + String(topic));
    Serial.println("Message: " + String(json));
    DeserializationError error = deserializeJson(doc, json);
    if (error)
    {
        Serial.println("deserializeJson failed");
        Serial.println(error.f_str());
        return;
    }
    if ((strstr((char *)payload, "set_state") != NULL) && (strstr((char *)payload, "digital_out") != NULL))
    {
        /** Set DOs*/
        bool do_data[NUM_DO];
        MLT_DIO.getAllDOState(do_data, NUM_DO);
        for (int i = 0; i < NUM_DO; i++)
        {
            String key = "digital_out_" + String(i + 1); // digital_out_1 to digital_out_8

            if (doc["params"].containsKey(key))
            {
                bool state = doc["params"][key].as<bool>();
                do_data[i] = state;
                ESP32_ThingsboardService.sendTelemertry(key, do_data[i]);

                // Set manual override flags based on user commands
                if (i == DO_PUMP) { // digital_out_1 corresponds to the pump
                    if (!state) {
                        manual_override_pump = true;
                        Serial.println("Manual override: Pump turned off via MQTT.");
                    } else {
                        manual_override_pump = false;
                        Serial.println("Manual override cleared: Pump turned on via MQTT.");
                    }
                } else { // digital_out_2 to digital_out_8 correspond to valves
                    if (!state) {
                        manual_override_valves[i] = true;
                        Serial.println("Manual override: Valve " + String(i) + " turned off via MQTT.");
                    } else {
                        manual_override_valves[i] = false;
                        Serial.println("Manual override cleared: Valve " + String(i) + " turned on via MQTT.");
                    }
                }
            }
        }
        MLT_DIO.writeAllDO(do_data, NUM_DO);
    }
    if (doc["params"].containsKey("power"))
    {
        bool val = doc["params"]["power"].as<bool>();
        bool do_data[NUM_DO];
        MLT_DIO.getAllDOState(do_data, NUM_DO);
        for (int i = 0; i < NUM_DO; i++)
        {
            do_data[i] = val;
            String key = "digital_out_" + String(i + 1);
            ESP32_ThingsboardService.sendTelemertry(key, val);
        }
        MLT_DIO.writeAllDO(do_data, NUM_DO);

        // If power is being turned off, set manual overrides for all DOs
        if (!val) {
            manual_override_pump = true;
            for (uint8_t i = 0; i < NUM_DO; i++) {
                manual_override_valves[i] = true;
            }
            Serial.println("Manual override: Power turned off via MQTT.");
        } else {
            // If power is being turned on, clear all manual overrides
            manual_override_pump = false;
            for (uint8_t i = 0; i < NUM_DO; i++) {
                manual_override_valves[i] = false;
            }
            Serial.println("Manual override cleared: Power turned on via MQTT.");
        }
    }
    if (strstr((char *)payload, "read_device_time") != NULL)
    {
#if (USE_DS1307 == true)
        String time_str = Mlt_DS1307.getTimeString();
#else
        String time_str = Mlt_ESP32Ntp.getTimeString();
#endif
        ESP32_ThingsboardService.sendTelemertry("device_time", time_str);
    }
    if (strstr((char *)payload, "update_schedule") != NULL)
    {
        Serial.println("Updating Mlt_Schedule......");
        Mlt_Schedule.httpGETRequestPagination();
    }
    if (doc["params"].containsKey("firmware_update"))
    {
        bool val = doc["params"]["firmware_update"].as<bool>();
        if (val == true)
        {
            Serial.println("Checking new firmware......");
            OTA_status_check_t otaStatus = Mlt_ESP32OTA.checkFirmwareVersion();
            if (otaStatus == NEW_FIRM_AVAILBLE)
            {
                Mlt_ESP32OTA.executeOTA();
            }
        }
    }
#if (USE_DS1307 == true)
    if (strstr((char *)payload, "sync_device_time") != NULL)
    {
        struct tm time_info;
        if (Mlt_DS1307.getInitSuccess() && Mlt_ESP32Ntp.getNtpTime(&time_info))
        {
            Mlt_DS1307.setTime(time_info);
            String time_str = Mlt_DS1307.getTimeString();
            ESP32_ThingsboardService.sendTelemertry("device_time", time_str);
        }
    }
#endif
}

void updateIOStateToThingsboard()
{
    DynamicJsonDocument data(512);
    data["local_ip"] = WiFi.localIP().toString();
    bool di_data[NUM_DI];
    MLT_DIO.getAllDISate(di_data, NUM_DI);
    for (int i = 0; i < NUM_DI; i++)
    {
        String key_in = "digital_in_" + (String)(i + 1);
        data[key_in] = di_data[i];
    }
    bool do_data[NUM_DO];
    MLT_DIO.getAllDOState(do_data, NUM_DO);
    for (int i = 0; i < NUM_DO; i++)
    {
        String key_out = "digital_out_" + (String)(i + 1);
        data[key_out] = do_data[i];
    }
    String objectString;
    serializeJson(data, objectString);
    ESP32_ThingsboardService.sendTelemertry(objectString);
    ESP32_ThingsboardService.sendTelemertry("firmware_version", Mlt_ESP32OTA.getFirmwareVersion());
}

String getCurrentTimeString() {
#if (USE_DS1307 == true)
    return Mlt_DS1307.getHourAndMinuteString();
#else
    return Mlt_ESP32Ntp.getHourAndMinuteString();
#endif
}

String getCurrentDayOfWeekString() {
#if (USE_DS1307 == true)
    return Mlt_DS1307.getDayOfWeekString();
#else
    return Mlt_ESP32Ntp.getDayOfWeekString();
#endif
}

// Helper to convert time string (HH:MM) to total minutes since 00:00
int timeStringToMinutes(const String &time_str) {
    int hours = time_str.substring(0, 2).toInt();
    int minutes = time_str.substring(3).toInt();
    return hours * 60 + minutes;
}

// Check if current time is between start_time and end_time
bool isCurrentTimeWithinRange(const String &current_time, const String &start_time, const String &end_time) {
    int current_minutes = timeStringToMinutes(current_time);
    int start_minutes = timeStringToMinutes(start_time);
    int end_minutes = timeStringToMinutes(end_time);

    return (current_minutes >= start_minutes && current_minutes <= end_minutes);
}

void resetManualOverrideFlags() {
    manual_override_pump = false;
    Serial.println("Manual override flags reset.");
    for (uint8_t i = 1; i < NUM_DO; i++) {
        manual_override_valves[i] = false;
        Serial.println("manual_override_valves[" + String(i) + "] set to false.");
    }
    Serial.println("All manual override flags cleared after schedule ended.");
}
/**
 * @brief Turns off all Digital Outputs (DOs) and updates Thingsboard telemetry.
 */
void turnOffAllDOs() {
    bool do_data[NUM_DO];
    
    // Set all DO states to false
    for (int i = 0; i < NUM_DO; i++) {
        do_data[i] = false;
    }
    
    // Write the new DO states to hardware
    MLT_DIO.writeAllDO(do_data, NUM_DO);
    
    // Update Thingsboard telemetry for each DO
    for (int i = 0; i < NUM_DO; i++) {
        String key = "digital_out_" + String(i + 1);
        ESP32_ThingsboardService.sendTelemertry(key, false);
    }
    
    Serial.println("Executed turnOffAllDOs(): All Digital Outputs have been set to OFF.");
}

void checkAndExecSchedule() {
    TSchedule m_schedule;
    String current_time = getCurrentTimeString();  // Get current time as "HH:MM"
    Serial.println("Checking schedules at current time: " + current_time);

    bool schedule_was_active = schedule_running;  // Track the previous state
    schedule_running = false;  // Reset schedule_running to false before checking schedules
    bool schedule_just_started = false; // New flag to track if a schedule has just started

    for (int i_schedule = 0; i_schedule < Mlt_Schedule.getNumberOfSchedule(); i_schedule++) {
        m_schedule = Mlt_Schedule.getSchedule(i_schedule);
        Serial.println("Evaluating schedule " + String(i_schedule + 1) + "...");

        // Check if the schedule is enabled
        if (m_schedule.enable) {
            Serial.println("Schedule is enabled.");

            // Check if the current time falls within the schedule's time range
            if (isCurrentTimeWithinRange(current_time, m_schedule.start_time, m_schedule.end_time)) {
                Serial.println("Current time falls within the schedule's active range.");

                // Check if the schedule matches the current day based on the interval
                if (strstr(m_schedule.interval.c_str(), getCurrentDayOfWeekString().c_str())) {
                    Serial.println("Schedule matches the current day. Preparing to execute...");

                    // If the schedule was not running previously, it just started
                    if (!schedule_was_active) {
                        schedule_just_started = true;
                    }

                    // If the schedule has just started, clear any lingering manual override flags
                    if (schedule_just_started) {
                        clearManualOverrideFlags();  // Clear manual overrides at the start of the schedule
                        schedule_just_started = false;  // Reset the flag after clearing overrides
                    }

                    // Run the schedule actions
                    executeSchedule(i_schedule, m_schedule);  // Execute the schedule actions
                    schedule_running = true;  // Mark that a schedule is running

                    // Update IO state to reflect the latest changes
                    updateIOStateToThingsboard();
                } else {
                    Serial.println("Schedule does not match the current day. Skipping execution.");
                }
            } else {
                Serial.println("Current time is outside the schedule's active range.");
            }
        } else {
            Serial.println("Schedule is disabled. Skipping.");
        }
    }

    // **Reset manual overrides after schedule finishes**
    if (schedule_was_active && !schedule_running) {
        resetManualOverrideFlags();  // Reset the manual override flags here
        Serial.println("Manual override flags reset after schedule finished.");
        turnOffAllDOs();
    }
}

// New function to clear manual overrides at the start of a schedule
void clearManualOverrideFlags() {
    Serial.println("Clearing manual override flags at the start of the schedule.");
    manual_override_pump = false;
    for (uint8_t i = 1; i < NUM_DO; i++) {
        if (manual_override_valves[i]) {
            manual_override_valves[i] = false;
            Serial.println("manual_override_valves[" + String(i) + "] cleared.");
        }
    }
}

void executeSchedule(int i_schedule, TSchedule &m_schedule)
{
    Serial.println("Executing schedule " + String(i_schedule + 1) + " with " + String(m_schedule.action_count) + " actions.");

    for (int i_action = 0; i_action < m_schedule.action_count; i_action++)
    {
        bool do_data[NUM_DO];
        MLT_DIO.getAllDOState(do_data, NUM_DO);

        // Execute each action in the schedule
        for (int i_do = 0; i_do < NUM_DO; i_do++) // i_do = 0 to 7 (DO1 to DO8)
        {
            String key = "digital_out_" + String(i_do + 1);

            if (m_schedule.action[i_action].key == key)
            {
                // If manual override is active, skip setting the output for pump or valves
                if (i_do == DO_PUMP && manual_override_pump) {
                    Serial.println("Manual override active for pump (DO1). Skipping pump action.");
                    continue;  // Skip turning the pump on or off
                }
                if (i_do >= DO_VALVE_1 && i_do <= DO_VALVE_7 && manual_override_valves[i_do]) {
                    Serial.println("Manual override active for valve " + String(i_do) + " (DO" + String(i_do + 1) + "). Skipping valve action.");
                    continue;  // Skip turning the valve on or off
                }

                // If no manual override, set the digital output state as per the schedule
                bool state = (m_schedule.action[i_action].value == "true");  // Convert string "true"/"false" to boolean
                do_data[i_do] = state;                                      // Set the output state
                ESP32_ThingsboardService.sendTelemertry(key, state);       // Send telemetry data

                // Execute the action
                MLT_DIO.writeAllDO(do_data, NUM_DO);

                // Log the execution of the action
                Serial.println("Executed action: Set " + key + " to " + String(state ? "ON" : "OFF"));
            }
        }
    }

    // Update IO state to Thingsboard after executing all actions
    updateIOStateToThingsboard();
    Serial.println("All actions for schedule " + String(i_schedule + 1) + " executed. IO state updated.");
}

void updateThingsboardByEvent()
{
    DynamicJsonDocument data(512);
    bool di_data[NUM_DI];
    MLT_DIO.getAllDISate(di_data, NUM_DI);
    for (int i = 0; i < NUM_DI; i++)
    {
        String key = "digital_in_" + (String)(i + 1);
        data[key] = di_data[i];
    }
    String objectString;
    serializeJson(data, objectString);
    if (objectString.length() > 5)
    {
        ESP32_ThingsboardService.sendTelemertry(objectString);
    }
}

void connectionStatusLedLoop()
{
    // static uint32_t check_connection_status_led = millis();
    // if (millis() - check_connection_status_led > 1000)
    // {
    //     if (WiFi.status() != WL_CONNECTED)
    //     {
    //         if (millis() - check_connection_status_led > (LOST_WIFI_LED_INTERVAL_SEC * 1000))
    //         {
    //             digitalWrite(LED_CONNECTION_PIN, !digitalRead(LED_CONNECTION_PIN));
    //             check_connection_status_led = millis();
    //         }
    //     }
    //     else if (ESP32_ThingsboardService.mqttIsConnected() == false)
    //     {
    //         if (millis() - check_connection_status_led > (LOST_MQTT_LED_INTERVAL_SEC * 1000))
    //         {
    //             digitalWrite(LED_CONNECTION_PIN, !digitalRead(LED_CONNECTION_PIN));
    //             check_connection_status_led = millis();
    //         }
    //     }
    //     else if (digitalRead(LED_CONNECTION_PIN) == LOW)
    //     {
    //         digitalWrite(LED_CONNECTION_PIN, HIGH);
    //     }
    // }
}