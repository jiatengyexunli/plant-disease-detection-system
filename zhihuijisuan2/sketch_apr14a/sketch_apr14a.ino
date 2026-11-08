#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <time.h>

// ==================== 使用者配置 ====================
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
const char *host = "YOUR_SERVER_IP";
uint16_t port = 8080;
const char *path_data = "/machine/machineRecord";
const char *path_img = "/machine/machineCamera";

static const size_t IMG_BUF_SIZE = 49152;
static uint8_t imageBuffer[IMG_BUF_SIZE];

WiFiClient client;

void syncNTP()
{
    configTime(8 * 3600, 0, "ntp.ntsc.ac.cn", "pool.ntp.org");
    time_t now = time(nullptr);
    for (int i = 0; i < 80 && now < 8 * 3600 * 2; i++)
    {
        delay(100);
        now = time(nullptr);
    }
}

String getRealTime()
{
    struct tm ti;
    if (!getLocalTime(&ti))
    {
        return "";
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    return String(buf);
}

void uploadJSON(String json)
{
    if (json.length() < 10 || json[0] != '{' ||
        WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[JSON] Invalid data or WiFi disconnected");
        return;
    }

    // STM32 上传的固定日期不可信，用 ESP8266 从 NTP 获取的时间替换。
    String timestamp = getRealTime();
    const String timeKey = "\"createTime\":\"";
    int timeStart = json.indexOf(timeKey);
    if (timeStart >= 0 && timestamp.length() > 0)
    {
        timeStart += timeKey.length();
        int timeEnd = json.indexOf('"', timeStart);
        if (timeEnd > timeStart)
        {
            json = json.substring(0, timeStart) + timestamp +
                   json.substring(timeEnd);
        }
    }

    HTTPClient http;
    String url = "http://" + String(host) + ":" + String(port) + path_data;
    http.begin(client, url);
    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(json);
    if (code >= 200 && code < 300)
    {
        Serial.printf("[JSON] Upload success: HTTP %d\n", code);
    }
    else
    {
        Serial.printf("[JSON] Upload failed: HTTP %d, %s\n",
                      code, http.errorToString(code).c_str());
    }
    http.end();
}

void uploadIMG(const uint8_t *data, size_t length)
{
    if (length < 4 || data[0] != 0xFF || data[1] != 0xD8 ||
        data[length - 2] != 0xFF || data[length - 1] != 0xD9)
    {
        Serial.printf("[IMG] Invalid JPEG, len=%u\n", (unsigned int)length);
        return;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[IMG] WiFi disconnected, skip upload");
        return;
    }

    HTTPClient http;
    String url = "http://" + String(host) + ":" + String(port) + path_img;
    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("diseaseResult", "1");
    http.addHeader("warning", "Disease detected");
    http.addHeader("id", "1");

    String timeStr = getRealTime();
    if (timeStr.length() > 0)
    {
        http.addHeader("create_time", timeStr);
    }

    Serial.printf("[IMG] Uploading %u bytes\n", (unsigned int)length);
    int code = http.POST(const_cast<uint8_t *>(data), length);
    if (code >= 200 && code < 300)
    {
        Serial.printf("[IMG] Upload success: HTTP %d\n", code);
    }
    else
    {
        Serial.printf("[IMG] Upload failed: HTTP %d, %s\n",
                      code, http.errorToString(code).c_str());
    }
    http.end();
}

void setup()
{
    Serial.begin(115200);
    delay(100);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi connected");
        syncNTP();
    }
    else
    {
        Serial.println("WiFi timeout; reconnect will retry");
    }
}

void loop()
{
    static size_t imageLen = 0;
    static uint8_t previous = 0;
    static bool inJpeg = false;
    static uint32_t lastDataTime = 0;
    static String jsonBuffer;

    while (Serial.available())
    {
        uint8_t current = (uint8_t)Serial.read();
        lastDataTime = millis();

        if (!inJpeg)
        {
            // 普通 JSON 和调试文本不应累积到 JPEG 缓冲区。
            if (previous == 0xFF && current == 0xD8)
            {
                imageBuffer[0] = 0xFF;
                imageBuffer[1] = 0xD8;
                imageLen = 2;
                inJpeg = true;
                jsonBuffer = ""; // 丢弃图片前未完成的半行文本
                previous = current;
                continue;
            }

            if (current == '\n')
            {
                if (jsonBuffer.startsWith("SENSOR:"))
                {
                    uploadJSON(jsonBuffer.substring(7));
                }
                else if (jsonBuffer.startsWith("{"))
                {
                    uploadJSON(jsonBuffer);
                }
                jsonBuffer = "";
            }
            else if (current >= 32 && current <= 126 && jsonBuffer.length() < 512)
            {
                jsonBuffer += (char)current;
            }

            previous = current;
            continue;
        }

        if (imageLen < IMG_BUF_SIZE)
        {
            imageBuffer[imageLen++] = current;
        }
        else
        {
            Serial.println("[IMG] Buffer overflow, image dropped");
            imageLen = 0;
            inJpeg = false;
            previous = current;
            continue;
        }

        if (imageLen >= 2 &&
            imageBuffer[imageLen - 2] == 0xFF &&
            imageBuffer[imageLen - 1] == 0xD9)
        {
            uploadIMG(imageBuffer, imageLen);
            imageLen = 0;
            inJpeg = false;
            previous = current;
            continue;
        }

        previous = current;
    }

    if (inJpeg && millis() - lastDataTime > 8000)
    {
        Serial.println("[IMG] Transfer timeout, image dropped");
        imageLen = 0;
        inJpeg = false;
    }

    static uint32_t lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 10000)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("[WiFi] Reconnecting...");
            WiFi.reconnect();
        }
        lastWiFiCheck = millis();
    }

    delay(1);
    yield();
}
