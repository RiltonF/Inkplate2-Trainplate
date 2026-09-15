#include "Inkplate.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>

#include "secrets.h"

// ---------------------------------------------------------
// Configuration
// ---------------------------------------------------------

constexpr uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr int SEARCH_BEFORE_MINUTES = 20;

Inkplate display;
WebServer server(80);
Preferences prefs;

struct Config {
    String from;
    String to;
    String departureTime;
    String trainHint;
    String language;
    // Bit 0 = Sunday
    // Bit 1 = Monday
    // ...
    // Bit 6 = Saturday
    uint8_t days = 0b0111110;  // Mon-Fri by default
};

struct TrainState {
    bool valid = false;
    bool cancelled = false;
    bool platformChanged = false;
    bool trainHintFallback = false;

    String train;
    String direction;
    String platform;
    String alert;

    time_t departureEpoch = 0;
    int delaySeconds = 0;
};

Config config;
TrainState currentState;

uint32_t lastPoll = 0;
bool forcePoll = true;

int apiFailureCount = 0;

String lastFingerprint;
String lastError;


// ---------------------------------------------------------
// Helpers
// ---------------------------------------------------------

String htmlEscape(String s)
{
    s.replace("&", "&amp;");
    s.replace("<", "&lt;");
    s.replace(">", "&gt;");
    s.replace("\"", "&quot;");
    return s;
}


String urlEncode(const String &input)
{
    const char hex[] = "0123456789ABCDEF";

    String output;

    for (size_t i = 0; i < input.length(); i++) {
        uint8_t c = input[i];

        if (
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~'
        ) {
            output += (char)c;
        } else {
            output += '%';
            output += hex[c >> 4];
            output += hex[c & 0x0F];
        }
    }

    return output;
}


String shorten(String s, size_t maxLength)
{
    if (s.length() <= maxLength)
        return s;

    return s.substring(0, maxLength - 3) + "...";
}


bool hasIssue(const TrainState &s)
{
    return
        s.cancelled ||
        s.delaySeconds > 0 ||
        s.platformChanged ||
        s.alert.length() > 0 ||
        s.trainHintFallback;
}


String makeFingerprint(const TrainState &s)
{
    return
        s.train + "|" +
        String((long)s.departureEpoch) + "|" +
        String(s.delaySeconds) + "|" +
        s.platform + "|" +
        String(s.cancelled) + "|" +
        String(s.platformChanged) + "|" +
        s.alert;
}


// ---------------------------------------------------------
// Persistent configuration
// ---------------------------------------------------------

void loadConfig()
{
    prefs.begin("traincfg", false);

    config.from =
        prefs.getString("from", "");

    config.to =
        prefs.getString("to", "");

    config.departureTime =
        prefs.getString("time", "07:30");

    config.trainHint =
        prefs.getString("hint", "");

    config.language =
        prefs.getString("lang", "en");

    // Default = Monday-Friday
    config.days =
        prefs.getUChar("days", 0b0111110);
}


void saveConfig()
{
    prefs.putString("from", config.from);
    prefs.putString("to", config.to);
    prefs.putString("time", config.departureTime);
    prefs.putString("hint", config.trainHint);
    prefs.putString("lang", config.language);
    prefs.putUChar("days", config.days);
}


bool configIsValid()
{
    return
        config.from.length() &&
        config.to.length() &&
        config.departureTime.length() == 5 &&
        config.days != 0;
}


// ---------------------------------------------------------
// Time handling
// ---------------------------------------------------------

bool makeTargetAndQueryTime(
    time_t &targetEpoch,
    char *dateBuffer,
    size_t dateBufferSize,
    char *timeBuffer,
    size_t timeBufferSize)
{
    time_t now;
    time(&now);

    // NTP not synced yet
    if (now < 1700000000)
        return false;

    // No days selected
    if (config.days == 0) {
        lastError = "No travel days selected";
        return false;
    }

    struct tm localNow;
    localtime_r(&now, &localNow);

    int hour =
        config.departureTime.substring(0, 2).toInt();

    int minute =
        config.departureTime.substring(3, 5).toInt();

    // -----------------------------------------------------
    // Find today or the next enabled day
    // -----------------------------------------------------

    struct tm selectedDate = localNow;

    bool found = false;

    for (int offset = 0; offset < 7; offset++) {

        struct tm candidate = localNow;

        candidate.tm_mday += offset;

        // Let mktime normalize month/year changes and DST.
        candidate.tm_hour = 12;
        candidate.tm_min = 0;
        candidate.tm_sec = 0;

        time_t candidateEpoch = mktime(&candidate);

        localtime_r(
            &candidateEpoch,
            &candidate
        );

        if (dayEnabled(candidate.tm_wday)) {
            selectedDate = candidate;
            found = true;
            break;
        }
    }

    if (!found) {
        lastError = "No enabled travel day found";
        return false;
    }

    // -----------------------------------------------------
    // Build the target departure time on that day
    // -----------------------------------------------------

    selectedDate.tm_hour = hour;
    selectedDate.tm_min = minute;
    selectedDate.tm_sec = 0;

    targetEpoch =
        mktime(&selectedDate);

    // Search slightly before configured departure
    time_t queryEpoch =
        targetEpoch -
        SEARCH_BEFORE_MINUTES * 60;

    struct tm queryTime;
    localtime_r(
        &queryEpoch,
        &queryTime
    );

    strftime(
        dateBuffer,
        dateBufferSize,
        "%d%m%y",
        &queryTime
    );

    strftime(
        timeBuffer,
        timeBufferSize,
        "%H%M",
        &queryTime
    );

    return true;
}

const char *DAY_NAMES[] = {
    "Sunday",
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday"
};

bool dayEnabled(int tmWeekday)
{
    if (tmWeekday < 0 || tmWeekday > 6)
        return false;

    return config.days & (1 << tmWeekday);
}

String selectedDaysText()
{
    String result;

    for (int day = 0; day < 7; day++) {

        if (!dayEnabled(day))
            continue;

        if (result.length())
            result += ", ";

        result += DAY_NAMES[day];
    }

    return result;
}

// ---------------------------------------------------------
// iRail
// ---------------------------------------------------------

bool trainNameMatches(String actual, String requested)
{
    if (requested.length() == 0)
        return true;

    actual.toUpperCase();
    requested.toUpperCase();

    actual.replace(" ", "");
    requested.replace(" ", "");

    return actual.indexOf(requested) >= 0;
}


bool fetchTrain(TrainState &result)
{
    result = TrainState();

    if (WiFi.status() != WL_CONNECTED) {
        lastError = "Wi-Fi disconnected";
        return false;
    }

    time_t targetEpoch;

    char dateString[16];
    char queryTimeString[16];

    if (!makeTargetAndQueryTime(
            targetEpoch,
            dateString,
            sizeof(dateString),
            queryTimeString,
            sizeof(queryTimeString))) {

        lastError = "Clock not synchronized";
        return false;
    }

    String url =
        "https://api.irail.be/connections/"
        "?from=" + urlEncode(config.from) +
        "&to=" + urlEncode(config.to) +
        "&date=" + String(dateString) +
        "&time=" + String(queryTimeString) +
        "&timesel=departure"
        "&format=json"
        "&typeOfTransport=trains"
        "&results=6"
        "&lang=" + urlEncode(config.language);

    Serial.println(url);

    WiFiClientSecure client;

    // Convenient for a small personal project.
    //
    // For a security-sensitive deployment you would validate
    // the server certificate instead.
    client.setInsecure();

    HTTPClient http;

    http.setTimeout(12000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) {
        lastError = "Could not start HTTP request";
        return false;
    }

    http.addHeader("Accept", "application/json");

    // iRail recommends identifying applications with User-Agent.
    http.addHeader(
        "User-Agent",
        "InkplateTrainDashboard/0.1"
    );

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        lastError =
            "iRail HTTP " + String(httpCode);

        http.end();
        return false;
    }

    String body = http.getString();

    http.end();

    JsonDocument document;
    JsonDocument filter;

    // Fields we actually need from each connection
    filter["connection"][0]["departure"]["time"] = true;
    filter["connection"][0]["departure"]["delay"] = true;
    filter["connection"][0]["departure"]["canceled"] = true;

    filter["connection"][0]["departure"]["vehicle"] = true;
    filter["connection"][0]["departure"]["vehicleinfo"]["shortname"] = true;

    filter["connection"][0]["departure"]["direction"]["name"] = true;

    filter["connection"][0]["departure"]["platform"] = true;
    filter["connection"][0]["departure"]["platforminfo"]["name"] = true;
    filter["connection"][0]["departure"]["platforminfo"]["normal"] = true;

    filter["connection"][0]["departure"]["alerts"]["alert"][0]["header"] = true;
    filter["connection"][0]["departure"]["alerts"]["alert"][0]["lead"] = true;

    filter["connection"][0]["alerts"]["alert"][0]["header"] = true;
    filter["connection"][0]["alerts"]["alert"][0]["lead"] = true;

    filter["connection"][0]["vias"]["number"] = true;

    Serial.print("JSON bytes: ");
    Serial.println(body.length());
    DeserializationError jsonError =
        deserializeJson(
        document,
        body,
        DeserializationOption::Filter(filter),
        DeserializationOption::NestingLimit(20)
    );

    /* new streamptr code to use stream instead of allocatin more memory    
    WiFiClient *stream = http.getStreamPtr();

    JsonDocument document;
    JsonDocument filter;

    // Fields we actually need from each connection
    filter["connection"][0]["departure"]["time"] = true;
    filter["connection"][0]["departure"]["delay"] = true;
    filter["connection"][0]["departure"]["canceled"] = true;

    filter["connection"][0]["departure"]["vehicle"] = true;
    filter["connection"][0]["departure"]["vehicleinfo"]["shortname"] = true;

    filter["connection"][0]["departure"]["direction"]["name"] = true;

    filter["connection"][0]["departure"]["platform"] = true;
    filter["connection"][0]["departure"]["platforminfo"]["name"] = true;
    filter["connection"][0]["departure"]["platforminfo"]["normal"] = true;

    filter["connection"][0]["departure"]["alerts"]["alert"][0]["header"] = true;
    filter["connection"][0]["departure"]["alerts"]["alert"][0]["lead"] = true;

    filter["connection"][0]["alerts"]["alert"][0]["header"] = true;
    filter["connection"][0]["alerts"]["alert"][0]["lead"] = true;

    filter["connection"][0]["vias"]["number"] = true;

    DeserializationError jsonError =
        deserializeJson(
        document,
        *stream,
        DeserializationOption::Filter(filter),
        DeserializationOption::NestingLimit(20)
    );

    http.end();*/

    if (jsonError) {
        lastError =
            "JSON error: " +
            String(jsonError.c_str());

        return false;
    }

    JsonArray connections =
        document["connection"].as<JsonArray>();

    // TODO: Add description to the Alerts!!! and fix the time /date displayed onthe screen
    serializeJsonPretty(connections, Serial);
    Serial.println();


    if (connections.size() == 0) {
        lastError = "No connections returned";
        return false;
    }

    // -----------------------------------------------------
    // Pick the connection closest to configured departure
    // -----------------------------------------------------

    JsonObject best;
    long bestScore = LONG_MAX;

    bool foundHint = false;

    for (JsonObject connection : connections) {

        JsonObject departure =
            connection["departure"];

        time_t departureEpoch =
            departure["time"] | 0;

        String train =
            departure["vehicleinfo"]["shortname"] | "";

        if (train.length() == 0)
            train = departure["vehicle"] | "";

        bool hintMatches =
            trainNameMatches(
                train,
                config.trainHint
            );

        if (config.trainHint.length() && hintMatches)
            foundHint = true;

        // First pass preference:
        // if a train hint exists, strongly prefer it.
        long score =
            labs((long)(departureEpoch - targetEpoch));

        if (config.trainHint.length() && !hintMatches)
            score += 24L * 60L * 60L;

        // Prefer a direct service.
        int viaCount =
            connection["vias"]["number"] | 0;

        if (viaCount > 0)
            score += 20L * 60L;

        if (score < bestScore) {
            bestScore = score;
            best = connection;
        }
    }

    if (best.isNull()) {
        lastError = "Could not select train";
        return false;
    }

    JsonObject departure =
        best["departure"];

    result.valid = true;

    result.departureEpoch =
        departure["time"] | 0;

    result.delaySeconds =
        departure["delay"] | 0;

    result.cancelled =
        (departure["canceled"] | 0) != 0;

    result.train =
        departure["vehicleinfo"]["shortname"] | "";

    if (result.train.length() == 0)
        result.train =
            departure["vehicle"] | "";

    result.direction =
        departure["direction"]["name"] | "";

    result.platform =
        departure["platforminfo"]["name"] | "";

    if (result.platform.length() == 0)
        result.platform =
            departure["platform"].as<String>();

    String normalPlatform =
        departure["platforminfo"]["normal"].as<String>();

    result.platformChanged =
        normalPlatform == "0";

    // -----------------------------------------------------
    // Alert/message
    // -----------------------------------------------------

    JsonArray alerts =
        best["alerts"]["alert"].as<JsonArray>();

    if (alerts.size() > 0) {

        result.alert =
            alerts[0]["header"] | "";

        if (result.alert.length() == 0)
            result.alert =
                alerts[0]["lead"] | "";
    }

    // Some alerts may also be attached to departure.
    if (result.alert.length() == 0) {

        JsonArray departureAlerts =
            departure["alerts"]["alert"].as<JsonArray>();

        if (departureAlerts.size() > 0) {

            result.alert =
                departureAlerts[0]["header"] | "";

            if (result.alert.length() == 0)
                result.alert =
                    departureAlerts[0]["lead"] | "";
        }
    }

    if (
        config.trainHint.length() &&
        !foundHint
    ) {
        result.trainHintFallback = true;

        if (result.alert.length() == 0)
            result.alert =
                "Configured train not found; showing nearest service";
    }

    lastError = "";

    return true;
}


// ---------------------------------------------------------
// Inkplate drawing
// ---------------------------------------------------------

void drawWrapped(
    String text,
    int x,
    int y,
    int charsPerLine,
    int maxLines,
    uint16_t color)
{
    display.setTextColor(color);
    display.setTextSize(1);

    int start = 0;

    for (int line = 0;
         line < maxLines && start < text.length();
         line++) {

        int end =
            min(
                start + charsPerLine,
                (int)text.length()
            );

        // Prefer breaking on a space.
        if (end < text.length()) {

            int space =
                text.lastIndexOf(' ', end);

            if (space > start)
                end = space;
        }

        String part =
            text.substring(start, end);

        part.trim();

        display.setCursor(x, y + line * 10);
        display.print(part);

        start = end;

        while (
            start < text.length() &&
            text[start] == ' '
        )
            start++;
    }
}


void drawTrain(const TrainState &s)
{
    display.clearDisplay();

    // Route
    display.setTextColor(INKPLATE2_BLACK);
    display.setTextSize(1);
    display.setCursor(2, 2);

    String route =
        config.from + " > " + config.to;

    display.print(shorten(route, 34));

    display.drawLine(
        0,
        15,
        211,
        15,
        INKPLATE2_BLACK
    );

    // Train + departure
    struct tm depTime;
    localtime_r(&s.departureEpoch, &depTime);

    char departureString[8];

    strftime(
        departureString,
        sizeof(departureString),
        "%H:%M",
        &depTime
    );

    display.setTextSize(2);
    display.setTextColor(INKPLATE2_BLACK);

    display.setCursor(2, 22);
    display.print(shorten(s.train, 12));

    display.setCursor(140, 22);
    display.print(departureString);

    // departure date
    char dateString[16];

    strftime(
        dateString,
        sizeof(dateString),
        "%a %d/%m",
        &depTime
    );
    display.setTextSize(1);
    display.setCursor(140, 42);
    display.print(dateString);

    bool issue = hasIssue(s);

    uint16_t statusColor =
        issue
            ? INKPLATE2_RED
            : INKPLATE2_BLACK;

    display.setTextColor(statusColor);
    display.setTextSize(2);
    display.setCursor(2, 48);

    if (s.cancelled) {

        display.print("CANCELLED");

    } else if (s.delaySeconds > 0) {

        int delayMinutes =
            (s.delaySeconds + 30) / 60;

        display.print("+");
        display.print(delayMinutes);
        display.print(" min");

    } else {

        display.print("ON TIME");
    }

    // Platform
    display.setTextSize(1);
    display.setCursor(155, 52);

    if (s.platform.length()) {
        display.print("PL ");
        display.print(s.platform);
    }

    // Alert/footer
    if (issue) {

        display.drawLine(
            0,
            72,
            211,
            72,
            INKPLATE2_RED
        );

        String message;

        if (s.platformChanged) {
            message =
                "Platform changed";

            if (s.alert.length())
                message += " - ";
        }

        message += s.alert;

        if (
            message.length() == 0 &&
            s.delaySeconds > 0
        )
            message = "Train delayed";

        drawWrapped(
            message,
            2,
            78,
            34,
            2,
            INKPLATE2_RED
        );
    }

    display.display();
}


void drawSetupScreen()
{
    display.clearDisplay();

    display.setTextColor(INKPLATE2_BLACK);

    display.setTextSize(2);
    display.setCursor(2, 5);
    display.print("TRAIN SETUP");

    display.setTextSize(1);

    display.setCursor(2, 38);
    display.print("Open:");

    display.setCursor(2, 53);
    display.print("http://trainplate.local");

    display.setCursor(2, 70);
    display.print(WiFi.localIP());

    display.display();
}


void drawApiError(String message)
{
    display.clearDisplay();

    display.setTextColor(INKPLATE2_RED);

    display.setTextSize(2);
    display.setCursor(2, 8);
    display.print("DATA STALE");

    display.setTextSize(1);

    drawWrapped(
        message,
        2,
        44,
        34,
        3,
        INKPLATE2_RED
    );

    display.display();
}


// ---------------------------------------------------------
// Web UI
// ---------------------------------------------------------

String webPage()
{
    String html;

    html.reserve(5000);

    html += R"HTML(
<!doctype html>
<html>
<head>
<meta name="viewport"
      content="width=device-width,initial-scale=1">
<title>Trainplate</title>

<style>
body {
    font-family: system-ui, sans-serif;
    background:#f4f4f4;
    margin:0;
}
main {
    max-width:520px;
    margin:30px auto;
    background:white;
    padding:24px;
    border-radius:12px;
}
label {
    display:block;
    margin-top:15px;
    font-weight:600;
}
input, select {
    box-sizing:border-box;
    width:100%;
    padding:10px;
    font-size:16px;
    margin-top:4px;
}
button {
    padding:11px 18px;
    margin-top:20px;
    font-size:16px;
}
.status {
    margin-top:25px;
    padding:15px;
    background:#eee;
    border-radius:8px;
}
.error {
    color:#b00020;
}
small {
    color:#666;
}
.days {
    display:flex;
    gap:6px;
    flex-wrap:wrap;
    margin-top:8px;
}

.day {
    display:flex;
    align-items:center;
    gap:4px;
    background:#eee;
    padding:7px 10px;
    border-radius:7px;
    font-weight:500;
    margin:0;
}

.day input {
    width:auto;
    margin:0;
}
</style>
</head>

<body>
<main>
<h1>Trainplate</h1>

<form method="POST" action="/save">

<label>From</label>
<input
    name="from"
    id="from"
    list="stations"
    required
    value=")HTML";

    html += htmlEscape(config.from);

    html += R"HTML(">

<label>To</label>
<input
    name="to"
    id="to"
    list="stations"
    required
    value=")HTML";

    html += htmlEscape(config.to);

    html += R"HTML(">

<datalist id="stations"></datalist>

<label>Usual departure</label>
<input
    name="time"
    type="time"
    required
    value=")HTML";

    html += htmlEscape(config.departureTime);

    html += R"HTML(">

<label>Travel days</label>

<div class="days">
)HTML";

html += "<br>Days: ";
html += htmlEscape(selectedDaysText());

const char *shortDayNames[] = {
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat"
};

for (int day = 0; day < 7; day++) {

    html += "<label class='day'>";

    html += "<input type='checkbox' name='day";
    html += String(day);
    html += "'";

    if (dayEnabled(day))
        html += " checked";

    html += ">";

    html += shortDayNames[day];

    html += "</label>";
}

html += R"HTML(
</div>

<label>Train number <small>(optional)</small></label>
<input
    name="hint"
    placeholder="e.g. IC2031"
    value=")HTML";

    html += htmlEscape(config.trainHint);

    html += R"HTML(">

<label>Language</label>
<select name="lang">
)HTML";

    const char *languages[] =
        {"en", "nl", "fr", "de"};

    for (auto language : languages) {
        html += "<option value='";
        html += language;
        html += "'";

        if (config.language == language)
            html += " selected";

        html += ">";
        html += language;
        html += "</option>";
    }

    html += R"HTML(
</select>

<button type="submit">Save</button>
</form>

<form method="POST" action="/refresh">
<button type="submit">Refresh now</button>
</form>

<div class="status">
)HTML";

    if (currentState.valid) {

        html += "<strong>";
        html += htmlEscape(currentState.train);
        html += "</strong><br>";

        html += "Platform: ";
        html += htmlEscape(currentState.platform);

        html += "<br>Delay: ";
        html += String(currentState.delaySeconds / 60);
        html += " min";

        if (currentState.alert.length()) {
            html += "<br><br>";
            html += htmlEscape(currentState.alert);
        }

    } else {
        html += "No train data yet.";
    }

    if (lastError.length()) {
        html += "<p class='error'>";
        html += htmlEscape(lastError);
        html += "</p>";
    }

    html += R"HTML(
</div>

<script>
// Station autocomplete happens in the browser, not on the ESP32.
fetch('https://api.irail.be/stations/?format=json&lang=en')
  .then(r => r.json())
  .then(data => {
      const list = document.getElementById('stations');
      const stations =
          Array.isArray(data.station)
              ? data.station
              : [data.station];

      stations.forEach(s => {
          const option = document.createElement('option');
          option.value = s.standardname || s.name;
          list.appendChild(option);
      });
  })
  .catch(() => {});
</script>

</main>
</body>
</html>
)HTML";

    return html;
}


void setupWebServer()
{
    server.on("/", HTTP_GET, []() {
        server.send(
            200,
            "text/html; charset=utf-8",
            webPage()
        );
    });


    server.on("/save", HTTP_POST, []() {
        config.from = server.arg("from");
        config.to = server.arg("to");
        config.departureTime = server.arg("time");
        config.trainHint = server.arg("hint");
        config.language = server.arg("lang");

        config.from.trim();
        config.to.trim();
        config.trainHint.trim();

        config.days = 0;
        for (int day = 0; day < 7; day++) {
            String field =
                "day" + String(day);
            if (server.hasArg(field))
                config.days |= (1 << day);
        }

        saveConfig();

        forcePoll = true;

        server.send(
            200,
            "text/html; charset=utf-8",
            webPage()
        );
    });


    server.on("/refresh", HTTP_POST, []() {
        forcePoll = true;

        server.send(
            200,
            "text/html; charset=utf-8",
            webPage()
        );
    });

    server.begin();
}


// ---------------------------------------------------------
// Poll/update
// ---------------------------------------------------------

void pollIRail()
{
    lastPoll = millis();
    forcePoll = false;

    TrainState newState;

    if (fetchTrain(newState)) {

        apiFailureCount = 0;

        String fingerprint =
            makeFingerprint(newState);

        currentState = newState;

        // E-paper only gets refreshed when something visible
        // has actually changed.
        if (fingerprint != lastFingerprint) {

            drawTrain(currentState);

            lastFingerprint =
                fingerprint;
        }

        return;
    }

    apiFailureCount++;

    Serial.print("iRail error: ");
    Serial.println(lastError);

    // Clock hasn't synchronized yet.
    // Try again in roughly 10 seconds instead of 5 minutes.
    if (lastError == "Clock not synchronized") {
        lastPoll =
            millis() - (POLL_INTERVAL_MS - 10000UL);

        return;
    }

    // Don't redraw the display for one temporary timeout.
    // After three missed polls (~15 minutes), flag it.
    if (apiFailureCount >= 3) {

        String errorFingerprint =
            "ERROR|" + lastError;

        if (errorFingerprint != lastFingerprint) {

            drawApiError(lastError);

            lastFingerprint =
                errorFingerprint;
        }
    }
}


// ---------------------------------------------------------
// Arduino
// ---------------------------------------------------------

void setup()
{
    Serial.begin(115200);

    display.begin();

    loadConfig();

    WiFi.mode(WIFI_STA);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    display.clearDisplay();
    display.setTextColor(INKPLATE2_BLACK);
    display.setTextSize(1);
    display.setCursor(2, 2);
    display.print("Connecting to WiFi...");
    display.display();

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // Belgium timezone including daylight-saving time.
    configTzTime(
        "CET-1CEST,M3.5.0,M10.5.0/3",
        "pool.ntp.org",
        "time.cloudflare.com"
    );

    // Wait for initial NTP synchronization
    Serial.print("Waiting for NTP");

    struct tm timeinfo;

    if (getLocalTime(&timeinfo, 15000)) {
        Serial.println();
        Serial.printf(
            "Clock synchronized: %04d-%02d-%02d %02d:%02d:%02d\n",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec
        );
    } else {
        Serial.println();
        Serial.println("NTP sync timed out");
    }

    if (MDNS.begin("trainplate")) {
        Serial.println(
            "Web UI: http://trainplate.local/"
        );
    }

    setupWebServer();

    if (!configIsValid())
        drawSetupScreen();
    else
        forcePoll = true;
}


void loop()
{
    server.handleClient();

    if (
        configIsValid() &&
        (
            forcePoll ||
            (uint32_t)(millis() - lastPoll)
                >= POLL_INTERVAL_MS
        )
    ) {
        pollIRail();
    }

    delay(2);
}