# ESP32 WiFi 主从通讯（Arduino）

两台 ESP32：一台开热点（AP），一台连接（STA）。TCP 匹配成功后互相发数据。

两套独立示例，不要混烧。

| 工作方式 | 主机 | 从机 |
|----------|------|------|
| 定时文本 | `ap_timer` | `sta_timer` |
| 命令帧 | `ap_cmd` | `sta_cmd` |

## 准备

1. Arduino IDE 2.x
2. 开发板管理器安装 `esp32`（Espressif）
3. 开发板选 **ESP32 Dev Module**
4. 两台 ESP32、两根 USB，两个串口监视器，波特率 **115200**，换行选 **Newline**

共同网络：SSID `ESP32_LINK`，密码 `esp32link`，AP `192.168.4.1`，TCP `8080`。

推荐上电顺序：先 AP，再 STA。STA 会自动重试，顺序反了也能连上，只是多等几秒。

## Timer 验收

1. 打开 `ap_timer`，烧录并打开串口。应看到 `AP SSID=ESP32_LINK` 和 `waiting for STA...`
2. 打开 `sta_timer`，烧录并打开串口。应看到 `WiFi connecting...`，然后 `WiFi IP=`，然后 `tcp connected`
3. AP 应出现 `client matched`
4. 两边每秒出现 `tx:` 和 `rx:`，对端 `seq=` 递增
5. 拔掉 STA USB 再插回：STA 重连，AP 再次 `client matched`，`seq` 从 1 再计

## Cmd 验收

油门硬件：AP 摇杆 ADC 接 GPIO32；STA 电调信号接 GPIO23。先 AP 后 STA。STA 上电约 5 秒电调自检期间 TCP/LED 仍可用，油门指令会等到自检结束再落地。

1. 打开 `ap_cmd` 烧录并开串口，再打开 `sta_cmd` 烧录并开串口
2. 匹配成功后两边周期性出现 `tx: PING` 和 `rx: PONG`（也会看到对端的 `rx: PING` 与本端 `tx: PONG`）
3. 在 AP 串口输入 `on` 回车 → STA 板载灯亮，AP 出现 `rx: LED_ACK on`
4. 在 STA 串口输入 `off` 回车 → AP 板载灯灭，STA 出现 `rx: LED_ACK off`
5. 输入 `foo` → 打印 `unknown cmd`
6. 若板载灯电平相反：只改对应 sketch 顶部的 `LED_ON` / `LED_OFF`
7. AP 上电打印 `Center Value:`。摇杆回中时 STA 电调保持最低油门；推高则加速，松开则回最低油门
8. AP 仅在真正发送时打印 `tx: THROTTLE <duty>`，不要每 10ms 刷 ADC
9. 拔掉 STA：电机立刻停。插回匹配后，若摇杆仍在高位，电机恢复到对应油门

坏帧（checksum 错或未知 cmd）打印 `bad frame`，TCP 保持连接，之后 ping/pong 仍继续。油门坏帧（len≠2）同样只打印 `bad frame`，电机保持当前 duty（断线除外）。

## Cmd 协议

AP 和 STA 用同一套二进制帧，必须两边一起改，否则对端会当成 `bad frame`。

### 帧格式

```
偏移    大小        字段
0       1           同步头 0xAA
1       1           同步头 0x55
2       1           cmd（命令码）
3       1           len（payload 字节数，0–32）
4       len         payload（可为 0 字节）
4+len   1           checksum
```

- 最小帧 5 字节（无 payload）
- checksum = `cmd XOR len XOR payload[0] XOR ... XOR payload[len-1]`（不含两个同步头）
- 例子：PING = `AA 55 01 00 01`；SET_LED on = `AA 55 03 01 01 03`；SET_THROTTLE duty=205 → `AA 55 05 02 00 CD cs`（cs = 0x05 XOR 0x02 XOR 0x00 XOR 0xCD）

### 指令表

| cmd | 宏名 | payload | 谁发 | 收到后做什么 |
|-----|------|---------|------|--------------|
| `0x01` | `CMD_PING` | 无（len=0） | 双方每秒各发一次 | 立刻回 `CMD_PONG` |
| `0x02` | `CMD_PONG` | 无（len=0） | 收到 PING 的一方 | 串口打印 `rx: PONG` |
| `0x03` | `CMD_SET_LED` | 1 字节：`0x00` 灭，`0x01` 亮 | 本机串口输入 `on` / `off` | 写 GPIO 2，回 `CMD_LED_ACK`（回显同一状态） |
| `0x04` | `CMD_LED_ACK` | 1 字节：回显的亮灭 | 被控端 | 串口打印 `rx: LED_ACK on` 或 `off` |
| `0x05` | `CMD_SET_THROTTLE` | 2 字节大端 uint16：PWM duty（205–410） | AP：duty 变化或 TCP 刚匹配 | STA 钳位后写电调 PWM，无 ACK |
| `0x06`…`0xFF` | （未使用） | — | — | 当前会打印 `bad frame` 并丢弃，不断开 TCP |

串口只认两行文本（与 cmd 码的对应）：`on` → SET_LED(1)，`off` → SET_LED(0)。其他非空行打印 `unknown cmd`。

### 以后怎么加新指令

`ap_cmd/ap_cmd.ino` 和 `sta_cmd/sta_cmd.ino` 各改一遍，命令码和 payload 必须一致。假设要加「蜂鸣」`CMD_BEEP = 0x06`，payload 1 字节表示时长：

1. **占用一个未用 cmd**  
   在两份 sketch 顶部宏区追加 `#define CMD_BEEP 0x06`。不要复用 `0x01`–`0x05`。

2. **补日志名**  
   在 `cmdName()` 的 `switch` 里加 `case CMD_BEEP: return "BEEP";`。

3. **补接收逻辑**  
   在 `handleFrame()` 加 `case CMD_BEEP:`：先检查 `len` 是否符合约定（不符就 `Serial.println("bad frame"); return;`），再执行动作。需要应答时调用 `sendFrame(CMD_XXX, payload, len)`。

4. **补发送入口（三选一或组合）**  
   - 串口触发：在 `handleSerial()` 增加例如 `beep` → `sendFrame(CMD_BEEP, ...)`  
   - 定时触发：在 `loop()` 里像 PING 那样按间隔 `sendFrame`  
   - 作为应答：只在某个 `handleFrame` case 里回包，不必加串口

5. **可选：更友好的串口打印**  
   若 payload 需要写成 `on`/`ms=20` 这类文字，改 `logFrame()`；不改则只打印命令名。

6. **改本表**  
   在上面的指令表加一行，写清 cmd、payload 长度和含义、谁发、收到后做什么。

约束：`len` 不能超过 32（`MAX_PAYLOAD`）。未知 cmd 和校验失败都只打印 `bad frame`，不会断 TCP。

## 两种工作方式差在哪

- Timer：TCP 上直接发一行可读文本，对端当字符串打印。适合先确认链路通。
- Cmd：TCP 上发 `[AA 55][cmd][len][payload][xor]`，对端按命令码做事（回 PONG、改 LED）。适合后续加控制指令。
