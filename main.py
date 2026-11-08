from fpioa_manager import fm
from machine import UART
import sensor, image, time, lcd
from maix import KPU
import gc

# ==================== 串口配置 ====================
fm.register(7, fm.fpioa.UART1_TX, force=True)
fm.register(9, fm.fpioa.UART1_RX, force=True)
uart1 = UART(UART.UART1, 115200, 8, 0, 0, timeout=100, read_buf_len=256)

temp = 0
humi = 0
light = 0
soil = 0
disease_detected = False

# ==================== 图片传输控制 ====================
SEND_COOLDOWN = 10000  # 冷却时间10秒，避免重复发送
IMG_QUALITY = 50      # JPEG压缩质量 (降低质量，减小体积)
# ticks_ms() 是开机计时，不能默认写 0，否则上电后的前 10 秒会误判为冷却中。
last_send_time = time.ticks_ms() - SEND_COOLDOWN

# 选中的 model-264961 模型标签顺序为 ["normal", "disease"]。
DISEASE_CLASS_ID = 1

# 初始化显示与摄像头
lcd.init()
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)  # 320x240
sensor.set_windowing((224, 224))     # 推理输入和屏幕显示使用同一个 224x224 区域
sensor.skip_frames(time=1000)
clock = time.clock()

# 创建推理图像缓冲
od_img = image.Image(size=(224, 224))

# ==================== 模型参数 ====================
anchor = (0.84, 1.62, 1.33, 2.0, 0.59, 1.44, 2.83, 2.72, 1.94, 2.78)

# 加载模型。加载或初始化失败时必须停止推理，不能在空模型上继续调用 YOLO 层。
kpu = KPU()
try:
    kpu.load_kmodel("/sd/model-264961.kmodel")
    kpu.init_yolo2(anchor, 5, 224, 224, 224, 224, 7, 7, 0.5, 0.3, 2)
    print("Model loaded successfully.")
except Exception as e:
    print("Failed to load model:", e)
    try:
        lcd.clear(lcd.RED)
        lcd.draw_string(20, 100, "MODEL LOAD FAILED", lcd.WHITE, lcd.RED)
    except Exception:
        pass
    raise

# ==================== 串口数据解析 ====================
def parse_data(line):
    global temp, humi, light, soil
    try:
        if line.startswith("DATA:"):
            data = line.replace("DATA:", "").split(",")
            temp = int(data[0])
            humi = int(data[1])
            soil = int(data[2])
            light = int(data[3])
    except:
        pass

# ==================== 图片发送函数 ====================
def send_disease_image():
    """检测到病害时拍照并通过串口发送 - 直接JPEG传输版本"""
    global last_send_time
    current_time = time.ticks_ms()

    # 冷却检查：避免频繁发送
    if time.ticks_diff(current_time, last_send_time) < SEND_COOLDOWN:
        return False

    try:
        # 重新拍一张干净的照片（不带标注框）
        photo = sensor.snapshot()
        time.sleep_ms(50)  # 等传感器稳定

        # JPEG压缩：平衡画质和传输速度
        jpeg_img = photo.compress(quality=IMG_QUALITY)
        img_len = jpeg_img.size()

        # 新的传输协议：直接发送JPEG数据
        # STM32会自动检测JPEG的0xFFD8和0xFFD9标记
        uart1.write(jpeg_img)  # 直接发送JPEG二进制数据

        last_send_time = current_time
        print("[IMG] Sent: %d bytes" % img_len)

        # 等待足够时间让STM32处理完成
        time.sleep_ms(3000)  # 等待3秒确保传输完成

        return True

    except Exception as e:
        print("[IMG] Send error:", e)
        return False

# ==================== 主循环 ====================
while True:
    clock.tick()
    img = sensor.snapshot()
    disease_detected = False

    # 读取串口传感器数据
    try:
        if uart1.any():
            data = uart1.readline().decode().strip()
            parse_data(data)
    except:
        pass

    # AI 推理
    od_img.draw_image(img, 0, 0)
    od_img.pix_to_ai()
    kpu.run_with_output(od_img)
    dets = kpu.regionlayer_yolo2()

    if dets:
        for d in dets:
            # regionlayer_yolo2 返回: [x, y, w, h, class_id, confidence]
            x, y, w, h = d[0], d[1], d[2], d[3]
            class_id = int(d[4])

            # 根据 JSON labels: ["normal", "disease"] -> 0:normal, 1:disease
            if class_id == DISEASE_CLASS_ID:
                disease_detected = True
                img.draw_rectangle(x, y, w, h, color=(255, 0, 0))
                img.draw_string(x, y-10 if y>10 else y+2, "DISEASE",
                              color=(255, 255, 0), scale=1)
                # 🔥 检测到病害，立即拍照发送给STM32
                send_disease_image()
            else:  # normal
                img.draw_rectangle(x, y, w, h, color=(0, 255, 0))
                img.draw_string(x, y-10 if y>10 else y+2, "NORMAL",
                              color=(0, 255, 0), scale=1)

    # 绘制传感器信息
    img.draw_string(2, 2, "MODEL:OK", color=(0,255,0), scale=1)
    img.draw_string(2, 20, "Temp:%d C" % temp, scale=1)
    img.draw_string(2, 35, "Humi:%d %%" % humi, scale=1)
    img.draw_string(2, 50, "Light:%d lx" % light, scale=1)
    img.draw_string(2, 65, "Soil:%d %%" % soil, scale=1)

    # 状态显示
    if disease_detected:
        img.draw_string(2, 85, "STATUS:DISEASE", color=(255,0,0), scale=1)
    else:
        img.draw_string(2, 85, "STATUS:NORMAL", color=(0,255,0), scale=1)

    lcd.display(img)
    gc.collect()

kpu.deinit()
