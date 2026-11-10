// WiFi, firmware updates over the air (ArduinoOTA and upload page), web interface (web.cpp)

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <time.h>
#include "wlan.h"
#include "pins.h"
#include "link.h"
#include "web.h"

wifi_cfg_t wifi_cfg = {};

static Preferences prefs;
// Browsers (Chrome on Android in particular) keep spare connections open that send nothing at first.
// WebServer serves one connection at a time and would wait up to 5 s for a request on such a socket while
// the other requests queue up. A connection without a request is parked as soon as another one waits,
// and served once its request arrives; parked connections are closed after HTTP_PARK_MS.
#define HTTP_PARK_MAX     4
#define HTTP_PARK_MS   5000

class FastWebServer : public WebServer {
public:
    using WebServer::WebServer;

    void handleClient (void) override
    {
        uint32_t now = millis();

        if(_currentStatus == HC_WAIT_READ && _currentClient && !_currentClient.available() &&
            (_server.hasClient() || parked_ready())) {
            int i = free_slot();
            if(i >= 0) {
                parked[i] = _currentClient;
                parked_at[i] = _statusChange;
            } else
                _currentClient.stop();
            _currentClient = NetworkClient();
            _currentStatus = HC_NONE;
        }

        if(_currentStatus == HC_NONE) {
            for(int i = 0; i < HTTP_PARK_MAX; i++) {
                if(!parked[i])
                    continue;
                if(!parked[i].connected() || now - parked_at[i] > HTTP_PARK_MS) {
                    parked[i].stop();
                    parked[i] = NetworkClient();
                } else if(parked[i].available()) {
                    _currentClient = parked[i];
                    parked[i] = NetworkClient();
                    _currentStatus = HC_WAIT_READ;
                    _statusChange = now;
                    break;
                }
            }
        }

        WebServer::handleClient();
    }

private:
    NetworkClient parked[HTTP_PARK_MAX];
    uint32_t parked_at[HTTP_PARK_MAX] = {};

    bool parked_ready (void)
    {
        for(int i = 0; i < HTTP_PARK_MAX; i++) {
            if(parked[i] && parked[i].available())
                return true;
        }
        return false;
    }

    int free_slot (void)
    {
        for(int i = 0; i < HTTP_PARK_MAX; i++) {
            if(!parked[i])
                return i;
        }
        return -1;
    }
};

static FastWebServer server(80);
static bool updates_ok = false, upload_ok = false;
static uint8_t web_mode = Web_View;
static wifi_state_t state = Wifi_Off;
static bool scan_failed = false, scan_paused = false;  // a connection attempt waits for the scan
static volatile uint8_t fail_reason = 0;    // last disconnect reason (esp_wifi), 0 = none
static bool services = false;           // OTA, upload page and mDNS are running
static char ip[16] = "";
static void (*on_progress)(unsigned pct) = NULL;
static bool scanning = false;

// The WiFi chip needs a moment after a disconnect: with the co-processor of the ESP32-P4 a connection
// started at once was sometimes lost, the display waited for ever. So it starts WIFI_SETTLE_MS later
// (several restarts in a row, e.g. at boot, give one), and a try that hangs WIFI_RETRY_MS starts again.
#define WIFI_SETTLE_MS  1000
#define WIFI_RETRY_MS   15000
// the first connection only comes when everything else runs, WIFI_BOOT_MS after the end of setup()
#define WIFI_BOOT_MS    1000
static bool begin_pending = false;
static uint8_t fast_tries = 0;      // quick tries after a refused login (202)
static uint32_t begin_at = 0, connecting_since = 0, boot_end = 0;

void wifi_boot_done (void)
{
    boot_end = millis();
}
static int scan_n = 0;
static char scan_ssid[WIFI_SCAN_MAX][33];
static char host[33] = WIFI_HOST_DEFAULT;
static char m_ssid[33] = "", m_pass[65] = "";      // machine network of the Octopus-Bridge, see wlan.h
static bool home_mode = false;      // in the network of wifi_cfg although paired: firmware update
static bool machine_ok = true;      // the display is not on the USB cable of the bridge
static bool deck_mode = false;      // PC-Deck mode: only the home network, also with "Home: Off"

// the machine network is used when paired, without the USB cable, not for a firmware update and not as a PC-Deck
static bool use_machine (void)
{
    return *m_ssid && machine_ok && !home_mode && !deck_mode;
}

#define TIME_TZ         "CET-1CEST,M3.5.0,M10.5.0/3"   // Germany, summer time
#define TIME_RESYNC_MS  (12UL * 3600 * 1000)

static bool update_mode = false;
static uint32_t time_next = 0;

static const char *target_ssid (void)
{
    return use_machine() ? m_ssid : wifi_cfg.ssid;
}

static const char *target_pass (void)
{
    return use_machine() ? m_pass : wifi_cfg.pass;
}

static bool sender_on = false;

// sender bridge: TCP connection <-> second UART to the controller, only on boards with sender pins
// (ESP32-P4: the Octopus-Bridge at the controller takes the sender)
#ifdef PIN_SENDER_TX

static WiFiServer sender_server(SENDER_PORT);
static WiFiClient sender_client;
static HardwareSerial &sender_uart = Serial2;

static void sender_close (void)
{
    if(sender_client)
        sender_client.stop();
    if(sender_on)
        link_send("TC,0");
    sender_on = false;
}

// One sender at a time, bytes are passed through unchanged in both directions.
static void sender_poll (void)
{
    if(sender_server.hasClient()) {
        WiFiClient c = sender_server.accept();
        if(sender_client && sender_client.connected())
            c.stop();               // busy
        else {
            sender_client = c;
            sender_client.setNoDelay(true);
            while(sender_uart.available())
                sender_uart.read();
            sender_on = true;
            link_send("TC,1");
        }
    }

    if(sender_on && !sender_client.connected()) {
        sender_close();
        return;
    }

    if(sender_on) {
        uint8_t buf[512];
        int n;
        if((n = sender_client.available()) > 0) {
            n = sender_client.read(buf, min(n, (int)sizeof(buf)));
            if(n > 0)
                sender_uart.write(buf, n);
        }
        if((n = sender_uart.available()) > 0) {
            n = sender_uart.read(buf, min(n, (int)sizeof(buf)));
            if(n > 0)
                sender_client.write(buf, n);
        }

        // the controller may have restarted: repeat the connection state once a second
        static uint32_t last = 0;
        if(millis() - last >= 1000) {
            last = millis();
            link_send("TC,1");
        }
    } else {
        while(sender_uart.available())
            sender_uart.read();     // nothing to pass it to
    }
}

#endif // PIN_SENDER_TX

// Upload page for a firmware update, only accepted with the OTA password and while updates are allowed
static const char update_page[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>Firmware update</title>"
    "<style>body{font-family:sans-serif;background:#070418;color:#fff;padding:24px}input,button{font-size:18px;margin:8px 0}</style>"
    "</head><body><h2>Firmware update</h2><p>Only while the machine is idle, in alarm or not connected.</p>"
    "<form method=POST action=/update enctype=multipart/form-data><input type=file name=firmware accept=.bin><br>"
    "<button>Update</button></form></body></html>";

// the OTA password (upload page) or the token of a browser logged in for control (web interface)
static bool update_auth (void)
{
    return *wifi_cfg.ota && (web_may_control() || server.authenticate("admin", wifi_cfg.ota));
}

static void update_setup (void)
{
    server.on("/update", HTTP_GET, []() {
        if(!update_auth())
            return server.requestAuthentication();
        server.send(200, "text/html", update_page);
    });

    server.on("/update", HTTP_POST, []() {
        bool ok = upload_ok && !Update.hasError();
        server.sendHeader("Connection", "close");
        server.send(ok ? 200 : 400, "text/plain", ok ? "Update done, restarting" :
                    !update_auth() ? "Wrong password" : !updates_ok ? "Not now: the machine is busy" : "Update failed");
        if(ok) {
            delay(500);
            ESP.restart();
        }
    }, []() {
        HTTPUpload &up = server.upload();
        if(up.status == UPLOAD_FILE_START) {
            // with the file size (web interface: /update?size=n) the progress counts to the end of the file,
            // without it to the end of the partition
            long size = server.hasArg("size") ? server.arg("size").toInt() : 0;
            upload_ok = update_auth() && updates_ok && Update.begin(size > 0 ? (size_t)size : UPDATE_SIZE_UNKNOWN);
        }
        else if(up.status == UPLOAD_FILE_WRITE) {
            if(upload_ok && Update.write(up.buf, up.currentSize) != up.currentSize)
                upload_ok = false;
        } else if(up.status == UPLOAD_FILE_END) {
            if(upload_ok)
                upload_ok = Update.end(true);
        } else if(up.status == UPLOAD_FILE_ABORTED) {
            if(upload_ok)
                Update.abort();
            upload_ok = false;
        }
    });
}

static void services_start (void)
{
    MDNS.begin(host);

#ifdef PIN_SENDER_TX
    sender_server.begin();
    sender_server.setNoDelay(true);
    MDNS.addService("telnet", "tcp", SENDER_PORT);
#endif

    if(*wifi_cfg.ota) {
        ArduinoOTA.setHostname(host);
        ArduinoOTA.setPassword(wifi_cfg.ota);
        ArduinoOTA.setMdnsEnabled(false);   // MDNS is started above
        ArduinoOTA.begin();
        MDNS.enableArduino(3232, true);
    }

    // web interface and upload page
    static bool routes = false;
    if(!routes) {
        update_setup();
        web_begin(server);
        routes = true;
    }
    server.begin();
    MDNS.addService("http", "tcp", 80);

    services = true;
}

static void services_stop (void)
{
    if(services) {
#ifdef PIN_SENDER_TX
        sender_close();
        sender_server.end();
#endif
        ArduinoOTA.end();
        server.stop();
        MDNS.end();
        services = false;
    }
}

// The board setup (ESP32-P4: start of the WiFi co-processor) only runs once WiFi is used, so the display
// works without WiFi even when the co-processor does not answer.
static bool board_ready = false;

static void board_init_once (void)
{
    if(!board_ready) {
        board_ready = true;
        wifi_board_init();
    }
}

// The radio is only switched off when WiFi is turned off: switching it off and on again right away fails
// with the co-processor of the ESP32-P4, its RPC link is not up again yet.
static void start (void)
{
    services_stop();

    // the machine network of a bridge is on without the USB cable, wifi_cfg.enabled only switches the
    // home network; paired, the radio stays on (the co-processor of the ESP32-P4 does not like a quick
    // off and on, the cable may be pulled any moment)
    if(!use_machine() && (!(wifi_cfg.enabled || deck_mode) || !*wifi_cfg.ssid)) {
        if(board_ready) {
            if(*m_ssid)
                WiFi.disconnect();
            else {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
            }
        }
        state = Wifi_Off;
        begin_pending = false;
        return;
    }

    board_init_once();
    if(WiFi.getMode() == WIFI_STA)
        WiFi.disconnect();
    else
        WiFi.mode(WIFI_STA);
    state = Wifi_Connecting;
    fail_reason = 0;
    scan_paused = false;
    fast_tries = 0;
    begin_pending = true;
    begin_at = millis();
}

static void begin_now (void)
{
    begin_pending = false;
    connecting_since = millis();
    WiFi.setHostname(host);
    WiFi.setSleep(false);       // short response times for the sender
    WiFi.setAutoReconnect(true);
    WiFi.begin(target_ssid(), target_pass());
}

// Board specific WiFi setup before the first use, e.g. the SDIO pins of an ESP32-C6 co-processor
// on the ESP32-P4. The ESP32-S3 has WiFi on chip and needs nothing.
__attribute__((weak)) void wifi_board_init (void)
{
}

void wifi_begin (void (*progress)(unsigned pct))
{

    on_progress = progress;

    prefs.begin("wifi", false);
    if(prefs.isKey("cfg") && prefs.getBytesLength("cfg") == sizeof(wifi_cfg))
        prefs.getBytes("cfg", &wifi_cfg, sizeof(wifi_cfg));
    prefs.getString("host", host, sizeof(host));
    prefs.getString("mssid", m_ssid, sizeof(m_ssid));
    prefs.getString("mpass", m_pass, sizeof(m_pass));
    web_mode = prefs.getUChar("web", Web_View);
    if(!*host)
        strcpy(host, WIFI_HOST_DEFAULT);

    setenv("TZ", TIME_TZ, 1);       // local time also before the first sync
    tzset();

#ifdef PIN_SENDER_TX
    sender_uart.setRxBufferSize(4096);
    sender_uart.setTxBufferSize(4096);
    sender_uart.begin(SENDER_BAUD, SERIAL_8N1, PIN_SENDER_RX, PIN_SENDER_TX);
#endif

    // why a connection attempt fails, 8 = we disconnected ourselves
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if(info.wifi_sta_disconnected.reason != 8)
            fail_reason = info.wifi_sta_disconnected.reason;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    // progress of both update paths, the display shows it
    Update.onProgress([](size_t done, size_t total) {
        if(on_progress && total)
            on_progress((unsigned)(done * 100 / total));
    });

    start();
}

void wifi_apply (void)
{
    prefs.putBytes("cfg", &wifi_cfg, sizeof(wifi_cfg));
    if(!use_machine())
        start();        // in the machine network the home settings wait, the link is not interrupted
}

void wifi_poll (bool updates_allowed)
{
    if(state == Wifi_Off)
        return;

    if(scanning || scan_paused)
        ;
    else if(begin_pending) {
        if(millis() - begin_at >= WIFI_SETTLE_MS && millis() >= boot_end + WIFI_BOOT_MS)
            begin_now();
        return;
    } else if(state == Wifi_Connecting && millis() - connecting_since >= WIFI_RETRY_MS) {
        Serial.printf("WiFi: no connection to '%s' after %u s, again\n", target_ssid(), WIFI_RETRY_MS / 1000);
        WiFi.disconnect();
        begin_pending = true;
        begin_at = millis();
        return;
    }

    static uint8_t reported = 0;
    if(fail_reason != reported) {
        reported = fail_reason;
        if(reported && state != Wifi_Connected)
            Serial.printf("WiFi: no connection to '%s': %s (%u)\n", target_ssid(), wifi_fail_text(), reported);
    }

    // the first login after a start is refused with 202 (auth fail) nearly every time, the next one works:
    // again at once, twice at most (a really wrong password then waits WIFI_RETRY_MS)
    if(state == Wifi_Connecting && !scanning && !scan_paused && fail_reason == 202 && fast_tries < 2) {
        fast_tries++;
        fail_reason = 0;
        WiFi.disconnect();
        begin_pending = true;
        begin_at = millis();
        return;
    }

    if(WiFi.status() == WL_CONNECTED) {
        if(state != Wifi_Connected) {
            state = Wifi_Connected;
            fast_tries = 0;
            fail_reason = 0;
            strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
            services_start();
        }
    } else if(state == Wifi_Connected) {
        state = Wifi_Connecting;    // reconnects by itself, else again after WIFI_RETRY_MS
        connecting_since = millis();
        services_stop();
    }

#ifdef PIN_SENDER_TX
    if(state == Wifi_Connected && services)
        sender_poll();
    else if(sender_on)
        sender_close();
#endif

    // the web interface always, firmware updates only while the machine is idle, in alarm or not connected
    if(state == Wifi_Connected && services) {
        updates_ok = updates_allowed;
        if(updates_allowed && *wifi_cfg.ota)
            ArduinoOTA.handle();
        server.handleClient();
        web_poll();
    }
}

// After a scan: WiFi off again, or the paused connection attempt goes on
static void scan_done (void)
{
    if(state == Wifi_Off)
        WiFi.mode(WIFI_OFF);
    else if(scan_paused) {
        scan_paused = false;
        begin_pending = true;
        begin_at = millis();
    }
}

void wifi_scan_start (void)
{
    if(scanning)
        return;

    board_init_once();
    if(WiFi.getMode() == WIFI_OFF)
        WiFi.mode(WIFI_STA);    // scanning needs the station mode, switched off again afterwards

    // the WiFi chip refuses to scan while it tries to connect: the attempt waits for the scan
    if(state == Wifi_Connecting && !scan_paused) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect();
        scan_paused = true;
        delay(100);
    }

    WiFi.scanDelete();
    scanning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
    if(!scanning) {
        delay(200);
        scanning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
    }
    scan_failed = !scanning;
    scan_n = 0;
    if(scan_failed)
        scan_done();
}

int wifi_scan_result (void)
{
    if(!scanning)
        return scan_failed ? WIFI_SCAN_ERROR : scan_n;

    int n = WiFi.scanComplete();
    if(n == WIFI_SCAN_RUNNING)
        return -1;

    scanning = false;
    scan_failed = n < 0;
    scan_n = 0;

    // the results come sorted by signal strength, the same network may show up more than once
    for(int i = 0; i < n && scan_n < WIFI_SCAN_MAX; i++) {
        String ssid = WiFi.SSID(i);
        bool dup = ssid.length() == 0;
        for(int j = 0; j < scan_n && !dup; j++)
            dup = ssid == scan_ssid[j];
        if(!dup)
            strlcpy(scan_ssid[scan_n++], ssid.c_str(), sizeof(scan_ssid[0]));
    }
    WiFi.scanDelete();
    scan_done();

    return scan_failed ? WIFI_SCAN_ERROR : scan_n;
}

// why the connection attempt fails, in words
const char *wifi_fail_text (void)
{
    static char buf[24];

    switch(fail_reason) {
        case 0:
            return "";
        case 201: case 210: case 211: case 212:     // no AP found (at all, with compatible security, ...)
            return "network not found";
        case 2: case 15: case 202: case 204: case 205:  // auth expired, handshake timeout, auth failed
            return "wrong password?";
        case 200:                                   // beacon timeout
            return "signal lost";
        default:
            snprintf(buf, sizeof(buf), "error %u", fail_reason);
            return buf;
    }
}

const char *wifi_scan_ssid (int i)
{
    return i >= 0 && i < scan_n ? scan_ssid[i] : "";
}

const char *wifi_host (void)
{
    return host;
}

void wifi_set_host (const char *name)
{
    char h[33];
    size_t n = 0;

    for(; *name && n < sizeof(h) - 1; name++) {
        char c = *name >= 'A' && *name <= 'Z' ? *name + 32 : *name;
        if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && n))
            h[n++] = c;
    }
    h[n] = '\0';

    strlcpy(host, n ? h : WIFI_HOST_DEFAULT, sizeof(host));
    prefs.putString("host", host);
}

uint8_t wifi_web_mode (void)
{
    return web_mode;
}

void wifi_set_web_mode (uint8_t mode)
{
    web_mode = mode <= Web_Control ? mode : Web_Off;
    prefs.putUChar("web", web_mode);
}

bool wifi_sender (void)
{
    return sender_on;
}

// Sent by the bridge on every plug-in: only a change is stored and reconnects.
void wifi_set_machine (const char *ssid, const char *pass)
{
    if(!*ssid || (!strcmp(ssid, m_ssid) && !strcmp(pass, m_pass)))
        return;

    strlcpy(m_ssid, ssid, sizeof(m_ssid));
    strlcpy(m_pass, pass, sizeof(m_pass));
    prefs.putString("mssid", m_ssid);
    prefs.putString("mpass", m_pass);
    Serial.println("WiFi: paired with the machine network of the bridge");

    start();
}

const char *wifi_machine_ssid (void)
{
    return m_ssid;
}

bool wifi_machine (void)
{
    return use_machine() && state == Wifi_Connected;
}

static void to_home (bool on)
{
    home_mode = on;
    start();
}

// The display link has priority: the time is only fetched while the home network is the normal one (not
// paired with a bridge, or on its USB cable), the clock runs on by itself in between.
void wifi_time_poll (void)
{
    uint32_t now = millis();

    if(!use_machine() && !update_mode && state == Wifi_Connected && (int32_t)(now - time_next) >= 0) {
        configTzTime(TIME_TZ, "pool.ntp.org", "time.google.com");
        time_next = now + TIME_RESYNC_MS;
    }
}

void wifi_machine_allowed (bool on)
{
    if(on != machine_ok) {
        machine_ok = on;
        home_mode = update_mode;
        start();
    }
}

bool wifi_time_valid (void)
{
    return time(NULL) > 1700000000;     // after 2023: set
}

void wifi_update_mode (bool on)
{
    if(on == update_mode)
        return;

    update_mode = on;
    if(on && !wifi_cfg.enabled) {
        wifi_cfg.enabled = true;
        prefs.putBytes("cfg", &wifi_cfg, sizeof(wifi_cfg));
    }
    to_home(on);
}

void wifi_deck_mode (bool on)
{
    if(on != deck_mode) {
        deck_mode = on;
        start();
    }
}

bool wifi_update_mode_on (void)
{
    return update_mode;
}

wifi_state_t wifi_state (void)
{
    return state;
}

const char *wifi_ip (void)
{
    return state == Wifi_Connected ? ip : "";
}

wifi_state_t wifi_home_state (void)
{
    return use_machine() ? Wifi_Off : state;
}
