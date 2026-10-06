// IP: 192.168.1.4 | MAC: c8f74240bcb4=======================================================================================
// ROBOT SKETCH: ULTIMATE VERSION V5.3.9
// W/ PERMANENT IR SLEEP, NON-BLOCKING AVOIDANCE, NTP TIME, & 1-MINUTE ALARM CLOCK
// Modified: Add SLEEP IR Wake Reaction (STATE_SLEEP_ALERT) - Rustech patch
// MODIFIED: Merged conflicting STATE_SLEEP with STATE_SLEEPING_IR, Fixed IR Logic.
// ESPNOW BAGONG UPDATE KONTROL ANG ROBOT VIA ANOTHER ESP32 (MASTER-SLAVE)
// Modified: Added Music Player Functionality w/ DFRo
// bot DFPlayer Mini & DF2301Q Voice Module
// MERGED: Added FaceRobot Android app HTTP control (/command?dir=X), active ONLY in STATE_BOOT_WAIT
// V5.3.9: CAMERA OBSTACLE AVOIDANCE (depth model sa FaceRobot app) habang STATE_MOVING:
//         - /ping ay sumasagot na ng "PONG|<STATE>" para malaman ng app kung naka-MOVING ang robot.
//         - Bagong commands (tinatanggap lang habang MOVING / AVOIDING): NAV_LEFT, NAV_RIGHT, NAV_BACK, NAV_CLEAR
//           (+ optional &servo=<angle>). Ang ESP32 pa rin ang may huling desisyon: ultrasonic/IR avoidance
//           ang laging mauuna, at kapag walang bagong hint sa loob ng NAV_HINT_TIMEOUT_MS, tuloy-tuloy lang ang forward.
// V5.3.8: Tinanggal ang 30s auto-stop ng STATE_MOVING (AUTO ay tuloy-tuloy hanggang FORCE_STOP / voice "stop").
//         Ibalik sa true ang MOVING_AUTO_STOP_ENABLED kung gusto ulit ng 30s stop + PIR wait.
//         FIX boot crash (Guru Meditation LoadProhibited) kapag hindi agad naka-connect ang WiFi:
//         HINDI na ino-off ang WiFi radio sa Offline Mode (may ESP-NOW + Bluetooth na gumagamit nito),
//         at dinagdagan ang connect attempts (10 -> 20, lalabas agad kapag naka-connect na).
// V5.3.7 FIX: Ang AUTO (app/Bluetooth/alarm) ay agad na natatapos ang moving timer at hindi tumutuloy.
//         Sanhi: ang 'now' ay kinuha sa simula ng loop(), pero ang movementStartTime ay nase-set ng
//         millis() sa loob ng server.handleClient() -> mas bago pa kaysa 'now' -> unsigned underflow
//         (now - movementStartTime = napakalaking numero) -> agad "60s done". Ngayon signed compare na.
// V5.3.6: AUTO command mula sa app ay tinatanggap na sa ANUMANG state (dati STATE_BOOT_WAIT lang kaya nai-IGNORE).
//         Non-blocking na ang time check (dating hanggang 5s ang block ng getLocalTime kapag walang NTP sync).
// V5.3.5: Kapag na-disconnect ang app (10s walang ping), babalik na sa roboEyes ang OLED.
// V5.3.4 FIX: Inayos ang updateEyes() (dating nag-i-infinite recursion kaya hindi nagbo-boot).
//             Kapag naka-link ang FaceRobot app, IP address na lang ang lalabas sa OLED.
//             Ang roboEyes.update() ay DAPAT nasa loob LANG ng updateEyes(); lahat ng iba ay updateEyes() na.
// =======================================================================================

#include <Wire.h>
#include "BluetoothSerial.h" // Para sa future Bluetooth control (optional)
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <FluxGarage_RoboEyes.h>
#include <math.h>
#include <Arduino.h>
#include "DFRobot_DF2301Q.h"
DFRobot_DF2301Q_I2C asr;
static unsigned long lastVoiceTime = 0;
const unsigned long VOICE_COOLDOWN = 2000;
BluetoothSerial SerialBT;
unsigned long lastTelemetryTime = 0;

#include <ESP32Servo.h>
Servo liftServo;
#define SERVO_PIN 15
#define SERVO_MIN_ANGLE 0
#define SERVO_MAX_ANGLE 110   // physical limit - lagpas dito, babangga sa floor
int currentServoAngle = 0;    // simulan sa pinakababa (folded/down position)
int targetServoAngle = 0;     // ang gustong marating ng servo
unsigned long lastServoStepTime = 0;
const unsigned long SERVO_STEP_INTERVAL_MS = 1; // pagitan ng bawat hakbang
const int SERVO_STEP_DEG = 1;                    // 1° bawat 25ms = ~40°/s

#include <HardwareSerial.h>      // Para sa Serial2 (Pins 16 & 17)
#include <DFRobotDFPlayerMini.h> // Ang library ng MP3 module
DFRobotDFPlayerMini myDFPlayer;

// >>>>>>>>>>>>>>>>>> NTP & WIFI INCLUDES <<<<<<<<<<<<<<<<<<

#include <esp_now.h>
uint8_t broadcastAddress[] = {0x20, 0x6E, 0xF1, 0x84, 0x66, 0xE8};
#include <WiFi.h>
#include "time.h"
#include <WebServer.h> // Para sa FaceRobot Android app HTTP control (/command?dir=X)
#include <Preferences.h> // V5.3.13: pang-save ng natutunang haba ng kanta (NVS)

// ========================= OLED CONFIG & DECLARATION =========================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);
RoboEyes<Adafruit_SH1106G> roboEyes(display);

#define OPM_INPUT_PIN 34
bool opmActive = false;
unsigned long opmTimer = 0;
float V0 = 0.014; float P0 = -30.2;
float V1 = 0.732; float P1 = -20.2;
float V2 = 3.300; float P2 = -10.30;
int hulingTugtog = 0; // 0 = wala pa, 15 = LOS, 16 = Good, 17 = Warning

// >>>>>>>>>>>>>>>>>> NTP & WIFI CONFIG (PALITAN ITO) <<<<<<<<<<<<<<<<<<

const char* ssid = "Rusty";
const char* password = "12345678";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800; // GMT+8 for Philippines
const int   daylightOffset_sec = 0;

// ========================= FACE-TRACKING HTTP SERVER (FaceRobot Android app) =========================
// Ginagamit ng Android FaceRobot app para mag-send ng dir=FORWARD/BACKWARD/LEFT/RIGHT/STOP/SEARCH
// sa http://<esp32-ip>/command?dir=X . Gagana lang ito kapag STATE_BOOT_WAIT ang currentState
// (tignan ang handleCommand() function sa baba). Gumagamit ng DHCP IP na galing sa WiFi connect
// sa setup() -- kung gusto ni Boss Rusty ng laging parehong IP, i-DHCP-reserve na lang ang MAC
// (c8f74240bcb4) sa router, imbes na gumamit ng WiFi.config() static IP (dati itong sanhi ng
// brownout boot hang, kaya DHCP na lang ang ginagamit dito).
WebServer server(80);
unsigned long lastFaceCommandTime = 0;
const unsigned long FACE_COMMAND_TIMEOUT = 600; // ms - kung wala nang bagong command dito, babalik sa autonomous mode
const int FACE_TURN_SPEED = 130;     // mas mabagal na turn - dating 220 masyadong bilis, kaya nagwawild
const int FACE_FORWARD_SPEED = 140;  // mas mabagal ding forward/backward para sa follow
bool appLinked = false;                 // true = may nakausap nang app -> IP-only ang OLED
unsigned long lastAppContactTime = 0;
unsigned long lastIpDrawTime = 0;
bool ipScreenNeedsDraw = true;
const unsigned long IP_REDRAW_INTERVAL = 2000;
const unsigned long APP_LINK_TIMEOUT_MS = 10000; // 10s walang ping/command mula sa app = disconnected -> balik sa roboEyes (0 = laging IP-only)
// ========================= ALARM CONFIGURATION =========================

const int ALARM_HOUR = 06;
const int ALARM_MINUTE = 00;
const bool isAlarmSet = true;
bool isAlarmTriggeredToday = false;
unsigned long alarmTriggerTime = 0;
const unsigned long ALARM_DURATION = 120000; // 2 mimutes
const int ALARM_TONE_FREQ = 1000;
unsigned long pirDisableUntil = 0;
const unsigned long PIR_DISABLE_DURATION = 120000; // 2 minutes
unsigned long pitDisableUntil = 0;
const unsigned long PIT_DISABLE_DURATION = 60000; // 1 minute
unsigned long cryingStartTime = 0; // Para sa timer ng emosyon
bool isCrying = false;             // Para malaman kung kasalukuyang umiiyak
bool isDisplayingNumber = false; // Bagong flag para sa numero

// ========================= SENSOR & OUTPUT PINS =========================

#define IR_SENSOR_LEFT 19    // Pit Detection (Active-HIGH)
#define IR_SENSOR_RIGHT 16   // Pit Detection (Active-HIGH)

unsigned long pauseDuration = 800; // Hinto ng 0.8 seconds bago lumingon

// Avoidance Pins reverted to 36/39 (Logic inverted in loop to fix trigger issue)

#define IR_AVOID_LEFT 36
#define IR_AVOID_RIGHT 39
#define PIR_MIC_PIN 35
#define BUZZER_PIN 18
#define LED_PIN 2


// ========================= TB6612 MOTOR DRIVER =========================

#define AIN1 25
#define AIN2 26
#define PWMA 27
#define BIN1 32
#define BIN2 33
#define PWMB 14
#define STBY 13

// ========================= ULTRASONIC =========================

#define TRIG 4
#define ECHO 23

// ========================= PWM CONFIG =========================

#define FREQ 1000
#define RES 8
#define CH_A 4
#define CH_B 5

// Hiwalay na LEDC channel para sa buzzer - dating gamit ng tone()/noTone() ay
// pareho ang LEDC hardware timers ng Servo library (channels 0-7), kaya
// nagkakabanggaan/nasisira ang 50Hz timing ng servo. Ang channel 10 ay nasa
// ibang timer group na, kaya ligtas na hiwalay sa servo (0-7) at motors (4,5).
#define BUZZER_LEDC_CHANNEL 6   // high-speed group, timer3 — hiwalay sa servo (low-speed 0-3) at motors (timer2)

// ========================= STATE MACHINE & TIMERS =========================

enum RobotState {
  STATE_BOOT_WAIT, 
  STATE_MOVING,
  STATE_STOPPED_WAITING_FOR_PIR,
  STATE_PIR_ACTIVE_AND_DETECTING,
  STATE_SLEEPING_IR,            // <-- Merged STATE_SLEEP into this
  STATE_SLEEP_ALERT,            // <-- NEW: transient alert state for sleep wake reaction
  STATE_AVOIDING_REVERSE,
  STATE_AVOIDING_STOP,
  STATE_AVOIDING_TURN,
  STATE_SLEEP_ULTRASONIC_OSC,
  STATE_DANCING,
  STATE_SHAKING,
  STATE_EXPRESSION,
  STATE_MANUAL,
};

int shakeDuration = 3000; // Default ay 3 seconds

RobotState currentState = STATE_MOVING;
enum EdgeDetected { EDGE_NONE = 0, EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_BOTH = 3 };
EdgeDetected lastEdgeDetected = EDGE_NONE;
//const int BASE_SPEED = 120;
//const int TURN_SPEED = 200;
unsigned long lastBluetoothTime = 0;
int currentBaseSpeed = 160; // Ito ang default speed
int currentTurnSpeed = 220; // Default turn speed
const unsigned long MOVEMENT_DURATION = 30000;
// ---------------- V5.3.9: CAMERA NAV HINTS (mula sa FaceRobot app) ----------------
uint8_t navHint = 0;                       // 0 = wala, 1 = LEFT, 2 = RIGHT, 3 = BACK, 4 = CLEAR
unsigned long navHintTime = 0;             // millis() nang huling dumating ang hint
const unsigned long NAV_HINT_TIMEOUT_MS = 700; // kapag mas luma pa dito ang hint, ituring na expired (forward na ulit)
const int NAV_TURN_SPEED = 150;            // bilis ng spin habang camera-nav (mas mabagal kaysa avoidance turn)
const bool MOVING_AUTO_STOP_ENABLED = false; // V5.3.8: false = tuloy-tuloy ang STATE_MOVING (walang 30s stop). true = dating behavior
const unsigned long PIR_ACTIVATION_DELAY = 3000;
const long detectionCooldown = 5000;
const int PRESENCE_TRIGGER_CM = 30; // Distansya (cm) na magsasabing "may tao" — kapalit ng PIR sensor
// ========================= STUCK DETECTION LOGIC (FIXED) =========================

int consecutiveIrTriggers = 0;
unsigned long lastIrTriggerTime = 0;
const unsigned long IR_TRIGGER_TIMEOUT = 500; // 250-500ms window to count as rapid triggers
const int IR_TRIGGER_MAX = 480;
const unsigned long IR_RESET_TIMEOUT = 10000; // 10 seconds reset

// ========================= END STUCK DETECTION LOGIC (FIXED) ======================

const unsigned long REVERSE_DURATION = 300; // milliseconds
const unsigned long TURN_DURATION = 900; // milliseconds
unsigned long avoidanceStartTime = 0; //
unsigned long movementStartTime = 0;
unsigned long pirActivationStartTime = 0;
unsigned long lastDetectionTime = 0;
unsigned long lastCuriousBeep = 0; // For curious beeps
bool isPirDetectionActive = false;
unsigned long shakeStartTime = 0; // For shaking state



unsigned long lastDistanceReadTime = 0;
const unsigned long DISTANCE_READ_INTERVAL = 200;
long currentDistance = 0;
const unsigned long TURN_DURATION_OSC = 200;
const unsigned long STOP_DURATION_OSC = 80;
const unsigned long CYCLE_DURATION_OSC = TURN_DURATION_OSC + STOP_DURATION_OSC;
unsigned long lastOscillationTime = 0;
bool isTurningLeft = true;
unsigned long toneEndTime = 0;
bool isPlayingCurious = false;
int curiousPattern[][2] = {{500, 80}, {650, 100}, {800, 120}, {600, 150}};
int currentToneIndex = 0;
unsigned long lastToneStartTime = 0;
bool isAvoidBeepActive = false;
int avoidToneIndex = 0;
unsigned long avoidToneEnd = 0;
unsigned long glideNextUpdate = 0;
int avoidPattern[][2] = { {600, 90}, {800, 120}, {900, 200} };
const int AVOID_TONE_COUNT = 3;
int glideFreq = 700;
int glideTarget = 900;
int glideStep = 20;
unsigned long glideInterval = 10;
unsigned long sleepModeStartTime = 0;
const unsigned long TIRED_FACE_DURATION = 60000; // 1 minute
bool isTiredFaceDone = false;

// ========================= MUSIC PLAYER VARIABLES =========================
int currentSongNumber = 1; // Simulan sa kanta #1
int totalMusicFiles = 49;  // Kabuuang bilang ng kanta sa SD card
bool isMusicActive = false;
static unsigned long lastCheckTime = 0; //
bool isAdvertPlaying = false; // Para malaman kung nagpe-play ng advert
unsigned long advertStartTime = 0;
unsigned long ADVERT_DURATION = 3000; // 3 seconds (Haba ng "Yes Boss" mo)

// ---- ADVERT / MUSIC FIX (V5.3.10) ----
// Sanhi ng auto-next: ang DFPlayer ay madalas magpadala ng DOBLENG "PlayFinished" pagkatapos ng advertise().
// Yung una = advert done (na-handle), yung pangalawa = napagkamalang "tapos na ang kanta" -> shuffle next.
// Dagdag pa: 3s lang ang timeout ng isAdvertPlaying, at may mga delay() sa loop kaya nahuhuli ang event.
unsigned long advertGuardUntil = 0;      // hanggang kailan IGNORE ang lahat ng "finished" event
unsigned long lastFinishedEventTime = 0; // para sa duplicate filter
int lastFinishedValue = -1;
bool resumePending = false;              // non-blocking resume pagkatapos ng advert
int resumeStage = 0;                     // 1 = unang tingin, 2 = pangalawang tingin
unsigned long resumeAt = 0;
bool nextCheckPending = false;           // i-verify muna kung talagang tapos na ang kanta bago mag-next
unsigned long nextCheckAt = 0;
int nextCheckStage = 0;                  // 1 = unang tingin, 2 = pangalawang tingin (bago mag-next)
int currentAdvertNum = -1;               // anong advert/greeting ang kasalukuyang tumutugtog

unsigned long songStartTime = 0;   // kailan huling nag-play ng kanta (para malaman kung totoong tapos na)

// Pumili ng random na kanta na HINDI kapareho ng kasalukuyan (iwas ulit-ulit)
int pickRandomSong() {
  if (totalMusicFiles <= 1) return 1;
  int n;
  do { n = random(1, totalMusicFiles + 1); } while (n == currentSongNumber);
  return n;
}

// ---- SONG WATCHDOG (V5.3.12) ----
// May mga MP3 na hindi nagpapadala ng "finished" event ang DFPlayer (naka-loop lang), kaya hindi nag-ne-next.
// Solusyon: bilangin ang oras ng pagtugtog (hindi kasama habang may advert), at i-force next kapag lumampas.
//  - DEFAULT_MAX_SONG_MS: limit para sa lahat ng kanta. 0 = PATAY (default) para hindi maputol ang mahahabang kanta (hal. 30 minuto)
//  - songLimits[]: eksaktong haba (segundo) ng mga kantang problema, para eksakto ang next
const unsigned long DEFAULT_MAX_SONG_MS = 0;   // 0 = walang default limit (songLimits[] at natutunang haba lang ang gagana)
struct SongLimit { int track; unsigned int seconds; };
const SongLimit songLimits[] = {
  // {45, 140},     // HALIMBAWA: track 45 = 2:20 (140 segundo). Idagdag dito ang mga kantang umuulit.
  {0, 0}            // dummy / dulo ng listahan - HUWAG BURAHIN
};
unsigned long songPlayedMs = 0;
unsigned long lastSongTick = 0;

// ---- ADVERT THROTTLE (V5.3.13) ----
// Kapag naka-connect ang app, nagpapadala ito ng PLAY (advert) tuwing ~20-60s, at ang bawat advert ay nag-iinterrupt sa kanta.
// Kung nire-restart ng DFPlayer mo ang kanta pagkatapos ng advert, hindi na ito aabot sa dulo (kaya parang "naka-loop").
// ADVERT_MIN_GAP_MS = pinakamaikling pagitan ng dalawang advert HABANG MAY TUMUTUGTOG NA MUSIC. 0 = patay (lahat ng advert tutunog).
// Halimbawa: 120000 = isang advert lang kada 2 minuto habang may music. (Walang music = laging tutunog.)
const unsigned long ADVERT_MIN_GAP_MS = 0;
unsigned long lastAdvertStartTime = 0;
int advertsThisSong = 0;

// ---- AUTO-LEARN NG HABA NG KANTA (V5.3.13) ----
// Kapag natapos NANG NATURAL ang isang kanta (may finished event + state stopped), isinasave ang tunay na haba nito sa flash.
// Sa susunod na tugtugin, kahit mag-loop ang DFPlayer (walang finished event), mag-ne-next ang robot sa EKSAKTONG dulo ng kanta.
Preferences songPrefs;
uint16_t learnedSec[256] = {0};
bool songInterrupted = false;   // true kung naputol ang kanta ng dance/alert (hindi tama ang oras para i-learn)

void loadSongDurations() {
  songPrefs.begin("songdur", false);
  for (int i = 1; i <= totalMusicFiles && i < 256; i++) {
    char key[8];
    snprintf(key, sizeof(key), "s%d", i);
    learnedSec[i] = songPrefs.getUShort(key, 0);
  }
  Serial.println("[LEARN] Na-load ang mga natutunang haba ng kanta.");
}

void learnSongDuration(int track, unsigned long sec) {
  if (track <= 0 || track >= 256) return;
  if (sec < 20 || sec > 1200) return;                 // masyadong maikli/mahaba = hindi kapani-paniwala
  uint16_t old = learnedSec[track];
  if (old == 0 || sec + 2 < old) {                    // unang beses, o mas maikli (mas tumpak)
    learnedSec[track] = (uint16_t)sec;
    char key[8];
    snprintf(key, sizeof(key), "s%d", track);
    songPrefs.putUShort(key, (uint16_t)sec);
    Serial.print("[LEARN] Track "); Serial.print(track);
    Serial.print(" = "); Serial.print(sec); Serial.println("s (na-save)");
  }
}

unsigned long songLimitMs(int track) {
  for (int i = 0; songLimits[i].track != 0; i++) {
    if (songLimits[i].track == track) return (unsigned long)songLimits[i].seconds * 1000UL + 4000UL; // +4s palugit
  }
  if (track > 0 && track < 256 && learnedSec[track] > 0) {
    return (unsigned long)learnedSec[track] * 1000UL + 6000UL;   // natutunang haba + 6s palugit
  }
  return DEFAULT_MAX_SONG_MS;
}

// Palaging gamitin ito para mag-play ng kanta sa /MP3 folder
void playSongTracked(int n) {
  myDFPlayer.playMp3Folder(n);
  songInterrupted = false;
  songPlayedMs = 0;
  lastSongTick = millis();
  songStartTime = millis();
  advertsThisSong = 0;
}

// Gamitin ito sa halip na direktang myDFPlayer.advertise()
// - may music: i-pause ang kanta, tugtugin ang advert (ADVERT folder), tapos babalik sa parehong kanta
// - walang music: normal play lang
void playAdvertOverMusic(uint8_t advertNum) {
  if (isMusicActive) {
    if (ADVERT_MIN_GAP_MS > 0 && lastAdvertStartTime != 0 && (millis() - lastAdvertStartTime) < ADVERT_MIN_GAP_MS) {
      Serial.print("[ADVERT] advertise("); Serial.print(advertNum);
      Serial.println(") SKIPPED - masyadong madalas habang may music (ADVERT_MIN_GAP_MS)");
      return;
    }
    lastAdvertStartTime = millis();
    advertsThisSong++;
    isAdvertPlaying = true;
    advertStartTime = millis();
    ADVERT_DURATION = 30000;   // safety net LANG (mahaba ang greeting tracks ng app; ang tunay na pag-clear ay galing sa "finished" event)
    currentAdvertNum = advertNum;
    advertGuardUntil = 0;
    resumePending = false;
    nextCheckPending = false;
    Serial.print("[ADVERT] advertise("); Serial.print(advertNum);
    Serial.print(") during song "); Serial.print(currentSongNumber);
    Serial.print(" t="); Serial.println(millis());
    myDFPlayer.advertise(advertNum);
  } else {
    myDFPlayer.play(advertNum);
  }
}

// ---------------- NEW: Sleep Alert (IR wake reaction) ----------------

unsigned long sleepAlertStart = 0;
const unsigned long SLEEP_ALERT_DURATION = 3000;   // 5 seconds reaction (set 3000ms)
const unsigned long SLEEP_ALERT_OSC_INTERVAL = 60; // ms between left/right during alert

// ---------------- SLEEP MODE Avoidance Movement ---

bool sleepAvoidActive = false;
unsigned long sleepAvoidStart = 0;
const unsigned long SLEEP_AVOID_DURATION = 2500; // 2.5 seconds
int sleepAvoidDirection = 0;  // -1 = left, +1 = right

//----------------- HEADLIGHT threshold (adjust to taste)

//const int HEADLIGHT_THRESHOLD = 700; // if analogRead(LDR_PIN) > this => it's dark -> headlight ON

// ================= PIR SLEEP VARIABLES =================
bool hasRespondedToPIR = false;
unsigned long lastPIRTriggerTime = 0;
unsigned long pirCooldown = 900000; // 15 minutes cooldown 

// ================= LED BLINK VARIABLES =======================
unsigned long lastBlinkTime = 0;
const int blinkInterval = 200; // Bilis ng kurap (200ms)
bool ledState = LOW;

// ----------------- BAGONG ULTRASONIC SLEEP VARIABLES ---

const int ULTRASONIC_TRIGGER_CM = 5;    // Distansya para mag-trigger (5cm)
const unsigned long OSC_MOVEMENT_DURATION = 250; // 250ms movement (pwede baguhin)
const unsigned long OSC_STOP_DURATION = 80;   // Maikling stop sa pagitan
unsigned long ultrasonicOscStart = 0;     // Timer start
const unsigned long ULTRASONIC_OSC_TOTAL_DURATION = 2500; // 2.5 seconds total oscillation
bool isOscillatingForward = true;         // Flag para sa Abante/Atras cycle

// ==================DANCE MODE VARIABLES ==============

const unsigned long DANCE_DURATION = 56000; // 1 minute dance session
unsigned long danceStartTime = 0;
// Dance Cycle Timing (Non-blocking):
const unsigned long DANCE_OSC_DURATION = 800;  // Mabilis na Atras/Abante/Kaliwa/Kanan
const unsigned long DANCE_TURN_DURATION = 2000; // 2 seconds turn
unsigned long lastDanceStepTime = 0;
int danceStep = 0; // 0=Atras/Abante, 1=Kaliwa/Kanan, 2=Turn Right, 3=Turn Left
const int DANCE_MAX_STEPS = 4;
int atrasAbanteCycleCount = 0; // Para bilangin ang Atras/Abante cycles
const int AT_AB_CYCLES_TARGET = 2; // Target: Dalawang (2) cycles

// ========================= FUNCTION DECLARATIONS =========================

long getDistance();
void playBootSound_NB();
void playSleepBeep_NB();
void playCuriousBeep_NB();
void updateBuzzer();
void displayTime();
void checkAlarm();
void playAvoidBeep_NB();
void forward();
void reverse();
void turnRight();
void turnLeft();
void stopBot();
void showRustechOPM();
void systemReboot();
void handle_bluetooth_data();
void handleCommand(); // FaceRobot Android app HTTP command handler (STATE_BOOT_WAIT only)
void drawIpScreen();
void onAppContact();
void updateEyes();
void setServoAngle(int angle); // Servo control function
void updateServo();

// ========================= BUZZER CONTROL (NON-BLOCKING) =========================

// Kapalit ng built-in tone()/noTone() - gumagamit ng sariling LEDC channel
// (BUZZER_LEDC_CHANNEL) para hindi na ito makipag-agawan sa mga LEDC timer na
// ginagamit ng Servo library o ng motors.
void buzzTone(int freq) {
  ledcWriteTone(BUZZER_LEDC_CHANNEL, freq);
}
void buzzOff() {
  ledcWriteTone(BUZZER_LEDC_CHANNEL, 0); // explicit na patayin ang tone, hindi lang duty=0
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);
}

void startTone(int frequency, unsigned long duration) {
  isPlayingCurious = false;
  buzzTone(frequency);
  toneEndTime = millis() + duration;
}

void updateBuzzer() {
  unsigned long now = millis();
  if (toneEndTime > 0 && now >= toneEndTime) {
    buzzOff();
    toneEndTime = 0;
  }

  if (isPlayingCurious && now >= lastToneStartTime + curiousPattern[currentToneIndex][1] + 50) {
    currentToneIndex++;
    if (currentToneIndex < 4) {
      buzzTone(curiousPattern[currentToneIndex][0]);
      lastToneStartTime = now;
    } else {
      buzzOff();
      isPlayingCurious = false;
    }
  }

  if (isAvoidBeepActive && avoidToneIndex < AVOID_TONE_COUNT) {
    if (now >= avoidToneEnd) {
      avoidToneIndex++;
      if (avoidToneIndex >= AVOID_TONE_COUNT) {
        buzzOff();
        isAvoidBeepActive = false;
      } else if (avoidToneIndex == 2) {
        buzzTone(glideFreq);
        avoidToneEnd = now + avoidPattern[2][1];
        glideNextUpdate = now + glideInterval;
      } else {
        buzzTone(avoidPattern[avoidToneIndex][0]);
        avoidToneEnd = now + avoidPattern[avoidToneIndex][1];
      }
    }

    if (avoidToneIndex == 2 && now >= glideNextUpdate) {
      if (glideFreq < glideTarget) {
        glideFreq += glideStep;
        buzzTone(glideFreq);
      }
      glideNextUpdate = now + glideInterval;
    }
  }

  // ============ ALARM TONE LOGIC ===============

  if (alarmTriggerTime > 0) {
    if (now < alarmTriggerTime + ALARM_DURATION) {
      if ((now % 800) < 400) {
        if (toneEndTime == 0 && !isPlayingCurious && !isAvoidBeepActive) {
          buzzTone(ALARM_TONE_FREQ);
        }
      } else {
        buzzOff();
      }
    } else {
      buzzOff();
      alarmTriggerTime = 0;
      Serial.println("Alarm tone ended after 2 minute.");
    }
  }
}

void playBootSound_NB() {
  int tonesArr[] = {300, 400, 500, 650, 800, 1000};
  for (int i = 0; i < 6; i++) {
    buzzTone(tonesArr[i]);
    delay(100);
  }
  buzzOff();
}

void playSleepBeep_NB() {
  for (int i = 0; i < 2; i++) {
    buzzTone(200);
    delay(300);
    buzzOff();
    delay(200);
  }
  buzzOff();
}

void playCuriousBeep_NB() {
  if (isPlayingCurious) return;
  currentToneIndex = 0;
  isPlayingCurious = true;
  lastToneStartTime = millis();
  buzzTone(curiousPattern[0][0]);
}

void playAvoidBeep_NB() {
  if (isAvoidBeepActive) return;
  isAvoidBeepActive = true;
  avoidToneIndex = 0;
  glideFreq = avoidPattern[2][0];
  buzzTone(avoidPattern[0][0]);
  avoidToneEnd = millis() + avoidPattern[0][1];
}

// ========================= TIME DISPLAY FUNCTION (TWO-LINE CENTERED) =========================

// V5.3.6: Non-blocking na kapalit ng getLocalTime(). Ang getLocalTime() ay maghihintay ng hanggang 5 SEGUNDO
// kapag hindi pa naka-sync ang NTP (hal. walang internet ang WiFi) at tatawagin pa ito bawat loop -
// dahilan ng matinding lag at pagka-delay ng mga command ng robot.
bool getLocalTimeNB(struct tm* info) {
  time_t nowT;
  time(&nowT);
  localtime_r(&nowT, info);
  return info->tm_year > (2016 - 1900);
}

void displayTime() {
  // V5.3.4: Kapag naka-link ang app at hindi voice-command ang nag-display (isDisplayingNumber),
  // IP screen lang ang ipapakita imbes na oras.
  if (appLinked && !isDisplayingNumber) {
    updateEyes();
    return;
  }

  struct tm timeinfo;

  if (!getLocalTimeNB(&timeinfo)) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0);
    display.println("NTP Sync Failed");
    display.println("Check WiFi");
    display.display();
    return;
  }

  char hour_minute_output[6];
  strftime(hour_minute_output, 6, "%I:%M", &timeinfo);

  char ampm_output[3];
  strftime(ampm_output, 3, "%p", &timeinfo);

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  const int BIG_TEXT_SIZE = 3;
  display.setTextSize(BIG_TEXT_SIZE);

  int16_t x1, y1;
  uint16_t w_time, h_time;
  display.getTextBounds(hour_minute_output, 0, 0, &x1, &y1, &w_time, &h_time);

  int time_y_position = (SCREEN_HEIGHT / 2) - (h_time / 2) - 5;
  display.setCursor((SCREEN_WIDTH - w_time) / 2, time_y_position);
  display.println(hour_minute_output);

  const int SMALL_TEXT_SIZE = 1;
  display.setTextSize(SMALL_TEXT_SIZE);

  uint16_t w_ampm, h_ampm;
  display.getTextBounds(ampm_output, 0, 0, &x1, &y1, &w_ampm, &h_ampm);

  int ampm_y_position = time_y_position + h_time + 5;
  display.setCursor((SCREEN_WIDTH - w_ampm) / 2, ampm_y_position);
  display.println(ampm_output);

  display.display();
}

// ========================= ALARM CHECK FUNCTION =========================

void checkAlarm() {
  // V5.3.6: isang beses lang bawat segundo mag-check (dati bawat loop)
  static unsigned long lastAlarmCheck = 0;
  if (millis() - lastAlarmCheck < 1000) return;
  lastAlarmCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return; // Skip alarm check kung offline
  
  struct tm timeinfo;
  if (!getLocalTimeNB(&timeinfo)) return;

  if (!isAlarmSet) return;
  
  if (!getLocalTimeNB(&timeinfo)) {
    return;
  }

  // =======================Midnight Reset=====================================

  if (isAlarmTriggeredToday && (timeinfo.tm_hour == 0 && timeinfo.tm_min == 1)) {
    isAlarmTriggeredToday = false;
    Serial.println("Alarm state reset for the new day.");
  }

  // =======================Alarm Time============================================

  if (!isAlarmTriggeredToday &&
      timeinfo.tm_hour == ALARM_HOUR &&
      timeinfo.tm_min == ALARM_MINUTE) {
    isAlarmTriggeredToday = true;
    alarmTriggerTime = millis();
    pirDisableUntil = millis() + PIR_DISABLE_DURATION;
    pitDisableUntil = millis() + PIT_DISABLE_DURATION;

    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("!!! ALARM TRIGGERED: 🔔 WAKE UP! 🔔 !!!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    
    isAvoidBeepActive = false;
    isPlayingCurious = false;
    toneEndTime = 0;
    buzzOff(); // Stop any avoidance beeps

    // WAKE-UP ROUTINE (if sleeping from IR only)

    if (currentState == STATE_SLEEPING_IR || currentState == STATE_SLEEP_ALERT) {
      display.clearDisplay();
      playBootSound_NB();
      roboEyes.setMood(HAPPY);
      roboEyes.setAutoblinker(ON, 3, 2);
      roboEyes.setIdleMode(ON, 2, 2);
      roboEyes.setCuriosity(ON);
      isTiredFaceDone = false;
      currentState = STATE_MOVING;
      movementStartTime = millis();
      ipScreenNeedsDraw = true; // V5.3.4: siguraduhing mag-redraw ang IP screen pagkatapos ng clearDisplay
      Serial.println("Robot WOKE UP via ALARM!");
    } else {

      roboEyes.setMood(HAPPY);
      roboEyes.anim_laugh();
    }
  }
}

// ========================= SETUP =========================

void drawIpScreen() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(8, 2);
  display.print("ROBOT IP ADDRESS:");
  display.drawFastHLine(0, 13, 128, SH110X_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    String ipStr = WiFi.localIP().toString();
    int fontSize = (ipStr.length() <= 10) ? 2 : 1;
    int charWidth = (fontSize == 2) ? 12 : 6;
    int xPos = (128 - ipStr.length() * charWidth) / 2;
    if (xPos < 0) xPos = 0;
    display.setTextSize(fontSize);
    display.setCursor(xPos, 24);
    display.print(ipStr);
    display.setTextSize(1);
    display.setCursor(2, 48);
    display.print("SSID: ");
    display.print(ssid);
  } else {
    display.setTextSize(2);
    display.setCursor(10, 25);
    display.print("NO WIFI");
  }
  display.display();
}

void onAppContact() {
  lastAppContactTime = millis();
  if (!appLinked) {
    appLinked = true;
    ipScreenNeedsDraw = true;
    Serial.println("📱 FaceRobot app linked - OLED eyes OFF, IP only");
  }
}

// ========================= updateEyes() - FIXED V5.3.4 =========================
// PAALALA: DITO LANG DAPAT MANATILI ANG roboEyes.update() sa buong sketch.
// Kung papalitan mo rin ang roboEyes.update() sa loob nito ng updateEyes(),
// mag-i-infinite recursion ito at hindi magbo-boot ang robot!
void updateEyes() {
  // Kapag may naka-lock na display (numero / IP / oras mula sa voice command), huwag galawin
  if (isDisplayingNumber) return;

  if (appLinked && APP_LINK_TIMEOUT_MS > 0 && (millis() - lastAppContactTime > APP_LINK_TIMEOUT_MS)) {
    appLinked = false;
    display.clearDisplay(); // V5.3.5: burahin ang IP screen bago bumalik ang roboEyes
    Serial.println("📱 FaceRobot app disconnected - balik sa roboEyes");
  }

  if (!appLinked) {
    roboEyes.update();   // <-- ITO LANG ang lugar na dapat may roboEyes.update()
    return;
  }

  // May naka-link na app: IP screen lang, hindi eyes
  if (ipScreenNeedsDraw || (millis() - lastIpDrawTime > IP_REDRAW_INTERVAL)) {
    drawIpScreen();
    lastIpDrawTime = millis();
    ipScreenNeedsDraw = false;
  }
}

// V5.3.9: Pangalan ng state para sa /ping reply (para malaman ng app kung MOVING ang robot)
const char* stateName(RobotState st) {
  switch (st) {
    case STATE_BOOT_WAIT: return "BOOT_WAIT";
    case STATE_MOVING: return "MOVING";
    case STATE_STOPPED_WAITING_FOR_PIR: return "STOPPED_WAITING_FOR_PIR";
    case STATE_PIR_ACTIVE_AND_DETECTING: return "PIR_ACTIVE";
    case STATE_SLEEPING_IR: return "SLEEPING_IR";
    case STATE_SLEEP_ALERT: return "SLEEP_ALERT";
    case STATE_AVOIDING_REVERSE: return "AVOIDING_REVERSE";
    case STATE_AVOIDING_STOP: return "AVOIDING_STOP";
    case STATE_AVOIDING_TURN: return "AVOIDING_TURN";
    case STATE_SLEEP_ULTRASONIC_OSC: return "SLEEP_ULTRASONIC_OSC";
    case STATE_DANCING: return "DANCING";
    case STATE_SHAKING: return "SHAKING";
    case STATE_EXPRESSION: return "EXPRESSION";
    case STATE_MANUAL: return "MANUAL";
    default: return "UNKNOWN";
  }
}

void handlePing() {
  onAppContact();
  server.send(200, "text/plain", String("PONG|") + stateName(currentState));
}

void setup() {
  Serial.begin(115200);
  loadSongDurations();
  delay(1000);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);

  // 2. SIMULAN ANG BLUETOOTH
  if(!SerialBT.begin("Rustech_Robot_BT")){
    Serial.println("❌ Bluetooth Error!");
  } else {
    Serial.println("✅ Bluetooth Ready! Pair mo na sa phone.");
  }
ESP32PWM::allocateTimer(0);
ESP32PWM::allocateTimer(1);
ESP32PWM::allocateTimer(2);
ESP32PWM::allocateTimer(3);
liftServo.setPeriodHertz(50);
liftServo.attach(SERVO_PIN, 500, 2500); // i-adjust base sa totoong range ng unit mo
liftServo.write(currentServoAngle); // simulan sa DOWN position

//  ==================== ESP-NOW SETUP =========================

  // 1. I-set ang WiFi mode
  WiFi.mode(WIFI_STA);

  // 2. Initialize ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // 3. I-register ang S3 bilang partner (peer)
  esp_now_peer_info_t peerInfo;
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  // Dapat pareho ang channel kung gagamit ng router
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
  
  Serial.println("Robot Sender Ready!");
//  ==================== END ESP-NOW SETUP =========================

  // FIX: I-initialize muna ang I2C bus (Wire.begin()) BAGO tawagin ang setClock()/
  // setTimeOut() -- dati ito ang sanhi ng "[E][Wire.cpp:381] setClock(): could not
  // acquire lock" dahil wala pang existing I2C lock/mutex na ma-a-acquire kasi hindi
  // pa naiinitialize ang bus. Default SDA=21, SCL=22 sa ESP32.
  Wire.begin();
  Wire.setClock(100000); // I-set sa 100kHz (Standard Speed) para mas stable
  Wire.setTimeOut(50);   // Dagdagan ang timeout (in milliseconds)

  display.begin(0x3C, true);
  display.clearDisplay();

  if (!asr.begin()) {
    Serial.println("Voice Recognition Module not found!");
  } else {
    Serial.println("Voice Recognition Module Ready.");
    // Set volume (0-10)
    asr.setVolume(10);
    // I-set ang wake time (gaano katagal bago matulog ulit ang module)
    asr.setWakeTime(15);
  }

  display.display();

  // FIX: Kahit tama ang constructor ng RoboEyes<AdafruitDisplay> sa library header
  // (RoboEyes(AdafruitDisplay &disp) : display(&disp) {}), sa runtime NULL pa rin
  // ang roboEyes.display pointer (nakumpirma via debug print) sa oras na tatawagin
  // ang roboEyes.begin() -- dahil dito nagcra-crash ang display->clearDisplay() sa
  // loob ng begin() (LoadProhibited, EXCVADDR 0x8). Malamang quirk ito sa pagconstruct
  // ng global templated object sa ESP32/Arduino toolchain. Public member naman pala
  // ang "display" sa loob ng RoboEyes class, kaya diretso na lang nating i-set dito
  // bago tumawag ng begin() -- ligtas na paraan para i-bypass ang broken constructor.
  roboEyes.display = &display;

  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);
  roboEyes.setCuriosity(ON);

  pinMode(BUZZER_PIN, OUTPUT);
  // Buzzer sa sarili nang LEDC channel/timer (hiwalay sa Servo at Motors) para
  // maiwasan ang conflict na dating sanhi ng biglaang paggalaw/paghinto ng servo.
  ledcSetup(BUZZER_LEDC_CHANNEL, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
  pinMode(LED_PIN, OUTPUT);

  //==== Line-following IR (Pit Detection - Active-HIGH, needs PULLDOWN)

  pinMode(IR_SENSOR_LEFT, INPUT_PULLDOWN);
  pinMode(IR_SENSOR_RIGHT, INPUT_PULLDOWN);

  // Avoidance IR (Reverted to 36/39 - Input only. Logic inverted in loop)

  pinMode(IR_AVOID_LEFT, INPUT);
  pinMode(IR_AVOID_RIGHT, INPUT);

  pinMode(PIR_MIC_PIN, INPUT_PULLDOWN);
  //pinMode(LDR_PIN, INPUT);

  //pinMode(BUTTON_WAKEUP, INPUT_PULLUP);

  // ----------------- NEW: Sleep IR pin -----------------

  //pinMode(SLEEP_IR_PIN, INPUT_PULLDOWN);
  //pinMode(BUTTON_DANCE, INPUT_PULLUP);

  digitalWrite(LED_PIN, LOW);

  pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT); pinMode(BIN2, OUTPUT);
  pinMode(STBY, OUTPUT); digitalWrite(STBY, HIGH);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  ledcSetup(CH_A, FREQ, RES);
  ledcSetup(CH_B, FREQ, RES);
  ledcAttachPin(PWMA, CH_A);
  ledcAttachPin(PWMB, CH_B);

  // >>>>>>>>>>>>>>>>>> WIFI & NTP SETUP (SMART BYPASS) <<<<<<<<<<<<<<<<<<
Serial.print("Connecting to WiFi: ");
Serial.println(ssid);
WiFi.begin(ssid, password);

int connectAttempts = 0;
// Binawasan natin ang attempts sa 10 para mas mabilis mag-boot sa labas
while (WiFi.status() != WL_CONNECTED && connectAttempts < 20) { 
  delay(300); // Binilisan ang delay para hindi halatang naghihintay
  Serial.print(".");
  roboEyes.anim_confused(); // OK lang ito basta hindi masyadong matagal
  updateEyes();
  connectAttempts++;
}

if (WiFi.status() == WL_CONNECTED) {
  Serial.println("\nWiFi connected.");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println("Time synced successfully.");
  }

  // --- FACE-TRACKING HTTP SERVER (FaceRobot Android app) ---
  // Gagana lang ang commands na natatanggap dito kapag STATE_BOOT_WAIT
  // ang currentState (tignan ang handleCommand()).
  server.on("/command", handleCommand);
  server.on("/ping", handlePing);
  server.begin();
  Serial.print("Face-Track HTTP Server Started! IP: ");
  Serial.println(WiFi.localIP());
} else {
  // Eto ang magic: Kung walang WiFi, patayin ang WiFi radio para makatipid sa battery
  WiFi.disconnect();
  // V5.3.8: HUWAG i-WIFI_OFF dito - nagdudulot ng "wifi not stop / Failed to deinit Wi-Fi driver"
  // at Guru Meditation crash dahil naka-init na ang ESP-NOW at Bluetooth. Disconnect lang.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  Serial.println("\nOffline Mode: walang WiFi connection (naka-standby ang radio para iwas crash).");
}
// >>>>>>>>>>>>>>>>>> END WIFI & NTP SETUP <<<<<<<<<<<<<<<<<<

  buzzOff();
  playBootSound_NB();
  roboEyes.setMood(HAPPY);

  movementStartTime = 0;
  currentState = STATE_BOOT_WAIT;

  stopBot();
  buzzOff();

roboEyes.setMood(TIRED);
roboEyes.setAutoblinker(ON, 8, 5);
roboEyes.setIdleMode(ON, 2, 2);
roboEyes.setCuriosity(OFF);

Serial.println("🤖 Robot powered ON. Waiting for BOSS RUSTY...");

// Seed the random number generator para hindi laging pareho ang shuffle
  randomSeed(analogRead(34));

  // Initialize Hardware Serial2 (RX=5, TX=17)
  Serial2.begin(9600, SERIAL_8N1, 5, 17); 
  
  Serial.println(F("🤖 Initializing Robot Audio System..."));
  delay(2000); // Mahalaga itong delay para sa SD card stability

  if (myDFPlayer.begin(Serial2)) {
    Serial.println(F("✅ SWAK! DFPlayer Online."));
    
    // --- Silent Kickstart Logic ---
    // Ginagawa natin ito para "ma-unlock" ang advertise() function
    myDFPlayer.volume(30);
    delay(100);
    myDFPlayer.play(3);      // I-play ang unang kanta sandali
    delay(2000);              // Hayaan mag-load ang SD card (0.8 sec)
    myDFPlayer.stop();       // Itigil agad
    
    Serial.println(F("🔊 Audio session unlocked and ready."));
  } 
  else {
    // Kapag ayaw mag-connect
    Serial.println(F("❌ NEGA. Check Wiring: ESP32 GPIO5(RX) to DFPlayer TX."));
    Serial.println(F("💡 Tip: Siguraduhing FAT32 ang format ng SD card."));
  }
}

// ========================= MOTOR CONTROL =========================

void forward() {
  digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW);
  ledcWrite(CH_A, currentBaseSpeed); ledcWrite(CH_B, currentBaseSpeed);
}

void reverse() {
  digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH);
  digitalWrite(BIN1, LOW); digitalWrite(BIN2, HIGH);
  ledcWrite(CH_A, currentBaseSpeed); ledcWrite(CH_B, currentBaseSpeed);
}

void turnRight() {
  digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, LOW); digitalWrite(BIN2, HIGH);
  ledcWrite(CH_A, currentTurnSpeed); ledcWrite(CH_B, currentTurnSpeed);
}

void turnLeft() {
  digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH);
  digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW);
  ledcWrite(CH_A, currentTurnSpeed); ledcWrite(CH_B, currentTurnSpeed);
}

void stopBot() {
  ledcWrite(CH_A, 0);
  ledcWrite(CH_B, 0);
}

// V5.3.9: Mabagal na spin para sa camera-nav. Kaparehong pin pattern ng turnLeft()/turnRight(), iba lang ang bilis.
void navSpin(bool left) {
  if (left) {
    digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH);
    digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW);
  } else {
    digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);
    digitalWrite(BIN1, LOW);  digitalWrite(BIN2, HIGH);
  }
  ledcWrite(CH_A, NAV_TURN_SPEED);
  ledcWrite(CH_B, NAV_TURN_SPEED);
}

// ========================= ULTRASONIC =========================

long getDistance() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long duration = pulseIn(ECHO, HIGH, 30000);
  return duration * 0.034 / 2;
}

// ========================= FACE-TRACKING HTTP COMMAND HANDLER =========================
// Tumatanggap ng commands mula sa Android FaceRobot app
// (http://<esp32-ip>/command?dir=FORWARD|BACKWARD|LEFT|RIGHT|STOP|SEARCH)
//
// GAGANA LANG ITO KAPAG currentState == STATE_BOOT_WAIT.
// Sa lahat ng ibang states (STATE_MOVING, STATE_SLEEPING_IR, STATE_DANCING, atbp.)
// i-i-IGNORE ang command para hindi ito makipag-agawan sa kung anumang ginagawa
// ng robot sa main FSM sa oras na yun.
void handleCommand() {
  if (!server.hasArg("dir")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }

  String dir = server.arg("dir");
    onAppContact();

  if (dir == "FORCE_STOP") {
    myDFPlayer.stop();
    stopBot();
    buzzOff();
    myDFPlayer.play(10); // Play the "Yes Boss" advert
    navHint = 0; // V5.3.9
    isMusicActive = false;
    danceStartTime = 0;
    digitalWrite(LED_PIN, LOW);
    ledState = LOW;
    roboEyes.setMood(DEFAULT);
    currentState = STATE_BOOT_WAIT;
    Serial.println("🛑 Face-Track: FORCE STOP (anumang state)");
    lastFaceCommandTime = millis();
    server.send(200, "text/plain", "OK: FORCE_STOP");
    return;
}

// V5.3.6: AUTO - tinatanggap sa ANUMANG state (hindi na kailangan STATE_BOOT_WAIT).
// Dati, kapag nasa STATE_PIR_ACTIVE_AND_DETECTING / SHAKING / EXPRESSION / DANCING / SLEEPING ang robot,
// nai-i-IGNORE ang AUTO kahit narinig ng app, at hindi ito nire-retry ng app.
else if (dir == "AUTO") {
    stopBot();
    digitalWrite(LED_PIN, LOW);
    ledState = LOW;
    danceStartTime = 0;
    isDisplayingNumber = false;
    ipScreenNeedsDraw = true;
    isCrying = false;
    roboEyes.setSweat(OFF);
    sleepAvoidActive = false;
    navHint = 0; // V5.3.9
    isPirDetectionActive = false;
    isTiredFaceDone = false;
    consecutiveIrTriggers = 0;
    currentBaseSpeed = 160;
    currentTurnSpeed = 220;
    roboEyes.setMood(DEFAULT);
    currentState = STATE_MOVING;
    movementStartTime = millis();
    lastFaceCommandTime = millis();
    Serial.println("🤖 Face-Track: AUTO MODE ACTIVATED (any state)");
    server.send(200, "text/plain", "OK: AUTO");
    return;
}

// V5.3.9: CAMERA NAV HINTS (depth model sa FaceRobot app) - tinatanggap lang habang MOVING / AVOIDING.
// NAV_LEFT  = lumiko pakaliwa (libre ang kaliwa)     NAV_RIGHT = lumiko pakanan (libre ang kanan)
// NAV_BACK  = dead end / harang sa harap -> avoidance sequence (reverse, pause, turn)
// NAV_CLEAR = libre ang daan -> forward
else if (dir.startsWith("NAV_")) {
    bool navState = (currentState == STATE_MOVING || currentState == STATE_AVOIDING_REVERSE ||
                     currentState == STATE_AVOIDING_STOP || currentState == STATE_AVOIDING_TURN);
    if (!navState) {
      server.send(200, "text/plain", "IGNORED: not in MOVING");
      return;
    }
    if (server.hasArg("servo")) {
      setServoAngle(server.arg("servo").toInt()); // camera tilt para sa nav scan
    }
    if (currentState == STATE_MOVING) {
      if (dir == "NAV_LEFT") navHint = 1;
      else if (dir == "NAV_RIGHT") navHint = 2;
      else if (dir == "NAV_BACK") navHint = 3;
      else navHint = 4; // NAV_CLEAR
      navHintTime = millis();
      static unsigned long lastNavLog = 0;   // debug: isang print bawat 1s lang para hindi mapuno ang Serial
      if (millis() - lastNavLog > 1000) {
        lastNavLog = millis();
        Serial.print("NAV hint received: ");
        Serial.println(dir);
      }
    }
    server.send(200, "text/plain", "OK: " + dir);
    return;
}

else if (dir == "MUSIC_ON") {
    Serial.println("🎵 Face-Track: MUSIC MODE ON (shuffle)");
    isMusicActive = true;
    myDFPlayer.play(3); // Play the first track to initialize
    delay(2000); // Wait for 2 seconds to ensure the player is ready
    currentSongNumber = random(1, totalMusicFiles + 1);
    playSongTracked(currentSongNumber);
    roboEyes.setMood(HAPPY);
}
else if (dir == "MUSIC_OFF") {
    Serial.println("🎵 Face-Track: MUSIC MODE OFF");
    isMusicActive = false;
    myDFPlayer.stop();
    roboEyes.setMood(DEFAULT);
}
else if (dir == "MUSIC_NEXT") {
    if (isMusicActive) {
      currentSongNumber++;
      if (currentSongNumber > totalMusicFiles) currentSongNumber = 1;
      myDFPlayer.play(3); // Play the first track to initialize
      delay(2000); // Wait for 2 seconds to ensure the player is ready
      playSongTracked(currentSongNumber);
      Serial.print("🎵 Next Track: ");
      Serial.println(currentSongNumber);
    }
}

  if (currentState != STATE_BOOT_WAIT) {
    Serial.print("Face-Track Command IGNORED (currentState != STATE_BOOT_WAIT): ");
    Serial.println(dir);
    server.send(200, "text/plain", "IGNORED: Robot busy, not in STATE_BOOT_WAIT");
    return;
  }

  Serial.print("Face-Track Command Received: ");
  Serial.println(dir);

  bool leftPit = digitalRead(IR_SENSOR_LEFT) == HIGH;
  bool rightPit = digitalRead(IR_SENSOR_RIGHT) == HIGH;

  // --- DIRECTION & ACTION COMMANDS ---
  if (dir == "FORWARD") {
    if (leftPit || rightPit) {
      Serial.println("⚠️ Forward blocked! Pit detected.");
      stopBot();
      roboEyes.setMood(ANGRY);
    }
    else if (currentDistance > 0 && currentDistance < 5) {
      reverse();
      roboEyes.setMood(ANGRY);
    }
    else if (currentDistance >= 5 && currentDistance < 15) {
      stopBot();
      static unsigned long lastFwdBlink = 0;
      if (millis() - lastFwdBlink > 5000) {
        roboEyes.anim_laugh();
        lastFwdBlink = millis();
      }
      roboEyes.setMood(HAPPY);
    }
    else {
      digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);
      digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW);
      ledcWrite(CH_A, FACE_FORWARD_SPEED); ledcWrite(CH_B, FACE_FORWARD_SPEED);
      roboEyes.setMood(DEFAULT);
    } 
  } 
  else if (dir == "GREET") {
    if (server.hasArg("track")) {
      int trackNum = server.arg("track").toInt();
      playAdvertOverMusic(trackNum); // V5.3.10: may music = advertise (pause+resume), walang music = play
      Serial.print("🔊 Face-Track: GREETING TRACK ");
      Serial.println(trackNum);
    }
  } 
  else if (dir == "PLAY") {
    int trackNum = 3;
    if (server.hasArg("track")) {
      int trackNum = server.arg("track").toInt();
      playAdvertOverMusic(trackNum); // V5.3.10: may music = advertise (pause+resume), walang music = play
      Serial.print("🔊 Face-Track: PLAY TRACK ");
      Serial.println(trackNum);
    }
}
  else if (dir == "BACKWARD") {
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH);
    digitalWrite(BIN1, LOW); digitalWrite(BIN2, HIGH);
    ledcWrite(CH_A, FACE_FORWARD_SPEED); ledcWrite(CH_B, FACE_FORWARD_SPEED);
    roboEyes.setMood(DEFAULT);
  } 
  else if (dir == "LEFT") {
    digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW);
    digitalWrite(BIN1, LOW); digitalWrite(BIN2, HIGH);
    ledcWrite(CH_A, FACE_TURN_SPEED); ledcWrite(CH_B, FACE_TURN_SPEED);
    roboEyes.anim_confused();
  } 
  else if (dir == "RIGHT") {
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH);
    digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW);
    ledcWrite(CH_A, FACE_TURN_SPEED); ledcWrite(CH_B, FACE_TURN_SPEED);
    roboEyes.anim_confused();
  } 
  else if (dir == "DANCE") {
    currentState = STATE_DANCING;
    danceStartTime = millis();
    lastDanceStepTime = millis();
    songInterrupted = true; myDFPlayer.play(12);
    danceStep = 0;
    atrasAbanteCycleCount = 0;
    roboEyes.setMood(HAPPY);
    roboEyes.anim_laugh();
    Serial.println("💃 Face-Track: DANCE MODE ACTIVATED");
  } 
  else if (dir == "STOP" || dir == "SEARCH") {
    stopBot();
    roboEyes.setMood(DEFAULT);
  } 
  else if (dir == "SHAKING") {
    stopBot();
    roboEyes.setMood(ANGRY);
    roboEyes.anim_laugh();
    shakeDuration = 3000;
    shakeStartTime = millis();
    currentState = STATE_SHAKING;
    Serial.println("🤖 Face-Track: SHAKING MODE ACTIVATED");
  }
  // (Ang AUTO ay hina-handle na sa itaas, bago ang state gate - tignan ang V5.3.6 note)
  else if (dir == "OPM") {
    hulingTugtog = 0;
    Serial.println("🖥️ Face-Track: RUSTECH OPM DISPLAY STARTED (30s block)");
    unsigned long lockStartTime = millis();
    while (millis() - lockStartTime < 30000) {
      showRustechOPM();
      delay(100);
      yield();
    }
    Serial.println("🖥️ Face-Track: RUSTECH OPM DISPLAY FINISHED");
    ipScreenNeedsDraw = true; // V5.3.4: ibalik agad ang IP screen pagkatapos ng OPM display
    if (appLinked) lastAppContactTime = millis(); // V5.3.5: nag-block ng 30s, kaya huwag ituring na disconnected ang app
  } 
  else if (dir == "LASER_ON") {
    digitalWrite(2, HIGH);
    Serial.println("🔦 Face-Track: LASER ON");
    playAdvertOverMusic(13);
    playBootSound_NB();
  } 
  else if (dir == "LASER_OFF") {
    digitalWrite(2, LOW);
    Serial.println("🔦 Face-Track: LASER OFF");
    playAdvertOverMusic(14);
    playSleepBeep_NB();
  }  
  else if (dir == "LIFT_UP") {
    setServoAngle(SERVO_MAX_ANGLE);
    Serial.println("🦾 Face-Track: SERVO LIFT UP");
  } 
  else if (dir == "LIFT_DOWN") {
    setServoAngle(SERVO_MIN_ANGLE);
    Serial.println("🦾 Face-Track: SERVO LIFT DOWN");
  }

  // --- SERVO TRACKING CHECK ---
  if (server.hasArg("servo")) {
    int servoAngleReq = server.arg("servo").toInt();
    setServoAngle(servoAngleReq);
    Serial.print("Servo value received & applied: ");
    Serial.println(servoAngleReq);
  } else {
    Serial.println("Has servo arg? NO");
  }  
  
  // Final update & HTTP Response
  lastFaceCommandTime = millis();
  server.send(200, "text/plain", "OK: " + dir);
}

// ========================= MAIN LOOP (W/ SLEEP AVOID FIX & BEEP) =========================

void loop() {
  unsigned long now = millis();
  updateServo();
  checkAlarm();
  updateBuzzer();
  handle_bluetooth_data();
  server.handleClient(); // Palaging naka-listen sa FaceRobot app; ang aktwal na pag-galaw
                          // ay naka-gate sa loob ng handleCommand() (STATE_BOOT_WAIT lang)

//=========== PINAGSAMANG MUSIC & ADVERT LOGIC (V5.3.10 FIX) =================
if (myDFPlayer.available()) {
    uint8_t type = myDFPlayer.readType();
    int value = myDFPlayer.read(); 

    Serial.print("[DF] type="); Serial.print(type);
    Serial.print(" value="); Serial.print(value);
    Serial.print(" t="); Serial.print(millis());
    Serial.print(" advertFlag="); Serial.print(isAdvertPlaying);
    Serial.print(" robot="); Serial.print(stateName(currentState));
    Serial.print(" song="); Serial.println(currentSongNumber);

    if (type == DFPlayerPlayFinished) {
        unsigned long t = millis();

        // 1. DUPLICATE FILTER: madalas dalawang beses ipadala ng DFPlayer ang parehong "finished"
        bool duplicate = (value == lastFinishedValue && (t - lastFinishedEventTime) < 1500);
        lastFinishedValue = value;
        lastFinishedEventTime = t;

        if (duplicate) {
            Serial.print("Duplicate finished event ignored: ");
            Serial.println(value);
        }
        // 2. Advert ang natapos -> resume lang, HUWAG mag-next
        else if (isAdvertPlaying) {
            isAdvertPlaying = false;
            advertGuardUntil = t + 1500;   // ignore ang anumang extra event pagkatapos nito
            Serial.println("Advert done. Resuming music (no next)...");
            if (isMusicActive) {
                resumePending = true;
                resumeStage = 1;
                resumeAt = t + 600;   // bigyan ng oras ang DFPlayer na mag-auto-resume
            }
        }
        // 3. Nasa guard window pa (kakatapos lang ng advert / kakalipat ng track) -> ignore
        else if ((long)(advertGuardUntil - t) > 0) {
            Serial.print("Finished event ignored (guard window): ");
            Serial.println(value);
        }
        // 4. Tunay na natapos ang kanta -> shuffle next
        else if (isMusicActive) {
            // HUWAG agad mag-next: itanong muna sa DFPlayer kung tumutugtog pa (tingnan sa baba)
            nextCheckPending = true;
            nextCheckStage = 1;
            nextCheckAt = t + 700;
            Serial.print("Finished event for track ");
            Serial.print(value);
            Serial.print(" (kanta nag-play ng ");
            Serial.print(songPlayedMs / 1000);
            Serial.print("s, adverts sa kantang ito: ");
            Serial.print(advertsThisSong);
            Serial.println(") -> verifying DFPlayer state...");
        }
        else {
            Serial.print("Sound ");
            Serial.print(value);
            Serial.println(" finished. Idle mode.");
        }
    }
}

// Non-blocking resume - STATE-AWARE (V5.3.14)
// Natuklasan: kapag nag-advert NG MALAPIT NA MATAPOS ang kanta, "stopped" na ang DFPlayer pagkatapos ng advert.
// Ang dating start() ay nagre-restart ng kasalukuyang kanta mula sa umpisa = "inuulit ang kanta".
// Ngayon: playing = ayos na | paused = start() | stopped = tapos na ang kanta -> NEXT (hindi start()).
if (resumePending && (long)(millis() - resumeAt) >= 0) {
    if (!isMusicActive || isAdvertPlaying) {
        resumePending = false;   // na-stop ang music o may bagong advert
    } else {
        int stRaw = myDFPlayer.readState();                 // 512 stopped, 513 playing, 514 paused
        int st = (stRaw < 0) ? stRaw : (stRaw & 0xFF);
        Serial.print("[RESUME] check "); Serial.print(resumeStage);
        Serial.print(" raw="); Serial.print(stRaw);
        Serial.print(" -> "); Serial.println(st);
        if (st == 1) {
            resumePending = false;
            Serial.println("[RESUME] Tumutugtog na ang kanta (auto-resume ng DFPlayer) -> WALANG start().");
        }
        else if (st == 2) {
            resumePending = false;
            Serial.println("[RESUME] Naka-pause -> start().");
            myDFPlayer.start();
        }
        else if (resumeStage == 1) {
            resumeStage = 2;
            resumeAt = millis() + 800;   // baka nasa gitna pa ng resume, tingnan ulit
        }
        else {
            resumePending = false;
            // STOPPED pagkatapos ng advert = natapos na ang kanta habang may advert -> next, HUWAG i-restart
            currentSongNumber = pickRandomSong();
            playSongTracked(currentSongNumber);
            advertGuardUntil = millis() + 1000;
            Serial.print("[RESUME] Tapos na pala ang kanta -> NEXT. Now Playing: ");
            Serial.println(currentSongNumber);
        }
    }
}

// SONG WATCHDOG: kapag lumampas na sa haba ng kanta pero walang "finished" event -> FORCE NEXT
{
    unsigned long tickNow = millis();
    unsigned long dt = tickNow - lastSongTick;
    lastSongTick = tickNow;
    if (isMusicActive && !isAdvertPlaying && !nextCheckPending) {
        songPlayedMs += dt;   // hindi binibilang habang may advert (naka-pause ang kanta)
        unsigned long songLimit = songLimitMs(currentSongNumber);
        if (songLimit > 0 && songPlayedMs > songLimit) {
            Serial.print("[WATCHDOG] Track ");
            Serial.print(currentSongNumber);
            Serial.print(" lumampas na (");
            Serial.print(songPlayedMs / 1000);
            Serial.println("s) walang finished event -> FORCE NEXT.");
            currentSongNumber = pickRandomSong();
            playSongTracked(currentSongNumber);
            advertGuardUntil = millis() + 1000;
            Serial.print("Now Playing: ");
            Serial.println(currentSongNumber);
        }
    }
}

// VERIFY: totoo bang tapos na ang kanta? (2 beses titingnan bago mag-next)
if (nextCheckPending && (long)(millis() - nextCheckAt) >= 0) {
    if (!isMusicActive || isAdvertPlaying) {
        nextCheckPending = false;   // may advert na pumasok / na-stop na ang music
    } else {
        // SD card: 512 = stopped, 513 = playing, 514 = paused (nasa low byte ang tunay na state)
        int stRaw = myDFPlayer.readState();
        int st = (stRaw < 0) ? stRaw : (stRaw & 0xFF);   // 0 = stopped, 1 = playing, 2 = paused
        Serial.print("DFPlayer state (check "); Serial.print(nextCheckStage);
        Serial.print(") raw="); Serial.print(stRaw);
        Serial.print(" -> "); Serial.println(st);
        if (st == 1) {
            nextCheckPending = false;
            Serial.println("Tumutugtog pa -> false finished event, HINDI mag-next.");
        }
        else if (st == 2) {
            nextCheckPending = false;
            Serial.println("Naka-pause pala -> resume lang, HINDI mag-next.");
            myDFPlayer.start();
        }
        else if (nextCheckStage == 1) {
            // baka nasa gitna pa ng resume -> bigyan ng pangalawang pagkakataon
            nextCheckStage = 2;
            nextCheckAt = millis() + 800;
        }
        else {
            nextCheckPending = false;
            if (!songInterrupted) learnSongDuration(currentSongNumber, songPlayedMs / 1000);
            currentSongNumber = pickRandomSong();
            playSongTracked(currentSongNumber);
            advertGuardUntil = millis() + 1000;
            Serial.print("Tapos na talaga. Now Playing: ");
            Serial.println(currentSongNumber);
        }
    }
}

// Safety net: kung hindi dumating ang "finished" ng advert, i-clear din ang flag
// (ang guard window ang sasalo sa late/duplicate events)
if (isAdvertPlaying && (millis() - advertStartTime > ADVERT_DURATION)) {
    isAdvertPlaying = false;
    advertGuardUntil = millis() + 1500;
}

  // --- BUTTON DEBOUNCING LOGIC ---

//  int reading = digitalRead(BUTTON_WAKEUP);
//  if (reading != lastButtonState) {
//    lastButtonDebounceTime = now;
//  }

//  if ((now - lastButtonDebounceTime) > DEBOUNCE_DELAY) {
//    if (reading != buttonState) {
//      buttonState = reading;

//      if (buttonState == LOW && currentState == STATE_BOOT_WAIT) {
//  display.clearDisplay();
//  buzzOff();
//  playBootSound_NB();

//  roboEyes.setMood(HAPPY);
//  roboEyes.setAutoblinker(ON, 3, 2);
//  roboEyes.setIdleMode(ON, 2, 2);
//  roboEyes.setCuriosity(ON);

//  movementStartTime = now;
//  currentState = STATE_MOVING;

//  Serial.println("✅ WAKE BUTTON PRESSED — Robot STARTING!");
//}
//    }
//  }
//  lastButtonState = reading;

  if (currentState == STATE_SHAKING) {
    unsigned long elapsed = millis() - shakeStartTime;
    
    // Gagamit tayo ng variable na 'shakeDuration' imbes na fixed na 3000
    if (elapsed < shakeDuration) { 
      if ((elapsed / 150) % 2 == 0) {
        turnLeft();
      } else {
        turnRight();
      }
    } else {
      stopBot();
      roboEyes.setMood(DEFAULT);
      currentState = STATE_BOOT_WAIT;
      shakeDuration = 3000; // I-reset sa default (3s) para sa susunod na command
      Serial.println("Shake finished!");
    }
}

  // ====== NEW: DANCE BUTTON TRIGGER LOGIC (GPIO 5) ======

//  int danceReading = digitalRead(BUTTON_DANCE);

  // Assume same debounce variables are reused for simplicity if no conflict
  // If you want separate debouncing, you need to declare new global variables.

//  if (danceReading == LOW && currentState == STATE_SLEEPING_IR) {
    // Button is pressed and robot is sleeping
//    danceStartTime = now;
//    lastDanceStepTime = now;
//    danceStep = 0; // Simulan sa Atras/Abante

    // WAKE-UP/START DANCE ROUTINE
//    display.clearDisplay();
//    alarmTriggerTime = 0;
//    buzzOff();
//    playBootSound_NB();
//    roboEyes.setMood(HAPPY);
//    roboEyes.anim_laugh();
//    roboEyes.setAutoblinker(ON, 2, 1);
//    roboEyes.setIdleMode(ON, 1, 1);
//    roboEyes.setCuriosity(OFF);
//    isTiredFaceDone = false;

//    currentState = STATE_DANCING;
//    Serial.println("💃 Dance Mode Activated by Button 5! Time to Party!");
//  }

  //int ldrValue = analogRead(LDR_PIN);

 // --- LDR HEADLIGHT BEHAVIOR ---
// Dagdagan natin ng check: "Dapat HINDI STATE_DANCING"
 //if (currentState != STATE_DANCING) { 
   //int ldrValue = analogRead(LDR_PIN);

   //if (ldrValue > HEADLIGHT_THRESHOLD) {
     //digitalWrite(LED_PIN, HIGH);
   //} else {
     //digitalWrite(LED_PIN, LOW);
   //}
 //}

  // --- ULTRASONIC NON-BLOCKING UPDATE (LAG FIX) ---

  if (now - lastDistanceReadTime >= DISTANCE_READ_INTERVAL) {
    currentDistance = getDistance();
    lastDistanceReadTime = now;
  }
  long distance = currentDistance;

  // Line-following IR (Pit Detection)

  bool leftPit = digitalRead(IR_SENSOR_LEFT) == HIGH;
  bool rightPit = digitalRead(IR_SENSOR_RIGHT) == HIGH;
  bool anyPitDetected = leftPit || rightPit;
  if (now < pitDisableUntil) {
    leftPit = false;
    rightPit = false;
    anyPitDetected = false;
  }
  // Avoidance IR (Obstacle Detection) - Inverted logic: HIGH = No object, LOW = Object Detected
  bool leftAvoid = digitalRead(IR_AVOID_LEFT) == LOW;
  bool rightAvoid = digitalRead(IR_AVOID_RIGHT) == LOW;

  // ========= All triggers DITO BABAGUHIN ANG DISTANCE NG ULTRASONIC ===========
  // ========= All triggers DITO BABAGUHIN ANG DISTANCE NG ULTRASONIC ===========

  bool anyAvoidanceTrigger = anyPitDetected ||
   !leftAvoid ||
   !rightAvoid ||
   (distance > 0 && distance < 10);
  bool isTriggered = false;
  
  // --- BAGONG KONDISYON: Tiyakin na ang kasalukuyang oras (now) ay HINDI pa tapos sa disable time
  if (now < pirDisableUntil) {
      Serial.print("PIR Disabled. Remaining: ");
      Serial.print((pirDisableUntil - now) / 1000);
      Serial.println("s");
      // Huwag nang mag-check ng PIR sensor kung naka-disable
      isPirDetectionActive = false; // Tiyakin na naka-OFF ang flag
      
      // Update sa Display habang disabled ang PIR (optional)
      if (currentState == STATE_PIR_ACTIVE_AND_DETECTING) {
           roboEyes.setMood(DEFAULT); // Para hindi siya galawin
      }
      
  } else {
        // Kapag tapos na ang disable time, pwede na siyang mag-check ng distansya (Ultrasonic)
    if (isPirDetectionActive && distance > 0 && distance < PRESENCE_TRIGGER_CM) {
      if (now - lastDetectionTime >= detectionCooldown) {
        isTriggered = true;
        lastDetectionTime = now;
        Serial.println(">>> ULTRASONIC: PRESENCE DETECTED <<<");
      }
    }
  }

  // ====================== CONSECUTIVE IR TRIGGER LOGIC (PIT SENSORS ONLY - NEW LOGIC) =========================

  if (anyPitDetected) {
    if (now - lastIrTriggerTime < IR_TRIGGER_TIMEOUT) {
      consecutiveIrTriggers++;
      Serial.print("Stuck Counter: ");
      Serial.println(consecutiveIrTriggers);
    } else {
      consecutiveIrTriggers = 1;
    }
    lastIrTriggerTime = now;
  } else {
    if (now - lastIrTriggerTime >= IR_RESET_TIMEOUT && consecutiveIrTriggers > 0) {
      consecutiveIrTriggers = 0;
      Serial.println("Stuck Counter RESET (Robot free for 10s).");
    }
  }

  // ===============+=== END STUCK DETECTION LOGIC (FIXED) =========================
  // === Check for IR Sleep Condition (PERMANENT STUCK/PIT MODE) ===================

  if (consecutiveIrTriggers >= IR_TRIGGER_MAX) {
    if (currentState != STATE_SLEEPING_IR && currentState != STATE_SLEEP_ALERT) {
      stopBot();
      playSleepBeep_NB();

      alarmTriggerTime = 0;

      buzzOff();
      roboEyes.setMood(TIRED);
      roboEyes.setAutoblinker(ON, 8, 5);
      roboEyes.setIdleMode(ON, 2, 2);
      roboEyes.setCuriosity(ON);

      sleepModeStartTime = now;
      isTiredFaceDone = false;

      currentState = STATE_SLEEPING_IR;
      Serial.println("Robot entering PERMANENT SLEEP (Pit/Stuck Detection).");
      consecutiveIrTriggers = 0;

      if (!isDisplayingNumber) {
    updateEyes(); 
  }
      return;
    }
  }

  // ================ MAIN FSM =========================================

 // ================= VOICE RECOGNITION LOGIC (STABLE VERSION) =================
 // ================= VOICE COMMAND HANDLER ===========================
static unsigned long lastCheck = 0;

if (millis() - lastCheck > 300) { 
  lastCheck = millis();
  uint8_t cmdID = asr.getCMDID(); 
  yield();

  if (cmdID != 0) {
    Serial.print("Command Detected: ");
    Serial.println(cmdID);
    
     if (cmdID == 1) {
    Serial.println("Robot: Wake Word Detected!");

    playAdvertOverMusic(3);

    // --- Animations ---
    stopBot();
    roboEyes.setMood(ANGRY);
    roboEyes.anim_laugh();
    shakeDuration = 1270;
    shakeStartTime = millis();
    currentState = STATE_SHAKING;
    playBootSound_NB();
    cmdID = 0;
}

    // 1. WAKE WORD (Hello Robot)
 if (cmdID == 2) {
    Serial.println("Robot: Wake Word Detected!");

    playAdvertOverMusic(2);

    // --- Animations ---
    stopBot();
    roboEyes.setMood(ANGRY);
    roboEyes.anim_laugh();
    shakeDuration = 1270;
    shakeStartTime = millis();
    currentState = STATE_SHAKING;
    playBootSound_NB();
    cmdID = 0;
}

    // 2. DANCE COMMAND (ID 5)
   else if (cmdID == 5) { 
    Serial.println("💃 Starting Dance Mode...");
    
    unsigned long now = millis();
    danceStartTime = now;          
    lastDanceStepTime = now;
    songInterrupted = true; myDFPlayer.play(12);       
    atrasAbanteCycleCount = 0;   
    danceStep = 0;               
    
    // Debugging lines para makita natin sa Serial Monitor:
    Serial.print("Dance Start Time: "); Serial.println(danceStartTime);
    Serial.print("Target Duration: "); Serial.println(DANCE_DURATION);
    
    currentState = STATE_DANCING; 
    roboEyes.setMood(HAPPY);
    roboEyes.anim_laugh();
}

    // 3. MOVE FORWARD (ID 22)
    else if (cmdID == 130) {
      playAdvertOverMusic(7);
      Serial.println("Robot: Moving Forward...");
      roboEyes.setMood(HAPPY);
      forward();
      movementStartTime = now;
      // Pwedeng lagyan ng auto-stop after 2 seconds para hindi bumangga
      // O kaya hayaan mo lang kung gusto mo ikaw mag-stop via voice
      currentState = STATE_MOVING; 
    }
    else if (cmdID == 80) { 
  Serial.println("OSCILLATING MOVEMENT STARTED"); 
  
   // Imbes na mag-while loop, magse-set lang tayo ng markers
   if (currentState != STATE_SHAKING) { // Siguraduhin na hindi pa nag-she-shake
     currentState = STATE_SHAKING;      // Gawa ka ng bagong State sa taas ng code mo
     shakeStartTime = millis();
     playBootSound_NB();         // I-record ang simula (Dapat defined itong variable sa itaas)
     roboEyes.setMood(HAPPY);
   }
 }

    // 4. STOP COMMAND (Dapat ay ibang ID, halimbawa ID 10)
    else if (cmdID == 93) { 
      Serial.println("🛑 STOP COMMAND RECEIVED"); // STOP PLAYING
      isMusicActive = false;
      myDFPlayer.stop();
      stopBot();              // Itigil ang mga motor
       myDFPlayer.play(10);
      currentState = STATE_BOOT_WAIT; // Patayin ang kahit anong state (Dancing o Moving)
      danceStartTime = 0;     // Siguradong hinto ang dance timer
      playBootSound_NB();
      roboEyes.setMood(ANGRY);
      Serial.println("Robot: I have stopped.");
    }

   // 5. DISPLAY SMILEY FACE (ID 62)
    else if (cmdID == 62) { 
      Serial.println("😊 Mood: Smiley/Happy");
      stopBot(); 
      isCrying = false;
      roboEyes.setSweat(OFF);
      roboEyes.setMood(HAPPY);
      roboEyes.anim_laugh();
      playBootSound_NB(); // Beep feedback
      
      currentState = STATE_EXPRESSION; // Lipat sa expression state
      cryingStartTime = millis();      // Gamitin natin itong timer para sa duration (5 seconds)
    }

    // 6. DISPLAY CRYING FACE (ID 63)
    else if (cmdID == 63) { 
      Serial.println("😢 Mood: Crying/Sad");
      stopBot();
      roboEyes.setMood(TIRED); 
      roboEyes.setSweat(ON);
      playCuriousBeep_NB(); // Beep feedback
      
      isCrying = true;
      currentState = STATE_EXPRESSION; // Lipat sa expression state
      cryingStartTime = millis();      // Simula ng 5 seconds
    }

   // 8. DISPLAY NUMBER 1 (ID 53)
    else if (cmdID == 53) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 1");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("1");
      display.display();
    }

    // 9. DISPLAY NUMBER 2 (ID 54)
    else if (cmdID == 54) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 2");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("2");
      display.display();
    }

    // 10. DISPLAY NUMBER 3 (ID 55)
    else if (cmdID == 55) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 3");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("3");
      display.display();
    }

    // 11. DISPLAY NUMBER 4 (ID 56)
    else if (cmdID == 56) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 4");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("4");
      display.display();
    }

    // 12. DISPLAY NUMBER 5 (ID 57)
    else if (cmdID == 57) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 5");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("5");
      display.display();
    }
   
    // 13. DISPLAY NUMBER 6 (ID 58)
    else if (cmdID == 58) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 6");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("6");
      display.display();
    }

    // 14. DISPLAY NUMBER 7 (ID 59)
    else if (cmdID == 59) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 7");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("7");
      display.display();
    }

    // 15. DISPLAY NUMBER 8 (ID 60)
    else if (cmdID == 60) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 8");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("8");
      display.display();
    }

    // 16. DISPLAY NUMBER 9 (ID 61)
    else if (cmdID == 61) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 9");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("9");
      display.display();
    }

    // 17. DISPLAY NUMBER 0 (ID 52)
    else if (cmdID == 52) {
      playAdvertOverMusic(3); 
      Serial.println("🔢 Command: Display Number 0");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; // Ito ang magsasabi sa loop na huwag mag-drawing ng mata
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      display.setTextSize(7);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(45, 10);       
      display.print("0`");
      display.display();
    }

    // 18. DISPLAY PERFECT HEART (ID 64)
    else if (cmdID == 64) {
      playAdvertOverMusic(3); 
      Serial.println("❤️ Command: Display Perfect Heart");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      isDisplayingNumber = true; 
      currentState = STATE_EXPRESSION;
      cryingStartTime = millis();
      
      display.clearDisplay();
      
      // 1. Dalawang Bilog sa taas (Lalo nating pinagdikit)
      display.fillCircle(45, 25, 17, SH110X_WHITE); // Kaliwa
      display.fillCircle(83, 25, 17, SH110X_WHITE); // Kanan
      
      // 2. Rectangle sa gitna para mapuno yung "V" gap sa taas ng triangle
      display.fillRect(35, 25, 58, 15, SH110X_WHITE);
      
      // 3. Triangle sa baba (Mas malapad para smooth ang koneksyon)
      display.fillTriangle(31, 35, 97, 35, 64, 62, SH110X_WHITE);
      
      display.display(); 
    }

    // 19. DISPLAY TIME (ID 8)
    else if (cmdID == 8) {
      playAdvertOverMusic(4); 
      Serial.println("⏰ Command: Display Time");
      stopBot(); 
      playBootSound_NB(); // Beep feedback
      
      isDisplayingNumber = true; // Gagamitin natin ito para ma-lock ang screen
      currentState = STATE_EXPRESSION; 
      cryingStartTime = millis();
      
      // Tatawagin natin ang existing function mo para mag-display ng oras
      displayTime(); 
    }
    
    // 20. MOVE FORWARD (ID 22)
    else if (cmdID == 22) {
      playAdvertOverMusic(5); 
      Serial.println("🤖 Command: MOVE FORWARD");
      bool leftPit = digitalRead(IR_SENSOR_LEFT);
      bool rightPit = digitalRead(IR_SENSOR_RIGHT); 

    if (leftPit == HIGH || rightPit == HIGH) { // KUNG NAKAKITA NG BUTAS
      reverse();
      roboEyes.setMood(ANGRY); // O kahit anong "panic" mood
    }
      roboEyes.setMood(HAPPY);
      forward(); 
      
      delay(700); // Bahagyang delay para maiwasan ang sobrang dali ng pag-stop 
      stopBot(); // Agad na stop para sa voice command
    }

    // 21. MOVE BACKWARD (ID 23)
    else if (cmdID == 23) {
      playAdvertOverMusic(6); 
      Serial.println("🤖 Command: MOVE BACKWARD");
      roboEyes.setMood(ANGRY);
      reverse();
      
      delay(700); // Bahagyang delay para maiwasan ang sobrang dali ng pag-stop 
      stopBot(); // Agad na stop para sa voice command
    }
    // 22. LOCK MUSIC MODE TO /MP3 FOLDER (ID 92)
  if (cmdID == 92) {
    Serial.println("Music Mode: Shuffle Start");
    isMusicActive = true;
    myDFPlayer.play(3);
    delay(2000); // Konting pahinga para matapos ang sound
    
    // Kukuha ng random number mula 1 hanggang sa total ng files mo
    currentSongNumber = random(1, totalMusicFiles + 1); 
    
    playSongTracked(currentSongNumber);
    isAdvertPlaying = false; resumePending = false; nextCheckPending = false;
    
    Serial.print("Shuffled Start! Playing Track: ");
    Serial.println(currentSongNumber);
    cmdID = 0;
}

    // 23. NEXT TRACK COMMAND (ID 95)
  if (cmdID == 95) {
    Serial.println("Command: Next Track");

    // 1. Opsyonal: Voice feedback para alam mong narinig ka
    // "Ok, next song!" (Siguraduhing may 0002.mp3 sa ADVERT folder)
    myDFPlayer.advertise(3);
    advertGuardUntil = millis() + 3000; // ignore stray "finished" events habang lumilipat
    delay(500); // Konting pahinga para matapos ang sound

    // 2. Logic para sa paglipat ng kanta
    currentSongNumber++; 

    // 3. I-check kung lumampas na sa dami ng kanta (totalMusicFiles)
  if (currentSongNumber > totalMusicFiles) {
        currentSongNumber = 1; // Balik sa unang kanta
    }

    // 4. I-play na ang susunod na track sa /mp3 folder
    playSongTracked(currentSongNumber);
    isAdvertPlaying = false; resumePending = false; nextCheckPending = false; // naputol na ang advert, i-clear ang flag
    
    // 5. I-update ang timer (kung ginagamit mo pa yung timer logic)
    // lastCheckTime = millis(); 

    Serial.print("Lipat sa Track #: ");
    Serial.println(currentSongNumber);

    // Reset ang cmdID
    cmdID = 0;
}

    // 24. PREVIOUS TRACK COMMAND (ID 94)
  if (cmdID == 94) {
    Serial.println("Command: Previous Track");

    // 1. Opsyonal: Voice feedback (Kung may "Previous" sound ka sa ADVERT folder)
    myDFPlayer.advertise(3); 
    advertGuardUntil = millis() + 3000; // ignore stray "finished" events habang lumilipat
    delay(500);

    // 2. Logic para sa pagbabawas ng kanta
    currentSongNumber--; 

    // 3. I-check kung bumaba sa 1 (Loop back sa huling kanta)
    if (currentSongNumber < 1) {
        currentSongNumber = totalMusicFiles; // Kung nasa track 1, tatalon sa dulo
    }

    // 4. I-play ang kanta mula sa /mp3 folder
    playSongTracked(currentSongNumber);
    isAdvertPlaying = false; resumePending = false; nextCheckPending = false; // naputol na ang advert, i-clear ang flag

    Serial.print("Bumalik sa Track #: ");
    Serial.println(currentSongNumber);

    // Reset ang cmdID
    cmdID = 0;
}

  // 25. TURN LEFT 90 DEGREES (ID 25) ---
if (cmdID == 25) {
    Serial.println("Command: Turn Left 90 Degrees");
    playAdvertOverMusic(3);
    turnLeft(); // Tatawagin nito yung function na nilaga
    delay(800); // Adjust ang delay depende sa bilis ng motor mo
    cmdID = 0;    // Reset para hindi paulit-ulit ang ikot
}

  // 26. TURN RIGHT 90 DEGREES (ID 28) ---
if (cmdID == 28) {
    Serial.println("Command: Turn Right 90 Degrees");
    playAdvertOverMusic(3);
    turnRight(); // Tatawagin nito yung function na nilagay natin sa 
    delay(800); // Adjust ang delay depende sa bilis ng motor mo
    cmdID = 0;
}
  // 27. VFL ON/OFF COMMANDS (ID 103/104) ---

if (cmdID == 9) {
  digitalWrite(2, HIGH); // Bukas ang Laser
  Serial.println("VFL ON");
  playAdvertOverMusic(13);
  playBootSound_NB();
  cmdID = 0;
}
  else if (cmdID == 10) { // O kung anong OFF command mo
  digitalWrite(2, LOW);  // Patay ang Laser
  Serial.println("VFL OFF");
  playAdvertOverMusic(14);
  playSleepBeep_NB();
  cmdID = 0;
}

// 28. DISPLAY RUSTECH OPM (ID 48) ---
if (cmdID == 48 || cmdID == 11) { // Pwede mo ring gamitin yung OFF command para rito
  hulingTugtog = 0; // Importante: Reset para bumoses agad
  unsigned long lockStartTime = millis();
  while (millis() - lockStartTime < 30000) { // 30 seconds lock
    showRustechOPM();
    delay(100); // Konting hinga para sa I2C
    yield();
  }
  ipScreenNeedsDraw = true; // V5.3.4: ibalik agad ang IP screen pagkatapos ng OPM display
    if (appLinked) lastAppContactTime = millis(); // V5.3.5: nag-block ng 30s, kaya huwag ituring na disconnected ang app
}

// 29. reset command (ID 82)
if (cmdID == 82) {
  Serial.println("Resetting system...");
  systemReboot();
}

// 30. DISPLAY ROBOT IP ADDRESS (ID 49)
if (cmdID == 49) {
  // Guard: kung nagpapakita na ng IP, huwag na ulit i-retrigger/i-reset
  // ang timer kahit paulit-ulit pang mabasa ang parehong cmdID ng voice module
  if (!(isDisplayingNumber && currentState == STATE_EXPRESSION)) {
    Serial.println("📶 Command: Display IP Address");
    stopBot();
    playBootSound_NB();

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(8, 2);
    display.print("ROBOT IP ADDRESS:");
    display.drawFastHLine(0, 13, 128, SH110X_WHITE);

    if (WiFi.status() == WL_CONNECTED) {
      String ipStr = WiFi.localIP().toString();

      // Dynamic text size - lumiliit kapag mahaba ang IP (hal. galing hotspot)
      int fontSize = (ipStr.length() <= 10) ? 2 : 1;
      int charWidth = (fontSize == 2) ? 12 : 6;
      int textWidthPx = ipStr.length() * charWidth;
      int xPos = (128 - textWidthPx) / 2;
      if (xPos < 0) xPos = 0;

      display.setTextSize(fontSize);
      display.setCursor(xPos, 24);
      display.print(ipStr);

      display.setTextSize(1);
      display.setCursor(2, 48);
      display.print("SSID: ");
      display.print(ssid);
    } else {
      display.setTextSize(2);
      display.setCursor(10, 25);
      display.print("NO WIFI");
    }

    display.display();

    isDisplayingNumber = true;
    currentState = STATE_EXPRESSION;
    cryingStartTime = millis();
  }
  cmdID = 0;
}

//  ================ VOICE COMMANDS FOR SMART HOME (FORWARD TO S3) ==================
   /* else if (cmdID == 6) {
        stopBot();
      playBootSound_NB(); // Beep feedback
      roboEyes.setMood(ANGRY);
      roboEyes.anim_laugh();
      myDFPlayer.play(8);
      shakeDuration = 270; // Mas mabilis na shake para sa ON command
      shakeStartTime = millis();
      currentState = STATE_SHAKING;
      Serial.println("💡 Command: TURN ON (S3)");
      
      // I-forward ang ID 6 sa S3
      esp_now_send(broadcastAddress, (uint8_t *) &cmdID, sizeof(cmdID));
      
      // Opsyonal: Lagyan ng mood o eyes ang robot kapag nag-ON
      roboEyes.setMood(HAPPY);
    }

    // --- TURN OFF (ID 7) ---
    else if (cmdID == 7) { 
        stopBot();
       playAvoidBeep_NB(); // Beep feedback
       roboEyes.setMood(ANGRY);
       roboEyes.anim_laugh();
       myDFPlayer.play(9);
       shakeDuration = 270; // Mas mabilis na shake para sa ON command
       shakeStartTime = millis();
       currentState = STATE_SHAKING;     

      Serial.println("🌑 Command: TURN OFF (S3)");
      
      // I-forward ang ID 7 sa S3
      esp_now_send(broadcastAddress, (uint8_t *) &cmdID, sizeof(cmdID));
      
      // Opsyonal: Mood ng robot kapag nag-OFF
      roboEyes.setMood(ANGRY);
    }*/
    lastVoiceTime = millis(); // I-update ang cooldown
  }
}
  // ===========================================================================

  


  switch (currentState) {

  case STATE_EXPRESSION: {
      stopBot();
      
      // Kung HINDI numero ang pinapakita, ituloy ang animation ng mata (Smiley o Crying)
      if (!isDisplayingNumber) {
          updateEyes();
      }

      // Timer para bumalik sa dati
      if (millis() - cryingStartTime > 5000) {
        roboEyes.setSweat(OFF);
        isCrying = false;
        isDisplayingNumber = false; // Reset ang flag
        ipScreenNeedsDraw = true;   // V5.3.4: kapag naka-link ang app, ibalik agad ang IP screen
        currentState = STATE_BOOT_WAIT;
        Serial.println("Back to normal mode.");
      }
      break;
    }
    
case STATE_BOOT_WAIT: {
  bool pirDetected = (distance > 0 && distance < PRESENCE_TRIGGER_CM);
  static unsigned long lastPIRTriggerTime = 0;

  if (pirDetected) {
    if (millis() - lastPIRTriggerTime > 900000) {
      Serial.println("ULTRASONIC: Presence detected!");
      playAdvertOverMusic(11); // may music = pause + advert (ADVERT/0011.mp3) + resume; walang music = play(11) sa root
      lastPIRTriggerTime = millis();
      roboEyes.setMood(HAPPY);
      roboEyes.anim_laugh();
      shakeDuration = 1270;
      shakeStartTime = millis();
      currentState = STATE_SHAKING;
      playBootSound_NB();
    }
  }

  // === SAFETY CHECK - LAGING NAKA-ON kahit naka-connect ang FaceRobot app ===
  // Ngayon kasama na ang ULTRASONIC (distansya) bukod sa pit sensors
  bool bwLeftPit = digitalRead(IR_SENSOR_LEFT);
  bool bwRightPit = digitalRead(IR_SENSOR_RIGHT);
  bool bwTooCloseUltra = (distance > 0 && distance < 5);

  if (bwLeftPit == HIGH || bwRightPit == HIGH || bwTooCloseUltra) {
    currentBaseSpeed = 180;
    reverse();
    roboEyes.setMood(ANGRY);
    lastFaceCommandTime = 0;  // i-clear para di agad bumalik sa face-follow habang aatras pa
  }
  else {
    bool faceTrackingActive = (millis() - lastFaceCommandTime < FACE_COMMAND_TIMEOUT);

    if (!faceTrackingActive) {
      currentBaseSpeed = 180;

      if (distance > 0 && distance < 5) {
        reverse();
        roboEyes.setMood(ANGRY);
      }
      else if (distance >= 5 && distance < 15) {
        stopBot();
        static unsigned long lastBlink = 0;
        if (millis() - lastBlink > 5000) {
          roboEyes.anim_laugh();
          lastBlink = millis();
        }
        roboEyes.setMood(HAPPY);
      }
      else if (distance >= 15 && distance <= 17) {
        forward();
        roboEyes.setMood(DEFAULT);
      }
      else {
        stopBot();
        roboEyes.setMood(DEFAULT);
      }
    }
  }

  if (!isDisplayingNumber) {
    updateEyes();
  }

  break;
}

case STATE_MANUAL: {
bool leftPit = digitalRead(IR_SENSOR_LEFT);
bool rightPit = digitalRead(IR_SENSOR_RIGHT); 
int distance = getDistance();

if (leftPit == HIGH || rightPit == HIGH) { // KUNG NAKAKITA NG BUTAS
    reverse();
    delay(500); // Bahagyang delay para maiwasan ang sobrang dali ng pag-stop
    stopBot();
    delay(200); // Konting pahinga bago mag-react ulit
    roboEyes.setMood(ANGRY); // O kahit anong "panic" mood
} 
if (distance > 0 && distance <= 15) { 
        stopBot();      // 1. Hinto agad
        delay(100);
        reverse();      // 2. Atras
        delay(400);     // 3. Konting distansya
        stopBot();      // 4. Hinto ulit
        
        roboEyes.setMood(ANGRY); // Magugulat si Rustech
        Serial.print("🚫 TOO CLOSE! Distance: ");
        Serial.println(distance);
    }
    // DITO SA MANUAL, WALANG SENSORS! 
    // Hihintayin lang niya ang susunod na Bluetooth command.
    // Pwede mo lang i-update ang mata dito.
    updateEyes(); 
    
    // OPTIONAL: Kung gusto mo bumalik sa auto pag walang pinipindot ng 10 seconds
    
    if (millis() - lastBluetoothTime > 3000) {
      currentState = STATE_BOOT_WAIT;
    }
    }
    break;


    case STATE_MOVING: {
      if (anyAvoidanceTrigger) {
        stopBot();
        playAvoidBeep_NB();
        currentBaseSpeed = 120; // Kung mabilis pa rin ang 150, gawin mong 100 o 120
        currentTurnSpeed = 200;
        ledcWrite(CH_A, currentBaseSpeed); 
        ledcWrite(CH_B, currentBaseSpeed);

        // Logic for IR Avoid is inverted: HIGH = No object, !HIGH (LOW) = Object Detected

        bool triggeredLeft = leftPit || (!leftAvoid);
        bool triggeredRight = rightPit || (!rightAvoid);

        if (distance > 0 && distance < 35) {
          lastEdgeDetected = EDGE_BOTH;
          Serial.println("Avoidance: ULTRASONIC");
        } else if (triggeredLeft && triggeredRight) {
          lastEdgeDetected = EDGE_BOTH;
          Serial.println("Avoidance: IR BOTH");
        } else if (triggeredLeft) {
          lastEdgeDetected = EDGE_LEFT;
          Serial.println("Avoidance: IR LEFT");
        } else if (triggeredRight) {
          lastEdgeDetected = EDGE_RIGHT;
          Serial.println("Avoidance: IR RIGHT");
        } else {
          lastEdgeDetected = EDGE_BOTH;
        }

        avoidanceStartTime = now;
        currentState = STATE_AVOIDING_REVERSE;

        if (anyAvoidanceTrigger) roboEyes.setMood(ANGRY);
        else roboEyes.setMood(TIRED);
      } else {
        // V5.3.9: camera nav hints (signed compare - ang navHintTime ay nase-set sa loob ng handleClient)
        bool navFresh = (navHint != 0) && ((long)(now - navHintTime) < (long)NAV_HINT_TIMEOUT_MS);
        if (navFresh && navHint == 3) {
          // NAV_BACK: kapareho ng sensor avoidance (reverse -> pause -> turn)
          navHint = 0;
          stopBot();
          playAvoidBeep_NB();
          currentBaseSpeed = 120;
          currentTurnSpeed = 200;
          lastEdgeDetected = EDGE_BOTH;
          avoidanceStartTime = now;
          currentState = STATE_AVOIDING_REVERSE;
          roboEyes.setMood(ANGRY);
          Serial.println("Avoidance: CAMERA BACK");
        } else if (navFresh && navHint == 1) {
          navSpin(true);   // lumiko pakaliwa habang libre ang kaliwa
          roboEyes.setMood(HAPPY);
        } else if (navFresh && navHint == 2) {
          navSpin(false);  // lumiko pakanan habang libre ang kanan
          roboEyes.setMood(HAPPY);
        } else {
          forward();
          roboEyes.setMood(HAPPY);
        }

        if (now - lastCuriousBeep > 5000 && !isPlayingCurious) {
          playCuriousBeep_NB();
          roboEyes.anim_confused();
          lastCuriousBeep = now;
        }
      }

      if (MOVING_AUTO_STOP_ENABLED && (long)(now - movementStartTime) >= (long)MOVEMENT_DURATION) { // V5.3.7: signed compare - iwas underflow kapag mas bago ang movementStartTime kaysa 'now'
        stopBot();
        pirActivationStartTime = now;
        isPirDetectionActive = false;

        lastOscillationTime = now;

        currentState = STATE_STOPPED_WAITING_FOR_PIR;
        roboEyes.setMood(DEFAULT);
        Serial.println("60s done. Waiting for PIR…");
      }
      break;
    }

    case STATE_AVOIDING_REVERSE: {
      reverse();
      roboEyes.setMood(ANGRY);
      roboEyes.anim_confused();
      if (now - avoidanceStartTime >= REVERSE_DURATION) {
        stopBot();
        avoidanceStartTime = now;
        currentState = STATE_AVOIDING_STOP;
        Serial.println("Reverse done. Starting turn...");
      }
      break;
    }

    case STATE_AVOIDING_STOP: {
    stopBot(); 
    // Pwede mo rin palitan mood dito para kunwari nag-iisip ang robot
    roboEyes.setMood(DEFAULT); 

    if (now - avoidanceStartTime >= pauseDuration) {
        avoidanceStartTime = now; 
        currentState = STATE_AVOIDING_TURN; // Ngayon, safe na siyang lumingon
        Serial.println("Pause done. Now turning...");
    }
    break;
}

   case STATE_AVOIDING_TURN: {
      roboEyes.setMood(ANGRY);
      switch (lastEdgeDetected) {
        case EDGE_LEFT: turnRight(); break;
        case EDGE_RIGHT: turnLeft(); break;
        case EDGE_BOTH: turnLeft(); break;
        default: turnRight(); break;
      }

      if (now - avoidanceStartTime >= TURN_DURATION) {
        stopBot();
        currentState = STATE_MOVING;
        roboEyes.setMood(HAPPY);
        Serial.println("Turn done. Resuming movement.");
        lastEdgeDetected = EDGE_NONE;
      }
      break;
    }

    case STATE_STOPPED_WAITING_FOR_PIR: {
      if (now - pirActivationStartTime >= PIR_ACTIVATION_DELAY) {
        stopBot();
        isPirDetectionActive = true;
        currentState = STATE_PIR_ACTIVE_AND_DETECTING;
        Serial.println("PIR Activated (after delay)!");
        break;
      }

      unsigned long timeInCycle = now - lastOscillationTime;

      if (timeInCycle < TURN_DURATION_OSC) {
        if (isTurningLeft) turnLeft();
        else turnRight();
      } else if (timeInCycle < CYCLE_DURATION_OSC) {
        stopBot();
      } else {
        isTurningLeft = !isTurningLeft;
        lastOscillationTime = now;
        Serial.print("Oscillation Flip: Next turn ");
        Serial.println(isTurningLeft ? "LEFT" : "RIGHT");
      }
      break;
    }

    case STATE_PIR_ACTIVE_AND_DETECTING: {
      stopBot();
      if (isTriggered) {
        movementStartTime = now;
        currentState = STATE_MOVING;
        isPirDetectionActive = false;
        playBootSound_NB();
        roboEyes.anim_laugh();
        roboEyes.setMood(HAPPY);
        Serial.println("Motion detected! Movement start.");
      }
      break;
    }

    case STATE_SLEEP_ALERT: 
      {
        // 1. Face Update
        roboEyes.setMood(ANGRY); 
        if (!isDisplayingNumber) {
          updateEyes(); 
        }

        // 2. Oscillate left/right quickly (Dapat nasa loob ito ng case)
        if (((now - sleepAlertStart) / SLEEP_ALERT_OSC_INTERVAL) % 2 == 0) {
          turnLeft();
        } else {
          turnRight();
        }

        // 3. End the alert after duration
        if (now - sleepAlertStart >= SLEEP_ALERT_DURATION) {
          stopBot();
          sleepModeStartTime = now;
          isTiredFaceDone = false;
          currentState = STATE_SLEEPING_IR;
          Serial.println("Sleep alert finished. Returning to SLEEPING_IR.");
        }
      } // Dito dapat ang saradong brace ng case logic
      break; // Importante itong break para hindi tumuloy sa susunod na case

    case STATE_SLEEP_ULTRASONIC_OSC: {
      // Chekehin kung tapos na ang total duration
      if (now - ultrasonicOscStart >= ULTRASONIC_OSC_TOTAL_DURATION) {
        stopBot();
        // Bumalik sa pagkakatulog
        sleepModeStartTime = now;
        isTiredFaceDone = false;
        currentState = STATE_SLEEPING_IR;
        roboEyes.setMood(TIRED);
        roboEyes.setAutoblinker(ON, 8, 5);
        Serial.println("Ultrasonic Oscillation DONE. Back to SLEEP.");
        break;
      }

      unsigned long cycleTime = now - ultrasonicOscStart;
      unsigned long timeInCurrentCycle = cycleTime % (OSC_MOVEMENT_DURATION + OSC_STOP_DURATION);
      // Non-blocking Atras-Abante logic
      if (timeInCurrentCycle < OSC_MOVEMENT_DURATION) {
        // Movement part
        if (isOscillatingForward) {
          forward();
          roboEyes.setMood(ANGRY);      
          roboEyes.anim_confused(); // Abante: Galit/Confused
        } else {
          reverse();
          roboEyes.setMood(ANGRY);      
          roboEyes.anim_confused();
        }
      } else {
        // Stop part
        stopBot();     
        roboEyes.setMood(ANGRY); 
        roboEyes.anim_confused();
        // I-flip ang direksyon para sa susunod na cycle
        if (timeInCurrentCycle >= (OSC_MOVEMENT_DURATION + OSC_STOP_DURATION) - 50) { 
          isOscillatingForward = !isOscillatingForward;
        }
      }
      if (!isDisplayingNumber) {
    updateEyes(); 
}
      break;
    } // End of STATE_SLEEP_ULTRASONIC_OSC case
    
    case STATE_DANCING: {
      unsigned long now = millis();

    // --- ITO ANG DAGDAG PARA SA LED BLINK ---
    if (now - lastBlinkTime >= 80) {
        lastBlinkTime = now;
        ledState = !ledState; // Pagpapalitin ang ON at OFF
        digitalWrite(LED_PIN, ledState);
    }
      // 1. Check if 1 minute is over
      if (now - danceStartTime >= DANCE_DURATION) {
        stopBot();
        digitalWrite(LED_PIN, LOW); // Siguraduhing naka-off ang LED
        ledState = LOW;
        roboEyes.setMood(HAPPY);
        currentState = STATE_BOOT_WAIT;
        break;
 
      }

// 2.===== Dance Steps Logic
      switch (danceStep) {
        case 0: { // Mabilis na Atras/Abante (Oscillate)
            // Chekehin muna kung tapos na ang 2 cycles
            if (atrasAbanteCycleCount >= AT_AB_CYCLES_TARGET) {
                // Done with 2 cycles, lipat sa next step
                danceStep = 1;
                lastDanceStepTime = now;
                stopBot();
                atrasAbanteCycleCount = 0; // I-reset para sa susunod na Dance Mode
                break;
            }
            // Logic para sa isang (1) cycle (Forward then Reverse)
            if (now - lastDanceStepTime < DANCE_OSC_DURATION / 2) {
                forward();
                roboEyes.anim_confused();
            } else if (now - lastDanceStepTime < DANCE_OSC_DURATION) {
                reverse();
                roboEyes.anim_confused();
            } else {
                // Tapos na ang isang Atras/Abante cycle
                stopBot();
                atrasAbanteCycleCount++; // Dagdagan ang counter
                lastDanceStepTime = now; // I-reset ang timer para sa next cycle
            }
            break;
        }
        case 1: { // Dito nagsisimula ang new scope
          // Ito ang mga linyang nagde-declare ng local variables:
          const unsigned long TURN_MS = 150;
          const unsigned long STOP_MS = 50;
          const unsigned long CYCLE_MS = TURN_MS + STOP_MS;
          // ... (the rest of the dancing logic)
          unsigned long timeInStep = now - lastDanceStepTime;
          if (timeInStep < 1500) { 
            unsigned long cycleTime = timeInStep % (2 * CYCLE_MS);
            if (cycleTime < TURN_MS) {
              turnLeft(); 
              roboEyes.anim_confused();
            } else if (cycleTime < CYCLE_MS) {
              stopBot();
            } else if (cycleTime < CYCLE_MS + TURN_MS) {
              turnRight(); 
              roboEyes.anim_confused();
            } else {
              stopBot();
            }
            } else {
            danceStep = 2;
            lastDanceStepTime = now;
            stopBot();
            }
            break;
            } // Dito nagtatapos ang new scope
      

        case 2: // Ikot sa Kanan (2 seconds)
          turnRight();
          roboEyes.anim_confused();
          if (now - lastDanceStepTime >= DANCE_TURN_DURATION) {
            danceStep = 3;
            lastDanceStepTime = now;
            stopBot();
          }
          break;

        case 3: // Ikot sa Kaliwa (2 seconds)
          turnLeft();
          roboEyes.anim_confused();
          if (now - lastDanceStepTime >= DANCE_TURN_DURATION) {
            // Tapos na ang cycle, bumalik sa Step 0
            danceStep = 0;
            lastDanceStepTime = now;
            stopBot();
            }
            break;
          }
          updateEyes();
          break;
         } // End of STATE_DANCING case


    case STATE_SLEEPING_IR: {
      stopBot();

            // Ultrasonic-based wake logic (replaces SLEEP_IR_PIN)
      if (distance > 0 && distance < ULTRASONIC_TRIGGER_CM) {
        if (currentState != STATE_SLEEP_ALERT) {
          sleepAlertStart = now;
          alarmTriggerTime = 0;
          buzzOff();
          playAvoidBeep_NB();
          roboEyes.anim_confused();
          roboEyes.setAutoblinker(ON, 1, 1);
          currentState = STATE_SLEEP_ALERT;
          Serial.println("ULTRASONIC triggered -> STATE_SLEEP_ALERT");
          break;
        }
      }
      // IDINAGDAG: ULTRASONIC WAKE LOGIC (MAGTR-TRIGGER NG OSCILLATION)
      
      //if (distance > 0 && distance < ULTRASONIC_TRIGGER_CM) {
        //ultrasonicOscStart = now;
        //isOscillatingForward = false; // Simulan sa Atras
        //currentState = STATE_SLEEP_ULTRASONIC_OSC;
        //playAvoidBeep_NB(); // Gumawa ng kaunting ingay
        //roboEyes.setMood(ANGRY); 
        //roboEyes.anim_confused(); // Dagdag animation (Curious/Confused)
        //roboEyes.setAutoblinker(ON, 1, 1);
        //Serial.println("ULTRASONIC WAKE: Activating Atras/Abante Oscillation!");        
        //break;
      //}

//======== Sleep Avoidance (IR_AVOID) logic

      // Sleep Avoidance (IR_AVOID) logic
      //bool sleepLeftAvoid = digitalRead(IR_AVOID_LEFT) == HIGH;
      //bool sleepRightAvoid = digitalRead(IR_AVOID_RIGHT) == HIGH;

      //if (!sleepAvoidActive) {
        //if (sleepLeftAvoid) {
          //sleepAvoidActive = true;
          //sleepAvoidStart = millis();
          //sleepAvoidDirection = +1; // Turn Right para lumayo sa Left object
          //roboEyes.anim_confused();
          //roboEyes.setMood(ANGRY);
          //playAvoidBeep_NB();
          //Serial.println("SLEEP AVOID: RIGHT 2 sec (Object on LEFT)");
        //} else if (sleepRightAvoid) {
          //sleepAvoidActive = true;
          //sleepAvoidStart = millis();
          //sleepAvoidDirection = -1; // Turn Left para lumayo sa Right object
          //roboEyes.anim_confused();
          //roboEyes.setMood(ANGRY);
          //playAvoidBeep_NB();
          //Serial.println("SLEEP AVOID: LEFT 2 sec (Object on RIGHT)");
        //}
      //}

      // Run the avoidance movement while active
      if (sleepAvoidActive) {
        if (sleepAvoidDirection == -1) {
          turnLeft();
        } else if (sleepAvoidDirection == +1) {
          turnRight();
        }
        roboEyes.setMood(ANGRY);
        if (!isDisplayingNumber) {
        updateEyes(); 
      }

        // stop after 2 seconds

        if (millis() - sleepAvoidStart >= SLEEP_AVOID_DURATION) {
          sleepAvoidActive = false;
          stopBot();
          roboEyes.anim_confused();
          roboEyes.setMood(TIRED);
          if (!isDisplayingNumber) {
        updateEyes(); 
      }
          Serial.println("SLEEP AVOID: DONE");
        }
      } else {
        stopBot(); // Make sure it stays stopped if no sleep avoid active
      }

  //===== Alarm & Display Logic (Run only if not actively avoiding)

      if (!sleepAvoidActive) {
        if (isAlarmTriggeredToday && alarmTriggerTime > 0) {
          roboEyes.setMood(HAPPY);
          roboEyes.anim_laugh();
        } else if (!isTiredFaceDone) {
          roboEyes.setMood(TIRED);
          if (now - sleepModeStartTime >= TIRED_FACE_DURATION) {
            isTiredFaceDone = true;
            display.clearDisplay();
          }
        }

        if (isTiredFaceDone && alarmTriggerTime == 0) {
          displayTime(); // V5.3.4: kapag naka-link ang app, IP screen ang ipapakita (tignan ang displayTime())
        }
      }
      break;

    } //====== End of STATE_SLEEPING_IR case

    default:
      break;
  } // End of switch (currentState)

  //======== Update eyes if not in IR sleeping mode

  if (currentState != STATE_SLEEPING_IR && currentState != STATE_SLEEP_ALERT) {
    if (!isDisplayingNumber) {
        updateEyes(); 
      }
  } else if (!isTiredFaceDone && alarmTriggerTime == 0) {
    if (!isDisplayingNumber) {
        updateEyes(); 
      }
  }

  // --- I2C AUTO-RECOVERY WATCHDOG ---
// ESP32 I2C Stability Patch
  // Imbes na Wire.lastError(), gagamit tayo ng timeout check
  static unsigned long lastI2CCheck = 0;
  if (millis() - lastI2CCheck > 1000) { // Check every 1 second
    lastI2CCheck = millis();
    
    Wire.beginTransmission(0x3C); // Subukan kausapin ang OLED (Address 0x3C)
    if (Wire.endTransmission() != 0) { 
      Serial.println("I2C Bus Busy or Error - Re-initializing...");
      Wire.begin(); 
      display.begin(0x3C, true);
      ipScreenNeedsDraw = true; // V5.3.4: mag-redraw pagkatapos ng I2C recovery
    }
  }
 
  // --- TELEMETRY SYSTEM PARA SA C++ APP ---
static unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 200; // Mag-sesend kada 0.2 seconds (mabilis!)

if (millis() - lastTelemetryTime > telemetryInterval) {
    lastTelemetryTime = millis();

    // 1. Kunin ang distance (Dapat ang variable name ay match sa sensor code mo)
    // Kung ang variable mo ay 'currentDistance', 'distance', o 'distance_cm', yun ang gamitin.
    int telemetryDist = currentDistance; 

    // 2. I-send ang data gamit ang format na "T:Value"
    // Mahalaga ang "T:" dahil yan ang hinahanap ng C++ App natin.
    //SerialBT.print("T:"); 
    //SerialBT.println(telemetryDist); 
}

}// End of void loop()

void handleSecurityGuard() {
  bool humanDetected = digitalRead(PIR_MIC_PIN);
  unsigned long now = millis();

  // Kung may tao AT tapos na ang 15 seconds na pahinga
  if (humanDetected && (now - lastPIRTriggerTime > pirCooldown)) {
    
    Serial.println("🚨 SECURITY ALERT: Motion Detected!");
    
    // 1. Itigil ang ginagawa
    stopBot(); 
    
    // 2. Mag-react (Palitan ang eyes at tumunog)
    roboEyes.setMood(ANGRY); // Kunwari striktong guard
    songInterrupted = true; myDFPlayer.playMp3Folder(11); // Halimbawa: Track 12 ay "Sino yan?!"
    
    // 3. Animation (Look around)
    roboEyes.anim_confused();

    // 4. I-update ang huling oras ng trigger
    lastPIRTriggerTime = now;

    // 5. Opsyonal: I-set ang state sa IDLE para maghintay ng command
    // currentState = STATE_IDLE; 
  }

}

// ====== FUNCTION PARA SA GEMINI OPM MODE (ID 100) ======
void showRustechOPM() {
  // 1. Adaptive warm-up: ibasa hanggang stable na ang value (max 25 tries)
  int prevReading = analogRead(OPM_INPUT_PIN);
  delay(2);
  for (int attempt = 0; attempt < 25; attempt++) {
    int newReading = analogRead(OPM_INPUT_PIN);
    if (abs(newReading - prevReading) < 25) break; // stable na
    prevReading = newReading;
    delay(2);
  }

  // 2. Basahin ang Power (averaging)
  long sum = 0;
  const int OPM_SAMPLES = 60;
  for (int i = 0; i < OPM_SAMPLES; i++) {
    sum += analogRead(OPM_INPUT_PIN);
    delayMicroseconds(150);
  }
  float voltage = (sum / (float)OPM_SAMPLES * 3.3) / 4095.0;
  
  float dbm;
  if (voltage < 0.020) dbm = -99.9; // Noise Floor
  else if (voltage <= V1) dbm = P0 + (voltage - V0) * (P1 - P0) / (V1 - V0);
  else dbm = P1 + (voltage - V1) * (P2 - P1) / (V2 - V1);

  // --- START NG VOICE FEEDBACK LOGIC ---
  int kasalukuyangStatus = 0; 
  
  if (dbm < -40.0) kasalukuyangStatus = 15; // LOS
  else if (dbm >= -24.0 && dbm <= -10.0) kasalukuyangStatus = 16; // Good
  else if (dbm >= -30.0 && dbm < -24.0) kasalukuyangStatus = 17; // Warning

  // Preno: Titunog lang kung may pagbabago sa signal
  if (kasalukuyangStatus != 0 && kasalukuyangStatus != hulingTugtog) {
    playAdvertOverMusic(kasalukuyangStatus);
    hulingTugtog = kasalukuyangStatus; // Tandaan ang huling tinugtog
    Serial.print("Voice Play: "); Serial.println(kasalukuyangStatus);
  }
  // --- END NG VOICE FEEDBACK LOGIC ---

  // 2. I-display sa SH110X (Yung dati mong display code)
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  
  display.setTextSize(1);
  display.setCursor(20, 0);
  display.print("RUSTECH OPM v1.0");
  display.drawFastHLine(0, 10, 128, SH110X_WHITE);
  
  if (voltage >= 3.40) {
    display.setCursor(20, 30); display.setTextSize(2); display.print("SATURATED");
  } else if (dbm < -45.0) {
    display.setCursor(30, 30); display.setTextSize(4); display.print("LOS");
  } else {
    display.setCursor(10, 25); display.setTextSize(3); display.print(dbm, 1);
    display.setTextSize(1); display.print(" dBm");
    display.setCursor(40, 55); display.setTextSize(1);
    display.print("V: "); display.print(voltage, 3);
  }
  display.display();
}

void setup_bluetooth() {
  // Ang pangalan na lalabas sa phone mo pag nag-scan ka
  if(!SerialBT.begin("Rustech_BT_Remote")){
    Serial.println("❌ Bluetooth Error!");
  } else {
    Serial.println("✅ Bluetooth Ready! Pair now to 'Rustech_BT_Remote'");
  }
}

void handle_bluetooth_data() {
  if (SerialBT.available()) {
    lastBluetoothTime = millis();
    char incomingChar = SerialBT.read();
    Serial.print("Received: "); Serial.println(incomingChar);

    // --- 1. AUTO MODE (STATE_MOVING) ---
    if (incomingChar == 'A') { 
      currentState = STATE_MOVING; // Eto yung auto mode mo sa FSM
      movementStartTime = millis(); 
      roboEyes.setMood(HAPPY);
      Serial.println("🤖 PC: AUTO MODE ACTIVATED");
    }

    // --- 2. DANCE MODE (STATE_DANCING) ---
    if (incomingChar == 'T') {
      currentState = STATE_DANCING; // Gamit ang STATE_DANCING constant mo
      danceStartTime = millis();
      lastDanceStepTime = millis();
      songInterrupted = true; myDFPlayer.play(12); // Pinapatugtog ang track 12 para sa sayaw
      danceStep = 0;
      atrasAbanteCycleCount = 0;
      roboEyes.setMood(HAPPY);
      roboEyes.anim_laugh();
      Serial.println("💃 PC: DANCE MODE ACTIVATED");
    }
    
  // --- MUSIC PLAY COMMAND ---
    if (incomingChar == 'C') {
      Serial.println("PC Command: Music Play Start");
      isMusicActive = true;
      currentState = STATE_MANUAL; // Para hindi muna gumagalaw ang robot habang tumutugtog
      
      // I-play ang unang kanta o i-trigger ang shuffle start
      currentSongNumber = random(1, totalMusicFiles + 1);    
      playSongTracked(currentSongNumber);
      
      // Visual feedback sa OLED
      roboEyes.setMood(HAPPY);
      Serial.print("🎵 Playing Track: ");
      Serial.println(currentSongNumber);
    }

    // --- 3. MANUAL / STOP OVERRIDE ---
    if (incomingChar == 's') {
      currentState = STATE_MANUAL; // Lipat sa Manual para huminto ang Auto/Dance
      stopBot();
      Serial.println("🛑 PC: STOP / MANUAL MODE");
    }

    // --- 4. DIRECTIONAL OVERRIDES (W-A-S-D) ---
    // Pag pinindot ang arrow keys sa PC, dapat mag-Manual mode para hindi mag-clash
    if (incomingChar == 't') { currentState = STATE_MANUAL; forward(); }
    else if (incomingChar == 'b') { currentState = STATE_MANUAL; reverse(); }
    else if (incomingChar == 'l') { currentState = STATE_MANUAL; turnLeft(); }
    else if (incomingChar == 'r') { currentState = STATE_MANUAL; turnRight(); }

    // --- 5. EMERGENCY REBOOT ---
    if (incomingChar == 'X') { systemReboot(); }
  }

}

void systemReboot() {
  display.clearDisplay();
  
  // Font setup
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  
  // Center text (1.3" OLED is 128 pixels wide)
  display.setCursor(35, 10); 
  display.print("SYSTEM RESET");

  // Loading Bar Design
  display.drawRect(14, 25, 100, 10, SH110X_WHITE); // Frame
  
  for (int i = 0; i <= 96; i += 8) {
    display.fillRect(16, 27, i, 6, SH110X_WHITE); // Progress bar
    display.display();
    delay(100); // Bilis ng loading animation
  }

  display.setCursor(25, 45);
  display.print("Rebooting Prime...");
  display.setCursor(45, 55);
  display.print("V2.0");
  display.display();
  
  delay(1000); // Konting pause para mabasa ng user
  ESP.restart(); 
}

void setServoAngle(int angle) {
  // Target lang ang itinatakda dito; ang updateServo() ang gumagalaw nang dahan-dahan
  targetServoAngle = constrain(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
}

void updateServo() {
  unsigned long now = millis();
  if (now - lastServoStepTime < SERVO_STEP_INTERVAL_MS) return;
  lastServoStepTime = now;
  if (currentServoAngle == targetServoAngle) return;

  int diff = targetServoAngle - currentServoAngle;
  if (abs(diff) <= SERVO_STEP_DEG) currentServoAngle = targetServoAngle;
  else currentServoAngle += (diff > 0) ? SERVO_STEP_DEG : -SERVO_STEP_DEG;
  liftServo.write(currentServoAngle);
}
