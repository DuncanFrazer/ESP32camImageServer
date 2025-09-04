#include <WiFi.h>
#include "esp_camera.h"
#include "secrets.h"  // WIFI_SSID and WIFI_PASS

// ==== AI Thinker ESP32-CAM (OV2640) pin map ====
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ---- Onboard white LED (flash) ----
static int  LED_PIN = 4;              // white flash LED
static bool LED_ACTIVE_HIGH = true;   // can be flipped at runtime
static int  LED_DUTY = 25;

// Pick a timer/channel FAR from camera (camera uses LEDC_TIMER_0 / CHANNEL_0)
static const int LEDC_CH    = 7;      // free channel
static const int LEDC_TIMER = 3;      // free timer
static const int LEDC_FREQ  = 5000;   // 5 kHz
static const int LEDC_BITS  = 8;      // 0..255

WiFiServer server(80);
WiFiClient streamClient;
bool streamActive = false;
bool streamHdrSent = false;

static void led_detach() {
  ledcDetachPin(LED_PIN);
  pinMode(LED_PIN, OUTPUT);
}

static void led_attach() {
  ledcSetup(LEDC_CH, LEDC_FREQ, LEDC_BITS);  // timer 3
  ledcAttachPin(LED_PIN, LEDC_CH);
}

static void led_write_duty(uint8_t duty) {   // 0..255
  uint8_t out = LED_ACTIVE_HIGH ? duty : (255 - duty);
  ledcWrite(LEDC_CH, out);
}

static void led_off() { led_write_duty(0); }
static void led_on()  { led_write_duty(LED_DUTY); }

// Recovery for “stuck LED”
static void led_recover() {
  led_detach();
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH); // force OFF as GPIO
  delay(10);
  led_attach();
  led_off();
}

// ----- HTTP helpers -----
static void sendPlain(WiFiClient& c, const String& body) {
  c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
  c.print(body);
}

static void sendHtml(WiFiClient& c, const String& body) {
  c.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
  c.print(body);
}

static void sendJpeg(WiFiClient& client) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { client.println("HTTP/1.1 503 Service Unavailable\r\n\r\n"); return; }
  client.printf(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %u\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n", fb->len);
  client.write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

static void streamMjpeg(WiFiClient& client) {
  const char* hdr =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n";
  client.print(hdr);

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) break;

    client.print("--frame\r\n");
    client.printf("Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);

    delay(100); // gentle pace
  }
}

static String htmlPage(const IPAddress& ip) {
  // Minimal, responsive-ish UI with stream preview and LED controls
  String ipStr = ip.toString();
  String s;
  s.reserve(3000);
  s += "<!doctype html><html><head><meta charset='utf-8'/><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>ESP32-CAM Control</title>";
  s += "<style>body{font-family:sans-serif;margin:16px} .row{margin:12px 0} button{padding:8px 12px;margin-right:8px} ";
  s += "input[type=range]{width:260px} img{max-width:100%;height:auto;border:1px solid #ccc;border-radius:6px} ";
  s += ".card{padding:12px;border:1px solid #ddd;border-radius:8px;margin-bottom:16px;box-shadow:0 1px 2px rgba(0,0,0,.05)}";
  s += "</style></head><body>";
  s += "<h2>ESP32-CAM</h2>";

  s += "<div class='card'><h3>Stream</h3>";
  s += "<div class='row'><img id='stream' src='http://" + ipStr + "/stream' alt='stream'></div>";
  s += "<div class='row'><a href='/jpg' target='_blank'><button>Snapshot (/jpg)</button></a></div>";
  s += "</div>";

  s += "<div class='card'><h3>LED</h3>";
  s += "<div class='row'>";
  s += "<button onclick=\"fetch('/led/on').then(()=>{})\">On</button>";
  s += "<button onclick=\"fetch('/led/off').then(()=>{})\">Off</button>";
  s += "</div>";
  s += "<div class='row'>";
  s += "Brightness: <input id='slider' type='range' min='0' max='255' value='0' ";
  s += "oninput=\"duty.value=this.value\" ";
  s += "onchange=\"fetch('/led?duty='+this.value).then(()=>{})\">";
  s += " <output id='duty'>0</output>";
  s += "</div>";
  s += "</div>";

  s += "<div class='card'><h3>Info</h3>";
  s += "<div class='row'>IP: " + ipStr + "</div>";
  s += "<div class='row'>Endpoints: /, /jpg, /stream, /led/on, /led/off, /led?duty=0..255</div>";
  s += "</div>";

  s += "</body></html>";
  return s;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // ---- Camera (stable profile) ----
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 10000000;           // 20 MHz didn't work on my cheap board from aliexpress
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_SVGA;
  config.jpeg_quality = 15;
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = (psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH); // make sure it's OFF
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    for(;;) delay(1000);
  }
  Serial.println("Camera init OK");
  led_attach();
  led_on();

  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Wi-Fi connecting");
  for (int i=0; i<60 && WiFi.status()!=WL_CONNECTED; ++i) { delay(250); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  server.begin();
  Serial.println("HTTP server started. Open http://<ip>/");
}

void loop() {
  bool isStreamRequest = false;
  WiFiClient client = server.available();
  bool haveClient = (bool)client;   // proceed only if a new client connected

  if (haveClient) {
    String line = client.readStringUntil('\r');
    client.readStringUntil('\n');

    bool isStreamRequest = false;

    if (line.startsWith("GET / ")) {
      sendHtml(client, htmlPage(WiFi.localIP()));

    } else if (line.startsWith("GET /jpg")) {
      sendJpeg(client);

    } else if (line.startsWith("GET /stream")) {
      streamClient = client; // keep this socket for streaming
      streamClient.setNoDelay(true);
      streamActive = true;
      streamHdrSent = false;
      isStreamRequest = false;
      // IMPORTANT: do NOT call client.stop() here

    } else if (line.startsWith("GET /led/recover")) {
      led_recover();
      sendPlain(client, "LED recovered (PWM reset)\n");
    } else if (line.startsWith("GET /led/on")) {
      led_on();
      sendPlain(client, "LED ON\n");

    } else if (line.startsWith("GET /led/off")) {
      led_off();
      sendPlain(client, "LED OFF\n");
    }

    // /led?duty=N    -> PWM brightness
    else if (line.startsWith("GET /led?")) {
      int q = line.indexOf("duty=");
      if (q >= 0) {
        int amp = line.indexOf('&', q);
        String val = (amp > 0 ? line.substring(q+5, amp) : line.substring(q+5));
        val.trim();
        int duty = constrain(val.toInt(), 0, 255);
        led_write_duty((uint8_t)duty);
        LED_DUTY = duty;
        sendPlain(client, String("LED duty set to ") + duty + "\n");
      } else {
        sendPlain(client, "Usage: /led?duty=0..255\n");
      }
    }
    else if (line.startsWith("GET /led/polarity")) {
      bool newActiveHigh = LED_ACTIVE_HIGH;
      int q = line.indexOf("active=");
      if (q >= 0) {
        String val = line.substring(q+7);
        int sp = val.indexOf(' ');
        if (sp > 0) val = val.substring(0, sp);
        val.trim();
        if (val == "high") newActiveHigh = true;
        else if (val == "low") newActiveHigh = false;
      }
      LED_ACTIVE_HIGH = newActiveHigh;
      led_off(); // re-apply OFF with new polarity
      sendPlain(client, String("Polarity set to ") + (LED_ACTIVE_HIGH ? "active-high\n" : "active-low\n"));
    }
    else {
      // Fallback help
      sendPlain(client,
        "OK\n\n"
        "Endpoints:\n"
        "  /               - HTML control page\n"
        "  /jpg            - single JPEG\n"
        "  /stream         - MJPEG stream\n"
        "  /led/on         - LED on (full)\n"
        "  /led/off        - LED off\n"
        "  /led?duty=0..255- LED brightness\n");
    }
  }

  // send one frame to the active stream client per loop iteration
  if (streamActive) {
    if (!streamClient.connected()) {
      streamClient.stop();
      streamActive = false;
      streamHdrSent = false;
    } else {
      if (!streamHdrSent) {
        streamClient.print(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n"
        );
        streamHdrSent = true;
      }
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        streamClient.print("--frame\r\n");
        streamClient.printf("Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
        streamClient.write(fb->buf, fb->len);
        streamClient.print("\r\n");
        esp_camera_fb_return(fb);
      }
      delay(40); // adjust 20-80ms for frame rate
    }
  }

  if (!isStreamRequest) {
    client.stop();   // close normal requests
  }
}
