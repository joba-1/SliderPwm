/*
HTML Slider with server feedback on value changes to control PWM duty cycle (see app.cpp)

Influx DB can be created with command influx -execute "create database PROGNAME"
Monitors GPIO pin pulled to ground as key press
Use builtin led to represent health status
TODO
* maybe use websocket?
*/

#include <Arduino.h>

#include <app.h>
#include <FileSys.h>

#include <WiFiManager.h>

// Config for ESP8266 or ESP32
#if defined(ESP8266)
    #define HEALTH_LED_INVERTED false
    #define HEALTH_LED_PIN LED_BUILTIN
    #define HEALTH_LED_CHANNEL -1
    #define BUTTON_PIN 0

    // Web Updater
    #define WEBSERVER_H
    #include <ESPAsyncWebServer.h>
    // #include <ESP8266HTTPUpdateServer.h>
    // #include <ESP8266WebServer.h>
    #include <ESP8266WiFi.h>
    #include <ESP8266mDNS.h>
    #include <WiFiClient.h>
    #define WebServer ESP8266WebServer
    #define HTTPUpdateServer ESP8266HTTPUpdateServer

    // Post to InfluxDB
    #include <ESP8266HTTPClient.h>

    // Time sync
    #include <NTPClient.h>
    #include <WiFiUdp.h>
    WiFiUDP ntpUDP;
    NTPClient ntp(ntpUDP, NTP_SERVER);

    const char *hostname() { return WiFi.hostname().c_str(); }
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
    #define HEALTH_LED_INVERTED true
    #define HEALTH_LED_CHANNEL 0
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        #define HEALTH_LED_PIN -1
        #define BUTTON_PIN 0
    #else
        #define HEALTH_LED_PIN 8
        #define BUTTON_PIN 9
    #endif

    // Web Updater
    #include <ESPAsyncWebServer.h>
    #include <WiFi.h>
    #include <ESPmDNS.h>
    #include <WiFiClient.h>
    #include <SPIFFS.h>

    // Post to InfluxDB
    #include <HTTPClient.h>
    
    // Time sync
    #include <time.h>

    // Reset reason
    #include "rom/rtc.h"

    const char *hostname() { return WiFi.getHostname(); }
#elif defined(ESP32)
    #define HEALTH_LED_INVERTED false
    #ifdef LED_BUILTIN
        #define HEALTH_LED_PIN LED_BUILTIN
    #else
        #define HEALTH_LED_PIN 16
    #endif
    #define HEALTH_LED_CHANNEL 0
    #define BUTTON_PIN 0

    // Web Updater
    #include <ESPAsyncWebServer.h>
    #include <WiFi.h>
    #include <ESPmDNS.h>
    #include <WiFiClient.h>

    // Post to InfluxDB
    #include <HTTPClient.h>
    
    // Time sync
    #include <time.h>

    // Reset reason
    #include "rom/rtc.h"

    const char *hostname() { return WiFi.getHostname(); }
#else
    #error "No ESP8266 or ESP32, define your pins and includes here!"
#endif

// Health LED
#include <Breathing.h>
const uint32_t health_ok_interval = 5000;
const uint32_t health_err_interval = 1000;
Breathing health_led(health_ok_interval, HEALTH_LED_PIN, HEALTH_LED_INVERTED, HEALTH_LED_CHANNEL);
bool enabledBreathing = true;  // global flag to switch breathing animation on or off

// Infrastructure
#include <Syslog.h>
#include <FileSys.h>

FileSys fileSys;

// Web status page and OTA updater
#define WEBSERVER_PORT 80

AsyncWebServer web_server(WEBSERVER_PORT);
bool shouldReboot = false;  // after updates...

// Post to InfluxDB
int influx_status = 0;
time_t post_time = 0;

// publish to mqtt broker
#include <PubSubClient.h>

WiFiClient wifiMqtt;
PubSubClient mqtt(wifiMqtt);

// Syslog
WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);
char msg[512];  // one buffer for all syslog and json messages

char start_time[30];


void slog(const char *message, uint16_t pri = LOG_INFO) {
    static bool log_infos = true;
    
    if (pri < LOG_INFO || log_infos) {
        Serial.println(message);
        syslog.log(pri, message);
    }

    if (log_infos && millis() > 10 * 60 * 1000) {
        log_infos = false;  // log infos only for first 10 minutes
        slog("Switch off info level messages", LOG_NOTICE);
    }
}


void publish( const char *topic, const char *payload ) {
    if (mqtt.connected() && !mqtt.publish(topic, payload)) {
        slog("Mqtt publish failed");
    }
}


// Post data to InfluxDB
bool postInflux(const char *line) {
    static const char uri[] = "/write?db=" INFLUX_DB "&precision=s";

    WiFiClient wifiHttp;
    HTTPClient http;

    http.begin(wifiHttp, INFLUX_SERVER, INFLUX_PORT, uri);
    http.setUserAgent(PROGNAME);
    int prev = influx_status;
    influx_status = http.POST(line);
    String payload;
    if (http.getSize() > 0) { // workaround for bug in getString()
        payload = http.getString();
    }
    http.end();

    if (influx_status != prev) {
        snprintf(msg, sizeof(msg), "%d", influx_status);
        publish(MQTT_TOPIC "/status/DBResponse", msg);
    }

    if (influx_status < 200 || influx_status >= 300) {
        snprintf(msg, sizeof(msg), "Post %s:%d%s status=%d line='%s' response='%s'",
            INFLUX_SERVER, INFLUX_PORT, uri, influx_status, line, payload.c_str());
        slog(msg, LOG_ERR);
        return false;
    }

    post_time = time(NULL);
    return true;
}


// Wifi status as JSON
bool json_Wifi(char *json, size_t maxlen, const char *bssid, int8_t rssi) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Hostname\":\"%s\",\"Wifi\":{"
        "\"BSSID\":\"%s\","
        "\"IP\":\"%s\","
        "\"RSSI\":%d}}";

    int len = snprintf(json, maxlen, jsonFmt, hostname(), bssid, WiFi.localIP().toString().c_str(), rssi);

    return len < maxlen;
}


// Wifi status as JSON
bool json_Pwm(char *json, size_t maxlen, bool on) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Hostname\":\"%s\",\"Pwm\":{"
        "\"Duties\":[%s],"
        "\"Power\":%d}}";
    

    int len = snprintf(json, maxlen, jsonFmt, hostname(), get_duties(), get_power() ? 1 : 0);

    return len < maxlen;
}


// Report a change of duty or power
void report_pwm( bool on ) {
    static const char lineFmt[] =
        "Pwm,Host=%s,Version=" VERSION " "
        "DutyR=%d,"
        "DutyG=%d,"
        "DutyB=%d,"
        "DutyW=%d,"
        "Power=%d";
    static const uint32_t interval = 60000;
    static uint32_t prev_ms = 0;
    static uint32_t changed_ms = 0;
    static int prevDuty[LED_COUNT] = { -1, -1, -1, -1 };
    static bool prevPower = false;

    if (!WiFi.isConnected()) return;

    uint32_t now = millis();

    // rate limit changes
    for( int i = LED_START; i < LED_COUNT; i++ ) {
        led_t led = static_cast<led_t>(i);

        if (get_duty(led) != prevDuty[led] || on != prevPower) {
            changed_ms = now;
            if (!changed_ms) changed_ms--;
            prevDuty[led] = get_duty(led);
            prevPower = on;
        }
    }

    if ( (changed_ms && now - changed_ms > 1000) || (now - prev_ms > interval) ) {
        json_Pwm(msg, sizeof(msg), on);
        slog(msg);
        publish(MQTT_TOPIC "/json/Pwm", msg);

        snprintf(msg, sizeof(msg), lineFmt, hostname(), 
            get_duty(LED_R), get_duty(LED_G), get_duty(LED_B), get_duty(LED_W), on);
        postInflux(msg);

        prev_ms = now;
        changed_ms = 0;
    }
}


char lastBssid[] = "00:00:00:00:00:00";  // last known connected AP (for web page) 
int8_t lastRssi = 0;                     // last RSSI (for web page)

// Report a change of RSSI or BSSID
void report_wifi( int8_t rssi, const byte *bssid ) {
    static const char digits[] = "0123456789abcdef";
    static const char lineFmt[] =
        "Wifi,Host=%s,Version=" VERSION " "
        "BSSID=\"%s\","
        "IP=\"%s\","
        "RSSI=%d";
    static const uint32_t interval = 60000;
    static const int8_t min_diff = 5;
    static uint32_t prev = 0;
    static int8_t reportedRssi = 0;

    // Update for web page
    lastRssi = rssi;
    for (size_t i=0; i<sizeof(lastBssid); i+=3) {
        lastBssid[i] = digits[bssid[i/3] >> 4];
        lastBssid[i+1] = digits[bssid[i/3] & 0xf];
    }

    // RSSI rate limit for log and db
    int8_t diff = reportedRssi - lastRssi;
    if (diff < 0 ) {
        diff = -diff;
    }
    uint32_t now = millis();
    if (diff >= min_diff || (now - prev > interval) ) {
        json_Wifi(msg, sizeof(msg), lastBssid, lastRssi);
        slog(msg);
        publish(MQTT_TOPIC "/json/Wifi", msg);

        snprintf(msg, sizeof(msg), lineFmt, hostname(), lastBssid, WiFi.localIP().toString().c_str(), lastRssi);
        postInflux(msg);

        reportedRssi = lastRssi;
        prev = now;
    }
}


// check and report RSSI and BSSID changes
bool handle_wifi() {
    static byte prevBssid[6] = {0};
    static int8_t prevRssi = 0;
    static bool prevConnected = false;

    static const uint32_t reconnectInterval = 10000;  // try reconnect every 10s
    static const uint32_t reconnectLimit = 60;        // try restart after 10min
    static uint32_t reconnectPrev = 0;
    static uint32_t reconnectCount = 0;

    bool currConnected = WiFi.isConnected();
    int8_t currRssi = 0;
    byte *currBssid = prevBssid;

    if (currConnected) {
        currRssi = WiFi.RSSI();
        currBssid = WiFi.BSSID();

        if (!prevConnected) {
            report_wifi(prevRssi, prevBssid);
        }

        if (currRssi != prevRssi || memcmp(currBssid, prevBssid, sizeof(prevBssid))) {
            report_wifi(currRssi, currBssid);
        }

        memcpy(prevBssid, currBssid, sizeof(prevBssid));
        reconnectCount = 0;
    }
    else {
        uint32_t now = millis();
        if (reconnectCount == 0 || now - reconnectPrev > reconnectInterval) {
            WiFi.reconnect();
            reconnectCount++;
            if (reconnectCount > reconnectLimit) {
                Serial.println("Failed to reconnect WLAN, about to reset");
                for (int i = 0; i < 20; i++) {
                    digitalWrite(HEALTH_LED_PIN, (i & 1) ? LOW : HIGH);
                    delay(100);
                }
                ESP.restart();
                while (true)
                    ;
            }
            reconnectPrev = now;
        }
    }

    prevRssi = currRssi;
    prevConnected = currConnected;

    return currConnected;
}


char web_msg[80] = "";  // main web page displays and then clears this

// Standard web page
const char *main_page() {
    static const char fmt[] =
        "<!doctype html>\n"
        "<html lang=\"en\">\n"
        " <head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <link href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQAgMAAABinRfyAAAADFBMVEUqYbutnpTMuq/70SQgIef5AAAAVUlEQVQIHWOAAPkvDAyM3+Y7MLA7NV5g4GVqKGCQYWowYTBhapBhMGB04GE4/0X+M8Pxi+6XGS67XzzO8FH+iz/Dl/q/8gx/2S/UM/y/wP6f4T8QAAB3Bx3jhPJqfQAAAABJRU5ErkJggg==\" rel=\"icon\" type=\"image/x-icon\" />\n"
        "  <link href=\"bootstrap.min.css\" rel=\"stylesheet\">\n"
        "  <title>" PROGNAME " v" VERSION "</title>\n"
        " </head>\n"
        " <body>\n"
        "  <div class=\"container\">\n"
        "   <form action=\"/change\" method=\"post\" enctype=\"multipart/form-data\" id=\"form\">\n"
        "    <div class=\"row\">\n"
        "     <div class=\"col-12\">\n"
        "      <h1>" PROGNAME " v" VERSION "</h1>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-10\">\n"
        "      <input style=\"width:100%%\" id=\"slider0\" type=\"range\" min=\"0\" max=\"1000\" step=\"1\" value=\"%d\">\n"
        "     </div>\n"
        "     <div class=\"col-2\">\n"
        "      <div class=\"float-end\" id=\"sliderValue0\">%d</div>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-10\">\n"
        "      <input style=\"width:100%%\" id=\"slider1\" type=\"range\" min=\"0\" max=\"1000\" step=\"1\" value=\"%d\">\n"
        "     </div>\n"
        "     <div class=\"col-2\">\n"
        "      <div class=\"float-end\" id=\"sliderValue1\">%d</div>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-10\">\n"
        "      <input style=\"width:100%%\" id=\"slider2\" type=\"range\" min=\"0\" max=\"1000\" step=\"1\" value=\"%d\">\n"
        "     </div>\n"
        "     <div class=\"col-2\">\n"
        "      <div class=\"float-end\" id=\"sliderValue2\">%d</div>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-10\">\n"
        "      <input style=\"width:100%%\" id=\"slider3\" type=\"range\" min=\"0\" max=\"1000\" step=\"1\" value=\"%d\">\n"
        "     </div>\n"
        "     <div class=\"col-2\">\n"
        "      <div class=\"float-end\" id=\"sliderValue3\">%d</div>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-2\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-1\">Toggle</button>\n"
        "     </div>\n"
        "     <div class=\"col-8\"></div>\n"
        "    </div>\n"
        "   </form>\n"
        "   <div class=\"accordion\" id=\"infos\">\n"
        "    <div class=\"accordion-item\">\n"
        "     <h2 class=\"accordion-header\" id=\"heading1\">\n"
        "      <button class=\"accordion-button\" type=\"button\" data-bs-toggle=\"collapse\" data-bs-target=\"#infos1\" aria-expanded=\"true\" aria-controls=\"infos1\">\n"
        "       Infos\n"
        "      </button>\n"
        "     </h2>\n"
        "     <div id=\"infos1\" class=\"accordion-collapse collapse\" aria-labelledby=\"heading1\" data-bs-parent=\"#infos\">\n"
        "      <div class=\"accordion-body\">\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"pwm\">Pwm</label></div>\n"
        "        <div class=\"col\" id=\"pwm\"><a href=\"/json/Pwm\">JSON</a></div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"wifi\">Wifi</label></div>\n"
        "        <div class=\"col\" id=\"wifi\"><a href=\"/json/Wifi\">JSON</a></div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"update\">Post firmware image to</label></div>\n"
        "        <div class=\"col\" id=\"update\"><a href=\"/update\">/update</a></div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"start\">Last start time</label></div>\n"
        "        <div class=\"col\" id=\"start\">%s</div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"web\">Last web update</label></div>\n"
        "        <div class=\"col\" id=\"web\">%s</div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"influx\">Last influx update</label></div>\n"
        "        <div class=\"col\" id=\"influx\">%s</div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"status\">Influx status</label></div>\n"
        "        <div class=\"col\" id=\"status\">%d</div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"rssi\">RSSI %s</label></div>\n"
        "        <div class=\"col\" id=\"rssi\">%d</div>\n"
        "       </div>\n"
        "       <div class=\"row mt-4\">\n"
        "        <div class=\"col\">\n"
        "         <form action=\"breathe\" method=\"post\">\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"breathe\">Toggle Breath</button>\n"
        "         </form>\n"
        "        </div>\n"
        "        <div class=\"col\">\n"
        "         <form action=\"wipe\" method=\"post\">\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"wipe\">Wipe WLAN</button>\n"
        "         </form>\n"
        "        </div>\n"
        "        <div class=\"col\">\n"
        "         <form action=\"reset\" method=\"post\">\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"reset\">Reset ESP</button>\n"
        "         </form>\n"
        "        </div>\n"
        "       </div>\n"
        "      </div>\n"
        "     </div>\n"
        "    </div>\n"
        "   </div>\n"
        "   <div class=\"alert alert-primary alert-dismissible fade show\" role=\"alert\">\n"
        "    <strong>Status</strong> %s\n"
        "    <button type=\"button\" class=\"btn-close\" data-bs-dismiss=\"alert\" aria-label=\"Close\">\n"
        "     <span aria-hidden=\"true\"></span>\n"
        "    </button>\n"
        "   </div>\n"
        "   <div class=\"row\"><small>... by <a href=\"https://github.com/joba-1\">Joachim Banzhaf</a>, " __DATE__ " " __TIME__ "</small></div>\n"
        "  </div>\n"
        "  <script src=\"jquery.min.js\"></script>\n"
        "  <script src=\"bootstrap.bundle.min.js\"></script>\n"
        "  <script src=\"slider.js\"></script>\n"
        "  <script>\n"
        "   sliderCallback('slider0', 'sliderValue0', '/r');\n"
        "   sliderCallback('slider1', 'sliderValue1', '/g');\n"
        "   sliderCallback('slider2', 'sliderValue2', '/b');\n"
        "   sliderCallback('slider3', 'sliderValue3', '/w');\n"
        "  </script>\n"
        " </body>\n"
        "</html>\n";
    static char page[sizeof(fmt) + 500] = "";
    static char curr_time[30], influx_time[30];
    time_t now;
    time(&now);
    strftime(curr_time, sizeof(curr_time), "%FT%T", localtime(&now));
    strftime(influx_time, sizeof(influx_time), "%FT%T", localtime(&post_time));
    if( !*web_msg && (influx_status < 200 || influx_status >= 300 ) ) {
        snprintf(web_msg, sizeof(web_msg), "WARNING: %s",
            (influx_status < 200 || influx_status >= 300) ? "Database" : "");
    }
    snprintf(page, sizeof(page), fmt, get_value(LED_R), get_value(LED_R), 
        get_value(LED_G), get_value(LED_G), get_value(LED_B), get_value(LED_B), 
        get_value(LED_W), get_value(LED_W), start_time, curr_time, 
        influx_time, influx_status, lastBssid, lastRssi, web_msg);
    *web_msg = '\0';
    return page;
}

// Define web pages for update, reset or for event infos
void setup_webserver() {
    // css and js files
    // web_server.serveStatic("/bootstrap.min.css", fileSys, "/bootstrap.min.css").setCacheControl("max-age=600");
    web_server.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/bootstrap.min.css", "text/css");
    });
    // web_server.serveStatic("/bootstrap.bundle.min.js", fileSys, "/bootstrap.bundle.min.js").setCacheControl("max-age=600");
    web_server.on("/bootstrap.bundle.min.js", HTTP_GET, [](AsyncWebServerRequest *request){
       request->send(SPIFFS, "/bootstrap.bundle.min.js", "application/javascript");
    });
    // web_server.serveStatic("/jquery.min.js", fileSys, "/jquery.min.js").setCacheControl("max-age=600");
    web_server.on("/jquery.min.js", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/jquery.min.js", "application/javascript");
    });
    // web_server.serveStatic("/slider.js", fileSys, "/slider.js").setCacheControl("max-age=600");
    web_server.on("/slider.js", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/slider.js", "application/javascript");
    });

    // change slider value
    web_server.on("/change", HTTP_POST, [](AsyncWebServerRequest *request) {
        uint16_t prio = LOG_INFO;

        String arg = request->arg("button");
        if (!arg.isEmpty()) {
            if (arg.equals("button-1")) {
                snprintf(web_msg, sizeof(web_msg), "Button '%s' pressed: %s", arg.c_str(), app_status(true) ? "ON" : "OFF");
                slog(web_msg, prio);
            }
            request->redirect("/");  
        }
        else {
            for( int i = LED_START; i < LED_COUNT; i++ ) {
                led_t led = static_cast<led_t>(i);
                arg = request->arg(get_slider(i));
                if (!arg.isEmpty()) {
                    int value = arg.toInt();
                    app_value(led, value);
                    // TODO: comment this message for speed
                    // snprintf(web_msg, sizeof(web_msg), "Slider %d value now %d. Duty is %d", i, value, get_duty(led));
                    // slog(web_msg, prio);
                }
            }
            // snprintf(web_msg, sizeof(web_msg), "change: heap %d", ESP.getFreeHeap());
            // slog(web_msg);
            request->send(204, "text/html", "");  // much smoother slider experience than redirect()
        }
    });

    // change red slider value
    web_server.on("/r", HTTP_POST, [](AsyncWebServerRequest *request) {
        led_t led = LED_R;
        String arg = request->arg(get_slider(led));
        if (!arg.isEmpty()) {
            int value = arg.toInt();
            app_value(led, value);
            // TODO: comment this message for speed
            // snprintf(web_msg, sizeof(web_msg), "Slider r value now %d. Duty is %d", value, get_duty(led));
            // slog(web_msg, LOG_INFO);
        }
        request->send(204, "text/html", "");  // much smoother slider experience than redirect()
    });

    // change green slider value
    web_server.on("/g", HTTP_POST, [](AsyncWebServerRequest *request) {
        led_t led = LED_G;
        String arg = request->arg(get_slider(led));
        if (!arg.isEmpty()) {
            int value = arg.toInt();
            app_value(led, value);
            // TODO: comment this message for speed
            // snprintf(web_msg, sizeof(web_msg), "Slider g value now %d. Duty is %d", value, get_duty(led));
            // slog(web_msg, LOG_INFO);
        }
        request->send(204, "text/html", "");  // much smoother slider experience than redirect()
    });

    // change blue slider value
    web_server.on("/b", HTTP_POST, [](AsyncWebServerRequest *request) {
        led_t led = LED_B;
        String arg = request->arg(get_slider(led));
        if (!arg.isEmpty()) {
            int value = arg.toInt();
            app_value(led, value);
            // TODO: comment this message for speed
            // snprintf(web_msg, sizeof(web_msg), "Slider b value now %d. Duty is %d", value, get_duty(led));
            // slog(web_msg, LOG_INFO);
        }
        request->send(204, "text/html", "");  // much smoother slider experience than redirect()
    });

    // change white slider value
    web_server.on("/w", HTTP_POST, [](AsyncWebServerRequest *request) {
        led_t led = LED_W;
        String arg = request->arg(get_slider(led));
        if (!arg.isEmpty()) {
            int value = arg.toInt();
            app_value(led, value);
            // TODO: comment this message for speed
            // snprintf(web_msg, sizeof(web_msg), "Slider w value now %d. Duty is %d", value, get_duty(led));
            // slog(web_msg, LOG_INFO);
        }
        request->send(204, "text/html", "");  // much smoother slider experience than redirect()
    });

    web_server.on("/json/Wifi", [](AsyncWebServerRequest *request) {
        json_Wifi(msg, sizeof(msg), lastBssid, lastRssi);
        request->send(200, "application/json", msg);
    });

    web_server.on("/json/Pwm", [](AsyncWebServerRequest *request) {
        json_Pwm(msg, sizeof(msg), get_power());
        request->send(200, "application/json", msg);
    });

    // Call this page to reset the ESP
    web_server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        slog("RESET ESP32", LOG_NOTICE);
        request->send(200, "text/html",
                        "<html>\n"
                        " <head>\n"
                        "  <title>" PROGNAME " v" VERSION "</title>\n"
                        "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
                        " </head>\n"
                        " <body>Resetting...</body>\n"
                        "</html>\n");
        delay(200);
        ESP.restart();
    });

    // Index page
    web_server.on("/", [](AsyncWebServerRequest *request) { 
        request->send(200, "text/html", main_page());
    });

    // Toggle breathing status led if you dont like it or ota does not work
    web_server.on("/breathe", HTTP_ANY, [](AsyncWebServerRequest *request) {
        enabledBreathing = !enabledBreathing; 
        snprintf(web_msg, sizeof(web_msg), "%s", enabledBreathing ? "breathing enabled" : "breathing disabled");
        request->send(204, "text/html", "");
    });

    web_server.on("/wipe", HTTP_POST, [](AsyncWebServerRequest *request) {
        WiFiManager wm;
        wm.resetSettings();
        slog("Wipe WLAN config and reset ESP32", LOG_NOTICE);
        request->send(200, "text/html",
                        "<html>\n"
                        " <head>\n"
                        "  <title>" PROGNAME " v" VERSION "</title>\n"
                        "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
                        " </head>\n"
                        " <body>Wipe WLAN config. Connect to AP '" HOSTNAME "' and connect to http://192.168.4.1</body>\n"
                        "</html>\n");
        delay(200);
        ESP.restart();
    });

    // Firmware Update Form
    web_server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        static const char page[] =
            "<!doctype html>\n"
            "<html lang=\"en\">\n"
            " <head>\n"
            "  <meta charset=\"utf-8\">\n"
            "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "  <link href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQAgMAAABinRfyAAAADFBMVEUqYbutnpTMuq/70SQgIef5AAAAVUlEQVQIHWOAAPkvDAyM3+Y7MLA7NV5g4GVqKGCQYWowYTBhapBhMGB04GE4/0X+M8Pxi+6XGS67XzzO8FH+iz/Dl/q/8gx/2S/UM/y/wP6f4T8QAAB3Bx3jhPJqfQAAAABJRU5ErkJggg==\" rel=\"icon\" type=\"image/x-icon\" />\n"
            "  <link href=\"bootstrap.min.css\" rel=\"stylesheet\">\n"
            "  <title>" PROGNAME " v" VERSION "</title>\n"
            " </head>\n"
            " <body>\n"
            "  <div class=\"container\">\n"
            "    <div class=\"row\">\n"
            "     <div class=\"col-12\">\n"
            "      <h1>" PROGNAME " v" VERSION "</h1>\n"
            "     </div>\n"
            "    </div>\n"
            "    <form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">\n"
            "     <div class=\"row\">\n"
            "      <div class=\"col\">\n"
            "       <input type=\"file\" name=\"update\">\n"
            "      </div>\n"
            "      <div class=\"col\">\n"
            "       <input type=\"submit\" value=\"Update\">\n"
            "      </div>\n"
            "     </div>\n"
            "    </form>\n"
            "  </div>\n"
            "  <script src=\"bootstrap.bundle.min.js\"></script>\n"
            " </body>\n"
            "</html>\n";
 
        request->send(200, "text/html", page);
    });

    web_server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
        shouldReboot = !Update.hasError();
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "OK" : "FAIL");
        response->addHeader("Connection", "close");
        request->send(response);
    },[](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index){
            Serial.printf("Update Start: %s\n", filename.c_str());
            if(!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)){
                Update.printError(Serial);
                #ifdef ESP32
                snprintf(web_msg, sizeof(web_msg), "%s", Update.errorString());
                #endif
                request->redirect("/");  
            }
        }
        if(!Update.hasError()){
            if(Update.write(data, len) != len){
                Update.printError(Serial);
                #ifdef ESP32
                snprintf(web_msg, sizeof(web_msg), "%s", Update.errorString());
                #endif
                request->redirect("/");  
            }
        }
        if(final){
            if(Update.end(true)){
                Serial.printf("Update Success: %uB\n", index+len);
                snprintf(web_msg, sizeof(web_msg), "Update Success: %u Bytes", index+len);
            } else {
                Update.printError(Serial);
                #ifdef ESP32
                snprintf(web_msg, sizeof(web_msg), "%s", Update.errorString());
                #endif
            }
            request->redirect("/"); 
        }
    });

    // Catch all page
    web_server.onNotFound( [](AsyncWebServerRequest *request) { 
        snprintf(web_msg, sizeof(web_msg), "%s", "<h2>page not found</h2>\n");
        request->send(404, "text/html", main_page()); 
    });

    web_server.begin();

    MDNS.addService("http", "tcp", WEBSERVER_PORT);

    snprintf(msg, sizeof(msg), "Serving HTTP on port %d", WEBSERVER_PORT);
    slog(msg, LOG_NOTICE);
}


// handle on key press
// pin is pulled up if released and pulled down if pressed
// return true if &status has changed
bool handle_button( bool &status ) {
    static uint32_t prevTime = 0;
    static uint32_t debounceStatus = 1;
    static bool pressed = false;

    uint32_t now = millis();
    if( now - prevTime > 2 ) {  // debounce check every 2 ms, decision after 2ms/bit * 32bit = 64ms
        prevTime = now;

        // shift bits left, set lowest bit if button pressed
        debounceStatus = (debounceStatus << 1) | ((digitalRead(BUTTON_PIN) == LOW) ? 1 : 0);

        if( debounceStatus == 0 && pressed ) {
            pressed = status = false;
            return true;
        }
        else if( debounceStatus == 0xffffffff && !pressed ) {
            pressed = status = true;
            return true;
        }
    }
    return false;
}


// check ntp status
// return true if time is valid
bool check_ntptime() {
    static bool have_time = false;

    #if defined(ESP32)
        bool valid_time = time(0) > 1582230020;
    #else
        ntp.update();
        bool valid_time = ntp.isTimeSet();
    #endif

    if (!have_time && valid_time) {
        have_time = true;
        time_t now = time(NULL);
        strftime(start_time, sizeof(start_time), "%FT%T", localtime(&now));
        snprintf(msg, sizeof(msg), "Got valid time at %s", start_time);
        slog(msg, LOG_NOTICE);
        if (mqtt.connected()) {
            publish(MQTT_TOPIC "/status/StartTime", start_time);
        }
    }

    return have_time;
}


// Reset reason can be quite useful...
// Messages from arduino core example
void print_reset_reason(int core) {
#ifdef ESP32
  switch (rtc_get_reset_reason(core)) {
    case 1  : slog("Vbat power on reset");break;
    case 3  : slog("Software reset digital core");break;
    #if !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32C3) 
    case 4  : slog("Legacy watch dog reset digital core");break;
    #endif
    case 5  : slog("Deep Sleep reset digital core");break;
    #if !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32C3) 
    case 6  : slog("Reset by SLC module, reset digital core");break;
    #endif
    case 7  : slog("Timer Group0 Watch dog reset digital core");break;
    case 8  : slog("Timer Group1 Watch dog reset digital core");break;
    case 9  : slog("RTC Watch dog Reset digital core");break;
    case 10 : slog("Instrusion tested to reset CPU");break;
    case 11 : slog("Time Group reset CPU");break;
    case 12 : slog("Software reset CPU");break;
    case 13 : slog("RTC Watch dog Reset CPU");break;
    #if !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32C3) 
    case 14 : slog("for APP CPU, reseted by PRO CPU");break;
    #endif
    case 15 : slog("Reset when the vdd voltage is not stable");break;
    case 16 : slog("RTC Watch dog reset digital core and rtc module");break;
    default : slog("Reset reason unknown");
  }
#endif
}


// Called on incoming mqtt messages
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

    typedef struct cmd { const char *name; void (*action)(char *args); } cmd_t;
    
    static cmd_t cmds[] = { 
        { "color",  []( char *args ){
            char *end;
            int r = (int)strtol(args, &end, 0);
            if( args != end && r >= 0 && r <= 1000 ) {
                args = end;
                int g = (int)strtol(args, &end, 0);
                if( args != end && g >= 0 && g <= 1000 ) {
                    args = end;
                    int b = (int)strtol(args, &end, 0);
                    if( args != end && b >= 0 && b <= 1000 ) {
                        args = end;
                        int w = (int)strtol(args, &end, 0);
                        if( args != end && w >= 0 && w <= 1000 ) {
                            app_value(LED_R, r); 
                            app_value(LED_G, g); 
                            app_value(LED_B, b); 
                            app_value(LED_W, w); 
                        }
                    }
                }
            } } },
        { "red",    []( char *args ){ app_value(LED_R, max(min(atoi(args), 1000), 0)); } },
        { "green",  []( char *args ){ app_value(LED_G, max(min(atoi(args), 1000), 0)); } },
        { "blue",   []( char *args ){ app_value(LED_B, max(min(atoi(args), 1000), 0)); } },
        { "white",  []( char *args ){ app_value(LED_W, max(min(atoi(args), 1000), 0)); } },
        { "toggle", []( char *args ){ app_status(true); } },
        { "on",     []( char *args ){ if (!get_power()) app_status(true); } },
        { "off",    []( char *args ){ if (get_power()) app_status(true); } }
    };

    if( length > 0 ) {
        String data(payload, length);

        if (strcasecmp(MQTT_TOPIC "/cmd", topic) == 0) {
            for (auto &cmd: cmds) {
                size_t len = strlen(cmd.name);
                if (length >= len && strncasecmp(cmd.name, data.c_str(), len) == 0) {
                    snprintf(msg, sizeof(msg), "Execute mqtt command '%s' with '%.*s'", cmd.name, length-len, &data[len]);
                    slog(msg, LOG_INFO);
                    (*cmd.action)(&data[len]);
                    return;
                }
            }
        }
    }

    snprintf(msg, sizeof(msg), "Ignore mqtt %s: '%.*s'", topic, length, (char *)payload);
    slog(msg, LOG_WARNING);
}


bool handle_mqtt( bool time_valid ) {
    static const int32_t interval = 5000;  // if disconnected try reconnect this often in ms
    static uint32_t prev = -interval;      // first connect attempt without delay

    if (mqtt.connected()) {
        mqtt.loop();
        return true;
    }

    uint32_t now = millis();
    if (now - prev > interval) {
        prev = now;

        if (mqtt.connect(HOSTNAME, MQTT_TOPIC "/status/LWT", 0, true, "Offline")
            && mqtt.publish(MQTT_TOPIC "/status/LWT", "Online", true)
            && mqtt.publish(MQTT_TOPIC "/status/Hostname", HOSTNAME)
            && mqtt.publish(MQTT_TOPIC "/status/DBServer", INFLUX_SERVER)
            && mqtt.publish(MQTT_TOPIC "/status/DBPort", itoa(INFLUX_PORT, msg, 10))
            && mqtt.publish(MQTT_TOPIC "/status/DBName", INFLUX_DB)
            && mqtt.publish(MQTT_TOPIC "/status/Version", VERSION)
            && (!time_valid || mqtt.publish(MQTT_TOPIC "/status/StartTime", start_time))
            && mqtt.subscribe(MQTT_TOPIC "/cmd")) {
            snprintf(msg, sizeof(msg), "Connected to MQTT broker %s:%d using topic %s", MQTT_SERVER, MQTT_PORT, MQTT_TOPIC);
            slog(msg, LOG_NOTICE);
            return true;
        }

        int error = mqtt.state();
        mqtt.disconnect();
        snprintf(msg, sizeof(msg), "Connect to MQTT broker %s:%d failed with code %d", MQTT_SERVER, MQTT_PORT, error);
        slog(msg, LOG_ERR);
    }

    return false;
}


void handle_reboot() {
    static const int32_t reboot_delay = 1000;  // if should_reboot wait this long in ms
    static uint32_t start = 0;                 // first detected should_reboot

    if (shouldReboot) {
        uint32_t now = millis();
        if (!start) {
            start = now;
        }
        else {
            if (now - start > reboot_delay) {
                ESP.restart();
            }
        }
    }
}


// Startup
void setup() {
    setup_app();

    pinMode(HEALTH_LED_PIN, OUTPUT);
    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_INVERTED ? LOW : HIGH);

    pinMode(BUTTON_PIN, INPUT_PULLUP);  // to toggle load status

    Serial.begin(BAUDRATE);
    // #if defined(CONFIG_IDF_TARGET_ESP32S3)
    //     while(!Serial);
    // #endif
    // uint32_t prev = 0;
    // while( digitalRead(BUTTON_PIN) != LOW ) {
    //     uint32_t now = millis();
    //     if( now - prev > 1000 ) {
    //         Serial.printf("waiting %d\n", now);
    //         prev = now;
    //     }
    // }

    Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

    Serial.print("Pins for RGBW are");
    for( int i = LED_START; i < LED_COUNT; i++ ) {
        led_t led = static_cast<led_t>(i);
        Serial.printf(" %u", get_pin(led));
    }
    Serial.println();

    String host(HOSTNAME);
    host.toLowerCase();
    WiFi.hostname(host.c_str());
    WiFi.mode(WIFI_STA);

    // Syslog setup
    syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
    syslog.deviceHostname(hostname());
    syslog.appName("Joba1");
    syslog.defaultPriority(LOG_KERN);

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_INVERTED ? HIGH : LOW);

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    if (!wm.autoConnect(hostname(), hostname())) {
        Serial.println("Failed to connect WLAN, about to reset");
        for (int i = 0; i < 20; i++) {
            digitalWrite(HEALTH_LED_PIN, (i & 1) ? HIGH : LOW);
            delay(100);
        }
        ESP.restart();
        while (true)
            ;
    }

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_INVERTED ? LOW : HIGH);

    char msg[80];
    snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
        WiFi.localIP().toString().c_str());
    slog(msg, LOG_NOTICE);

    #if defined(ESP8266)
        ntp.begin();
    #else
        configTime(3600, 3600, NTP_SERVER);  // MEZ/MESZ
    #endif

    MDNS.begin(hostname());

    fileSys.begin();

    setup_webserver();

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);

#if defined(ESP32)
    print_reset_reason(0);
    print_reset_reason(1);
#endif

    health_led.limits(1, health_led.range() / 2);  // only barely off to 50% brightness
    health_led.begin();

    setup_app(true);  // TODO done twice since sometimes light stays off until toggled twice

    slog("Setup done", LOG_NOTICE);
}


// Main loop
void loop() {
    bool button_pressed;
    bool health = true;

    health &= handle_app();
    
    bool have_time = check_ntptime();

    health &= handle_mqtt(have_time);
    health &= handle_wifi();

    if (handle_button(button_pressed)) {
        app_status(button_pressed);
    }

    health &= (influx_status >= 200 && influx_status < 300);

    report_pwm(get_power());

    if (have_time && enabledBreathing) {
        health_led.interval(health ? health_ok_interval : health_err_interval);
        health_led.handle();
    }

    handle_reboot();
}