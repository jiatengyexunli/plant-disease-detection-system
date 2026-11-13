# 智慧植物病害监测系统

这是一个由 K210、STM32F103 和 ESP8266 组成的植物病害检测与环境监测原型。
K210 运行 MaixPy YOLOv2 模型识别正常/病害叶片，STM32F103 采集 DHT11、
BH1750 光照和土壤湿度，ESP8266 将 JSON 数据和病害图片上传到后端。


> 本 README 不使用图片。仓库中的 PNG 是用户选择保留的后端接口截图文件，
> 不是本说明的配图。

## 系统组成

```text
DHT11 / BH1750 / 土壤湿度
             |
             v
        STM32F103
       /         \
 UART1 /           \ UART2
      v             v
    K210          ESP8266
  病害识别        JSON/图片上传
      \             /
       \           v
        \------> 后端服务器
```

K210 与 STM32 之间的 UART1 同时承担：

- STM32 每 5 秒向 K210 发送 `DATA:temp,humi,soil,light`。
- K210 检测到病害后向 STM32 发送原始 JPEG 数据。
- STM32 检测 JPEG 的 `FF D8` 文件头和 `FF D9` 文件尾，再转发给 ESP8266。
- STM32 每 5 秒向 ESP8266 发送带 `SENSOR:` 前缀的 JSON。

## 已选文件与功能

| 文件或目录 | 功能 |
|---|---|
| `main.py` | MaixPy 病害识别、LCD 显示、串口数据解析、病害 JPEG 上传 |
| `清晰/main.py` | MaixHub 生成的 YOLOv2 检测脚本，附带 2.16 系列 anchors |
| `清晰/plant_model.kmodel` | 与 `清晰/report.json` 配套的 K210 模型 |
| `清晰/report.json` | 模型 anchors、输入尺寸、标签和训练评估结果 |
| `zhihuijisuan2/` | STM32CubeMX/CMake 环境监测与 UART 图片转发工程 |
| `zhihuijisuan2/sketch_apr14a/` | ESP8266 最新上传脚本 |
| `sketch_apr15a/` | ESP8266 上传脚本的独立旧目录，已同步到当前 JPEG 协议 |
| `拍照/untitled_2.py` | K210 按键拍照并保存到 SD 卡 |
| `训练集/` | 正常/异常两类样本；仓库只上传各 5 张代表图片 |
| PNG 文件 | 后端接口字段和请求头说明截图 |
| `model-264961.nncase.zip` 内容 | MaixHub 导出的 0.84 系列模型、`main.py` 和 `report.json` |
| `接线说明.md` | STM32、K210、ESP8266 和传感器的接线表 |

## 模型与脚本对应关系

K210 模型的 anchors 必须与脚本中的 anchors 完全对应，否则检测框会明显偏移。

| 脚本 | 模型 | 主要 anchors | 图像上传 |
|---|---|---|---|
| 根目录 `main.py` | 解压 `model-264961.nncase.zip` 后复制到 SD 卡的 `model-264961.kmodel` | `0.84,1.62,1.33,2.00,0.59,1.44,2.83,2.72,1.94,2.78` | 检测到病害后直接发送 JPEG |
| `清晰/main.py` | 复制 `清晰/plant_model.kmodel` 到 SD 卡根目录 | `2.16,2.03,2.34,2.69,2.94,2.62,3.38,3.06,3.92,3.11` | 发送检测文本 |
| `zhihuijisuan2/main.py` | 复制 `清晰/plant_model.kmodel` 到 SD 卡根目录 | 与 `清晰/report.json` 一致 | 延迟 3 帧并限制 10 秒冷却后发送 JPEG |

模型标签顺序均为：

```text
0 = normal
1 = disease
```

不能把 class 0 当作病害。原 `zhihuijisuan2/main.py` 使用 class 0 作为病害，
会把健康叶片误判为病害并触发图片上传。

## 硬件接线

详细表格见 [`接线说明.md`](接线说明.md)。核心连接如下：

| 功能 | STM32F103 | K210 / ESP8266 |
|---|---|---|
| 调试、K210 串口 | PA9 TX / PA10 RX | K210 IO9 RX / IO7 TX |
| ESP8266 串口 | PA2 TX / PA3 RX | ESP8266 RX / TX |
| DHT11 DATA | PA8 | DHT11 DATA，需 4.7 kOhm 上拉 |
| BH1750 | PB6 SCL / PB7 SDA | BH1750 SCL / SDA |
| 土壤湿度 | PA0 ADC1_IN0 | 模拟输出 |
| ESP8266 使能 | 3.3 V | CH_PD/EN 接高 |

所有设备必须共地。ESP8266 峰值电流可达约 300 mA，建议使用独立、稳定的
3.3 V 电源。

## STM32 代码功能

### 传感器采集

- DHT11 每 5 秒读取一次温度和整数湿度。
- 读取失败时保留上一次有效值，不再发送 0 度、0% 的伪数据。
- DHT11 微秒延时改由 Cortex-M3 DWT 周期计数器产生，Release 优化后仍能保持时序。
- BH1750 使用 I2C1，高速模式，计算 `lux = raw * 5 / 6`。
- 0 lux 被视为合法黑暗值，不再被误判为读取失败。
- 土壤湿度使用 ADC1_IN0，ADC 采样时间改为 239.5 周期，并把结果限制到 0..100%。

### UART 图片转发

- USART1 使用单字节接收中断，接收环形缓冲区大小为 4096 字节。
- 中断中检测 JPEG 开始/结束标记，并记录图片长度。
- USART2 使用中断发送。
- 每次发送前把环形缓冲区中的数据复制到独立 1024 字节发送缓冲区。
- 只有 HAL 成功启动中断发送后才推进环形缓冲区读指针。
- USART2 发送超时会调用 `HAL_UART_AbortTransmit()` 并恢复状态。
- JPEG 传输超过 10 秒没有结束时自动复位图片状态。
- USART 溢出等错误由 `HAL_UART_ErrorCallback()` 重新启动接收。

如果没有独立发送缓冲区，`HAL_UART_Transmit_IT()` 会在约 90 ms 内持续读取
接收环形区；K210 的新图片数据可能回绕并覆盖尚未发送的字节，造成图片损坏。

### 调试输出

`debug_printf()` 原先直接把 `vsnprintf()` 的返回值传给 `HAL_UART_Transmit()`。
`vsnprintf()` 返回的是“原本需要的长度”，可能大于 64 字节栈缓冲区，造成越界读。
现在长度会限制到实际缓冲区容量以内。

## ESP8266 上传协议

### 传感器 JSON

STM32 通过 USART2 发送：

```text
SENSOR:{"id":1,"airTemp":25,"airHumidity":60,"soilMoisture":40,"lightIntensity":500,"createTime":"1970-01-01T00:00:00.000Z"}
```

ESP8266 去掉 `SENSOR:`，把创建时间替换为 NTP 时间，然后请求：

```text
POST /machine/machineRecord
Content-Type: application/json
```

字段名与接口截图一致：

```text
id
soilMoisture
airTemp
airHumidity
lightIntensity
createTime
```

### 病害图片

ESP8266 在串口字节流中查找 `FF D8` 和 `FF D9`，收到完整 JPEG 后请求：

```text
POST /machine/machineCamera
Content-Type: image/jpeg
diseaseResult: 1
warning: Disease detected
create_time: 2026-01-01T12:00:00.000Z
id: 1
```

图片文件头、文件尾和长度都会再次校验。缓冲区不足时丢弃整张图片，不再上传
截断的 JPEG。

## 构建与验证

### STM32

工程目标为 STM32F103C6T6，C 语言标准 C11，CMake + Ninja + GNU Arm GCC。

```powershell
$tool = "C:\path\to\gnu-tools-for-stm32\bin"
$ninja = "C:\path\to\ninja"
$env:PATH = "$tool;$ninja;$env:PATH"

cmake -S . -B build/Release -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/gcc-arm-none-eabi.cmake" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel
```

本次 Debug 重建结果：

```text
0 Error, 0 Warning
RAM:   7,736 B / 10 KB (75.55%)
FLASH: 25,836 B / 32 KB (78.85%)
```

### K210

1. 把脚本和对应 `.kmodel` 复制到 TF 卡。
2. 根目录脚本使用 `/sd/model-264961.kmodel`。
3. `清晰/main.py` 和 `zhihuijisuan2/main.py` 使用 `/sd/plant_model.kmodel`。
4. K210 UART1 使用 IO7 TX、IO9 RX，波特率 115200。

### ESP8266

1. 安装 ESP8266 Arduino Core。
2. 打开 `sketch_apr14a.ino` 或 `sketch_apr15a.ino`。
3. 修改顶部的 Wi-Fi、服务器 IP 和端口。
4. 选择 ESP-01S 对应 Flash 大小并烧录。

仓库包含的 ESP8266 脚本已经去除真实 Wi-Fi 密码，默认值为：

```cpp
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
const char *host = "YOUR_SERVER_IP";
```


## 已知限制

- STM32 已完整编译验证；4 个 MaixPy 脚本通过 Python 语法检查。
- 图片协议没有硬件流控。虽然已修复发送缓冲覆盖问题，但 ESP8266 长时间阻塞时
  仍可能因 4 KB STM32 接收环形缓冲区不足而丢图。
- 模型精度有限。选中 `model-264961` 的 `report.json` 中，病害 AP 约为 0.59，
  验证准确率约为 0.795，适合作原型，不适合直接用于生产诊断。
- `zhihuijisuan2/main.py` 使用 `/sd/plant_model.kmodel`，运行时需要手动把
  `清晰/plant_model.kmodel` 复制到 TF 卡根目录。
- 后端 IP 是局域网地址，更换网络后必须修改 ESP8266 源码。
- UTC 时间以 `Z` 后缀上传，但 NTP `configTime()` 的时区参数是 UTC+8。若后端
  要求严格 UTC，应把时区参数改为 0；当前保留原始项目的东八区行为。
- 为控制仓库体积，仓库不包含 318 MB 的训练数据归档，只上传正常/异常各 5 张
  代表图片。模型、截图、样本图片和 CMSIS 预编译库使用 Git LFS 保存。
- 第三方 HAL、模型、训练数据和接口截图仍受各自原始许可约束。

## 安全说明

原始 ESP8266 源码包含真实 Wi-Fi 名称和密码。新仓库中上传的源码已替换为
占位符；`zhihuibianjie.zip` 和 `zhihuijisuan2.zip` 两个旧版快照按要求不包含
在仓库中。已经暴露过的密码仍应立即在路由器中修改。用于创建仓库的 GitHub
Classic PAT 也建议在推送完成后撤销。
