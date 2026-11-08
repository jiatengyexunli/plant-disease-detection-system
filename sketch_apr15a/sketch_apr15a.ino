#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <time.h>

// ==================== 必须由使用者填写的联网配置 ====================
// 不要把真实 Wi-Fi 密码提交到公开仓库。
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// 后端服务地址。部署到其他电脑后，这里的局域网 IP 也需要同步修改。
const char *host = "YOUR_SERVER_IP";
uint16_t port = 8080;
const char *path_img = "/machine/machineCamera";
const char *path_data = "/machine/machineRecord";

// QVGA JPEG 通常为 15~45 KB。缓冲区过小会静默上传截断图片。
static const size_t IMG_BUF_SIZE = 49152;
static uint8_t imgBuf[IMG_BUF_SIZE];

WiFiClient client;

// ==================== NTP 实时时间 ====================
void syncNTP()
{
    configTime(8 * 3600, 0, "ntp.ntsc.ac.cn", "pool.ntp.org");
    time_t now = time(nullptr);

    // 最多等待约 8 秒；失败时继续运行，由后端使用自己的接收时间。
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

// ==================== 图片上传 ====================
void uploadIMG(const uint8_t *data, size_t length)
{
    // 同时在入口和退出时校验 JPEG 文件头/尾，避免上传损坏数据。
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

// ==================== STM32 JSON 数据上传 ====================
void uploadJSON(String json)
{
    if (json.length() < 10 || json[0] != '{' ||
        WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    // STM32 没有 RTC，串口 JSON 中的占位时间由 ESP 的 NTP 时间替换。
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
    if (code < 200 || code >= 300)
    {
        Serial.printf("[JSON] Upload failed: HTTP %d\n", code);
    }
    http.end();
}

// ==================== 初始化 ====================
void setup()
{
    Serial.begin(115200);
    delay(100);

    Serial.println("\nConnecting WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    // 原代码会永久阻塞；这里限制为 30 秒，失败后主循环继续尝试重连。
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
        Serial.println("WiFi timeout; reconnect will retry in loop()");
    }
}

// ==================== 主循环 ====================
void loop()
{
    static size_t imgLen = 0;
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
            // 只有检测到真正的 FFD8 才进入图片模式，普通 JSON 不会堆积进图片缓冲区。
            if (previous == 0xFF && current == 0xD8)
            {
                imgBuf[0] = 0xFF;
                imgBuf[1] = 0xD8;
                imgLen = 2;
                inJpeg = true;
                jsonBuffer = ""; // 丢弃图片前未完成的半行文本
                previous = current;
                continue;
            }

            // 收集 STM32 的 SENSOR: 或直接 JSON 行。
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

        // 图片模式：逐个字节保存，缓冲区不足时明确丢弃整张图片。
        if (imgLen < IMG_BUF_SIZE)
        {
            imgBuf[imgLen++] = current;
        }
        else
        {
            Serial.println("[IMG] Buffer overflow, image dropped");
            imgLen = 0;
            inJpeg = false;
            previous = current;
            continue;
        }

        if (imgLen >= 2 && imgBuf[imgLen - 2] == 0xFF && imgBuf[imgLen - 1] == 0xD9)
        {
            uploadIMG(imgBuf, imgLen);
            imgLen = 0;
            inJpeg = false;
            previous = current;
            continue;
        }
        previous = current;
    }

    if (inJpeg && millis() - lastDataTime > 8000)
    {
        Serial.println("[IMG] Transfer timeout, image dropped");
        imgLen = 0;
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
