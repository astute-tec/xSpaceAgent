# 公网接入：设备 → Cloudflare → 6.6（唐僧）

目标：设备在**任何网络**（公司 WiFi、4G 热点、家里）都能把录音发进来，服务端在 6.6 上收，
处理完直接推给 OpenClaw「唐僧」。

```
设备 ──HTTPS──▶ xiaozhi.halmir.top ──Cloudflare 隧道──▶ 6.6:8787 ──本地──▶ openclaw agent --agent tangseng
```

---

## 一、6.6 上：起服务

### 1. 放代码

```bash
# 把仓库里的 hub/ 和 tools/ 拷到 6.6
mkdir -p ~/myWork-hub
# 从 Mac 拷过去（或 git clone）
rsync -av hub/ xspace@192.222.6.6:/home/xspace/myWork-hub/
```

### 2. 依赖

```bash
sudo apt install -y python3 ffmpeg
python3 -c "import serial" 2>/dev/null || pip3 install pyserial   # 只有 tools/ 需要

# whisper.cpp（本地 ASR）
git clone https://github.com/ggml-org/whisper.cpp ~/whisper.cpp
cd ~/whisper.cpp && cmake -B build && cmake --build build -j
mkdir -p ~/.cache/whisper
wget -O ~/.cache/whisper/ggml-medium.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin
```

> 6.6 是 x86，没有 Metal，`ggml-medium` 会慢（RTF≈1–2）。
> 想快就换 `ggml-small.bin`（466MB），中文质量略降。

### 3. 配置

```bash
mkdir -p ~/.config/mywork-hub
cat > ~/.config/mywork-hub/.env <<'EOF'
DEEPSEEK_API_KEY=sk-xxxx
DEEPSEEK_BASE_URL=https://api.deepseek.com/v1
LLM_MODEL_FAST=deepseek-flash
LLM_MODEL_PRO=deepseek-v4-pro
WHISPER_BIN=/home/xspace/whisper.cpp/build/bin/whisper-cli
WHISPER_MODEL=/home/xspace/.cache/whisper/ggml-medium.bin
MYWORK_ROOT=/home/xspace/myWork
EOF
chmod 600 ~/.config/mywork-hub/.env
```

### 4. 起服务

```bash
export DEVICE_TOKEN=$(openssl rand -hex 24)   # 记下来，设备要用
echo "设备 token: $DEVICE_TOKEN"

cd ~/myWork-hub
DEVICE_TOKEN=$DEVICE_TOKEN python3 scripts/public_relay.py --port 8787
# 另开一个终端验证：
curl -s localhost:8787/health
```

做成常驻服务：

```bash
sudo cp deploy/mywork-relay.service /etc/systemd/system/
sudo sed -i "s/__CHANGE_ME__/$DEVICE_TOKEN/" /etc/systemd/system/mywork-relay.service
sudo systemctl daemon-reload && sudo systemctl enable --now mywork-relay
```

---

## 二、Cloudflare 隧道

> 这一步需要**你的 Cloudflare 账号**（`halmir.top` 这个 zone 的权限），我这边没有凭据，
> 所以下面 3 条命令要你自己在 6.6 上执行，或者把 API Token 给我。

### 1. 装 cloudflared（6.6）

```bash
# Debian/Ubuntu x86_64
curl -L -o cloudflared.deb https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64.deb
sudo dpkg -i cloudflared.deb
```

### 2. 登录 + 建隧道 + 绑域名

```bash
cloudflared tunnel login
# 浏览器里选中 halmir.top 授权

cloudflared tunnel create xiaozhi
# 输出里会有一个 UUID，记下来

cloudflared tunnel route dns xiaozhi xiaozhi.halmir.top
```

### 3. 配配置（把 UUID 填进去）

```bash
sudo mkdir -p /etc/cloudflared
sudo cp ~/myWork-hub/deploy/cloudflared-config.yml /etc/cloudflared/config.yml
sudo sed -i "s/<TUNNEL-UUID>/实际UUID/" /etc/cloudflared/config.yml
sudo cp ~/.cloudflared/实际UUID.json /etc/cloudflared/
sudo cloudflared service install
sudo systemctl enable --now cloudflared
```

### 4. 验证

```bash
curl https://xiaozhi.halmir.top/health
# 期望：{"ok": true, ...}
```

第一访问可能要等 Cloudflare 证书签发（1–2 分钟）。

---

## 三、设备端

设备现在支持两种上传目标：

```
# 本地（内网直连，调试用）
HUB 192.168.2.27 8765

# 公网（HTTPS + token）
HUBURL https://xiaozhi.halmir.top/v1/upload <DEVICE_TOKEN>
```

用串口发命令即可（配网固件的 CLI）：

```bash
python3 - <<'EOF'
import serial, time
s = serial.Serial('/dev/cu.usbmodem1101', 115200, timeout=0.3)
s.write(b"HUBURL https://xiaozhi.halmir.top/v1/upload 你的token\n")
time.sleep(1)
print(s.read(4096).decode('utf-8','replace'))
EOF
```

---

## 四、安全

| 项 | 做法 |
|---|---|
| 认证 | 设备必须带 `Authorization: Bearer <DEVICE_TOKEN>`，服务端常量时间比较 |
| 大小 | 默认上限 32MB（`MAX_UPLOAD_MB`），超了直接 413 |
| 限流 | 每 IP 每分钟 30 次 |
| 传输 | 只有 Cloudflare 的 HTTPS，源站只监听 127.0.0.1:8787 |
| 不要 | ❌ 别把 8787 直接暴露到公网 ❌ 别把 token 写进 git |

**加强（可选）**：在 Cloudflare 上加 Access 策略，只允许特定邮箱/服务令牌访问，
这样即使 token 泄漏，外面也进不来。设备端不方便过 Access 的话就先不加。

---

## 五、当前状态

| 项 | 状态 |
|---|---|
| 中继服务 | ✅ 已实现并本机验证（401 / 202 / 转写 / 日报） |
| Cloudflare 隧道 | ⏳ 等账号授权（需要你操作） |
| 6.6 部署 | ⏳ 网络不通，暂时挡着 |
| 设备 HTTPS 上传 | ⏳ 固件要加（`HUBURL` 命令） |

**临时方案**：6.6 不通期间，中继可以先跑在 Mac 上，用 `cloudflared tunnel --url http://127.0.0.1:8787`
拿一个临时公网地址（`https://xxx.trycloudflare.com`）先验证设备公网上传。
