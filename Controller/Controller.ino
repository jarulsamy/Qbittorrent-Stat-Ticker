#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <Wire.h>

#include "Auth.h"

// Debug Tools
#define DEBUG      0
#define DBGln(...) DEBUG == 1 ? Serial.println(__VA_ARGS__) : NULL
#define DBGf(...)  DEBUG == 1 ? Serial.printf(__VA_ARGS__) : NULL

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
typedef struct
{
  const char* name;
  const char* pause;
  const char* resume;
  const char* query;
} URL;

typedef struct
{
  const URL* urls;
  size_t     dl_speed;
  size_t     ul_speed;
} Host;

#define HOST(name, addr)                            \
  {                                                 \
    name, addr "/api/v2/torrents/pause?hashes=all", \
        addr "/api/v2/torrents/resume?hashes=all",  \
        addr "/api/v2/transfer/info",               \
  }

static const URL urls[] = {
    HOST("Gen", "http://" USERNAME ":" PASSWORD "@10.0.0.3:9091"),
    HOST("PvT", "http://" USERNAME ":" PASSWORD "@10.0.0.3:9092"),
};
#define numHosts (sizeof(urls) / sizeof(urls[0]))
static Host hosts[numHosts] = {0};

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
  oled.setCursor(0, 8);
  oled.println("Name   DL    UL");
  for (int i = 0; i < numHosts; i++)
  {
    snprintf(rowBuf,
             SCREEN_WIDTH,
             "%3s %5u %5u\n",
             hosts[i].urls->name,
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
 * @param hostURLs: Ptr to URLs for specific host.
 * @param action: Either "PAUSE" or "RESUME" to apply to all torrents.
 */
void performAction(const URL* hostURLs, const int action)
{
  WiFiClient client;
  HTTPClient http;

  const char* url = action == PAUSE ? hostURLs->pause : hostURLs->resume;

  if (http.begin(client, url))
  {
    int httpCode = http.GET();
    http.end();
    if (httpCode < 0)
    {
      DBGf("[TOGGLE] FAIL... code: %s\n", http.errorToString(httpCode).c_str());
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
 * @param action: Either "PAUSE" or "RESUME" to apply to all torrents.
 */
void performAction(const int action)
{
  for (int i = 0; i < numHosts; i++)
  {
    performAction(&urls[i], action);
  }
}

/*
 * Query a qBittorrent host to get some more information.
 *
 * Gets current transfer rates.
 *
 * @param host: Which host to query and where to store results.
 */
void query(Host* host)
{
  WiFiClient client;
  HTTPClient http;

  char   buffer[LARGE_BUF_SIZE];
  int    httpCode;
  String payload;

  StaticJsonDocument<384> doc;
  DeserializationError    error;

  /* ---------- Transfer Rates ---------- */
  const char* query_addr = host->urls->query;
  http.begin(client, query_addr);
  httpCode = http.GET();
  if (httpCode < 0)
  {
    DBGf("[QUERY] FAIL... code: %s\n", http.errorToString(httpCode).c_str());
    // TODO: Propogate error!
    return;
  }

  payload = http.getString();
  http.end();

  error = deserializeJson(doc, payload);
  if (error)
  {
    return;
  }

  const long long dl_speed = doc["dl_info_speed"];
  const long long ul_speed = doc["up_info_speed"];

  host->dl_speed = dl_speed / 1000;
  host->ul_speed = ul_speed / 1000;
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

  DBGf("\n[WiFi] Connected to %s\n", SSID);
  Serial.print(F("[WiFi] IP: "));
  DBGln(WiFi.localIP());
}

void setup()
{
#if DEBUG
  Serial.begin(9600);
  Serial.setDebugOutput(true);
#endif
  DBGln("\n\n\n");

  for (unsigned i = 0; i < numHosts; ++i)
  {
    hosts[i].urls     = &urls[i];
    hosts[i].dl_speed = 0;
    hosts[i].ul_speed = 0;
  }

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
    DBGln(F("SSD1306 allocation failed"));
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

  if (action != NONE)
  {
    performAction(action);
    status = action;
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
