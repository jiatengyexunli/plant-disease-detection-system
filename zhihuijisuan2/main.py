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
last_send_time = 0
SEND_COOLDOWN = 10000  # 冷却时间10秒，避免重复发送
IMG_QUALITY = 55       # JPEG压缩质量 (降低质量，减小体积)
SEND_DELAY = 3         # 检测到病害后等3帧再拍，等摄像头稳定
pending_send = 0       # 待发送倒数计数
last_send_time = time.ticks_ms() - SEND_COOLDOWN

# 初始化显示与摄像头
lcd.init()
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)  # 320x240
sensor.set_windowing((224, 224))     # 模型输入为 224x224，避免只裁取左上角
sensor.skip_frames(time=1000)
clock = time.clock()

# 创建推理图像缓冲
od_img = image.Image(size=(224, 224))

# ==================== 跳帧推理 & GC控制 ====================
frame_count = 0
SKIP_FRAMES = 2        # 每2帧推理一次（可调为3）
GC_INTERVAL = 50       # 每50帧执行一次 gc.collect()
last_dets = []          # 缓存最近一次推理结果，非推理帧继续绘制

# ==================== 模型参数 ====================
anchor = (2.16, 2.03, 2.34, 2.69, 2.94, 2.62, 3.38, 3.06, 3.92, 3.11)
DISEASE_CLASS_ID = 1

# 加载模型。模型与本目录约定的 /sd/plant_model.kmodel 及 anchors 必须配套。
kpu = KPU()
try:
    kpu.load_kmodel("/sd/plant_model.kmodel")
    kpu.init_yolo2(anchor, 5, 224, 224, 224, 224, 7, 7, 0.8, 0.45, 2)
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
            # 格式: DATA:temp,humi,soil,light
            data = line.replace("DATA:", "").split(",")
            temp = int(data[0])
            humi = int(data[1])
            soil = int(data[2])
            light = int(data[3])
        elif line.startswith("SENSOR:") or line.startswith("{"):
            # 简单解析JSON格式，不使用json模块
            # 格式: {"id":1,"airTemp":25,"airHumidity":60,"soilMoisture":30,"lightIntensity":500,...}
            # 或者: SENSOR:{"id":1,"airTemp":25,"airHumidity":60,...}
            if line.startswith("SENSOR:"):
                line = line[7:]  # 去掉"SENSOR:"前缀

            # 简单字符串查找提取数字（不使用re模块）
            # 查找温度
            temp_idx = line.find('"airTemp":')
            if temp_idx != -1:
                # 找到冒号后的数字
                num_start = temp_idx + len('"airTemp":')
                num_end = line.find(',', num_start)
                if num_end == -1:
                    num_end = line.find('}', num_start)
                if num_end != -1:
                    temp_str = line[num_start:num_end].strip()
                    temp = int(temp_str)

            # 查找湿度
            humi_idx = line.find('"airHumidity":')
            if humi_idx != -1:
                num_start = humi_idx + len('"airHumidity":')
                num_end = line.find(',', num_start)
                if num_end == -1:
                    num_end = line.find('}', num_start)
                if num_end != -1:
                    humi_str = line[num_start:num_end].strip()
                    humi = int(humi_str)

            # 查找土壤湿度
            soil_idx = line.find('"soilMoisture":')
            if soil_idx != -1:
                num_start = soil_idx + len('"soilMoisture":')
                num_end = line.find(',', num_start)
                if num_end == -1:
                    num_end = line.find('}', num_start)
                if num_end != -1:
                    soil_str = line[num_start:num_end].strip()
                    soil = int(soil_str)

            # 查找光照
            light_idx = line.find('"lightIntensity":')
            if light_idx != -1:
                num_start = light_idx + len('"lightIntensity":')
                num_end = line.find(',', num_start)
                if num_end == -1:
                    num_end = line.find('}', num_start)
                if num_end != -1:
                    light_str = line[num_start:num_end].strip()
                    light = int(light_str)
    except Exception as e:
        print("[PARSE] Error:", e, "line:", line)

# ==================== 主循环 ====================
while True:
    clock.tick()
    frame_count += 1
    img = sensor.snapshot()
    disease_detected = False

    # 读取串口数据（传感器数据或调试信息）
    try:
        if uart1.any():
            # 读取所有可用数据
            raw_data = uart1.read(uart1.any())
            if raw_data:
                # 解码为字符串（可能包含多行）
                text = raw_data.decode().strip()
                # 按行分割
                lines = text.split('\n')
                for line in lines:
                    line = line.strip()
                    if line:
                        # 检查是否为传感器数据
                        if line.startswith("DATA:") or line.startswith("SENSOR:") or line.startswith("{"):
                            parse_data(line)
                        else:
                            # 调试信息，直接打印
                            print("[STM32] " + line)
    except Exception as e:
        print("[UART] Read error:", e)

    # AI 推理（跳帧：每SKIP_FRAMES帧推理一次）
    if frame_count % SKIP_FRAMES == 0:
        od_img.draw_image(img, 0, 0)
        od_img.pix_to_ai()
        kpu.run_with_output(od_img)
        dets = kpu.regionlayer_yolo2()
        last_dets = dets if dets else []

        # 检测到病害时启动延迟发送，不立刻拍照（避免 img.copy 内存峰值）
        if last_dets:
            for d in last_dets:
                # 模型标签顺序为 normal=0、disease=1，不能把正常叶片当病害。
                if (int(d[4]) == DISEASE_CLASS_ID and pending_send == 0 and
                        time.ticks_diff(time.ticks_ms(), last_send_time) >= SEND_COOLDOWN):
                    pending_send = SEND_DELAY
                    break

    # 所有帧绘制缓存的检测框，视觉上更连贯
    if last_dets:
        for d in last_dets:
            x, y, w, h = d[0], d[1], d[2], d[3]
            class_id = int(d[4])
            if class_id == DISEASE_CLASS_ID:
                disease_detected = True
                img.draw_rectangle(x, y, w, h, color=(255, 0, 0))
                img.draw_string(x, y-10 if y>10 else y+2, "DISEASE",
                              color=(255, 255, 0), scale=1)
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

    # 延迟发送逻辑：每帧递减计数，到0时拍一张稳定图发送
    if pending_send > 0:
        pending_send -= 1
        if pending_send == 0:
            photo = sensor.snapshot()
            jpeg_img = photo.compress(quality=IMG_QUALITY)
            try:
                img_bytes = jpeg_img.to_bytes()
            except AttributeError:
                try:
                    img_bytes = bytearray(jpeg_img)
                except:
                    uart1.write(jpeg_img)
                    img_bytes = None
            if img_bytes is not None:
                uart1.write(img_bytes)
            last_send_time = time.ticks_ms()
            time.sleep_ms(10)

    lcd.display(img)
    if frame_count % GC_INTERVAL == 0:
        gc.collect()

kpu.deinit()
