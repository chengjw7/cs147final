#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ESP32Servo.h>

// ========== 选择你的温湿度传感器 ==========
//#define USE_AHT20          // 如果是 AHT20，取消注释
#define USE_DHT20            // 你的板更可能是 DHT20

#ifdef USE_DHT20
  #include <DHT20.h>
  DHT20 dht20;
#else
  #include <Adafruit_AHTX0.h>
  Adafruit_AHTX0 aht;
#endif

// ========== Wi-Fi ==========
#define WIFI_SSID     "SETUP-4114"
#define WIFI_PASSWORD "bright8783alert"

// ========== ThingSpeak ==========
#define TS_CHANNEL_ID     "3199845"
// f1=tempC, f2=humidity, f3=ldr_raw, f4=command (OPEN/CLOSE/AUTO), f5=servo_deg
#define TS_READ_API_KEY   "JKJQFNBEDDVENAAM"
#define TS_WRITE_API_KEY  "A94QD484Q4O3IOLX"
static const char* TS_HOST = "https://api.thingspeak.com";

// ========== LDR ==========
#define LDR_PIN         34
#define LDR_SAMPLES     8
#define LUX_OPEN_TH     2500
#define LUX_CLOSE_TH    2000

// ========== 舵机 ==========
Servo servo;
#define SERVO_PIN       13
#define SERVO_MIN_DEG   0
#define SERVO_MAX_DEG   90
#define SERVO_STEP_DEG  5

// ========== 规则/时序 ==========
#define HOT_TEMP_C                30.0
#define HUMID_RH                  80.0

#define CTRL_PERIOD_MS            500
#define PRINT_PERIOD_MS           1500
#define SENSOR_MIN_INTERVAL_MS    1000   // ← 关键：3 秒采样一次
#define SENSOR_RETRY              3
#define SENSOR_RETRY_DELAY_MS     120     // 读失败后的短等待

#define CMD_POLL_MS               5000
#define UPLOAD_PERIOD_MS          20000   // TS 免费版 ≥15s

// ========== I2C ==========
#define SDA_PIN 21
#define SCL_PIN 22
#define I2C_CLOCK_HZ  50000       // 25~50k 更抗干扰

// ========== 可选：I²C 期间暂停 PWM(抗干扰) ==========
#define PAUSE_PWM_DURING_I2C  0   // 1=开启暂停，0=关闭（保持你原来的风格）

// ========== 全局状态 ==========
uint32_t lastPrint=0, lastCtrl=0;
uint32_t lastTHRead=0;        // 上次真正读取温湿度时间戳
uint32_t lastCmdPoll=0;
uint32_t lastUpload=0;

bool   g_bright=false;
int    g_servoPos=0;
String g_mode = "AUTO";       // "OPEN" / "CLOSE" / "AUTO"

float  g_lastT=NAN, g_lastH=NAN;
bool   g_hasGood=false;

bool   pwmEnabled = false;

// ========== 辅助 ==========
void servoPwmEnable(bool en){
  if (en && !pwmEnabled){
    servo.attach(SERVO_PIN);
    servo.write(g_servoPos);
    pwmEnabled = true;
  } else if (!en && pwmEnabled){
    servo.detach();
    pwmEnabled = false;
  }
}

int readLDRSmooth(){
  int sum=0;
  for(int i=0;i<LDR_SAMPLES;i++){ sum += analogRead(LDR_PIN); delay(2); }
  return sum / LDR_SAMPLES;
}

static inline bool plausible(float tC, float rh){
  return !isnan(tC) && !isnan(rh) && rh>=0 && rh<=100 && !(tC==0.0f && rh==0.0f);
}

void setServo(int deg){
  deg = constrain(deg, SERVO_MIN_DEG, SERVO_MAX_DEG);
  if (pwmEnabled) servo.write(deg);
  g_servoPos = deg;
}

void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi: connecting to %s", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED){
    Serial.print(".");
    delay(500);
    if (millis() - t0 > 30000){
      Serial.println("\nWiFi timeout, restarting...");
      ESP.restart();
    }
  }
  Serial.printf("\nWiFi OK, IP=%s\n", WiFi.localIP().toString().c_str());
}

// 预热
void thWarmup(){
#ifdef USE_DHT20
  for (int i=0;i<3;i++){ dht20.read(); delay(150); }
#else
  // AHTX0 无需特别预热，这里保持空或轻量访问
#endif
}

// 只在达到最小间隔时才真正读；否则返回缓存
bool getTempHum(float &tC, float &rh){
  uint32_t now = millis();
  if (now - lastTHRead < SENSOR_MIN_INTERVAL_MS){
    if (g_hasGood){ tC=g_lastT; rh=g_lastH; return true; }
    else return false;
  }

  bool ok=false;
  for (int i=0; i<SENSOR_RETRY; ++i){
#if PAUSE_PWM_DURING_I2C
    servoPwmEnable(false);
    delay(3);   // 让 PWM 停一下
#endif

#ifdef USE_DHT20
    int rc = dht20.read();  // 0 成功
    if (rc == 0){
      float tt = dht20.getTemperature();
      float hh = dht20.getHumidity();
      if (plausible(tt,hh)){ tC=tt; rh=hh; ok=true; }
    }
#else
    sensors_event_t hum, temp;
    if (aht.getEvent(&hum, &temp)){
      float tt = temp.temperature;
      float hh = hum.relative_humidity;
      if (plausible(tt,hh)){ tC=tt; rh=hh; ok=true; }
    }
#endif

#if PAUSE_PWM_DURING_I2C
    servoPwmEnable(true);
#endif

    if (ok) break;
    delay(SENSOR_RETRY_DELAY_MS);
  }

  lastTHRead = now;
  if (ok){ g_lastT=tC; g_lastH=rh; g_hasGood=true; }
  else if (g_hasGood){ tC=g_lastT; rh=g_lastH; ok=true; } // 用上次好值兜底
  return ok;
}

// 读取云端指令 field4
String tsPollCommand(){
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  String url = String(TS_HOST) + "/channels/" + TS_CHANNEL_ID + "/fields/4/last.txt?api_key=" + TS_READ_API_KEY;
  http.begin(url);
  int code = http.GET();
  String cmd;
  if (code == 200){
    cmd = http.getString();
    cmd.trim(); cmd.toUpperCase();
  }
  http.end();
  if (cmd=="OPEN" || cmd=="CLOSE" || cmd=="AUTO") return cmd;
  return "";
}

// 上传字段到 ThingSpeak: f1/f2(若有效), f3 LDR, f5 舵机角度
bool tsUpdate(int ldr){
  if (WiFi.status() != WL_CONNECTED) return false;
  String url = String(TS_HOST) + "/update?api_key=" + TS_WRITE_API_KEY
             + "&field3=" + String(ldr)
             + "&field5=" + String(g_servoPos);
  if (g_hasGood){
    url += "&field1=" + String(g_lastT, 2);
    url += "&field2=" + String(g_lastH, 2);
  }

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  String body = http.getString();
  http.end();

  if (code == 200 && body.toInt() > 0){
    Serial.printf("TS update OK: entry_id=%s\n", body.c_str());
    return true;
  }else{
    Serial.printf("TS update FAIL: http=%d body=%s\n", code, body.c_str());
    return false;
  }
}

void setup(){
  Serial.begin(115200);
  delay(300);
  Serial.println("\n🌞 Smart Curtain: 3s T/H + LDR + Servo + ThingSpeak");

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.setTimeOut(50);

#ifdef USE_DHT20
  if(!dht20.begin()){
    Serial.println("❌ DHT20 init fail (check 3V3/GND/SDA=21/SCL=22/0x38)");
  }else{
    Serial.println("✅ DHT20 OK, warm-up...");
    thWarmup();
  }
#else
  if(!aht.begin()){
    Serial.println("❌ AHT20 init fail");
  }else{
    Serial.println("✅ AHT20 OK");
  }
#endif

  servo.attach(SERVO_PIN);
  pwmEnabled = true;
  setServo(0);

  connectWiFi();

  // 允许上电后立刻第一次读
  lastTHRead = millis() - SENSOR_MIN_INTERVAL_MS;
}

void loop(){
  uint32_t now = millis();

  // 1) LDR 滞后判断
  int ldr = readLDRSmooth();
  if(!g_bright && ldr >= LUX_OPEN_TH)  g_bright = true;
  if( g_bright && ldr <= LUX_CLOSE_TH) g_bright = false;

  // 2) 温湿度（3s 节流 + 重试 + 缓存兜底）
  float tC=NAN, rh=NAN;
  bool thOK = getTempHum(tC, rh);

  // 3) 轮询云端指令（5s）
  if (now - lastCmdPoll >= CMD_POLL_MS){
    lastCmdPoll = now;
    String cmd = tsPollCommand();
    if (cmd.length() && cmd != g_mode){
      g_mode = cmd;
      Serial.printf("Cloud Command: %s\n", g_mode.c_str());
    }
  }

  // 4) 控制（每 500ms，平滑 ±5°）
  if(now - lastCtrl >= CTRL_PERIOD_MS){
    lastCtrl = now;
    int target = g_servoPos;
    if (g_mode == "OPEN") {
      target = SERVO_MAX_DEG;
    } else if (g_mode == "CLOSE") {
      target = SERVO_MIN_DEG;
    } else { // AUTO
      bool tooHotOrHumid = thOK && (tC >= HOT_TEMP_C || rh >= HUMID_RH);
      target = (g_bright && !tooHotOrHumid) ? SERVO_MAX_DEG : SERVO_MIN_DEG;
    }

    if (target > g_servoPos) g_servoPos = min(target, g_servoPos + SERVO_STEP_DEG);
    else if (target < g_servoPos) g_servoPos = max(target, g_servoPos - SERVO_STEP_DEG);
    setServo(g_servoPos);
  }

  // 5) 上传云端（20s）
  if (now - lastUpload >= UPLOAD_PERIOD_MS){
    lastUpload = now;
    tsUpdate(ldr);
    Serial.printf("LDR=%4d bright=%d Mode=%s Servo=%3d°  ",
                  ldr, g_bright, g_mode.c_str(), g_servoPos);
    if (thOK) Serial.printf("Temp=%.2f°C RH=%.2f%%\n", tC, rh);
    else      Serial.println("Temp/RH=NA");
  }

  // 6) 本地打印（1.5s，非必须）
  if (now - lastPrint >= PRINT_PERIOD_MS){
    lastPrint = now;
    Serial.printf("[dbg] LDR=%d  Servo=%d  TH=%s\n",
                  ldr, g_servoPos, thOK ? "OK" : "NA");
  }

  delay(5);
}
