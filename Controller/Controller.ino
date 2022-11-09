#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <Wire.h>

#include "Auth.h"

// TODO: Consider integrating all the SSD1306 I2C dependencies directly into
// this project, so we can take advantage of better (OLED) buffer management.

// Debug Tools
#define DEBUG 0

#ifdef DEBUG
#define DEBUG_println(...) Serial.println(__VA_ARGS__)
#define DEBUG_printf(...)  Serial.printf(__VA_ARGS__)
#else
#define DEBUG_println(...)
#define DEBUG_printf(...)
#endif  // DEBUG

// Button Assignments
#define DISP_BUTTON_PIN  D5
#define START_BUTTON_PIN D6
#define STOP_BUTTON_PIN  D7

// OLED Ports
#define SCL        D1
#define SDA        D2
#define OLED_RESET -1

// Misc Constants
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define BUF_SIZE       1024
#define QUERY_INTERVAL 1000

#define UNKNOWN_STATUS F("UNKNOWN")
#define SEEDING_STATUS F("SEEDING")
#define PAUSED_STATUS  F("PAUSED")

// Info about one qbittorrent instance.
typedef struct
{
  const char* name;
  const char* pause;
  const char* resume;
  const char* query;
} URL;

// Info about the rates of one qbittorrent instance
// Avoid copying around a bunch of stuff by just having a ptr to URLs.
typedef struct
{
  const URL* urls;
  size_t     dl_speed;
  size_t     ul_speed;
} Host;

// Convenience macro to generate all the URLs at compile-time.
#define HOST(name, addr)                            \
  {                                                 \
    name, addr "/api/v2/torrents/pause?hashes=all", \
        addr "/api/v2/torrents/resume?hashes=all",  \
        addr "/api/v2/transfer/info",               \
  }

static const URL urls[] = {
    HOST("Gen", "http://" USERNAME ":" PASSWORD "@HOST:PORT"),
    HOST("PvT", "http://" USERNAME ":" PASSWORD "@HOST:PORT"),
};
#define numHosts (sizeof(urls) / sizeof(urls[0]))
static Host hosts[numHosts] = {0};

// Interrupt handlers
enum Actions
{
  NONE = 0,
  TOGGLE_DISPLAY,
  PAUSE,
  RESUME,
};
static int status = NONE;

volatile int         action;
void ICACHE_RAM_ATTR pauseISR();
void ICACHE_RAM_ATTR resumeISR();
void ICACHE_RAM_ATTR displayISR();

// Display - 128 x 96 OLED
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
volatile bool    displayOn = 1;

/*
 * Draw an entire 'frame' to the display with all the necessary UI elements.
 */
void drawUI()
{
  // TODO: We could be smart about this, and just redraw specifically the
  // changed elements instead of the entire UI, and save some CPU cycles.
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
  // TODO: Find a way to do this without sprintf
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

/* Qbittorrent API ---------------------------------------------------------- */

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
      /* Erroneous HTTP transaction */
      DEBUG_printf("[TOGGLE] FAIL: %s\n", http.errorToString(httpCode).c_str());
      return;
    }
  }
  else
  {
    /* Failed to start HTTP session */
    DEBUG_printf("[TOGGLE] FAIL: Could not start HTTP session\n");
    return;
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

  int    httpCode;
  String payload;

  // We are dealing with fixed size payloads:
  // https://arduinojson.org/v6/assistant
  StaticJsonDocument<384> doc;
  DeserializationError    error;

  /* ---------- Transfer Rates ---------- */
  const char* query_addr = host->urls->query;
  http.begin(client, query_addr);
  httpCode = http.GET();
  if (httpCode < 0)
  {
    DEBUG_printf("[QUERY] FAIL... code: %s\n",
                 http.errorToString(httpCode).c_str());
    // TODO: Propogate error!
    return;
  }

  payload = http.getString();
  http.end();

  error = deserializeJson(doc, payload);
  if (error)
  {
    // TODO: Propogate error!
    return;
  }

  const long long dl_speed = doc["dl_info_speed"];
  const long long ul_speed = doc["up_info_speed"];

  host->dl_speed = dl_speed / 1000;
  host->ul_speed = ul_speed / 1000;

  /* ------------ Statistics ------------ */
  // TODO: Compute number of active/inactive torrents and such. This may require
  // proxying the JSON responses from qbittorrent, or writing a homebrew JSON
  // parser.
}

/*
 * Query all qBittorrent hosts to get all transfer rates.
 */
void query()
{
  for (int i = 0; i < numHosts; i++)
  {
    query(&hosts[i]);
  }
}

/* ISR ---------------------------------------------------------------------- */

/*
 * Set a flag to pause all the active torrents.
 */
void pauseISR()
{
  action = PAUSE;
}

/*
 * Set a flag to resume all the inactive torrents.
 */
void resumeISR()
{
  action = RESUME;
}

/*
 * Set a flag to toggle the display on or off.
 */
void displayISR()
{
  displayOn = !displayOn;
  action    = TOGGLE_DISPLAY;
}

/* Main --------------------------------------------------------------------- */

/*
 * Block while blinking the display until a WiFi connection is established.
 */
void waitForWiFi()
{
  DEBUG_println("[WiFi] Waiting");
  while (WiFi.status() != WL_CONNECTED)
  {
    /* Blink the screen while connecting to WiFi. */
    DEBUG_printf(".");
    oled.clearDisplay();
    oled.display();
    delay(500);

    oled.setCursor(0, SCREEN_HEIGHT / 2);
    oled.print("Connecting to ");
    oled.print(SSID);
    oled.println("...");
    oled.display();
    delay(500);
  }

  oled.clearDisplay();
  oled.setCursor(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
  oled.print("Connected to ");
  oled.println(SSID);
  delay(1000);
  oled.clearDisplay();
  oled.display();

  DEBUG_printf("\n[WiFi] Connected to %s\n", SSID);
  DEBUG_printf("[WiFi] IP: %s\n", WiFi.localIP());
}

void setup()
{
#if DEBUG
  Serial.begin(9600);
  Serial.setDebugOutput(true);
#endif
  DEBUG_println("\n\n\n");

  for (unsigned i = 0; i < numHosts; ++i)
  {
    hosts[i].urls     = &urls[i];
    hosts[i].dl_speed = 0;
    hosts[i].ul_speed = 0;
  }

  // Setup the pins
  pinMode(DISP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);

  // Setup interrupts
  attachInterrupt(digitalPinToInterrupt(DISP_BUTTON_PIN), displayISR, RISING);
  attachInterrupt(digitalPinToInterrupt(START_BUTTON_PIN), resumeISR, RISING);
  attachInterrupt(digitalPinToInterrupt(STOP_BUTTON_PIN), pauseISR, RISING);

  // Initialize the OLED over I2C
  Wire.begin(SDA, SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    DEBUG_println(F("SSD1306 allocation failed"));
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

  switch (action)
  {
    case NONE:
      break;
    case TOGGLE_DISPLAY:
      if (displayOn)
      {
        oled.ssd1306_command(SSD1306_DISPLAYON);
      }
      else
      {
        oled.ssd1306_command(SSD1306_DISPLAYOFF);
      }
      action = NONE;
      break;
    default:
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
