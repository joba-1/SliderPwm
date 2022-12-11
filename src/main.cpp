/*
Demo HTML Slider with server feedback on value changes

Influx DB can be created with command influx -execute "create database PROGNAME"
Monitors GPIO pin pulled to ground as key press
Use builtin led to represent health status
*/

/*
Generic ESP8266/ESP32 app framework

TODO Enable framework features with defines:

USE_WIFIMANAGER
USE_NTP
USE_SYSLOG
USE_MQTT
USE_WEBSERVER
USE_OTA
USE_INFLUX
USE_HEALTHLED

*/

void setup_app();
void handle_app();

#include <Arduino.h>

#ifdef USE_SPIFFS
#include <SPIFFS.h>
FS &Fs = SPIFFS; 
#endif
#ifdef USE_LITTLEFS
#include <LittleFS.h>
FS &Fs = LittleFS; 
#endif

int slider_value = 50;


// Config for ESP8266 or ESP32
#if defined(ESP8266)
    #define HEALTH_LED_ON LOW
    #define HEALTH_LED_OFF HIGH
    #define HEALTH_LED_PIN LED_BUILTIN
    #define BUTTON_PIN 0

    // Web Updater
    #include <ESP8266HTTPUpdateServer.h>
    #include <ESP8266WebServer.h>
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
#elif defined(ESP32)
    #define HEALTH_LED_ON HIGH
    #define HEALTH_LED_OFF LOW
    #ifdef LED_BUILTIN
        #define HEALTH_LED_PIN LED_BUILTIN
    #else
        #define HEALTH_LED_PIN 16
    #endif
    #define HEALTH_PWM_CH 0
    #define BUTTON_PIN 0

    // Web Updater
    #include <HTTPUpdateServer.h>
    #include <WebServer.h>
    #include <WiFi.h>
    #include <ESPmDNS.h>
    #include <WiFiClient.h>

    // Post to InfluxDB
    #include <HTTPClient.h>
    
    // Time sync
    #include <time.h>

    // Reset reason
    #include "rom/rtc.h"
#else
    #error "No ESP8266 or ESP32, define your rs485 stream, pins and includes here!"
#endif

// Infrastructure
// #include <LittleFS.h>
#include <EEPROM.h>
#include <Syslog.h>
#include <WiFiManager.h>

// Web status page and OTA updater
#define WEBSERVER_PORT 80

WebServer web_server(WEBSERVER_PORT);
HTTPUpdateServer esp_updater;

// Post to InfluxDB
int influx_status = 0;
time_t post_time = 0;

// publish to mqtt broker
#include <PubSubClient.h>

WiFiClient wifiMqtt;
PubSubClient mqtt(wifiMqtt);

// Breathing status LED
const uint32_t ok_interval = 5000;
const uint32_t err_interval = 1000;

uint32_t breathe_interval = ok_interval; // ms for one led breathe cycle
bool enabledBreathing = true;  // global flag to switch breathing animation on or off

#ifndef PWMRANGE
#define PWMRANGE 1023
#define PWMBITS 10
#endif

// Syslog
WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);
char msg[512];  // one buffer for all syslog and json messages
char start_time[30];


void setup_fs() {
#ifdef USE_SPIFFS
    if (!SPIFFS.begin(true))
#else
    if (!LittleFS.begin(true))
#endif
    {
        Serial.println("LittleFS Mount Failed");
    }
    else {
        File file = Fs.open("/hello.txt");
        if (!file) {
            Serial.println("Failed to open file for reading");
        }
        else {
            if (file.isDirectory()) {
                Serial.println("Cannot open directory");
            }
            else {
                while (file.available()) {
                    Serial.write(file.read());
                }
            }
            file.close();
        }
    }
    Serial.println("Setup FS done");
}


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

    int len = snprintf(json, maxlen, jsonFmt, WiFi.getHostname(), bssid, WiFi.localIP().toString().c_str(), rssi);

    return len < maxlen;
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
    static const uint32_t interval = 10000;
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

        snprintf(msg, sizeof(msg), lineFmt, WiFi.getHostname(), lastBssid, WiFi.localIP().toString().c_str(), lastRssi);
        postInflux(msg);

        reportedRssi = lastRssi;
        prev = now;
    }
}


// check and report RSSI and BSSID changes
void handle_wifi() {
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
                    digitalWrite(HEALTH_LED_PIN, (i & 1) ? HEALTH_LED_ON : HEALTH_LED_OFF);
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
}


char web_msg[80] = "";  // main web page displays and then clears this
bool changeIp = false;  // if true, ip changes after display of root url
IPAddress ip;           // the ip to change to (use DHCP if 0)

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
        "  <form action=\"/change\" method=\"post\" enctype=\"multipart/form-data\" id=\"form\">\n"
        "   <div class=\"container\">\n"
        "    <div class=\"row\">\n"
        "     <div class=\"col-12\">\n"
        "      <h1>" PROGNAME " v" VERSION "</h1>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row\">\n"
        "     <div class=\"col-9\">\n"
        "      <input style=\"width:100%%\" id=\"slider\" type=\"range\" min=\"1\" max=\"100\" step=\"1\" value=\"%d\" name=\"slider\">\n"
        "     </div>\n"
        "     <div class=\"col-3\" >\n"
        "      <div id=\"sliderAmount\">%d</div>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row\">\n"
        "     <div class=\"col-9\">\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-1\">ONE</button>\n"
        "     </div>\n"
        "     <div class=\"col-3\">\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-2\">TWO</button>\n"
        "     </div>\n"
        "    </div>\n"
        "   </div>\n"
        "  </form>\n"

        "  <p><strong>%s</strong></p>\n"
        "  <p><table>\n"
        "   <tr><td>Wifi</td><td><a href=\"/json/Wifi\">JSON</a></td></tr>\n"
        "   <tr><td></td></tr>\n"
        "   <tr><td>Post firmware image to</td><td><a href=\"/update\">/update</a></td></tr>\n"
        "   <tr><td>Last start time</td><td>%s</td></tr>\n"
        "   <tr><td>Last web update</td><td>%s</td></tr>\n"
        "   <tr><td>Last influx update</td><td>%s</td></tr>\n"
        "   <tr><td>Influx status</td><td>%d</td></tr>\n"
        "   <tr><td>RSSI %s</td><td>%d</td></tr>\n"
        "   <tr><form action=\"ip\" method=\"post\">\n"
        "    <td>IP <input type=\"text\" id=\"ip\" name=\"ip\" value=\"%s\" /></td>\n"
        "    <td><input type=\"submit\" name=\"change\" value=\"Change IP\" /></td>\n"
        "   </form></tr>\n"
        "  </table></p>\n"
        "  <p><table><tr>\n"
        "   <td><form action=\"/\" method=\"get\">\n"
        "    <input type=\"submit\" name=\"reload\" value=\"Reload\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"breathe\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"breathe\" value=\"Toggle Breathe\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"reset\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"reset\" value=\"Reset ESP\" />\n"
        "   </form></td>\n"
        "  </tr></table></p>\n"
        "  <p><small>... by <a href=\"https://github.com/joba-1\">Joachim Banzhaf</a>, " __DATE__ " " __TIME__ "</small></p>\n"

        "  <script src=\"jquery.min.js\"></script>\n"
        "  <script src=\"bootstrap.bundle.min.js\"></script>\n"
        " </body>\n"
        " <script>\n"
        "  var slider = document.getElementById('slider');\n"
        "  var sliderDiv = document.getElementById('sliderAmount');\n\n"
        "  slider.oninput = function() {\n"
        "   $.post({\n"
        "    url: '/change',\n"
        "    data: $('form').serialize(),\n"
//      "    success: function(response) { console.log(response); },\n"
        "    error: function(error) { alert(error); console.log(error); }\n"
        "   });\n"
        "   sliderDiv.innerHTML = this.value;\n"
        "  }\n\n"
        "  slider.onchange = function() {\n"
        "  }\n"
        " </script>\n"
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
    snprintf(page, sizeof(page), fmt, slider_value, slider_value, web_msg, start_time, curr_time, 
        influx_time, influx_status, lastBssid, lastRssi, WiFi.localIP().toString().c_str());
    *web_msg = '\0';
    return page;
}


// Read and write ip config
bool ip_config(uint32_t *ip, int num_ip, bool write = false) {
    const uint32_t magic = 0xdeadbeef;
    size_t got_bytes = 0;
    size_t want_bytes = sizeof(*ip) * num_ip;
    if (*ip != 0xffffffff && EEPROM.begin(want_bytes + sizeof(uint32_t))) {
        if (write) {
            got_bytes = EEPROM.writeBytes(0, ip, want_bytes);
            EEPROM.writeULong(want_bytes, magic);
            EEPROM.commit();

        }
        else {
            got_bytes = EEPROM.readBytes(0, ip, want_bytes);
            if (EEPROM.readULong(want_bytes) != magic) {
                got_bytes = 0;
            }
        }
        EEPROM.end();
    }
    return got_bytes == want_bytes && *ip != 0xffffffff;
}

// bool ip_config(uint32_t *ip, int num_ip, bool write = false) {
//     size_t got_bytes = 0;
//     size_t want_bytes = sizeof(*ip) * num_ip;
//     if (LittleFS.begin(write)) {
//         File f = LittleFS.open("ip.cfg", write ? "w" : "r", write);
//         if (f) {
//             got_bytes = write ? f.write((uint8_t *)ip, want_bytes) : f.read((uint8_t *)ip, want_bytes);
//             f.close();
//         }
//         LittleFS.end();
//     }
//     return got_bytes == want_bytes;
// }


// Define web pages for update, reset or for event infos
void setup_webserver() {
    // css and js files
    web_server.serveStatic("/bootstrap.min.css", Fs, "/bootstrap.min.css");
    web_server.serveStatic("/bootstrap.bundle.min.js", Fs, "/bootstrap.bundle.min.js");
    web_server.serveStatic("/jquery.min.js", Fs, "/jquery.min.js");

    // change slider value
    web_server.on("/change", HTTP_POST, []() {
        uint16_t prio = LOG_INFO;

        String arg = web_server.arg("slider");
        if (!arg.isEmpty()) {
            slider_value = arg.toInt();
            snprintf(web_msg, sizeof(web_msg), "Slider value now '%d'", slider_value);
            slog(web_msg, prio);
        }

        arg = web_server.arg("button");
        if (!arg.isEmpty()) {
            if (arg.equals("button-1")) {
                snprintf(web_msg, sizeof(web_msg), "Button '%s' pressed", arg.c_str());
                slog(web_msg, prio);
            }
            else if (arg.equals("button-2")) {
                snprintf(web_msg, sizeof(web_msg), "Button '%s' pressed", arg.c_str());
                slog(web_msg, prio);
            }
        }

        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    web_server.on("/json/Wifi", []() {
        json_Wifi(msg, sizeof(msg), lastBssid, lastRssi);
        web_server.send(200, "application/json", msg);
    });

    // Change host part of ip, if ip&subnet == 0 -> dynamic
    web_server.on("/ip", HTTP_POST, []() {
        String strIp = web_server.arg("ip");
        uint16_t prio = LOG_ERR;
        if (ip.fromString(strIp)) {
            uint32_t newIp = (uint32_t)ip;
            uint32_t oldIp = (uint32_t)WiFi.localIP();
            uint32_t subMask = (uint32_t)WiFi.subnetMask();
            if (newIp) {
                // make sure new ip is in the same subnet
                uint32_t netIp = oldIp & (uint32_t)WiFi.subnetMask();
                newIp = (newIp & ~subMask) | netIp;
            }
            if (newIp != oldIp) {
                // don't accidentially use broadcast address
                if ((newIp & ~subMask) != ~subMask) {
                    changeIp = true;
                    ip = newIp;
                    snprintf(web_msg, sizeof(web_msg), "Change IP to '%s'", ip.toString().c_str());
                    prio = LOG_WARNING;
                }
                else {
                    snprintf(web_msg, sizeof(web_msg), "Broadcast address '%s' not possible", IPAddress(newIp).toString().c_str());
                }
            }
            else {
                snprintf(web_msg, sizeof(web_msg), "No IP change for '%s'", strIp.c_str());
                prio = LOG_WARNING;
            }
        }
        else {
            snprintf(web_msg, sizeof(web_msg), "Invalid ip '%s'", strIp.c_str());
        }
        slog(web_msg, prio);

        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    // Call this page to reset the ESP
    web_server.on("/reset", HTTP_POST, []() {
        slog("RESET ESP32", LOG_NOTICE);
        web_server.send(200, "text/html",
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
    web_server.on("/", []() { 
        web_server.send(200, "text/html", main_page());

        if (changeIp) {
            delay(200);  // let the send finish
            bool ok = false;
            if (ip != INADDR_NONE) {  // static
                ok = WiFi.config(ip, WiFi.gatewayIP(), WiFi.subnetMask(), WiFi.dnsIP(0), WiFi.dnsIP(1));
            }
            else {  // dynamic
                // How to decide if gw and dns was dhcp provided or static?
                // Assuming it is fully dynamic with dhcp, so set to 0, not old values
                ok = WiFi.config(0U, 0U, 0U);
            }

            snprintf(msg, sizeof(msg), "New IP config ip:%s, gw:%s, sn:%s, d0:%s, d1:%s", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), 
                WiFi.subnetMask().toString().c_str(), WiFi.dnsIP(0).toString().c_str(), WiFi.dnsIP(1).toString().c_str());
            slog(msg, LOG_NOTICE);
            if (ok) {
                uint32_t ip[5] = { (uint32_t)WiFi.localIP(), (uint32_t)WiFi.gatewayIP(), (uint32_t)WiFi.subnetMask(), (uint32_t)WiFi.dnsIP(0), (uint32_t)WiFi.dnsIP(1) };
                if (ip_config(ip, 5, true)) {
                    slog("Wrote changed IP config");
                }
                else {
                    slog("Write changed IP config failed");
                }
            }
            changeIp = false;
        }
    });

    // Toggle breathing status led if you dont like it or ota does not work
    web_server.on("/breathe", HTTP_POST, []() {
        enabledBreathing = !enabledBreathing; 
        snprintf(web_msg, sizeof(web_msg), "%s", enabledBreathing ? "breathing enabled" : "breathing disabled");
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    web_server.on("/breathe", HTTP_GET, []() {
        snprintf(web_msg, sizeof(web_msg), "%s", enabledBreathing ? "breathing enabled" : "breathing disabled");
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    // Catch all page
    web_server.onNotFound( []() { 
        snprintf(web_msg, sizeof(web_msg), "%s", "<h2>page not found</h2>\n");
        web_server.send(404, "text/html", main_page()); 
    });

    web_server.begin();

    MDNS.addService("http", "tcp", WEBSERVER_PORT);

    snprintf(msg, sizeof(msg), "Serving HTTP on port %d", WEBSERVER_PORT);
    slog(msg, LOG_NOTICE);
}


// handle on key press
// pin is pulled up if released and pulled down if pressed
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


// Status led update
void handle_breathe() {
    static uint32_t start = 0;  // start of last breath
    static uint32_t min_duty = 1;  // limit min brightness
    static uint32_t max_duty = PWMRANGE / 2;  // limit max brightness
    static uint32_t prev_duty = 0;

    // map elapsed in breathing intervals
    uint32_t now = millis();
    uint32_t elapsed = now - start;
    if (elapsed > breathe_interval) {
        start = now;
        elapsed -= breathe_interval;
    }

    // map min brightness to max brightness twice in one breathe interval
    uint32_t duty = (max_duty - min_duty) * elapsed * 2 / breathe_interval + min_duty;
    if (duty > max_duty) {
        // second range: breathe out aka get darker
        duty = 2 * max_duty - duty;
    }

    duty = duty * duty / max_duty;  // generally reduce lower brightness levels

    if (duty != prev_duty) {
        // adjust pwm duty cycle
        prev_duty = duty;
        if (HEALTH_LED_ON == LOW) {
            // inverted
            duty = PWMRANGE - duty;
        }
        #if defined(ESP32)
            ledcWrite(HEALTH_PWM_CH, duty);
        #else
            analogWrite(HEALTH_LED_PIN, duty);
        #endif
    }
}


// Reset reason can be quite useful...
// Messages from arduino core example
void print_reset_reason(int core) {
  switch (rtc_get_reset_reason(core)) {
    case 1  : slog("Vbat power on reset");break;
    case 3  : slog("Software reset digital core");break;
    case 4  : slog("Legacy watch dog reset digital core");break;
    case 5  : slog("Deep Sleep reset digital core");break;
    case 6  : slog("Reset by SLC module, reset digital core");break;
    case 7  : slog("Timer Group0 Watch dog reset digital core");break;
    case 8  : slog("Timer Group1 Watch dog reset digital core");break;
    case 9  : slog("RTC Watch dog Reset digital core");break;
    case 10 : slog("Instrusion tested to reset CPU");break;
    case 11 : slog("Time Group reset CPU");break;
    case 12 : slog("Software reset CPU");break;
    case 13 : slog("RTC Watch dog Reset CPU");break;
    case 14 : slog("for APP CPU, reseted by PRO CPU");break;
    case 15 : slog("Reset when the vdd voltage is not stable");break;
    case 16 : slog("RTC Watch dog reset digital core and rtc module");break;
    default : slog("Reset reason unknown");
  }
}


// Called on incoming mqtt messages
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

    typedef struct cmd { const char *name; void (*action)(void); } cmd_t;
    
    static cmd_t cmds[] = { 
        // { "load on", [](){ esmart3.setLoad(true); } },
        // { "load off", [](){ esmart3.setLoad(false); } }
    };

    if (strcasecmp(MQTT_TOPIC "/cmd", topic) == 0) {
        for (auto &cmd: cmds) {
            if (strncasecmp(cmd.name, (char *)payload, length) == 0) {
                snprintf(msg, sizeof(msg), "Execute mqtt command '%s'", cmd.name);
                slog(msg, LOG_INFO);
                (*cmd.action)();
                return;
            }
        }
    }

    snprintf(msg, sizeof(msg), "Ignore mqtt %s: '%.*s'", topic, length, (char *)payload);
    slog(msg, LOG_WARNING);
}


void handle_mqtt( bool time_valid ) {
    static const int32_t interval = 5000;  // if disconnected try reconnect this often in ms
    static uint32_t prev = -interval;      // first connect attempt without delay

    if (mqtt.connected()) {
        mqtt.loop();
    }
    else {
        uint32_t now = millis();
        if (now - prev > interval) {
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
            }
            else {
                int error = mqtt.state();
                mqtt.disconnect();
                snprintf(msg, sizeof(msg), "Connect to MQTT broker %s:%d failed with code %d", MQTT_SERVER, MQTT_PORT, error);
                slog(msg, LOG_ERR);
            }
            prev = now;
        }
    }
 }


// Startup
void setup() {
    WiFi.mode(WIFI_STA);
    String host(HOSTNAME);
    host.toLowerCase();
    WiFi.hostname(host.c_str());

    pinMode(HEALTH_LED_PIN, OUTPUT);
    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);

    Serial.begin(BAUDRATE);
    Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

    // Syslog setup
    syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
    syslog.deviceHostname(WiFi.getHostname());
    syslog.appName("Joba1");
    syslog.defaultPriority(LOG_KERN);

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_OFF);

    WiFiManager wm;
    // wm.resetSettings();
    wm.setConfigPortalTimeout(180);
    uint32_t ip[5] = {0};
    if (ip_config(ip, 5) && ip[0]) {
        wm.setSTAStaticIPConfig(IPAddress(ip[0]), IPAddress(ip[1]), IPAddress(ip[2]), IPAddress(ip[3]));
    }
    if (!wm.autoConnect(WiFi.getHostname(), WiFi.getHostname())) {
        Serial.println("Failed to connect WLAN, about to reset");
        for (int i = 0; i < 20; i++) {
            digitalWrite(HEALTH_LED_PIN, (i & 1) ? HEALTH_LED_ON : HEALTH_LED_OFF);
            delay(100);
        }
        ESP.restart();
        while (true)
            ;
    }
    uint32_t ip2[5] = { (uint32_t)WiFi.localIP(), (uint32_t)WiFi.gatewayIP(), (uint32_t)WiFi.subnetMask(), (uint32_t)WiFi.dnsIP(0), (uint32_t)WiFi.dnsIP(1) };
    if (memcmp(ip, ip2, sizeof(ip))) {
        if (ip_config(ip2, 5, true)) {
            slog("Wrote IP config");
        }
        else {
            slog("Write IP config failed");
        }
    }

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
        WiFi.localIP().toString().c_str());
    slog(msg, LOG_NOTICE);

    #if defined(ESP8266)
        ntp.begin();
    #else
        configTime(3600, 3600, NTP_SERVER);  // MEZ/MESZ
    #endif

    MDNS.begin(WiFi.getHostname());

    setup_fs();
    esp_updater.setup(&web_server);
    setup_webserver();

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);

#if defined(ESP8266)
    analogWriteRange(PWMRANGE);  // for health led breathing steps
#elif defined(ESP32)
    print_reset_reason(0);
    print_reset_reason(1);  // assume 2nd core (should I ask?)

    ledcAttachPin(HEALTH_LED_PIN, HEALTH_PWM_CH);
    ledcSetup(HEALTH_PWM_CH, 1000, PWMBITS);
#else
    analogWriteRange(PWMRANGE);  // for health led breathing steps
#endif

    pinMode(BUTTON_PIN, INPUT_PULLUP);  // to toggle load status

    setup_app();

    slog("Setup done", LOG_NOTICE);
}


// Main loop
void loop() {
    bool button_pressed;

    handle_app();
    
    bool have_time = check_ntptime();
    
    if (have_time 
     && enabledBreathing) {
        breathe_interval = (influx_status < 200 || influx_status >= 300) ? err_interval : ok_interval;
        handle_breathe();  // health indicator
    }

    if (handle_button(button_pressed)) {
        // do something
    }
    web_server.handleClient();
    handle_mqtt(have_time);
    handle_wifi();
}