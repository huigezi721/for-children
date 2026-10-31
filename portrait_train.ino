/*
 * 画像认知训练 - ESP32 硬件端（固定 IP / App 联动版）
 * 针对自闭症儿童的多感官交互教育工具
 *
 * 玩法：
 *   App 出题（如"妈妈"）-> 通过 HTTP 通知本设备设置正确答案
 *   孩子在实体设备上按对应画像按钮 -> 按对亮绿灯，按错亮红灯（蜂鸣器提示）
 *   正确答案随出题对象动态变化
 *
 * 与 App 端约定：
 *   网段固定为 192.168.1.x，本机固定 IP 192.168.1.50（见下方 NET 配置）
 *   App 只需填 http://192.168.1.50 一次
 *
 * 硬件接线（ESP32 DevKit 经典款 + 交通灯模块 + 无源蜂鸣器 + 6 个按键）：
 *   交通灯红灯   GPIO 27
 *   交通灯黄灯   GPIO 26
 *   交通灯绿灯   GPIO 25
 *   蜂鸣器(无源) GPIO 33
 *   按键1(妈妈)  GPIO 15
 *   按键2(爸爸)  GPIO 2
 *   按键3(爷爷)  GPIO 4
 *   按键4(奶奶)  GPIO 5
 *   按键5(哥哥)  GPIO 18
 *   按键6(姐姐)  GPIO 19
 *
 * HTTP 接口：
 *   GET  /question?char=xxx   出题，设置正确答案（xxx 为画像 key）
 *   GET  /clear               清除状态，回到待出题
 *   GET  /status              查询当前状态与最近一次判定结果
 *   GET  /                    测试页
 *
 * 状态机：IDLE -> QUESTIONING -> CORRECT_FB / WRONG_FB -> 回到 QUESTIONING
 */

#include <WiFi.h>
#include <WebServer.h>

// ============ 修改为你自己的 WiFi ============
const char* ssid     = "A511";
const char* password = "adsl1234";

// ============ 固定 IP（必须与手机在同一网段）============
// 建议改成路由器 DHCP 池之外的地址，避免和别的设备抢 IP
IPAddress NET_LOCAL (192, 168, 1, 50);
IPAddress NET_GATEWAY(192, 168, 1, 1);
IPAddress NET_SUBNET (255, 255, 255, 0);

// ============ 画像映射（与 App 端保持一致）============
struct Portrait {
  const char* key;    // 出题 key（HTTP 里用的标识）
  const char* name;   // 中文名
  uint8_t     pin;    // 对应按键 GPIO
};

Portrait portraits[] = {
  { "mama",   "妈妈", 15 },
  { "baba",   "爸爸", 2  },
  { "yeye",   "爷爷", 4  },
  { "nainai", "奶奶", 5  },
  { "gege",   "哥哥", 18 },
  { "jiejie", "姐姐", 19 },
};
const int PORTRAIT_COUNT = sizeof(portraits) / sizeof(portraits[0]);

// ============ GPIO ============
const int PIN_LED_RED    = 27;
const int PIN_LED_YELLOW = 26;
const int PIN_LED_GREEN  = 25;
const int PIN_BUZZER     = 33;

// ============ 状态机 ============
enum AppState { IDLE, QUESTIONING, CORRECT_FB, WRONG_FB };
AppState state = IDLE;
String currentAnswer = "";      // 当前正确答案的 key
String lastPressed   = "";      // 最近一次按键的 key
bool   lastCorrect   = false;   // 最近一次判定是否正确
unsigned long pressSeq = 0;     // 按键序号，每按一次 +1，App 用它去重
unsigned long fbUntil  = 0;     // 反馈结束时间

WebServer server(80);

// ============ 按键去抖变量 ============
bool lastRawState[PORTRAIT_COUNT];
bool lastStableState[PORTRAIT_COUNT];
unsigned long lastDebounceTime[PORTRAIT_COUNT];
const unsigned long DEBOUNCE = 40;

void setupIO() {
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  for (int i = 0; i < PORTRAIT_COUNT; i++) {
    pinMode(portraits[i].pin, INPUT_PULLUP);   // 内部上拉，按下为 LOW
    lastRawState[i] = HIGH;
    lastStableState[i] = HIGH;
    lastDebounceTime[i] = 0;
  }
}

// 无源蜂鸣器方波驱动
void buzzTone(int freq, int ms) {
  tone(PIN_BUZZER, freq, ms);
}

void lightsAllOff() {
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);
}

void lightsCorrect() {
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_GREEN, HIGH);
}

void lightsWrong() {
  digitalWrite(PIN_LED_RED, HIGH);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);
}

// key -> 中文名
String nameOfKey(const String& key) {
  for (int i = 0; i < PORTRAIT_COUNT; i++) {
    if (key == portraits[i].key) return String(portraits[i].name);
  }
  return String("");
}

// ============ CORS（跨域，浏览器调试用）============
void addCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Max-Age", "86400");
}

// ============ HTTP 处理 ============
void sendJson(String body) {
  addCors();
  server.send(200, "application/json; charset=utf-8", body);
}

void handleOptions() {
  addCors();
  server.send(204);
}

void handleQuestion() {
  if (!server.hasArg("char")) {
    sendJson("{\"error\":\"missing char\"}");
    return;
  }
  String c = server.arg("char");
  bool found = false;
  for (int i = 0; i < PORTRAIT_COUNT; i++) {
    if (c == portraits[i].key) { found = true; break; }
  }
  if (!found) {
    sendJson("{\"error\":\"unknown char\"}");
    return;
  }
  currentAnswer = c;
  lastPressed = "";
  lastCorrect = false;
  state = QUESTIONING;
  lightsAllOff();
  buzzTone(880, 120);
  sendJson("{\"state\":\"QUESTIONING\",\"currentAnswer\":\"" + c
           + "\",\"name\":\"" + nameOfKey(c) + "\"}");
}

void handleClear() {
  currentAnswer = "";
  lastPressed = "";
  lastCorrect = false;
  state = IDLE;
  lightsAllOff();
  sendJson("{\"state\":\"IDLE\"}");
}

void handleStatus() {
  String st;
  switch (state) {
    case IDLE:         st = "IDLE"; break;
    case QUESTIONING:  st = "QUESTIONING"; break;
    case CORRECT_FB:   st = "CORRECT"; break;
    case WRONG_FB:     st = "WRONG"; break;
    default:           st = "IDLE"; break;
  }
  String json = "{\"state\":\"" + st
              + "\",\"currentAnswer\":\"" + currentAnswer
              + "\",\"answerName\":\"" + nameOfKey(currentAnswer)
              + "\",\"lastPressed\":\"" + nameOfKey(lastPressed)
              + "\",\"pressedKey\":\"" + lastPressed
              + "\",\"correct\":" + (lastCorrect ? "true" : "false")
              + ",\"seq\":" + String(pressSeq)
              + "}";
  sendJson(json);
}

void handleRoot() {
  addCors();
  String html = "<html><head><meta charset='utf-8'><title>画像认知训练</title></head><body>"
                "<h2>画像认知训练 - ESP32</h2>"
                "<p>状态: " + String(state) + "</p>"
                "<p>当前答案key: " + currentAnswer + "</p>"
                "<p>接口: <code>/question?char=mama</code> <code>/status</code> <code>/clear</code></p>"
                "<p>CORS 已开启，可直接用浏览器网页连接。</p>"
                "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void setupServer() {
  server.on("/", handleRoot);
  server.on("/question", HTTP_OPTIONS, handleOptions);
  server.on("/clear",    HTTP_OPTIONS, handleOptions);
  server.on("/status",   HTTP_OPTIONS, handleOptions);
  server.on("/",         HTTP_OPTIONS, handleOptions);
  server.on("/question", handleQuestion);
  server.on("/clear", handleClear);
  server.on("/status", handleStatus);
  server.begin();
}

void onButtonPressed(String key) {
  pressSeq++;
  lastPressed = key;
  bool correct = (key == currentAnswer);
  lastCorrect = correct;
  if (correct) {
    state = CORRECT_FB;
    lightsCorrect();
    buzzTone(1200, 250);
    fbUntil = millis() + 6000;   // 兜底：App 掉线时 6 秒后自动回待答
  } else {
    state = WRONG_FB;
    lightsWrong();
    buzzTone(300, 250);
    fbUntil = millis() + 6000;
  }
}

// ============ 按键扫描（标准去抖 + 下降沿触发）============
void updateButtonsAndState() {
  // 若反馈阶段结束，回到 QUESTIONING 可重试
  if ((state == CORRECT_FB || state == WRONG_FB) && millis() >= fbUntil) {
    state = QUESTIONING;
    lightsAllOff();
  }

  // 只有在 QUESTIONING（或反馈后回到该态）时才接受按键
  if (state != QUESTIONING) return;

  for (int i = 0; i < PORTRAIT_COUNT; i++) {
    bool reading = digitalRead(portraits[i].pin); // INPUT_PULLUP，按下为 LOW

    if (reading != lastRawState[i]) {
      lastRawState[i] = reading;
      lastDebounceTime[i] = millis();
    }

    if ((millis() - lastDebounceTime[i]) > DEBOUNCE
        && reading != lastStableState[i]) {
      lastStableState[i] = reading;
      if (reading == LOW) {
        onButtonPressed(portraits[i].key);
      }
    }
  }
}

// ============ 主程序 ============
void setup() {
  Serial.begin(115200);
  setupIO();

  WiFi.mode(WIFI_STA);
  WiFi.config(NET_LOCAL, NET_GATEWAY, NET_SUBNET);   // 固定 IP
  WiFi.begin(ssid, password);
  Serial.print("连接 WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  WiFi.setSleep(false);   // 关 modem sleep，响应更快
  Serial.println("\n连接成功");
  Serial.print("固定 IP: ");
  Serial.println(WiFi.localIP());

  setupServer();
  Serial.println("HTTP 服务已启动（CORS 已开启）");
  Serial.print("App 里填写: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  updateButtonsAndState();
  delay(5);
}
