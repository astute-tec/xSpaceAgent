# wifi-provision —— 扫码配网固件

给 **Waveshare ESP32-S3-Touch-AMOLED-1.75C** 用的最小配网固件：屏幕出二维码，手机扫码直接入网，选 WiFi、用手机键盘输密码，**不在 1.75 寸屏上打字**。

## 它做什么

1. 开机起**开放热点** `Xiaozhi-XXXX`（无密码，点一下就进）
2. 屏幕上画**配置页网址二维码**（`http://192.168.4.1/`）+ 6 位配对码
3. 手机连热点后自动弹页；没弹就相机扫码或手输地址
4. 强制门户自动弹出配置页：
   - 扫到的 WiFi 列表
   - 密码框（手机键盘输入，可显示明文）
   - 6 位配对码（防止别人误配）
5. 保存 → 设备连接 → **成功：白底黑勾**；**失败：白底黑叉 + 3 位原因码**，8 秒后回二维码
6. 凭据存 NVS，**开机自动重连**

## 构建与烧录

```bash
source ~/esp/esp-idf/export.sh
cd firmware/wifi-provision
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

串口看状态：

```bash
idf.py -p /dev/cu.usbmodem1101 monitor
# 或
python -m serial.tools.miniterm /dev/cu.usbmodem1101 115200
```

启动日志里会有：

```
prov: READY ap=Xiaozhi-54D9 pass=37788460 pair=038125 url=http://192.168.4.1
```

## 串口直连配网（**最可靠，推荐现场排查用**）

不用手机、不用扫码，USB 线上直接下发：

```bash
python3 tools/wifi_set.py "DYZN88"          # 交互式输入密码
python3 tools/wifi_set.py "DYZN88" --password xxx
```

或手动用串口工具发命令（115200）：

```
WIFI <ssid> <password>   # 下发并连接，返回 WIFI_OK ip=... 或 WIFI_FAIL reason=...
SCAN                     # 打印当前可见 WiFi
STATUS                   # 打印当前状态
```

成功输出：

```
prov: CLI_SET ssid="DYZN88" passlen=8
prov: CONNECTED to DYZN88, ip=192.168.2.28
prov: WIFI_OK ip=192.168.2.28
```

## 恢复原厂固件

刷机前已把整片 32MB Flash 备份到 `backup/amoled175c_factory_20261116.bin`（SHA256 见 `backup/SHA256SUMS`）。

```bash
cd ~/ESP32Dev
.venv/bin/esptool -b 921600 --port /dev/cu.usbmodem1101 \
  write-flash 0x0 backup/amoled175c_factory_20261116.bin
```

## 硬件信息（来自厂家 BSP，非猜测）

| 项 | 值 |
|---|---|
| 屏 | CO5300 AMOLED 466×466，QSPI（CS 12 / PCLK 38 / D0–D3 4,5,6,7 / RST 1） |
| 触摸 | CST9217（I2C 15/14，RST 2，INT 11）—— 本固件未用 |
| 音频 | ES8311（输出）+ ES7210（双麦），MCLK 16 / BCLK 9 / WS 45 / DIN 10 / DOUT 8，PA 46 |
| 电源 | AXP2101 @0x34（DC1 3.3V、ALDO1 3.3V 供麦） |
| Flash/PSRAM | 32MB / 8MB octal |

## 已知限制

- 只有配网，没有语音功能（主固件会复用这里的 `display.c` 与配网逻辑）
- 配对码每次重启都变（随机），页面上的旧码会失效
- **微信扫一扫打不开本地地址**，请用系统相机或浏览器直接输 `192.168.4.1`
- 热点开放 3–5 分钟窗口，靠配对码防止他人误配
- 扫到的 WiFi 列表包含隐藏网络以外的全部 2.4G 网络
- 成功后停在"白底黑勾"界面，不自动跳转到主应用（主应用尚未合并）
- 结果界面故意用黑白：该面板 RGB565 字节序与直观相反，彩色容易显示成错误颜色

## 目录

```
main/
  main.c          配网流程、HTTP、DNS 劫持、状态机
  display.c/h     CO5300 + AXP2101 初始化（照搬厂家 BSP）
  qrcodegen.c/h   Nayuki QR 生成库（MIT）
  web/index.html  手机配置页（编译时嵌入）
```
