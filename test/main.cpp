
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetServer.h>
#include <ModbusEthernet.h>
#include <ModbusRTU.h>
#include <Adafruit_ADS1X15.h>
#include <RTClib.h>
RTC_DS3231 rtc;

byte mac[] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
IPAddress ip(192, 168, 1, 177);

ModbusEthernet mb;
ModbusRTU mb_rtu;
Adafruit_ADS1015 ads;
const int alertReadyPin = 47;
uint8_t input[8] = {1, 2, 3, 4, 5, 6, 15, 16};        // nhập gpio input vào đây
uint8_t output[8] = {20, 19, 21, 42, 41, 40, 39, 38}; // nhập gpio output vào đây

void setup()
{
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 18, 17);
  for (int i = 0; i < 8; i++)
  {
    pinMode(input[i], INPUT);
    pinMode(output[i], OUTPUT);
    digitalWrite(output[i], true);
  }
  delay(1000);
  for (int i = 0; i < 8; i++)
  {
    digitalWrite(output[i], false);
  }

  pinMode(14, OUTPUT);
  digitalWrite(14, LOW);
  delay(500);
  digitalWrite(14, HIGH);
  Ethernet.init(10);
  Serial.println("Ethernet WebServer Example");
  Ethernet.begin(mac, ip);
  mb.server(); // Act as Modbus TCP server
  mb.addHreg(0, 0, 10);
  mb.addCoil(0, 0, 10);
  mb.addIsts(0, 0, 10);
  mb.addIreg(0, 0, 10);

  mb_rtu.begin(&Serial2);
  mb_rtu.slave(1);
  Serial.println(Ethernet.localIP());

  if (!ads.begin())
  {
    Serial.println("Failed to initialize ADS.");
    while (1)
      ;
  }
  if (!rtc.begin())
  {
    Serial.println("Couldn't find RTC!");
    Serial.flush();
    while (1)
      delay(10);
  }
  if (rtc.lostPower())
  {
    // this will adjust to the date and time at compilation
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  rtc.disable32K();
}

void loop()
{

  static unsigned long timeUpdateAds = millis();
  if (millis() - timeUpdateAds >= 500)
  {
    for (int i = 0; i < 4; i++)
    {
      mb.Ireg(i, ads.readADC_SingleEnded(i));
    }
    for (int i = 0; i < 8; i++)
    {
      mb.Coil(i, !mb.Coil(i));
    }
    char date[10] = "hh:mm:ss";
    rtc.now().toString(date);
    Serial.println(date);

    timeUpdateAds = millis();
  }
  for (int i = 0; i < 8; i++)
  {
    digitalWrite(output[i], mb.Coil(i));
    mb.Ists(i, digitalRead(input[i]));
  }
  mb.Hreg(0, random(0, 100));
  mb_rtu.task();
  mb.task();
}
