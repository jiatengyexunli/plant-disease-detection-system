import sensor, lcd, os, time
from fpioa_manager import fm
from Maix import GPIO

# ======================
# 初始化
# ======================
lcd.init()
lcd.rotation(2)

sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.skip_frames(time=1000)

# 按键 IO18
fm.register(18, fm.fpioa.GPIO0)
key = GPIO(GPIO.GPIO0, GPIO.IN, GPIO.PULL_UP)

# 蜂鸣器 IO17
fm.register(17, fm.fpioa.GPIO1)
beep = GPIO(GPIO.GPIO1, GPIO.OUT)
beep.value(0)  # 默认关闭

# 挂载SD卡
try:
    os.mountsd()
except:
    pass

photo_num = 0
# 重新上电后从 SD 卡中已有序号继续，避免反复覆盖 photo_1.jpg。
while os.path.exists("/sd/photo_%d.jpg" % (photo_num + 1)):
    photo_num += 1

# ======================
# 主循环
# ======================
while True:
    img = sensor.snapshot()
    lcd.display(img)

    if key.value() == 0:
        time.sleep_ms(20)
        if key.value() == 0:
            photo_num += 1
            fn = "/sd/photo_%d.jpg" % photo_num

            # 拍照闪屏特效
            lcd.clear(lcd.WHITE)
            beep.value(1)    # 蜂鸣器响
            time.sleep_ms(60)
            lcd.display(img)
            beep.value(0)    # 关闭

            # 保存失败不能让拍照程序退出，屏幕上给出明确提示。
            try:
                img.save(fn)
                lcd.draw_string(70, 100, "PHOTO SAVED", lcd.RED, lcd.WHITE)
                lcd.draw_string(60, 125, fn, lcd.RED, lcd.WHITE)
            except Exception as e:
                print("[PHOTO] save failed:", e)
                lcd.draw_string(55, 100, "SAVE FAILED", lcd.RED, lcd.WHITE)
                lcd.draw_string(55, 125, "CHECK SD CARD", lcd.RED, lcd.WHITE)
            time.sleep_ms(800)

            # 等待松开按键
            while key.value() == 0:
                time.sleep_ms(10)

    time.sleep_ms(50)
