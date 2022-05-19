#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <Wire.h>

#include "auth.h"

// Button Assignments
#define DISPLAY_BUTTON_PIN D5
#define START_BUTTON_PIN   D6
#define STOP_BUTTON_PIN    D7

// OLED Ports
#define SCL D1
#define SDA D2

// Misc Constants
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define BUF_SIZE       128
#define LARGE_BUF_SIZE 1024
#define QUERY_INTERVAL 1000

#define UNKNOWN_STATUS F("UNKNOWN")
#define SEEDING_STATUS F("SEEDING")
#define PAUSED_STATUS  F("PAUSED")
int status = 0;

// Info about one qbittorrent instance.
struct Host
{
  const char *name;
  const char *address;

  size_t dl_speed;
  size_t ul_speed;

  // TODO: Need to rework the qbit api to return smaller JSON responses for
  // these.
  size_t paused;
  size_t resumed;
};

// Some basic info about each of the hosts.
static struct Host hosts[] = {
    {"Gen", "http://" USERNAME ":" PASSWORD "@10.0.0.3:9091", 0, 0, 0, 0},
    {"PvT", "http://" USERNAME ":" PASSWORD "@10.0.0.3:9092", 0, 0, 0, 0},
};
// Compile-time :D
const int numHosts = sizeof(hosts) / sizeof(hosts[0]);

// Interrupt handlers
enum Actions
{
  NONE = 0,
  PAUSE,
  RESUME,
};
volatile int         action;
void ICACHE_RAM_ATTR pauseISR();
void ICACHE_RAM_ATTR resumeISR();
void ICACHE_RAM_ATTR displayISR();

// 128 x 96 OLED Display
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
volatile boolean displayOn = 1;

// JSON Parsing, just reuse a global one.
StaticJsonDocument<LARGE_BUF_SIZE> JSONBuffer;

/*
 * Draw an entire 'frame' to the display with all the necessary UI elements.
 */
void drawUI()
{
  char rowBuf[SCREEN_WIDTH] = {0};

  // Clear and default settings.
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  // Draw current status at the top.
  oled.setCursor(0, 0);
  switch (status)
  {
    case NONE:
      oled.println(UNKNOWN_STATUS);
      break;
    case PAUSE:
      oled.println(PAUSED_STATUS);
      break;
    case RESUME:
      oled.println(SEEDING_STATUS);
      break;
    default:
      break;
  }

  // Draw the IP address at the bottom.
  oled.setCursor(0, SCREEN_HEIGHT - 8);
  oled.print(F("IP: "));
  if (WiFi.status() == WL_CONNECTED)
  {
    oled.print(WiFi.localIP());
  }
  else
  {
    oled.print(F("N/A"));
  }

  // Draw the hosts in a somewhat tabular way.
  // TODO: Determine if there is a printf equivalent to this.
  // TODO: Or just use a buffer with sprintf.
  oled.setCursor(0, 8);
  oled.println("Name   DL    UL");
  for (int i = 0; i < numHosts; i++)
  {
    snprintf(rowBuf,
             SCREEN_WIDTH,
             "%3s %5u %5u\n",
             hosts[i].name,
             hosts[i].dl_speed,
             hosts[i].ul_speed);
    oled.print(rowBuf);
  }
  oled.display();
}

/*
 * Perform an HTTP get to "host"/api/v2/torrents/"action"?hashes=all
 *
 * Essentially can be used to perform a pause or resume action on all torrents.
 *
 * @param host: Base address for GET request.
 * @param action: Either "pause" or "resume" to apply to all torrents.
 */
void performAction(const char *host, const char *action)
{
  WiFiClient client;
  HTTPClient http;

  char buffer[BUF_SIZE];
  sprintf(buffer, "%s/api/v2/torrents/%s?hashes=all", host, action);

  if (http.begin(client, buffer))
  {
    Serial.printf("[TOGGLE] GET: %s\n", buffer);
    int httpCode = http.GET();
    http.end();
    if (httpCode < 0)
    {
      Serial.printf("[TOGGLE] FAIL... code: %s\n",
                    http.errorToString(httpCode).c_str());
      return;
    }
  }
}

/*
 * Perform an HTTP get to all hosts.
 *
 * Essentially can be used to perform a pause or resume action on all torrents
 * across all hosts.
 *
 * @param action: Either "pause" or "resume" to apply to all torrents.
 */
void performAction(const char *action)
{
  for (int i = 0; i < numHosts; i++)
  {
    performAction(hosts[i].address, action);
  }
}

/*
 * Query a qBittorrent host to get some more information.
 *
 * Gets current transfer rates.
 *
 * @param host: Which host to query and where to store results.
 */
void query(struct Host *host)
{
  WiFiClient client;
  HTTPClient http;

  char                 endpoint[BUF_SIZE];
  char                 buffer[LARGE_BUF_SIZE];
  int                  httpCode;
  String               payload;
  DeserializationError error;

  /* ---------- Transfer Rates ---------- */
  sprintf(buffer, "%s/api/v2/transfer/info", host->address);
  http.begin(client, buffer);
  httpCode = http.GET();
  if (httpCode < 0)
  {
    Serial.printf("[QUERY] FAIL... code: %s\n",
                  http.errorToString(httpCode).c_str());
    // TODO: Propogate error!
    return;
  }
  payload = http.getString();
  http.end();

  error = deserializeJson(JSONBuffer, payload);
  if (error)
  {
    Serial.println("[QUERY] Fail parsing payload");
    Serial.printf("[QUERY] %s\n", error.c_str());
    // TODO: Propogate error!
    return;
  }

  host->dl_speed = (size_t)JSONBuffer["dl_info_speed"] / 1000;
  host->ul_speed = (size_t)JSONBuffer["up_info_speed"] / 1000;
}

/*
 * Query a all qBittorrent hosts to get some more information.
 *
 * Gets current transfer rates.
 */
void query()
{
  for (int i = 0; i < numHosts; i++)
  {
    query(&hosts[i]);
  }
}

/*
 * Set a flag to pause all the active torrents.
 * The main 'loop' thread will perform this after the ISR terminates.
 */
void pauseISR()
{
  action = PAUSE;
}

/*
 * Set a flag to resume all the inactive torrents.
 * The main 'loop' thread will perform this after the ISR terminates.
 */
void resumeISR()
{
  action = RESUME;
}

/*
 * Toggle the display on or off.
 */
void displayISR()
{
  displayOn = !displayOn;

  if (displayOn)
  {
    oled.ssd1306_command(SSD1306_DISPLAYON);
  }
  else
  {
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

/*
 * Block while blinking the display until a WiFi connection is established.
 */
void waitForWiFi()
{
  Serial.print(F("[WiFi] Waiting"));
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    oled.clearDisplay();
    oled.display();
    delay(500);

    oled.setCursor(0, SCREEN_HEIGHT / 2);
    oled.print(F("Connecting to "));
    oled.print(SSID);
    oled.println(F("..."));
    oled.display();
    delay(500);
  }

  oled.clearDisplay();
  oled.setCursor(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
  oled.print(F("Connected to "));
  oled.println(SSID);
  delay(1000);
  oled.clearDisplay();
  oled.display();

  Serial.printf("\n[WiFi] Connected to %s\n", SSID);
  Serial.print(F("[WiFi] IP: "));
  Serial.println(WiFi.localIP());
}

void setup()
{
  Serial.begin(9600);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println();
  Serial.println();
  Serial.println();

  // Setup the pins
  pinMode(DISPLAY_BUTTON_PIN, INPUT_PULLUP);
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);

  // Setup interrupts
  attachInterrupt(
      digitalPinToInterrupt(DISPLAY_BUTTON_PIN), displayISR, RISING);
  attachInterrupt(digitalPinToInterrupt(START_BUTTON_PIN), resumeISR, RISING);
  attachInterrupt(digitalPinToInterrupt(STOP_BUTTON_PIN), pauseISR, RISING);

  // Initialize the OLED
  Wire.begin(SDA, SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  drawUI();

  // Connect to the WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PSK);
  waitForWiFi();
  drawUI();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    waitForWiFi();
  }

  if (action == PAUSE)
  {
    status = PAUSE;
    performAction("pause");
    action = NONE;
  }
  else if (action == RESUME)
  {
    status = RESUME;
    performAction("resume");
    action = NONE;
  }

  // Only run the continuous queries if the display is on.
  if (displayOn)
  {
    query();
  }
  drawUI();

  delay(QUERY_INTERVAL);
}
