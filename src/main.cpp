#include <Arduino.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <WebServer.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_system.h>
#include <vector>
#include "splash_image.h"
#include "nova_idle.h"
#include "nova_scanning.h"
#include "nova_success.h"
#include "nova_warning.h"
#include "nova_device.h"
#include "nova_wifi.h"
#include "nova_ble.h"
#include "nova_keyboard.h"
#include "nova_vault.h"

namespace {
constexpr char kFirmwareVersion[] = "v0.16.0";
constexpr uint8_t kSelectPin = 11;
constexpr uint8_t kNextPin = 12;
constexpr uint16_t kDnsPort = 53;
constexpr uint8_t kNormalBrightness = 135;
constexpr uint8_t kDimBrightness = 38;
constexpr uint32_t kAutoDimMs = 30000;
constexpr uint32_t kBatterySampleMs = 2000;
constexpr uint32_t kBatteryStepDischargingMs = 15000;
constexpr uint32_t kBatteryStepChargingMs = 30000;
constexpr size_t kBatteryWindow = 9;
constexpr uint32_t kDefaultPortalTimeoutMs = 5UL * 60UL * 1000UL;
constexpr size_t kMaximumUploadBytes = 1024UL * 1024UL;
constexpr size_t kFilesystemReserveBytes = 64UL * 1024UL;

WebServer server(80);
DNSServer dnsServer;
String apSsid;
String apPassword;
bool portalRunning = false;
bool portalQrVisible = false;

struct BatterySnapshot {
    int32_t level = -1;
    int16_t millivolts = -1;
    int16_t rawMillivolts = -1;
    int16_t minimumMillivolts = -1;
    int8_t charging = 2; // 0 discharging, 1 charging, 2 unknown
    uint32_t updatedAt = 0;
    uint32_t lastLevelStepAt = 0;
    float filteredMillivolts = 0;
    int16_t samples[kBatteryWindow] = {};
    size_t sampleCount = 0;
    size_t sampleIndex = 0;
};

BatterySnapshot battery;
uint32_t lastBatteryDraw = 0;
uint32_t lastDisplayActivity = 0;
String resetReason;
int16_t bootBatteryMillivolts = -1;
uint32_t portalStartedAt = 0;
uint32_t lastPortalClientAt = 0;
uint32_t portalTimeoutMs = kDefaultPortalTimeoutMs;
bool sleepRequested = false;
uint32_t sleepRequestedAt = 0;
bool sleeping = false;
bool routesConfigured = false;
uint32_t restoreEcoApAt = 0;

enum class DisplayMode { Normal, Dim, Off };
DisplayMode displayMode = DisplayMode::Normal;

enum class PowerProfile { Eco, Performance };
PowerProfile powerProfile = PowerProfile::Eco;

struct SurveyNetwork {
    String ssid;
    String bssid;
    String security;
    int32_t rssi;
    int32_t channel;
    bool isNew;
};

std::vector<SurveyNetwork> surveyResults;

struct BleObservation {
    String name;
    String address;
    String addressType;
    String services;
    String manufacturer;
    int32_t rssi;
    bool connectable;
    bool isNew;
    uint32_t firstSeenMs;
    uint32_t lastSeenMs;
    uint32_t observations;
};

std::vector<BleObservation> bleResults;
SemaphoreHandle_t bleResultsMutex = nullptr;
uint32_t bleScanStartedAt = 0;
bool bleScanning = false;
File activeUploadFile;
String activeUploadPath;
String activeUploadError;
bool activeUploadCreated = false;
int activeUploadStatus = 201;
size_t activeUploadCapacity = 0;

enum class KeyboardState { Off, Scanning, Connecting, Pairing, Connected, Failed };
KeyboardState keyboardState = KeyboardState::Off;
bool keyboardMode = false;
bool keyboardModeRequested = false;
bool keyboardBleReady = false;
uint32_t keyboardModeRequestedAt = 0;
volatile bool keyboardCandidateReady = false;
volatile bool keyboardDisconnected = false;
volatile bool keyboardExitRequested = false;
const NimBLEAdvertisedDevice *keyboardCandidate = nullptr;
NimBLEClient *keyboardClient = nullptr;
String keyboardName;
String keyboardMessage;
uint32_t keyboardPasskey = 0;
char keyboardText[65] = {};
char keyboardLastKey[24] = "None";
char keyboardRawReport[52] = "None";
uint32_t keyboardReportCount = 0;
size_t keyboardSubscribedCount = 0;
bool keyboardBootProtocol = false;
uint8_t keyboardPreviousKeys[6] = {};
portMUX_TYPE keyboardMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool keyboardScreenDirty = false;
uint32_t keyboardLastScreenDraw = 0;

void drawPortalScreen();
void drawKeyboardScreen();
void startPortal();
void stopPortal();

const char kPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>PocketLab</title>
  <style>
    :root{color-scheme:dark;--bg:#060b18;--card:#101a2c;--line:#293958;--cyan:#38d9f5;--violet:#9a67ff;--muted:#9db0ca;--green:#72e6a2;--red:#ff7882}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 85px 60px,#172153 0,#080f20 34%,var(--bg) 72%);background-attachment:fixed;font:16px system-ui;color:#eef5ff}
    main{max-width:920px;margin:auto;padding:22px}.hero{position:relative;display:flex;align-items:center;min-height:235px;overflow:hidden;margin-bottom:18px;padding:24px 230px 24px 24px;border:1px solid #354a75;border-radius:20px;background:linear-gradient(135deg,rgba(30,48,91,.96),rgba(12,21,40,.96));box-shadow:0 18px 50px #0008,inset 0 1px #7c69c755}
    .hero:before{content:'';position:absolute;inset:0;background:linear-gradient(120deg,transparent 35%,#29d9ff16 65%,#a35cff18);pointer-events:none}.hero-copy{position:relative;z-index:2}.hero h1{font-size:38px;letter-spacing:.02em}.hero-kicker{color:var(--cyan);font-weight:800;letter-spacing:.13em;text-transform:uppercase;font-size:12px}.nova-stage{position:absolute;right:12px;bottom:0;width:218px;height:232px;display:flex;align-items:flex-end;justify-content:center}.nova-stage:after{content:'';position:absolute;bottom:4px;width:170px;height:25px;border-radius:50%;background:#2ed7ff2c;filter:blur(9px)}#novaSprite{position:relative;z-index:1;display:block;max-width:205px;max-height:226px;object-fit:contain;filter:drop-shadow(0 10px 14px #000a);transition:opacity .2s,transform .25s}.nova-message{max-width:470px;margin-top:12px;padding:10px 13px;border-left:3px solid var(--cyan);border-radius:0 10px 10px 0;background:#07111db8;color:#dceaff}
    .mark{font-size:38px}.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:18px;margin:14px 0}.section-head{display:flex;align-items:center;justify-content:space-between;gap:16px;min-height:126px}.section-head>div{min-width:0;flex:1}.section-art{width:132px;height:132px;flex:0 0 132px;object-fit:contain;filter:drop-shadow(0 8px 12px #0008)}.device-head{align-items:flex-start}.device-head .section-art{margin-top:-5px}
    .view-tabs{position:sticky;top:8px;z-index:5;display:flex;gap:8px;margin:0 0 18px;padding:7px;border:1px solid var(--line);border-radius:14px;background:#07111ee8;backdrop-filter:blur(10px)}.view-tabs button{flex:1;margin:0;background:#182943;color:#dceaff}.view-tabs button.active{background:var(--cyan);color:#041318}.view[hidden]{display:none}
    .academy-intro{border-color:#654ca1;background:linear-gradient(135deg,#1a2143,#101a2c)}.safety-note{padding:12px 14px;border:1px solid #66509b;border-radius:12px;background:#241d42;color:#e8e0ff}.lesson-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.lesson{padding:14px;border:1px solid var(--line);border-radius:13px;background:#0a1425}.lesson h3{margin:0 0 6px}.lesson p{margin:0}
    .checkup-head{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}.progress-track{height:12px;margin:14px 0 18px;overflow:hidden;border-radius:999px;background:#07111e}.progress-fill{width:0;height:100%;border-radius:inherit;background:linear-gradient(90deg,var(--cyan),var(--violet));transition:width .3s}.checklist{display:grid;gap:10px}.check-item{display:grid;grid-template-columns:42px 1fr auto;align-items:center;gap:12px;padding:12px;border:1px solid var(--line);border-radius:13px;background:#0a1425}.check-item.done{border-color:#277858;background:#0b2822}.check-item.filtered{display:none}.check-icon{display:grid;place-items:center;width:34px;height:34px;border:2px solid #506582;border-radius:50%;color:transparent;font-weight:900}.check-item.done .check-icon{border-color:var(--green);background:var(--green);color:#062018}.check-copy strong{display:block}.check-copy span{display:block;margin-top:3px;color:var(--muted);font-size:14px}.check-item button{min-width:128px}
    .signal-layout{display:grid;grid-template-columns:minmax(210px,1fr) minmax(240px,1.25fr);gap:20px;align-items:center}.signal-gauge{position:relative;display:flex;align-items:flex-end;justify-content:center;gap:8px;height:150px;padding:18px;border:1px solid var(--line);border-radius:15px;background:#081323}.signal-gauge span{display:block;width:24px;border-radius:7px 7px 3px 3px;background:#233652;transition:height .4s,background .4s,box-shadow .4s}.signal-gauge span:nth-child(1){height:28px}.signal-gauge span:nth-child(2){height:52px}.signal-gauge span:nth-child(3){height:78px}.signal-gauge span:nth-child(4){height:106px}.signal-gauge span.on{background:var(--cyan);box-shadow:0 0 14px #38d9f577}.signal-reading{text-align:center}.signal-value{font-size:38px;font-weight:850}.signal-label{font-size:20px;font-weight:800;color:var(--cyan)}.signal-stats{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}.signal-stats span{flex:1;min-width:105px;padding:8px;border:1px solid var(--line);border-radius:10px;background:#081323;text-align:center;color:var(--muted)}.signal-stats strong{display:block;margin-top:2px;color:#eef5ff}.mini-chart{display:flex;align-items:flex-end;gap:3px;height:70px;margin-top:12px}.mini-chart span{flex:1;min-width:5px;border-radius:3px 3px 0 0;background:var(--violet)}.privacy-badge{display:inline-flex;align-items:center;gap:7px;padding:7px 10px;border:1px solid #277858;border-radius:999px;background:#0c2c24;color:var(--green);font-weight:750}.strength-result{margin-top:12px;padding:13px;border-radius:12px;background:#081323;border:1px solid var(--line)}
    .network-banner{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin:4px 0 15px;padding:13px 15px;border:1px solid #3a73a2;border-radius:13px;background:linear-gradient(90deg,#0a2538,#151d3a)}.network-banner small{display:block;color:var(--cyan);font-weight:800;letter-spacing:.08em;text-transform:uppercase}.network-banner strong{display:block;margin-top:3px;font-size:20px}.network-facts{display:flex;gap:7px;flex-wrap:wrap}.network-facts span{padding:6px 9px;border-radius:999px;background:#07111dbd;color:#dceaff}
    .glossary-controls{margin-bottom:12px}.glossary-list{display:grid;gap:10px}.glossary-list details{border:1px solid var(--line);border-radius:13px;background:#0a1425;overflow:hidden}.glossary-list summary{padding:15px;cursor:pointer;font-size:18px;font-weight:800;list-style:none}.glossary-list summary::-webkit-details-marker{display:none}.glossary-list summary:after{content:'＋';float:right;color:var(--cyan)}.glossary-list details[open] summary{border-bottom:1px solid var(--line)}.glossary-list details[open] summary:after{content:'−'}.glossary-list details p{margin:0;padding:14px 16px 17px}
    h1,h2{margin:.2em 0}p{color:var(--muted)}button,a.button{display:inline-block;background:var(--cyan);color:#041318;border:0;border-radius:10px;padding:10px 14px;font-weight:700;text-decoration:none;cursor:pointer;margin:3px}
    button.secondary{background:#263a56;color:#eef5ff}button.danger{background:var(--red)}input,select{background:#091524;color:#eef5ff;border:1px solid var(--line);border-radius:9px;padding:10px;margin:3px;max-width:100%}
    .scroll{overflow:auto}table{width:100%;border-collapse:collapse;white-space:nowrap}td,th{text-align:left;border-bottom:1px solid var(--line);padding:9px 6px}
    code{color:var(--cyan)}.pill{display:inline-block;border:1px solid var(--line);border-radius:999px;padding:5px 9px;margin:3px;color:var(--muted)}.new{color:var(--green);font-weight:800}.muted{color:var(--muted)}
    .chart{display:flex;align-items:flex-end;gap:5px;height:130px;padding-top:12px}.barwrap{flex:1;min-width:22px;text-align:center;color:var(--muted);font-size:12px}.bar{background:var(--cyan);border-radius:5px 5px 0 0;min-height:2px}.controls{display:flex;flex-wrap:wrap;align-items:center;gap:5px}
    .warning{background:#5a1e27;border:1px solid var(--red);border-radius:10px;color:#fff;padding:10px;margin-top:12px;font-weight:700}
    .battery-panel{display:flex;align-items:center;gap:18px;margin:14px 0}.battery-shell{position:relative;width:150px;height:68px;border:5px solid #dce8f7;border-radius:12px;padding:5px}.battery-shell:after{content:'';position:absolute;right:-13px;top:19px;width:9px;height:25px;background:#dce8f7;border-radius:0 5px 5px 0}.battery-fill{height:100%;width:0;background:var(--green);border-radius:5px;transition:width .8s,background .4s}.battery-shell.charging .battery-fill{animation:chargePulse 1.3s ease-in-out infinite}.battery-bolt{display:none;position:absolute;inset:0;align-items:center;justify-content:center;font-size:34px;color:#fff;text-shadow:0 2px 8px #000}.battery-shell.charging .battery-bolt{display:flex;animation:boltPulse 1.3s ease-in-out infinite}.battery-number{font-size:32px;font-weight:800}.battery-caption{color:var(--muted)}
    @keyframes chargePulse{50%{filter:brightness(1.6);box-shadow:0 0 18px var(--green)}}@keyframes boltPulse{50%{transform:scale(1.15);opacity:.65}}
    @media(max-width:620px){main{padding:12px}.hero{min-height:205px;padding:20px 135px 20px 17px}.hero h1{font-size:29px}.hero-copy>p{font-size:14px}.nova-stage{right:-12px;width:160px;height:202px}#novaSprite{max-width:150px;max-height:196px}.nova-message{font-size:13px;padding:8px}.section-head{min-height:96px;gap:8px}.section-art{width:96px;height:96px;flex-basis:96px}.battery-panel{align-items:flex-start;flex-direction:column}.battery-shell{width:135px;height:60px}.battery-number{font-size:28px}.lesson-grid,.signal-layout{grid-template-columns:1fr}.view-tabs{top:4px}.view-tabs button{padding:10px 7px;font-size:13px}.signal-gauge{height:125px}.check-item{grid-template-columns:38px 1fr}.check-item button{grid-column:1/-1;width:100%;margin:0}.network-banner{align-items:flex-start;flex-direction:column}}
    @media(max-width:360px){.section-art{display:none}.section-head{min-height:0}}
  </style>
</head>
<body><main>
  <div class="hero"><div class="hero-copy"><div class="hero-kicker">StickS3 field companion · <span id="firmwareVersion">v—</span></div><h1>PocketLab</h1><p>Wi-Fi, Bluetooth, files, and device tools.</p><div id="novaMessage" class="nova-message">Nova is ready.</div></div><div class="nova-stage"><img id="novaSprite" src="/assets/nova/idle.png" alt="Nova, PocketLab assistant"></div></div>
  <nav class="view-tabs" aria-label="PocketLab sections"><button id="homeTab" class="active" onclick="showView('home')">PocketLab Home</button><button id="academyTab" onclick="showView('academy')">Cyber Academy</button><button id="glossaryTab" onclick="showView('glossary')">Glossary</button></nav>
  <div id="homeView" class="view">
  <section class="card"><div class="section-head device-head"><div><h2>Device</h2><div class="battery-panel"><div id="batteryShell" class="battery-shell"><div id="batteryFill" class="battery-fill"></div><div class="battery-bolt">⚡</div></div><div><div id="batteryNumber" class="battery-number">--%</div><div id="batteryCaption" class="battery-caption">Reading battery…</div></div></div><div id="status">Loading…</div></div><img class="section-art" src="/assets/nova/device.png" alt="Nova monitoring PocketLab"></div>
    <div class="controls" style="margin-top:12px"><button class="secondary" onclick="setDisplay('normal')">Wake screen</button><button class="secondary" onclick="setDisplay('dim')">Dim screen</button><button class="secondary" onclick="setDisplay('off')">Screen off</button><button class="danger" onclick="sleepDevice()">Sleep PocketLab</button></div>
    <div class="controls" style="margin-top:8px"><label>Power profile <select id="profile" onchange="setProfile(this.value)"><option value="eco">Eco</option><option value="performance">Performance</option></select></label><label>Stop portal with no clients <select id="timeout" onchange="setTimeoutMinutes(this.value)"><option value="2">2 minutes</option><option value="5" selected>5 minutes</option><option value="10">10 minutes</option><option value="0">Never</option></select></label></div>
  </section>
  <section class="card"><div class="section-head"><div><h2>Wi-Fi survey</h2><p>Inventory nearby access points without joining them.</p></div><img class="section-art" src="/assets/nova/wifi.png" loading="lazy" alt="Nova searching for Wi-Fi"></div>
    <div class="controls"><button onclick="scan()">Run scan</button><button class="secondary" onclick="setBaseline()">Set current as baseline</button>
      <select id="sort" onchange="renderScan()"><option value="rssi">Strongest first</option><option value="channel">Channel</option><option value="ssid">Network name</option></select></div>
    <div id="summary"></div><div id="chart"></div><div id="scan" class="scroll"></div>
    <hr style="border-color:var(--line);border-width:1px 0 0;margin:18px 0">
    <div class="controls"><input id="session" maxlength="32" placeholder="Session name, e.g. Home"><button onclick="saveSession()">Save CSV + JSON</button></div><div id="saveStatus" class="muted"></div>
  </section>
  <section class="card"><div class="section-head"><div><h2>BLE inventory</h2><p>Runs an on-demand 10-second Bluetooth Low Energy advertisement scan. BLE is shut down immediately afterward.</p></div><img class="section-art" src="/assets/nova/ble.png" loading="lazy" alt="Nova recording Bluetooth devices"></div>
    <div class="controls"><button id="bleScanButton" onclick="scanBle()">Run BLE scan</button><button class="secondary" onclick="setBleBaseline()">Set current as baseline</button>
      <select id="bleSort" onchange="renderBle()"><option value="rssi">Strongest first</option><option value="name">Device name</option><option value="address">Address</option></select></div>
    <p class="muted">Randomized BLE addresses can change. A “NEW” address is an observation, not proof of a new physical device.</p><div id="bleSummary"></div><div id="bleResults" class="scroll"></div>
    <div class="controls" style="margin-top:14px"><input id="bleSession" maxlength="32" placeholder="BLE session name"><button onclick="saveBleSession()">Save CSV + JSON</button></div><div id="bleSaveStatus" class="muted"></div>
  </section>
  <section class="card"><div class="section-head"><div><h2>Bluetooth keyboard</h2><p>Pair a BLE HID keyboard and test it as a PocketLab input device.</p></div><img class="section-art" src="/assets/nova/keyboard.png" loading="lazy" alt="Nova holding a Bluetooth keyboard"></div>
    <button onclick="startKeyboardMode()">Start keyboard mode</button><p class="muted">PocketLab will stop Wi-Fi to free memory. Put one Logitech Easy-Switch slot into pairing mode, type the six-digit code shown on the StickS3, then press Enter. Press the M5 button or keyboard Esc to exit and restart the portal.</p><div id="keyboardStatus" class="muted"></div>
  </section>
  <section class="card"><div class="section-head"><div><h2>File vault</h2><p>Use PocketLab’s internal flash for small field files and saved survey reports.</p></div><img class="section-art" src="/assets/nova/vault.png" loading="lazy" alt="Nova standing beside a secure vault"></div>
    <div class="controls"><input id="uploadFile" type="file"><button id="uploadButton" onclick="uploadFile()">Upload file</button></div>
    <p class="muted">Maximum upload: 1 MB. PocketLab keeps 64 KB free for filesystem housekeeping.</p><div id="uploadStatus" class="muted"></div>
    <div id="files" class="scroll">Loading…</div></section>
  </div>

  <div id="academyView" class="view" hidden>
    <section class="card academy-intro"><div class="section-head"><div><div class="hero-kicker">Learn safely · Home use only</div><h2>Cyber Academy</h2><p>Friendly lessons for students, families, and adults who are new to networking.</p><div class="safety-note"><strong>Permission first:</strong> explore only equipment and networks you own or have clear permission to check. These lessons observe normal broadcasts; they do not crack passwords, disconnect devices, or attack networks.</div></div><img class="section-art" src="/assets/nova/wifi.png" alt="Nova teaching Wi-Fi safety"></div></section>

    <section class="card"><div class="checkup-head"><div><h2>Home-network checkup</h2><p>Complete each step at your own pace. Progress lasts only for this browser session.</p></div><button id="completedFilter" class="secondary" onclick="toggleCompletedVisibility()">Completed: shown</button></div><div class="progress-track" aria-label="Checkup progress"><div id="checkupProgress" class="progress-fill"></div></div><p id="checkupCount" class="muted">0 of 6 steps completed</p>
      <div class="checklist">
        <div class="check-item" data-step="permission"><div class="check-icon">✓</div><div class="check-copy"><strong>1. Confirm permission</strong><span>I own this network or have permission to check it.</span></div><button class="secondary" onclick="toggleCheckStep('permission')">Mark complete</button></div>
        <div class="check-item" data-step="identify"><div class="check-icon">✓</div><div class="check-copy"><strong>2. Identify the home network</strong><span>Find and select the correct Wi-Fi name in the signal explorer.</span></div><button class="secondary" onclick="toggleCheckStep('identify')">Mark complete</button></div>
        <div class="check-item" data-step="security"><div class="check-icon">✓</div><div class="check-copy"><strong>3. Review security</strong><span>Check the advertised security mode and prefer WPA3 or WPA2.</span></div><button class="secondary" onclick="toggleCheckStep('security')">Mark complete</button></div>
        <div class="check-item" data-step="coverage"><div class="check-icon">✓</div><div class="check-copy"><strong>4. Test coverage</strong><span>Measure a few rooms and look for weak areas or dead spots.</span></div><button class="secondary" onclick="toggleCheckStep('coverage')">Mark complete</button></div>
        <div class="check-item" data-step="wps"><div class="check-icon">✓</div><div class="check-copy"><strong>5. Check WPS manually</strong><span>Open the router settings and disable WPS PIN mode when possible.</span></div><button class="secondary" onclick="toggleCheckStep('wps')">Mark complete</button></div>
        <div class="check-item" data-step="router"><div class="check-icon">✓</div><div class="check-copy"><strong>6. Review router basics</strong><span>Check updates, administrator password, and physical placement.</span></div><button class="secondary" onclick="toggleCheckStep('router')">Mark complete</button></div>
      </div>
    </section>

    <section class="card"><div class="section-head"><div><h2>Wi-Fi signal explorer</h2><p>Walk around your home to find strong coverage and possible dead spots.</p></div><img class="section-art" src="/assets/nova/wifi.png" loading="lazy" alt="Nova measuring Wi-Fi"></div>
      <div class="network-banner"><div><small>Selected network</small><strong id="selectedNetworkName">None selected</strong></div><div class="network-facts"><span id="selectedNetworkBand">Band —</span><span id="selectedNetworkChannel">Channel —</span></div></div>
      <p><span class="privacy-badge">✓ No history saved</span></p>
      <div class="controls"><button id="academyScanButton" onclick="academyScan(false)">Find networks</button><select id="signalTarget" onchange="selectSignalTarget()"><option value="">Run a scan to choose your network</option></select><button id="walkButton" class="secondary" onclick="toggleWalkTest()">Start walk test</button><button class="secondary" onclick="resetSignalReadings()">Reset room readings</button></div>
      <p class="muted">Live mode measures about every nine seconds, smooths the display with the latest five readings, and keeps no more than 20 readings in this browser tab. It stops after three minutes or whenever this tab is hidden. Closing or refreshing the page clears everything.</p>
      <div class="signal-layout">
        <div><div id="signalGauge" class="signal-gauge" aria-label="Wi-Fi signal meter"><span></span><span></span><span></span><span></span></div><div class="signal-reading"><div id="signalValue" class="signal-value">— dBm</div><div id="signalLabel" class="signal-label">Choose a network</div></div></div>
        <div><div id="signalAdvice" class="strength-result">Run a scan, choose your own network, then move around the room. Lower negative numbers are stronger: −50 dBm is stronger than −80 dBm.</div><div id="signalStats" class="signal-stats"><span>Latest<strong>—</strong></span><span>Strongest<strong>—</strong></span><span>Weakest<strong>—</strong></span></div><div id="signalHistory" class="mini-chart" aria-label="Recent signal readings"></div><p id="signalMeta" class="muted"></p></div>
      </div>
    </section>

    <section class="card"><h2>Password-strength lesson</h2><p>Practice with a made-up example—not your real Wi-Fi password. The example stays in your browser and is never sent to PocketLab.</p>
      <div class="controls"><input id="practicePassword" type="password" autocomplete="off" spellcheck="false" maxlength="80" placeholder="Type a made-up example" oninput="checkPracticePassword()"><button class="secondary" onclick="clearPracticePassword()">Clear</button></div>
      <div id="passwordResult" class="strength-result">Try a long, unique passphrase made from unrelated words. Length matters more than swapping a few letters for symbols.</div>
    </section>

    <section class="card"><h2>Home Wi-Fi safety check</h2><div class="lesson-grid">
      <div class="lesson"><h3>🔒 Security mode</h3><p>Prefer WPA3, or WPA2-AES when WPA3 is unavailable. Replace WEP, WPA, or an open network.</p></div>
      <div class="lesson"><h3>🔘 WPS</h3><p>A normal Wi-Fi scan cannot reliably prove whether WPS is enabled. Check the router’s settings and disable WPS PIN mode when possible.</p></div>
      <div class="lesson"><h3>🧭 Router placement</h3><p>Place the router high and in the open, near the center of the home—not inside a cabinet or behind a television.</p></div>
      <div class="lesson"><h3>🔄 Updates</h3><p>Install router firmware updates and change the router administrator password from its factory default.</p></div>
    </div></section>

    <section class="card"><h2>What the readings mean</h2><div class="lesson-grid">
      <div class="lesson"><h3>−30 to −55 dBm</h3><p>Excellent to strong. Video calls and streaming should usually work well.</p></div>
      <div class="lesson"><h3>−56 to −67 dBm</h3><p>Good. A healthy range for most everyday devices.</p></div>
      <div class="lesson"><h3>−68 to −75 dBm</h3><p>Fair. Connections may slow down or become less reliable.</p></div>
      <div class="lesson"><h3>Below −75 dBm</h3><p>Weak. Try moving the router, adding an access point, or testing a less crowded channel.</p></div>
    </div></section>
  </div>

  <div id="glossaryView" class="view" hidden>
    <section class="card academy-intro"><div class="section-head"><div><div class="hero-kicker">PocketLab learning library</div><h2>Network glossary</h2><p>Tap any term to expand its plain-language explanation.</p></div><img class="section-art" src="/assets/nova/idle.png" alt="Nova ready to help"></div></section>
    <section class="card"><div class="glossary-controls"><button onclick="setGlossaryOpen(true)">Expand all</button><button class="secondary" onclick="setGlossaryOpen(false)">Collapse all</button></div><div class="glossary-list">
      <details><summary>Access point</summary><p>The device broadcasting Wi-Fi. It may be built into your router, or it may be a separate unit placed elsewhere in the home.</p></details>
      <details><summary>Channel</summary><p>A numbered slice of radio space used by Wi-Fi. Nearby access points on the same or overlapping channels may compete for airtime.</p></details>
      <details><summary>Dead spot</summary><p>A place where Wi-Fi is too weak or unreliable for normal use. Walls, distance, metal, appliances, and router placement can contribute.</p></details>
      <details><summary>dBm</summary><p>A signal-strength measurement. Wi-Fi values are normally negative: −50 dBm is stronger than −80 dBm because it is closer to zero.</p></details>
      <details><summary>GHz / frequency band</summary><p>The radio-frequency range used by a network. The StickS3 measures 2.4 GHz Wi-Fi, which generally reaches farther than higher-frequency Wi-Fi but can be more crowded.</p></details>
      <details><summary>Router</summary><p>The device that connects a home network to the internet and directs traffic between devices. Many home routers also contain the Wi-Fi access point.</p></details>
      <details><summary>SSID</summary><p>The human-readable Wi-Fi network name shown when you choose a network, such as “Home Wi-Fi.”</p></details>
      <details><summary>BSSID</summary><p>The hardware address identifying one particular Wi-Fi access point radio. Several access points can share the same SSID.</p></details>
      <details><summary>WPA2 and WPA3</summary><p>Modern Wi-Fi security standards. WPA3 is newer; WPA2 remains widely used. WEP and original WPA are outdated.</p></details>
      <details><summary>WPS</summary><p>Wi-Fi Protected Setup is a convenience feature for adding devices. PIN-based WPS has known weaknesses, so disabling it is recommended when it is not needed.</p></details>
      <details><summary>Wi-Fi password</summary><p>The secret used to join a protected wireless network. Use a long, unique password or passphrase and do not reuse an important account password.</p></details>
    </div></section>
  </div>
</main><script>
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let networks=[],bleDevices=[],signalHistory=[],signalTargetBssid='',walkTimer=null,walkStopTimer=null,walkBusy=false,showCompleted=true;
const completedSteps=new Set();
const novaSprites={idle:'/assets/nova/idle.png',scanning:'/assets/nova/scanning.png',success:'/assets/nova/success.png',warning:'/assets/nova/warning.png'};
let novaHoldUntil=0;
function setNova(state,message,holdMs=0){
 const sprite=document.querySelector('#novaSprite');
 sprite.style.opacity='.25';setTimeout(()=>{sprite.src=novaSprites[state]||novaSprites.idle;sprite.style.opacity='1'},120);
 document.querySelector('#novaMessage').textContent=message;
 novaHoldUntil=holdMs?Date.now()+holdMs:0;
}
function showView(name){
 const academy=name==='academy',glossary=name==='glossary',home=!academy&&!glossary;
 document.querySelector('#homeView').hidden=!home;document.querySelector('#academyView').hidden=!academy;document.querySelector('#glossaryView').hidden=!glossary;
 document.querySelector('#homeTab').classList.toggle('active',home);document.querySelector('#academyTab').classList.toggle('active',academy);document.querySelector('#glossaryTab').classList.toggle('active',glossary);
 if(!academy)stopWalkTest();
 window.scrollTo({top:0,behavior:'smooth'});
 setNova(academy?'success':'idle',academy?'Cyber Academy is ready. Learn safely on networks you own.':glossary?'Tap a glossary term to learn what it means.':'PocketLab tools are ready.',3500);
}
function toggleCheckStep(step){completedSteps.has(step)?completedSteps.delete(step):completedSteps.add(step);renderChecklist()}
function toggleCompletedVisibility(){showCompleted=!showCompleted;renderChecklist()}
function renderChecklist(){
 const items=[...document.querySelectorAll('.check-item')];
 items.forEach(item=>{const done=completedSteps.has(item.dataset.step),button=item.querySelector('button');item.classList.toggle('done',done);item.classList.toggle('filtered',done&&!showCompleted);button.textContent=done?'Mark not complete':'Mark complete';button.classList.toggle('secondary',!done)});
 const count=completedSteps.size,total=items.length;document.querySelector('#checkupProgress').style.width=(total?count/total*100:0)+'%';document.querySelector('#checkupCount').textContent=`${count} of ${total} steps completed`;document.querySelector('#completedFilter').textContent=showCompleted?'Completed: shown':'Completed: hidden';
 if(count===total)setNova('success','Home-network checkup complete—great work!',6000);
}
function setGlossaryOpen(open){document.querySelectorAll('.glossary-list details').forEach(item=>item.open=open)}
async function load(){
 await loadStatus();
 await loadFiles();
}
async function loadStatus(){
 const s=await (await fetch('/api/status')).json();
 const battery=s.battery_percent>=0?`${s.battery_percent}%`:'Unknown';
 const volts=s.battery_mv>0?`${(s.battery_mv/1000).toFixed(2)} V`:'Voltage unavailable';
 const minVolts=s.minimum_battery_mv>0?`${(s.minimum_battery_mv/1000).toFixed(2)} V`:'Unknown';
 const pct=Math.max(0,Math.min(100,s.battery_percent<0?0:s.battery_percent));
 const shell=document.querySelector('#batteryShell'),fill=document.querySelector('#batteryFill');shell.classList.toggle('charging',s.power_state==='Charging');fill.style.width=pct+'%';fill.style.background=s.low_battery?'var(--red)':s.power_state==='Charging'?'var(--green)':'var(--cyan)';
 document.querySelector('#firmwareVersion').textContent=s.firmware_version;
 document.querySelector('#batteryNumber').textContent=s.battery_percent>=0?'~'+s.battery_percent+'%':'Unknown';document.querySelector('#batteryCaption').textContent=`${s.power_state} • ${volts}`;
 document.querySelector('#profile').value=s.power_profile;document.querySelector('#timeout').value=String(s.portal_timeout_minutes);
 const slope=s.voltage_rate_mv_per_hour===null?'Collecting…':`${s.voltage_rate_mv_per_hour>0?'+':''}${s.voltage_rate_mv_per_hour} mV/hour`;
 document.querySelector('#status').innerHTML=`<span class="pill">Filtered ${volts}</span><span class="pill">Minimum ${minVolts}</span><span class="pill">Trend ${slope}</span><span class="pill">${s.ap_clients} connected device${s.ap_clients===1?'':'s'}</span><span class="pill">Profile ${esc(s.power_profile)}</span><span class="pill">CPU ${s.cpu_mhz} MHz</span><span class="pill">Display ${esc(s.display_state)}</span><span class="pill">Last reset: ${esc(s.reset_reason)}</span><span class="pill">IP ${esc(s.ip)}</span><span class="pill">Heap ${s.heap_kb} KB</span><span class="pill">PSRAM ${s.psram_kb} KB</span><span class="pill">Flash ${s.fs_used_kb}/${s.fs_total_kb} KB</span>${s.low_battery?'<div class="warning">Low battery: connect USB power soon.</div>':''}`;
 if(s.low_battery)setNova('warning',`Battery is low at ${battery}. Connect USB power soon.`,5500);
 else if(Date.now()>novaHoldUntil)setNova('idle',`${s.ap_clients} device${s.ap_clients===1?'':'s'} connected • ${battery} battery`);
}
async function setDisplay(mode){
 await fetch('/api/display',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mode='+encodeURIComponent(mode)});setTimeout(loadStatus,250);
}
async function setProfile(profile){await fetch('/api/power-profile',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'profile='+encodeURIComponent(profile)});setTimeout(loadStatus,250)}
async function setTimeoutMinutes(minutes){await fetch('/api/portal-timeout',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'minutes='+encodeURIComponent(minutes)});setTimeout(loadStatus,250)}
async function sleepDevice(){if(!confirm('Turn off Wi-Fi and the screen? Press the blue Face button to wake PocketLab.'))return;setNova('warning','Going to sleep. Press the blue Face button to wake PocketLab.',10000);await fetch('/api/sleep',{method:'POST'});document.querySelector('#status').innerHTML='<div class="warning">PocketLab is sleeping. Press the blue Face button to wake it.</div>'}
async function startKeyboardMode(){
 if(!confirm('PocketLab will temporarily stop Wi-Fi. Continue and pair a BLE keyboard?'))return;
 setNova('scanning','Preparing Bluetooth keyboard pairing. Watch the StickS3 screen.',10000);
 const status=document.querySelector('#keyboardStatus');status.textContent='Starting keyboard mode… Watch the StickS3 screen.';
 const r=await fetch('/api/keyboard-mode',{method:'POST'});if(!r.ok){status.textContent=await r.text();setNova('warning','Keyboard mode could not start.',5000)}
}
async function loadFiles(){
 const f=await (await fetch('/api/files')).json();
 document.querySelector('#files').innerHTML=f.length?`<table><tr><th>Type</th><th>Name</th><th>Size</th><th>Actions</th></tr>${f.map(x=>`<tr><td><span class="pill">${x.kind==='upload'?'Vault':'Report'}</span></td><td><code>${esc(x.name)}</code></td><td>${formatBytes(x.size)}</td><td><a class="button" href="/download?path=${encodeURIComponent(x.name)}">Download</a><button class="danger" onclick="deleteFile('${encodeURIComponent(x.name)}')">Delete</button></td></tr>`).join('')}</table>`:'No files yet.';
}
function formatBytes(bytes){return bytes<1024?bytes+' B':bytes<1048576?(bytes/1024).toFixed(1)+' KB':(bytes/1048576).toFixed(2)+' MB'}
async function uploadFile(){
 const input=document.querySelector('#uploadFile'),file=input.files[0],status=document.querySelector('#uploadStatus'),button=document.querySelector('#uploadButton');
 if(!file){status.textContent='Choose a file first.';setNova('warning','Choose a file before uploading.',3500);return}if(file.size>1048576){status.textContent='That file is larger than the 1 MB limit.';setNova('warning','That file exceeds the 1 MB limit.',4500);return}
 const body=new FormData();body.append('file',file,file.name);button.disabled=true;status.textContent=`Uploading ${file.name}…`;
 setNova('scanning',`Storing ${file.name} in the file vault…`,30000);
 try{const r=await fetch('/api/upload',{method:'POST',body});status.textContent=await r.text();if(r.ok){input.value='';await loadFiles();await loadStatus();setNova('success',`${file.name} is safely stored.`,4500)}else setNova('warning','The upload was not completed.',5000)}
 catch(error){status.textContent='Upload failed: '+(error.message||error);setNova('warning','The upload connection failed.',5000)}finally{button.disabled=false}
}
async function scan(){
 document.querySelector('#scan').innerHTML='<p>Scanning…</p>';
 setNova('scanning','Scanning nearby Wi-Fi channels…',30000);
 try{const response=await fetch('/api/wifi-scan');if(!response.ok)throw new Error(await response.text());networks=await response.json();renderScan();setNova('success',`Found ${networks.length} Wi-Fi network${networks.length===1?'':'s'}.`,4500)}
 catch(error){document.querySelector('#scan').innerHTML=`<div class="warning">${esc(error.message||error)}</div>`;setNova('warning','The Wi-Fi scan could not be completed.',5000)}
}
function populateSignalTargets(){
 const select=document.querySelector('#signalTarget'),previous=signalTargetBssid||select.value;
 const visible=[...networks].filter(x=>x.ssid).sort((a,b)=>b.rssi-a.rssi);
 select.innerHTML='<option value="">Choose your network or access point</option>'+visible.map(x=>`<option value="${esc(x.bssid)}">${esc(x.ssid)} · ${esc(x.bssid)} · ch ${x.channel}</option>`).join('');
 if(visible.some(x=>x.bssid===previous)){select.value=previous;signalTargetBssid=previous}
}
async function academyScan(fromTimer){
 if(walkBusy)return;walkBusy=true;
 const button=document.querySelector('#academyScanButton');button.disabled=true;if(!fromTimer)button.textContent='Scanning…';
 try{
  const response=await fetch('/api/wifi-scan');if(!response.ok)throw new Error(await response.text());networks=await response.json();
  populateSignalTargets();
  if(signalTargetBssid){const found=networks.find(x=>x.bssid===signalTargetBssid);if(found)addSignalReading(found);else showMissingSignal()}
  else{document.querySelector('#signalAdvice').textContent=`Found ${networks.length} nearby access points. Choose one that belongs to you.`;setNova('success','Choose your own Wi-Fi network to begin the signal lesson.',3500)}
 }catch(error){document.querySelector('#signalAdvice').textContent='Scan failed: '+(error.message||error);setNova('warning','The signal measurement could not be completed.',4500)}
 finally{walkBusy=false;button.disabled=false;button.textContent='Find networks'}
}
function selectSignalTarget(){
 signalTargetBssid=document.querySelector('#signalTarget').value;signalHistory=[];renderSignalHistory();
 if(!signalTargetBssid){setSelectedNetworkBanner(null);setSignalDisplay(null);return}
 const found=networks.find(x=>x.bssid===signalTargetBssid);if(found)addSignalReading(found);
}
function setSelectedNetworkBanner(network){
 document.querySelector('#selectedNetworkName').textContent=network?network.ssid:'None selected';document.querySelector('#selectedNetworkBand').textContent=network?'2.4 GHz':'Band —';document.querySelector('#selectedNetworkChannel').textContent=network?'Channel '+network.channel:'Channel —';
}
function signalGrade(rssi){
 if(rssi>=-55)return {label:'Strong',bars:4,color:'var(--green)',advice:'Great coverage here. This is a good location for video calls, streaming, and schoolwork.'};
 if(rssi>=-67)return {label:'Good',bars:3,color:'var(--cyan)',advice:'Healthy coverage for most uses. Keep checking the edges of the room.'};
 if(rssi>=-75)return {label:'Fair',bars:2,color:'#ffd166',advice:'Usable, but this may become unreliable through walls or with interference.'};
 return {label:'Weak / possible dead spot',bars:1,color:'var(--red)',advice:'Try moving closer, raising the router, or relocating it toward the center of the home.'};
}
function addSignalReading(network){
 signalHistory.push(network.rssi);if(signalHistory.length>20)signalHistory.shift();setSelectedNetworkBanner(network);setSignalDisplay(network);renderSignalHistory();
}
function setSignalDisplay(network){
 const bars=[...document.querySelectorAll('#signalGauge span')];
 if(!network){bars.forEach(x=>x.classList.remove('on'));document.querySelector('#signalValue').textContent='— dBm';document.querySelector('#signalLabel').textContent=signalTargetBssid?'Ready for new room':'Choose a network';renderSignalStats();return}
 const smoothed=median(signalHistory.slice(-5)),grade=signalGrade(smoothed);bars.forEach((x,i)=>{x.classList.toggle('on',i<grade.bars);if(i<grade.bars)x.style.background=grade.color});
 document.querySelector('#signalValue').textContent=smoothed+' dBm';document.querySelector('#signalLabel').textContent=grade.label;document.querySelector('#signalLabel').style.color=grade.color;
 const sameChannel=networks.filter(x=>x.bssid!==network.bssid&&x.channel===network.channel).length;
 const olderSecurity=network.security==='Open'||network.security==='WEP'||network.security==='WPA'||network.security==='WPA/WPA2';
 document.querySelector('#signalAdvice').textContent=grade.advice+(olderSecurity?' Security note: this access point advertises an open or older security mode; check the router settings and prefer WPA3 or WPA2.':'');
 document.querySelector('#signalMeta').textContent=`Rolling median of up to five readings shown · ${network.security} · ${sameChannel} other access point${sameChannel===1?'':'s'} heard on channel ${network.channel} · ${signalHistory.length} temporary reading${signalHistory.length===1?'':'s'}`;
 renderSignalStats();setNova(grade.bars<2?'warning':'success',`${grade.label}: ${smoothed} dBm smoothed.`,2500);
}
function median(values){if(!values.length)return null;const sorted=[...values].sort((a,b)=>a-b),middle=Math.floor(sorted.length/2);return sorted.length%2?sorted[middle]:Math.round((sorted[middle-1]+sorted[middle])/2)}
function renderSignalStats(){
 const values=document.querySelectorAll('#signalStats strong');if(!signalHistory.length){values.forEach(x=>x.textContent='—');return}
 values[0].textContent=signalHistory[signalHistory.length-1]+' dBm';values[1].textContent=Math.max(...signalHistory)+' dBm';values[2].textContent=Math.min(...signalHistory)+' dBm';
}
function showMissingSignal(){document.querySelector('#signalValue').textContent='Not seen';document.querySelector('#signalLabel').textContent='Move closer or measure again';document.querySelector('#signalAdvice').textContent='PocketLab did not hear that access point during this scan. This can happen near a coverage edge.'}
function renderSignalHistory(){
 const chart=document.querySelector('#signalHistory');if(!signalHistory.length){chart.innerHTML='';return}
 chart.innerHTML=signalHistory.map(rssi=>{const normalized=Math.max(5,Math.min(100,(rssi+100)*2));return `<span title="${rssi} dBm" style="height:${normalized}%"></span>`}).join('');
}
function resetSignalReadings(){signalHistory=[];renderSignalHistory();setSignalDisplay(null);document.querySelector('#signalAdvice').textContent=signalTargetBssid?'Room readings cleared. Move to the new location and measure again.':'Choose your network before starting room measurements.';document.querySelector('#signalMeta').textContent='';setNova('idle','Room readings cleared. Ready for a new location.',3000)}
function toggleWalkTest(){walkTimer?stopWalkTest():startWalkTest()}
function startWalkTest(){
 if(!signalTargetBssid){document.querySelector('#signalAdvice').textContent='Find networks and choose your own access point before starting the walk test.';return}
 document.querySelector('#walkButton').textContent='Stop walk test';document.querySelector('#walkButton').classList.remove('secondary');academyScan(true);walkTimer=setInterval(()=>academyScan(true),9000);walkStopTimer=setTimeout(()=>stopWalkTest('Walk test finished after three minutes to save battery.'),180000);
}
function stopWalkTest(message=''){if(walkTimer){clearInterval(walkTimer);walkTimer=null}if(walkStopTimer){clearTimeout(walkStopTimer);walkStopTimer=null}const button=document.querySelector('#walkButton');if(button){button.textContent='Start walk test';button.classList.add('secondary')}if(message){document.querySelector('#signalAdvice').textContent=message;setNova('idle',message,4000)}}
function checkPracticePassword(){
 const input=document.querySelector('#practicePassword'),value=input.value,result=document.querySelector('#passwordResult');
 if(!value){result.textContent='Try a long, unique passphrase made from unrelated words. Length matters more than swapping a few letters for symbols.';return}
 let score=0;if(value.length>=12)score++;if(value.length>=16)score++;if(value.length>=20)score++;if(/[a-z]/.test(value)&&/[A-Z]/.test(value))score++;if(/\d/.test(value)&&/[^A-Za-z0-9]/.test(value))score++;
 const predictable=/(password|qwerty|letmein|1234|abcd|admin|welcome|wifi)/i.test(value)||/(.)\1{3,}/.test(value);if(predictable)score=Math.max(0,score-2);
 const label=score>=5?'Strong practice example':score>=3?'Getting stronger':'Needs improvement';
 const color=score>=5?'var(--green)':score>=3?'#ffd166':'var(--red)';result.innerHTML=`<strong style="color:${color}">${label}</strong><br>${value.length} characters. ${predictable?'Avoid common words, sequences, and repeated characters. ':''}${value.length<16?'Aim for at least 16 characters. ':'Good length. '}Use a different password for every important account.`;
}
function clearPracticePassword(){const input=document.querySelector('#practicePassword');input.value='';checkPracticePassword();input.focus()}
function renderScan(){
 const mode=document.querySelector('#sort').value;
 const a=[...networks].sort((x,y)=>mode==='channel'?x.channel-y.channel:mode==='ssid'?(x.ssid||'').localeCompare(y.ssid||''):y.rssi-x.rssi);
 const fresh=a.filter(x=>x.is_new).length;
 document.querySelector('#summary').innerHTML=a.length?`<p><span class="pill">${a.length} networks</span><span class="pill">${fresh} new vs baseline</span></p>`:'';
 document.querySelector('#scan').innerHTML=a.length?`<table><tr><th>SSID</th><th>BSSID</th><th>Ch</th><th>RSSI</th><th>Security</th><th></th></tr>${a.map(x=>`<tr><td>${esc(x.ssid||'(hidden)')}</td><td><code>${esc(x.bssid)}</code></td><td>${x.channel}</td><td>${x.rssi}</td><td>${esc(x.security)}</td><td class="new">${x.is_new?'NEW':''}</td></tr>`).join('')}</table>`:'<p>No networks found.</p>';
 renderChart(a);
}
function renderChart(a){
 const counts=Array(14).fill(0);a.forEach(x=>{if(x.channel>=1&&x.channel<=13)counts[x.channel]++});
 const max=Math.max(1,...counts);document.querySelector('#chart').innerHTML=a.length?`<div class="chart">${counts.slice(1).map((n,i)=>`<div class="barwrap"><div>${n}</div><div class="bar" style="height:${Math.max(2,n/max*90)}px"></div><div>${i+1}</div></div>`).join('')}</div><p class="muted">Networks by 2.4 GHz channel</p>`:'';
}
async function saveSession(){
 if(!networks.length){document.querySelector('#saveStatus').textContent='Run a scan first.';setNova('warning','Run a Wi-Fi scan before saving a session.',4000);return}
 const name=document.querySelector('#session').value.trim()||'Survey';
 const body=new URLSearchParams({name,timestamp:new Date().toISOString()});
 const r=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
 document.querySelector('#saveStatus').textContent=await r.text();await loadFiles();setNova(r.ok?'success':'warning',r.ok?'Wi-Fi survey saved to the file vault.':'The Wi-Fi survey could not be saved.',4500);
}
async function setBaseline(){
 if(!networks.length){alert('Run a scan first.');setNova('warning','Run a Wi-Fi scan before setting the baseline.',4000);return}
 const r=await fetch('/api/baseline',{method:'POST'});alert(await r.text());networks.forEach(x=>x.is_new=false);renderScan();setNova(r.ok?'success':'warning',r.ok?'Wi-Fi baseline updated.':'The baseline could not be updated.',4000);
}
async function scanBle(){
 const button=document.querySelector('#bleScanButton');button.disabled=true;button.textContent='Scanning for 10 seconds…';
 document.querySelector('#bleResults').innerHTML='<p>Listening for BLE advertisements…</p>';
 setNova('scanning','Listening for nearby Bluetooth advertisements…',30000);
 try{
  const response=await fetch('/api/ble-scan');
  if(!response.ok)throw new Error(await response.text());
  bleDevices=await response.json();renderBle();setNova('success',`Found ${bleDevices.length} Bluetooth device${bleDevices.length===1?'':'s'}.`,4500);
 }catch(error){document.querySelector('#bleResults').innerHTML=`<div class="warning">${esc(error.message||error)}</div>`;setNova('warning','The Bluetooth scan could not be completed.',5000)}
 finally{button.disabled=false;button.textContent='Run BLE scan'}
}
function renderBle(){
 const mode=document.querySelector('#bleSort').value;
 const a=[...bleDevices].sort((x,y)=>mode==='name'?(x.name||'').localeCompare(y.name||''):mode==='address'?x.address.localeCompare(y.address):y.rssi-x.rssi);
 const fresh=a.filter(x=>x.is_new).length;
 document.querySelector('#bleSummary').innerHTML=a.length?`<p><span class="pill">${a.length} BLE devices</span><span class="pill">${fresh} new vs baseline</span></p>`:'';
 document.querySelector('#bleResults').innerHTML=a.length?`<table><tr><th>Name</th><th>Address</th><th>Type</th><th>RSSI</th><th>Connectable</th><th>Services</th><th>Manufacturer data</th><th>Seen</th><th></th></tr>${a.map(x=>`<tr><td>${esc(x.name||'(unnamed)')}</td><td><code>${esc(x.address)}</code></td><td>${esc(x.address_type)}</td><td>${x.rssi}</td><td>${x.connectable?'Yes':'No'}</td><td>${esc(x.services||'—')}</td><td><code>${esc(x.manufacturer||'—')}</code></td><td>${x.observations}×, ${x.first_seen_ms}–${x.last_seen_ms} ms</td><td class="new">${x.is_new?'NEW':''}</td></tr>`).join('')}</table>`:'<p>No BLE advertisements found. Try moving closer and scanning again.</p>';
}
async function setBleBaseline(){
 if(!bleDevices.length){alert('Run a BLE scan first.');setNova('warning','Run a Bluetooth scan before setting the baseline.',4000);return}
 const r=await fetch('/api/ble-baseline',{method:'POST'});alert(await r.text());if(r.ok){bleDevices.forEach(x=>x.is_new=false);renderBle()}setNova(r.ok?'success':'warning',r.ok?'Bluetooth baseline updated.':'The baseline could not be updated.',4000);
}
async function saveBleSession(){
 if(!bleDevices.length){document.querySelector('#bleSaveStatus').textContent='Run a BLE scan first.';setNova('warning','Run a Bluetooth scan before saving a session.',4000);return}
 const name=document.querySelector('#bleSession').value.trim()||'BLE-Survey';
 const body=new URLSearchParams({name,timestamp:new Date().toISOString()});
 const r=await fetch('/api/ble-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
 document.querySelector('#bleSaveStatus').textContent=await r.text();if(r.ok)await loadFiles();setNova(r.ok?'success':'warning',r.ok?'Bluetooth survey saved to the file vault.':'The Bluetooth survey could not be saved.',4500);
}
async function deleteFile(encoded){
 if(!confirm('Delete this file?'))return;
 const r=await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encoded});await loadFiles();setNova(r.ok?'success':'warning',r.ok?'File deleted.':'The file could not be deleted.',3500);
}
document.addEventListener('visibilitychange',()=>{if(document.hidden&&walkTimer)stopWalkTest('Walk test paused because the browser tab was hidden.')});
load();
setInterval(loadStatus,5000);
</script></body></html>
)HTML";

String jsonEscape(const String &value) {
    String out;
    out.reserve(value.length() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += F("\\\\"); break;
            case '"': out += F("\\\""); break;
            case '\n': out += F("\\n"); break;
            case '\r': out += F("\\r"); break;
            case '\t': out += F("\\t"); break;
            default:
                if (static_cast<uint8_t>(c) >= 0x20) out += c;
                break;
        }
    }
    return out;
}

String csvEscape(const String &value) {
    String out = value;
    out.replace("\"", "\"\"");
    return '"' + out + '"';
}

String safeName(String value) {
    value.trim();
    String out;
    out.reserve(32);
    for (char c : value) {
        if (isalnum(static_cast<unsigned char>(c))) out += c;
        else if ((c == '-' || c == '_') && !out.isEmpty()) out += c;
        else if (c == ' ' && !out.isEmpty() && !out.endsWith("-")) out += '-';
        if (out.length() >= 32) break;
    }
    while (out.endsWith("-") || out.endsWith("_")) out.remove(out.length() - 1);
    return out.isEmpty() ? String("Survey") : out;
}

String safeUploadedFilename(String value) {
    const int slash = std::max(value.lastIndexOf('/'), value.lastIndexOf('\\'));
    if (slash >= 0) value = value.substring(slash + 1);
    value.trim();
    String out;
    out.reserve(64);
    for (char c : value) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
        else if (c == '.' && !out.isEmpty() && !out.endsWith(".")) out += c;
        else if (c == ' ' && !out.isEmpty() && !out.endsWith("-")) out += '-';
        if (out.length() >= 64) break;
    }
    while (out.endsWith(".") || out.endsWith("-") || out.endsWith("_")) out.remove(out.length() - 1);
    return out;
}

String timestampForFilename(String value) {
    String out;
    out.reserve(20);
    for (char c : value) {
        if (isdigit(static_cast<unsigned char>(c))) out += c;
        if (out.length() >= 14) break;
    }
    return out.length() >= 8 ? out : String(millis());
}

String securityName(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return F("Open");
        case WIFI_AUTH_WEP: return F("WEP");
        case WIFI_AUTH_WPA_PSK: return F("WPA");
        case WIFI_AUTH_WPA2_PSK: return F("WPA2");
        case WIFI_AUTH_WPA_WPA2_PSK: return F("WPA/WPA2");
        case WIFI_AUTH_WPA2_ENTERPRISE: return F("WPA2 Enterprise");
        case WIFI_AUTH_WPA3_PSK: return F("WPA3");
        case WIFI_AUTH_WPA2_WPA3_PSK: return F("WPA2/WPA3");
        default: return F("Other");
    }
}

String bleAddressTypeName(uint8_t type) {
    switch (type) {
        case 0: return F("Public");
        case 1: return F("Random");
        case 2: return F("Public identity");
        case 3: return F("Random identity");
        default: return "Type " + String(type);
    }
}

String bytesToHex(const std::string &data, size_t maximumBytes = 24) {
    static constexpr char hex[] = "0123456789ABCDEF";
    String out;
    const size_t count = std::min(data.size(), maximumBytes);
    out.reserve(count * 2 + 3);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t value = static_cast<uint8_t>(data[i]);
        out += hex[value >> 4];
        out += hex[value & 0x0F];
    }
    if (data.size() > maximumBytes) out += F("...");
    return out;
}

String advertisedServices(const NimBLEAdvertisedDevice *device) {
    String out;
    const uint8_t count = device->getServiceUUIDCount();
    for (uint8_t i = 0; i < count && i < 8; ++i) {
        if (!out.isEmpty()) out += F(", ");
        out += device->getServiceUUID(i).toString().c_str();
    }
    if (count > 8) out += F(", ...");
    return out;
}

String advertisedManufacturer(const NimBLEAdvertisedDevice *device) {
    if (!device->haveManufacturerData()) return String();
    const std::string data = device->getManufacturerData();
    String out;
    if (data.size() >= 2) {
        const uint16_t company = static_cast<uint8_t>(data[0]) |
                                 (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
        char label[10];
        snprintf(label, sizeof(label), "0x%04X ", company);
        out = label;
    }
    out += bytesToHex(data);
    return out;
}

class PocketLabBleScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *device) override {
        if (!bleResultsMutex || xSemaphoreTake(bleResultsMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

        const String address = device->getAddress().toString().c_str();
        const uint32_t seenAt = millis() - bleScanStartedAt;
        for (auto &item : bleResults) {
            if (item.address.equalsIgnoreCase(address)) {
                item.rssi = device->getRSSI();
                item.lastSeenMs = seenAt;
                ++item.observations;
                if (item.name.isEmpty() && device->haveName()) item.name = device->getName().c_str();
                if (item.services.isEmpty()) item.services = advertisedServices(device);
                if (item.manufacturer.isEmpty()) item.manufacturer = advertisedManufacturer(device);
                xSemaphoreGive(bleResultsMutex);
                return;
            }
        }

        BleObservation observation{
            device->haveName() ? String(device->getName().c_str()) : String(),
            address,
            bleAddressTypeName(device->getAddressType()),
            advertisedServices(device),
            advertisedManufacturer(device),
            device->getRSSI(),
            device->isConnectable(),
            false,
            seenAt,
            seenAt,
            1,
        };
        bleResults.push_back(std::move(observation));
        xSemaphoreGive(bleResultsMutex);
    }
};

PocketLabBleScanCallbacks bleScanCallbacks;

bool keyboardNameLooksRelevant(String name) {
    name.toLowerCase();
    return name.indexOf("keyboard") >= 0 || name.indexOf("keys") >= 0 ||
           name.indexOf("k380") >= 0;
}

class PocketLabKeyboardScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *device) override {
        if (keyboardCandidateReady) return;
        const String name = device->haveName() ? String(device->getName().c_str()) : String();
        if (!device->isAdvertisingService(NimBLEUUID("1812")) && !keyboardNameLooksRelevant(name)) return;
        keyboardCandidate = device;
        keyboardName = name.isEmpty() ? String(device->getAddress().toString().c_str()) : name;
        keyboardCandidateReady = true;
        NimBLEDevice::getScan()->stop();
    }

    void onScanEnd(const NimBLEScanResults &results, int reason) override {
        if (!keyboardCandidateReady && keyboardMode) {
            keyboardState = KeyboardState::Failed;
            keyboardMessage = F("No HID keyboard found");
            keyboardScreenDirty = true;
        }
    }
};

class PocketLabKeyboardClientCallbacks : public NimBLEClientCallbacks {
    void onConnectFail(NimBLEClient *client, int reason) override {
        keyboardState = KeyboardState::Failed;
        keyboardMessage = "Connection failed (" + String(reason) + ')';
        keyboardScreenDirty = true;
    }

    void onDisconnect(NimBLEClient *client, int reason) override {
        keyboardDisconnected = true;
        keyboardState = KeyboardState::Failed;
        keyboardMessage = "Keyboard disconnected (" + String(reason) + ')';
        keyboardScreenDirty = true;
    }

    uint32_t onPassKeyDisplay(NimBLEConnInfo &connInfo) override {
        keyboardState = KeyboardState::Pairing;
        keyboardScreenDirty = true;
        drawKeyboardScreen();
        return keyboardPasskey;
    }

    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
        if (!connInfo.isEncrypted()) {
            keyboardState = KeyboardState::Failed;
            keyboardMessage = F("Passkey authentication failed");
        } else {
            keyboardMessage = connInfo.isBonded() ? F("Paired and bonded") : F("Encrypted connection");
        }
        keyboardScreenDirty = true;
    }
};

PocketLabKeyboardScanCallbacks keyboardScanCallbacks;
PocketLabKeyboardClientCallbacks keyboardClientCallbacks;

void setKeyboardLastKey(const char *label) {
    portENTER_CRITICAL(&keyboardMux);
    strncpy(keyboardLastKey, label, sizeof(keyboardLastKey) - 1);
    keyboardLastKey[sizeof(keyboardLastKey) - 1] = '\0';
    portEXIT_CRITICAL(&keyboardMux);
    keyboardScreenDirty = true;
}

char keyboardUsageToAscii(uint8_t usage, bool shifted) {
    if (usage >= 0x04 && usage <= 0x1D) {
        const bool upper = shifted;
        return (upper ? 'A' : 'a') + usage - 0x04;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static const char plain[] = "1234567890";
        static const char upper[] = "!@#$%^&*()";
        return shifted ? upper[usage - 0x1E] : plain[usage - 0x1E];
    }
    switch (usage) {
        case 0x2C: return ' ';
        case 0x2D: return shifted ? '_' : '-';
        case 0x2E: return shifted ? '+' : '=';
        case 0x2F: return shifted ? '{' : '[';
        case 0x30: return shifted ? '}' : ']';
        case 0x31: return shifted ? '|' : '\\';
        case 0x33: return shifted ? ':' : ';';
        case 0x34: return shifted ? '"' : '\'';
        case 0x35: return shifted ? '~' : '`';
        case 0x36: return shifted ? '<' : ',';
        case 0x37: return shifted ? '>' : '.';
        case 0x38: return shifted ? '?' : '/';
        default: return 0;
    }
}

void appendKeyboardCharacter(char value) {
    portENTER_CRITICAL(&keyboardMux);
    size_t length = strlen(keyboardText);
    if (value == '\b') {
        if (length) keyboardText[length - 1] = '\0';
    } else {
        if (length >= sizeof(keyboardText) - 1) {
            memmove(keyboardText, keyboardText + 1, sizeof(keyboardText) - 2);
            length = sizeof(keyboardText) - 2;
        }
        keyboardText[length] = value;
        keyboardText[length + 1] = '\0';
    }
    portEXIT_CRITICAL(&keyboardMux);
    keyboardScreenDirty = true;
}

void keyboardNotification(
    NimBLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t length,
    bool isNotify
) {
    char raw[52];
    size_t position = snprintf(raw, sizeof(raw), "%uB:", static_cast<unsigned>(length));
    for (size_t i = 0; i < length && i < 12 && position + 4 < sizeof(raw); ++i) {
        position += snprintf(raw + position, sizeof(raw) - position, " %02X", data[i]);
    }
    bool hasNonZeroData = false;
    for (size_t i = 0; i < length; ++i) if (data[i] != 0) hasNonZeroData = true;
    portENTER_CRITICAL(&keyboardMux);
    if (hasNonZeroData) {
        strncpy(keyboardRawReport, raw, sizeof(keyboardRawReport) - 1);
        keyboardRawReport[sizeof(keyboardRawReport) - 1] = '\0';
    }
    ++keyboardReportCount;
    portEXIT_CRITICAL(&keyboardMux);
    keyboardScreenDirty = true;

    // Standard keyboards commonly send either 7 bytes (five-key array) or
    // 8 bytes (six-key array). A ninth byte is usually a leading report ID.
    if (length < 7 || length > 9) return;
    const size_t offset = length == 9 ? 1 : 0;
    const uint8_t modifiers = data[offset];
    const bool shifted = (modifiers & 0x22) != 0;
    for (size_t i = offset + 2; i < length; ++i) {
        const uint8_t usage = data[i];
        if (usage == 0 || usage <= 3) continue;
        bool alreadyDown = false;
        for (uint8_t old : keyboardPreviousKeys) if (old == usage) alreadyDown = true;
        if (alreadyDown) continue;

        char label[24];
        const char ascii = keyboardUsageToAscii(usage, shifted);
        if (ascii) {
            appendKeyboardCharacter(ascii);
            snprintf(label, sizeof(label), "%c", ascii);
        } else {
            switch (usage) {
                case 0x28: appendKeyboardCharacter('|'); strcpy(label, "Enter"); break;
                case 0x29: strcpy(label, "Escape / exit"); keyboardExitRequested = true; break;
                case 0x2A: appendKeyboardCharacter('\b'); strcpy(label, "Backspace"); break;
                case 0x2B: appendKeyboardCharacter(' '); strcpy(label, "Tab"); break;
                case 0x4F: strcpy(label, "Right arrow"); break;
                case 0x50: strcpy(label, "Left arrow"); break;
                case 0x51: strcpy(label, "Down arrow"); break;
                case 0x52: strcpy(label, "Up arrow"); break;
                default: snprintf(label, sizeof(label), "HID 0x%02X", usage); break;
            }
        }
        setKeyboardLastKey(label);
    }
    memset(keyboardPreviousKeys, 0, sizeof(keyboardPreviousKeys));
    memcpy(
        keyboardPreviousKeys,
        data + offset + 2,
        std::min(sizeof(keyboardPreviousKeys), length - offset - 2)
    );
}

bool safePath(const String &path) {
    return path.startsWith("/") && path.indexOf("..") < 0 && path.length() <= 96;
}

bool managedPath(const String &path) {
    return safePath(path) && (path.startsWith("/logs/") || path.startsWith("/files/"));
}

int estimateLevelFromVoltage(int millivolts) {
    struct CurvePoint {
        int millivolts;
        int percent;
    };
    static constexpr CurvePoint curve[] = {
        {3300, 0}, {3400, 3}, {3500, 8}, {3600, 15}, {3700, 30},
        {3800, 50}, {3900, 70}, {4000, 85}, {4100, 95}, {4200, 100},
    };

    if (millivolts <= curve[0].millivolts) return 0;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (millivolts <= curve[i].millivolts) {
            const auto &low = curve[i - 1];
            const auto &high = curve[i];
            return low.percent +
                   (millivolts - low.millivolts) * (high.percent - low.percent) /
                       (high.millivolts - low.millivolts);
        }
    }
    return 100;
}

int medianBatteryVoltage() {
    if (battery.sampleCount == 0) return -1;
    int16_t sorted[kBatteryWindow];
    for (size_t i = 0; i < battery.sampleCount; ++i) sorted[i] = battery.samples[i];
    std::sort(sorted, sorted + battery.sampleCount);
    return sorted[battery.sampleCount / 2];
}

void refreshBattery(bool force = false) {
    const uint32_t now = millis();
    if (!force && battery.updatedAt != 0 && now - battery.updatedAt < kBatterySampleMs) return;

    const int16_t measured = M5.Power.getBatteryVoltage();
    const int charging = static_cast<int>(M5.Power.isCharging());
    if (charging >= 0 && charging <= 2) battery.charging = charging;

    if (measured >= 2800 && measured <= 4500) {
        battery.rawMillivolts = measured;
        if (battery.minimumMillivolts < 0 || measured < battery.minimumMillivolts) {
            battery.minimumMillivolts = measured;
        }
        battery.samples[battery.sampleIndex] = measured;
        battery.sampleIndex = (battery.sampleIndex + 1) % kBatteryWindow;
        if (battery.sampleCount < kBatteryWindow) ++battery.sampleCount;

        const int median = medianBatteryVoltage();
        if (battery.filteredMillivolts <= 0) battery.filteredMillivolts = median;
        else battery.filteredMillivolts += 0.18f * (median - battery.filteredMillivolts);
        battery.millivolts = static_cast<int16_t>(battery.filteredMillivolts + 0.5f);

        const int target = estimateLevelFromVoltage(battery.millivolts);
        if (battery.level < 0) {
            battery.level = target;
            battery.lastLevelStepAt = now;
        } else if (battery.millivolts <= 3325 && battery.charging != 1) {
            // A sustained critically-low filtered voltage should not be hidden by rate limiting.
            if (battery.level > 2) battery.level = 2;
        } else {
            const uint32_t stepInterval =
                battery.charging == 1 ? kBatteryStepChargingMs : kBatteryStepDischargingMs;
            if (now - battery.lastLevelStepAt >= stepInterval) {
                if (battery.charging == 1 && target >= battery.level + 2) {
                    ++battery.level;
                    battery.lastLevelStepAt = now;
                } else if (battery.charging != 1 && target <= battery.level - 2) {
                    --battery.level;
                    battery.lastLevelStepAt = now;
                }
            }
        }
    }
    battery.updatedAt = now;
}

void initializeBatteryEstimator() {
    for (size_t i = 0; i < kBatteryWindow; ++i) {
        refreshBattery(true);
        delay(25);
    }
    if (battery.millivolts > 0) battery.level = estimateLevelFromVoltage(battery.millivolts);
    battery.lastLevelStepAt = millis();
}

String powerState() {
    if (battery.charging == 1) return F("Charging");
    if (battery.charging == 0) return F("On battery");
    return F("Power state unknown");
}

String resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return F("Power on");
        case ESP_RST_EXT: return F("External reset");
        case ESP_RST_SW: return F("Software restart");
        case ESP_RST_PANIC: return F("Crash/panic");
        case ESP_RST_INT_WDT: return F("Interrupt watchdog");
        case ESP_RST_TASK_WDT: return F("Task watchdog");
        case ESP_RST_WDT: return F("Watchdog");
        case ESP_RST_DEEPSLEEP: return F("Deep-sleep wake");
        case ESP_RST_BROWNOUT: return F("Brownout");
        default: return "Other (" + String(static_cast<int>(reason)) + ')';
    }
}

String displayStateText() {
    if (displayMode == DisplayMode::Off) return F("Off");
    if (displayMode == DisplayMode::Dim) return F("Dimmed");
    return F("Normal");
}

String powerProfileText() {
    return powerProfile == PowerProfile::Eco ? String("eco") : String("performance");
}

void applyPowerProfile() {
    setCpuFrequencyMhz(powerProfile == PowerProfile::Eco ? 160 : 240);
    if (WiFi.getMode() != WIFI_MODE_NULL) {
        WiFi.setTxPower(powerProfile == PowerProfile::Eco ? WIFI_POWER_8_5dBm : WIFI_POWER_19_5dBm);
        WiFi.setSleep(powerProfile == PowerProfile::Eco);
    }
}

void setDisplayMode(DisplayMode mode) {
    displayMode = mode;
    if (mode == DisplayMode::Off) M5.Display.setBrightness(0);
    else if (mode == DisplayMode::Dim) M5.Display.setBrightness(kDimBrightness);
    else M5.Display.setBrightness(kNormalBrightness);
    if (mode == DisplayMode::Normal) lastDisplayActivity = millis();
}

bool lowBattery() {
    return battery.charging != 1 &&
           ((battery.level >= 0 && battery.level <= 15) ||
            (battery.millivolts > 0 && battery.millivolts <= 3500));
}

String batteryVoltageText() {
    if (battery.millivolts <= 0) return F("--.--V");
    char value[8];
    snprintf(value, sizeof(value), "%.2fV", battery.millivolts / 1000.0f);
    return String(value);
}

void drawBatteryBadge() {
    refreshBattery();
    auto &d = M5.Display;
    d.fillRect(158, 0, 82, 20, TFT_BLACK);
    d.setTextSize(1);
    d.setTextColor(lowBattery() ? TFT_RED : (battery.charging == 1 ? TFT_GREEN : TFT_WHITE), TFT_BLACK);
    d.setCursor(162, 6);
    if (battery.level >= 0) {
        d.print('~');
        d.print(battery.level);
        d.print('%');
        if (battery.charging == 1) d.print('+');
        d.print(' ');
        d.print(batteryVoltageText());
    } else {
        d.print("Battery ?");
    }
}

void drawStoppedBattery() {
    refreshBattery();
    auto &d = M5.Display;
    d.fillRect(0, 34, 240, 62, TFT_BLACK);
    d.setCursor(8, 38);
    d.setTextSize(2);
    d.setTextColor(lowBattery() ? TFT_RED : TFT_YELLOW, TFT_BLACK);
    if (battery.level >= 0) {
        d.print("Battery est: ");
        d.print(battery.level);
        d.println('%');
    } else {
        d.println("Battery: unknown");
    }
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(8, 67);
    d.print(batteryVoltageText());
    d.print("  ");
    d.print(powerState());
}

bool baselineContains(const String &bssid) {
    File file = LittleFS.open("/wifi-baseline.txt", FILE_READ);
    if (!file) return true; // No baseline yet: avoid calling every network "new".
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.equalsIgnoreCase(bssid)) {
            file.close();
            return true;
        }
    }
    file.close();
    return false;
}

void sendStatus() {
    refreshBattery();
    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    String body = F("{\"firmware_version\":\"");
    body += kFirmwareVersion;
    body += F("\",\"ip\":\"");
    body += WiFi.softAPIP().toString();
    body += F("\",\"heap_kb\":");
    body += ESP.getFreeHeap() / 1024;
    body += F(",\"psram_kb\":");
    body += ESP.getFreePsram() / 1024;
    body += F(",\"fs_free_kb\":");
    body += (total - used) / 1024;
    body += F(",\"fs_used_kb\":");
    body += used / 1024;
    body += F(",\"fs_total_kb\":");
    body += total / 1024;
    body += F(",\"battery_percent\":");
    body += battery.level;
    body += F(",\"battery_mv\":");
    body += battery.millivolts;
    body += F(",\"minimum_battery_mv\":");
    body += battery.minimumMillivolts;
    body += F(",\"power_state\":\"");
    body += powerState();
    body += F("\",\"low_battery\":");
    body += lowBattery() ? F("true") : F("false");
    body += F(",\"display_state\":\"");
    body += displayStateText();
    body += F("\",\"reset_reason\":\"");
    body += jsonEscape(resetReason);
    body += F("\",\"power_profile\":\"");
    body += powerProfileText();
    body += F("\",\"cpu_mhz\":");
    body += getCpuFrequencyMhz();
    body += F(",\"ap_clients\":");
    body += portalRunning ? static_cast<int>(WiFi.softAPgetStationNum()) : 0;
    body += F(",\"portal_timeout_minutes\":");
    body += portalTimeoutMs == 0 ? 0 : portalTimeoutMs / 60000;
    body += F(",\"voltage_rate_mv_per_hour\":");
    if (millis() >= 60000 && bootBatteryMillivolts > 0 && battery.millivolts > 0) {
        const int32_t rate =
            static_cast<int32_t>(battery.millivolts - bootBatteryMillivolts) * 3600000LL / millis();
        body += rate;
    } else {
        body += F("null");
    }
    body += '}';
    server.send(200, "application/json", body);
}

void appendFileList(File &directory, String &body, bool &first) {
    File file = directory.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            appendFileList(file, body, first);
        } else if (managedPath(file.path())) {
            if (!first) body += ',';
            first = false;
            body += F("{\"name\":\"");
            body += jsonEscape(file.path());
            body += F("\",\"size\":");
            body += file.size();
            body += F(",\"kind\":\"");
            body += String(file.path()).startsWith("/files/") ? F("upload") : F("report");
            body += '"';
            body += '}';
        }
        file = directory.openNextFile();
    }
}

void sendFiles() {
    String body = "[";
    File root = LittleFS.open("/");
    bool first = true;
    appendFileList(root, body, first);
    body += ']';
    server.send(200, "application/json", body);
}

void downloadFile() {
    if (!server.hasArg("path") || !managedPath(server.arg("path"))) {
        server.send(400, "text/plain", "Invalid path");
        return;
    }
    const String path = server.arg("path");
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + String(file.name()) + "\"");
    server.streamFile(file, "application/octet-stream");
    file.close();
}

void deleteFile() {
    if (!server.hasArg("path") || !managedPath(server.arg("path"))) {
        server.send(400, "text/plain", "Invalid path");
        return;
    }
    if (LittleFS.remove(server.arg("path"))) server.send(200, "text/plain", "Deleted");
    else server.send(404, "text/plain", "File not found");
}

void failActiveUpload(const String &message, int status) {
    activeUploadError = message;
    activeUploadStatus = status;
    if (activeUploadFile) activeUploadFile.close();
    if (activeUploadCreated && !activeUploadPath.isEmpty()) LittleFS.remove(activeUploadPath);
    activeUploadCreated = false;
}

void handleFileUploadData() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        if (activeUploadFile) activeUploadFile.close();
        activeUploadPath = String();
        activeUploadError = String();
        activeUploadCreated = false;
        activeUploadStatus = 201;
        activeUploadCapacity = 0;

        const String filename = safeUploadedFilename(upload.filename);
        if (filename.isEmpty()) {
            failActiveUpload("The filename has no supported characters.", 400);
            return;
        }
        if (!LittleFS.exists("/files") && !LittleFS.mkdir("/files")) {
            failActiveUpload("Could not create the file vault.", 500);
            return;
        }
        activeUploadPath = "/files/" + filename;
        if (LittleFS.exists(activeUploadPath)) {
            failActiveUpload("A file with that name already exists. Delete it first or rename it on your computer.", 409);
            return;
        }
        const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
        if (freeBytes <= kFilesystemReserveBytes) {
            failActiveUpload("PocketLab storage is full.", 507);
            return;
        }
        activeUploadCapacity = std::min(kMaximumUploadBytes, freeBytes - kFilesystemReserveBytes);
        activeUploadFile = LittleFS.open(activeUploadPath, FILE_WRITE);
        if (!activeUploadFile) {
            failActiveUpload("Could not open the destination file.", 500);
            return;
        }
        activeUploadCreated = true;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!activeUploadError.isEmpty()) return;
        const size_t projectedSize = upload.totalSize + upload.currentSize;
        if (projectedSize > kMaximumUploadBytes) {
            failActiveUpload("The upload exceeded the 1 MB limit.", 413);
            return;
        }
        if (projectedSize > activeUploadCapacity) {
            failActiveUpload("Not enough free storage while preserving the 64 KB safety reserve.", 507);
            return;
        }
        if (activeUploadFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
            failActiveUpload("The file could not be completely written.", 507);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (activeUploadFile) activeUploadFile.close();
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        failActiveUpload("The upload was interrupted.", 400);
    }
}

void finishFileUpload() {
    if (!activeUploadError.isEmpty()) {
        server.send(activeUploadStatus, "text/plain", activeUploadError);
        return;
    }
    if (activeUploadPath.isEmpty() || !LittleFS.exists(activeUploadPath)) {
        server.send(400, "text/plain", "No file was received.");
        return;
    }
    File file = LittleFS.open(activeUploadPath, FILE_READ);
    const size_t size = file ? file.size() : 0;
    if (file) file.close();
    server.send(201, "text/plain", "Uploaded " + activeUploadPath.substring(7) + " (" + String(size) + " bytes).");
}

void setBaseline() {
    if (surveyResults.empty()) {
        server.send(409, "text/plain", "Run a scan first.");
        return;
    }
    File file = LittleFS.open("/wifi-baseline.txt", FILE_WRITE);
    if (!file) {
        server.send(500, "text/plain", "Could not save baseline.");
        return;
    }
    for (const auto &network : surveyResults) file.println(network.bssid);
    file.close();
    for (auto &network : surveyResults) network.isNew = false;
    server.send(200, "text/plain", "Baseline saved. Future scans will flag new BSSIDs.");
}

void saveSurvey() {
    if (surveyResults.empty()) {
        server.send(409, "text/plain", "Run a scan first.");
        return;
    }

    const String session = safeName(server.arg("name"));
    const String timestamp = server.arg("timestamp");
    const String stem = "/logs/" + timestampForFilename(timestamp) + "-" + session;
    if (!LittleFS.exists("/logs") && !LittleFS.mkdir("/logs")) {
        server.send(500, "text/plain", "Could not create the logs folder.");
        return;
    }

    File csv = LittleFS.open(stem + ".csv", FILE_WRITE);
    File json = LittleFS.open(stem + ".json", FILE_WRITE);
    if (!csv || !json) {
        if (csv) csv.close();
        if (json) json.close();
        server.send(507, "text/plain", "Not enough storage for this survey.");
        return;
    }

    csv.println("timestamp,session,ssid,bssid,channel,rssi_dbm,security,new_vs_baseline");
    json.print(F("{\"timestamp\":\""));
    json.print(jsonEscape(timestamp));
    json.print(F("\",\"session\":\""));
    json.print(jsonEscape(session));
    json.print(F("\",\"networks\":["));

    bool first = true;
    for (const auto &network : surveyResults) {
        csv.print(csvEscape(timestamp));
        csv.print(',');
        csv.print(csvEscape(session));
        csv.print(',');
        csv.print(csvEscape(network.ssid));
        csv.print(',');
        csv.print(csvEscape(network.bssid));
        csv.printf(",%ld,%ld,", static_cast<long>(network.channel), static_cast<long>(network.rssi));
        csv.print(csvEscape(network.security));
        csv.printf(",%s\n", network.isNew ? "true" : "false");

        if (!first) json.print(',');
        first = false;
        json.print(F("{\"ssid\":\""));
        json.print(jsonEscape(network.ssid));
        json.print(F("\",\"bssid\":\""));
        json.print(jsonEscape(network.bssid));
        json.printf(
            "\",\"channel\":%ld,\"rssi\":%ld,\"security\":\"",
            static_cast<long>(network.channel),
            static_cast<long>(network.rssi)
        );
        json.print(jsonEscape(network.security));
        json.printf("\",\"is_new\":%s}", network.isNew ? "true" : "false");
    }
    json.println(F("]}"));
    csv.close();
    json.close();
    server.send(201, "text/plain", "Saved " + session + " as CSV and JSON.");
}

bool bleBaselineContains(const String &address) {
    File file = LittleFS.open("/ble-baseline.txt", FILE_READ);
    if (!file) return true; // No baseline yet: avoid calling every address "new".
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.equalsIgnoreCase(address)) {
            file.close();
            return true;
        }
    }
    file.close();
    return false;
}

String bleResultsJson() {
    String body = "[";
    for (size_t i = 0; i < bleResults.size(); ++i) {
        const auto &item = bleResults[i];
        if (i) body += ',';
        body += F("{\"name\":\"");
        body += jsonEscape(item.name);
        body += F("\",\"address\":\"");
        body += jsonEscape(item.address);
        body += F("\",\"address_type\":\"");
        body += jsonEscape(item.addressType);
        body += F("\",\"rssi\":");
        body += item.rssi;
        body += F(",\"connectable\":");
        body += item.connectable ? F("true") : F("false");
        body += F(",\"services\":\"");
        body += jsonEscape(item.services);
        body += F("\",\"manufacturer\":\"");
        body += jsonEscape(item.manufacturer);
        body += F("\",\"observations\":");
        body += item.observations;
        body += F(",\"first_seen_ms\":");
        body += item.firstSeenMs;
        body += F(",\"last_seen_ms\":");
        body += item.lastSeenMs;
        body += F(",\"is_new\":");
        body += item.isNew ? F("true") : F("false");
        body += '}';
    }
    body += ']';
    return body;
}

void setBleBaseline() {
    if (bleResults.empty()) {
        server.send(409, "text/plain", "Run a BLE scan first.");
        return;
    }
    File file = LittleFS.open("/ble-baseline.txt", FILE_WRITE);
    if (!file) {
        server.send(500, "text/plain", "Could not save BLE baseline.");
        return;
    }
    for (auto &item : bleResults) {
        file.println(item.address);
        item.isNew = false;
    }
    file.close();
    server.send(200, "text/plain", "BLE baseline saved. Future scans will flag new addresses.");
}

void saveBleSurvey() {
    if (bleResults.empty()) {
        server.send(409, "text/plain", "Run a BLE scan first.");
        return;
    }

    const String session = safeName(server.arg("name"));
    const String timestamp = server.arg("timestamp");
    const String stem = "/logs/" + timestampForFilename(timestamp) + "-BLE-" + session;
    if (!LittleFS.exists("/logs") && !LittleFS.mkdir("/logs")) {
        server.send(500, "text/plain", "Could not create the logs folder.");
        return;
    }

    File csv = LittleFS.open(stem + ".csv", FILE_WRITE);
    File json = LittleFS.open(stem + ".json", FILE_WRITE);
    if (!csv || !json) {
        if (csv) csv.close();
        if (json) json.close();
        server.send(507, "text/plain", "Not enough storage for this BLE survey.");
        return;
    }

    csv.println("timestamp,session,name,address,address_type,rssi_dbm,connectable,services,manufacturer_data,observations,first_seen_ms,last_seen_ms,new_vs_baseline");
    json.print(F("{\"timestamp\":\""));
    json.print(jsonEscape(timestamp));
    json.print(F("\",\"session\":\""));
    json.print(jsonEscape(session));
    json.print(F("\",\"warning\":\"Randomized BLE addresses can change and may not identify a physical device.\",\"devices\":"));
    json.print(bleResultsJson());
    json.println('}');

    for (const auto &item : bleResults) {
        csv.print(csvEscape(timestamp));
        csv.print(',');
        csv.print(csvEscape(session));
        csv.print(',');
        csv.print(csvEscape(item.name));
        csv.print(',');
        csv.print(csvEscape(item.address));
        csv.print(',');
        csv.print(csvEscape(item.addressType));
        csv.printf(",%ld,%s,", static_cast<long>(item.rssi), item.connectable ? "true" : "false");
        csv.print(csvEscape(item.services));
        csv.print(',');
        csv.print(csvEscape(item.manufacturer));
        csv.printf(",%lu,%lu,%lu,%s\n",
                   static_cast<unsigned long>(item.observations),
                   static_cast<unsigned long>(item.firstSeenMs),
                   static_cast<unsigned long>(item.lastSeenMs),
                   item.isNew ? "true" : "false");
    }
    csv.close();
    json.close();
    server.send(201, "text/plain", "Saved " + session + " BLE inventory as CSV and JSON.");
}

void drawBleScanningScreen() {
    if (displayMode == DisplayMode::Off) return;
    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(8, 12);
    d.println("BLE inventory");
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(8, 46);
    d.println("Scanning...");
    d.setTextSize(1);
    d.setCursor(8, 82);
    d.println("Listening for 10 seconds");
    d.setCursor(8, 101);
    d.println("Bluetooth turns off afterward");
}

void scanBle() {
    if (bleScanning) {
        server.send(409, "text/plain", "A BLE scan is already running.");
        return;
    }
    if (!bleResultsMutex) {
        server.send(500, "text/plain", "BLE scanner is unavailable.");
        return;
    }

    bleScanning = true;
    bleResults.clear();
    bleResults.reserve(64);
    bleScanStartedAt = millis();
    drawBleScanningScreen();

    if (!NimBLEDevice::init("PocketLab")) {
        bleScanning = false;
        if (displayMode != DisplayMode::Off) drawPortalScreen();
        server.send(500, "text/plain", "Could not initialize Bluetooth.");
        return;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&bleScanCallbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(60);
    scan->getResults(10000, false);
    scan->stop();
    scan->clearResults();
    NimBLEDevice::deinit(true);

    for (auto &item : bleResults) item.isNew = !bleBaselineContains(item.address);
    const String response = bleResultsJson();
    bleScanning = false;
    if (displayMode != DisplayMode::Off) drawPortalScreen();
    server.send(200, "application/json", response);
}

void scanWifi() {
    // The station radio is needed only while surveying. Eco mode otherwise keeps
    // the less expensive AP-only configuration alive for the WebUI.
    if (powerProfile == PowerProfile::Eco && WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
        applyPowerProfile();
        delay(30);
    }
    const int count = WiFi.scanNetworks(false, true, false, 300);
    surveyResults.clear();
    if (count > 0) surveyResults.reserve(count);
    String body = "[";
    for (int i = 0; i < count; ++i) {
        SurveyNetwork network{
            WiFi.SSID(i),
            WiFi.BSSIDstr(i),
            securityName(WiFi.encryptionType(i)),
            WiFi.RSSI(i),
            WiFi.channel(i),
            false,
        };
        network.isNew = !baselineContains(network.bssid);
        surveyResults.push_back(network);
        if (i) body += ',';
        body += F("{\"ssid\":\"");
        body += jsonEscape(network.ssid);
        body += F("\",\"bssid\":\"");
        body += network.bssid;
        body += F("\",\"channel\":");
        body += network.channel;
        body += F(",\"rssi\":");
        body += network.rssi;
        body += F(",\"security\":\"");
        body += network.security;
        body += F("\",\"is_new\":");
        body += network.isNew ? F("true") : F("false");
        body += '}';
    }
    body += ']';
    WiFi.scanDelete();
    server.send(200, "application/json", body);
    if (powerProfile == PowerProfile::Eco) restoreEcoApAt = millis() + 250;
}

void drawPortalScreen() {
    portalQrVisible = false;
    refreshBattery(true);
    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(6, 3);
    d.println("PocketLab");

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(6, 23);
    d.println("JOIN WI-FI");
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(6, 33);
    d.println(apSsid);

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(6, 52);
    d.println("PASSWORD");
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(6, 62);
    d.println(apPassword);

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(6, 81);
    d.println("OPEN IN BROWSER");
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(6, 91);
    d.println("http://pocketlab");

    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(6, 119);
    d.println("M5 button: stop portal");
    drawBatteryBadge();
    lastBatteryDraw = millis();
}

void showSplashScreen() {
    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);
    if (!d.drawPng(kPocketLabSplashPng, kPocketLabSplashPngLength, 0, 0)) {
        d.setTextColor(TFT_CYAN, TFT_BLACK);
        d.setTextSize(3);
        d.setTextDatum(middle_center);
        d.drawString("POCKETLAB", 120, 58);
        d.setTextDatum(top_left);
    }

    d.fillRoundRect(187, 4, 49, 15, 4, 0x0008);
    d.drawRoundRect(187, 4, 49, 15, 4, TFT_CYAN);
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, 0x0008);
    d.setCursor(191, 8);
    d.print(kFirmwareVersion);
    delay(2500);
}

void drawPortalQrBattery() {
    refreshBattery();
    auto &d = M5.Display;
    d.fillRect(4, 101, 106, 14, TFT_BLACK);
    d.setTextSize(1);
    d.setTextColor(lowBattery() ? TFT_RED : (battery.charging == 1 ? TFT_GREEN : TFT_WHITE), TFT_BLACK);
    d.setCursor(6, 104);
    if (battery.level >= 0) {
        d.print('~');
        d.print(battery.level);
        d.print('%');
        if (battery.charging == 1) d.print('+');
        d.print(' ');
        d.print(batteryVoltageText());
    } else {
        d.print("Battery ?");
    }
}

void drawPortalQrScreen() {
    portalQrVisible = true;
    refreshBattery(true);
    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);

    d.setTextSize(2);
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.setCursor(6, 4);
    d.println("Wi-Fi QR");
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(6, 24);
    d.println("Scan to join");
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.setCursor(6, 39);
    d.println(apSsid);
    d.setCursor(6, 53);
    d.println(apPassword);

    // Version 4 leaves a reliable quiet zone around this short, standard
    // Wi-Fi payload while keeping the modules large enough for phone cameras.
    const String payload = "WIFI:T:WPA;S:" + apSsid + ";P:" + apPassword + ";H:false;;";
    d.qrcode(payload.c_str(), 115, 5, 120, 4, false);

    drawPortalQrBattery();
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(6, 121);
    d.println("Face: details");
    lastBatteryDraw = millis();
}

void drawKeyboardScreen() {
    if (!keyboardMode || displayMode == DisplayMode::Off) return;
    // Clear before taking the snapshot. If a report arrives while the LCD is
    // being painted it will set this flag again, preserving the next redraw.
    keyboardScreenDirty = false;
    char typed[65];
    char lastKey[24];
    char rawReport[52];
    uint32_t reportCount;
    portENTER_CRITICAL(&keyboardMux);
    memcpy(typed, keyboardText, sizeof(typed));
    memcpy(lastKey, keyboardLastKey, sizeof(lastKey));
    memcpy(rawReport, keyboardRawReport, sizeof(rawReport));
    reportCount = keyboardReportCount;
    portEXIT_CRITICAL(&keyboardMux);

    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(6, 4);
    d.println("BLE Keyboard");
    d.setTextSize(1);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(192, 21);
    d.println(kFirmwareVersion);

    if (keyboardState == KeyboardState::Scanning) {
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(6, 35);
        d.println("Scanning for a HID keyboard...");
        d.setCursor(6, 54);
        d.println("Hold an Easy-Switch key until");
        d.setCursor(6, 67);
        d.println("its light flashes quickly.");
    } else if (keyboardState == KeyboardState::Connecting) {
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(6, 35);
        d.println("Connecting to:");
        d.setTextColor(TFT_YELLOW, TFT_BLACK);
        d.setCursor(6, 51);
        d.println(keyboardName.substring(0, 36));
    } else if (keyboardState == KeyboardState::Pairing) {
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(6, 28);
        d.println("Type this code, then Enter:");
        char code[7];
        snprintf(code, sizeof(code), "%06lu", static_cast<unsigned long>(keyboardPasskey));
        d.setTextColor(TFT_YELLOW, TFT_BLACK);
        d.setTextSize(3);
        d.setCursor(54, 48);
        d.println(code);
        d.setTextSize(1);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(6, 92);
        d.println("Do not type the code on PocketLab.");
    } else if (keyboardState == KeyboardState::Connected) {
        d.setTextColor(TFT_GREEN, TFT_BLACK);
        d.setCursor(6, 27);
        d.print("Connected: ");
        d.println(keyboardName.substring(0, 24));
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(6, 45);
        d.print("Last: ");
        d.println(lastKey);
        d.setCursor(6, 61);
        d.print("RX: ");
        d.print(reportCount);
        d.print(" sub:");
        d.println(keyboardSubscribedCount);
        d.setCursor(171, 61);
        d.println(keyboardBootProtocol ? "Boot" : "Report");
        d.setCursor(6, 77);
        d.println("Typed text:");
        d.setTextColor(TFT_YELLOW, TFT_BLACK);
        d.setTextSize(2);
        d.setCursor(6, 90);
        String tail = String(typed);
        if (tail.length() > 19) tail = tail.substring(tail.length() - 19);
        d.println(tail);
        d.setTextSize(1);
        d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        d.setCursor(6, 108);
        String rawLine = "RX" + String(reportCount) + ' ' + String(rawReport);
        d.println(rawLine.substring(0, 38));
    } else if (keyboardState == KeyboardState::Failed) {
        d.setTextColor(TFT_RED, TFT_BLACK);
        d.setCursor(6, 32);
        d.println(keyboardMessage.substring(0, 38));
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(6, 55);
        d.println("Face button: scan again");
        d.setCursor(6, 70);
        d.println("Check Easy-Switch pairing mode.");
    }

    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(6, 119);
    d.println("M5 button / Esc: return to portal");
    drawBatteryBadge();
    keyboardLastScreenDraw = millis();
    lastBatteryDraw = millis();
}

void startKeyboardScan() {
    keyboardCandidate = nullptr;
    keyboardCandidateReady = false;
    keyboardDisconnected = false;
    keyboardState = KeyboardState::Scanning;
    keyboardMessage = String();
    keyboardScreenDirty = true;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    scan->setScanCallbacks(&keyboardScanCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->start(15000, false, true);
    drawKeyboardScreen();
}

void beginKeyboardMode() {
    keyboardModeRequested = false;
    if (portalRunning) stopPortal();
    keyboardMode = true;
    keyboardState = KeyboardState::Scanning;
    keyboardPasskey = 100000UL + (esp_random() % 900000UL);
    keyboardExitRequested = false;
    keyboardDisconnected = false;
    keyboardClient = nullptr;
    keyboardName = String();
    keyboardSubscribedCount = 0;
    keyboardBootProtocol = false;
    memset(keyboardPreviousKeys, 0, sizeof(keyboardPreviousKeys));
    portENTER_CRITICAL(&keyboardMux);
    keyboardText[0] = '\0';
    strcpy(keyboardLastKey, "None");
    strcpy(keyboardRawReport, "None");
    keyboardReportCount = 0;
    portEXIT_CRITICAL(&keyboardMux);
    setCpuFrequencyMhz(160);
    setDisplayMode(DisplayMode::Normal);

    if (!NimBLEDevice::init("PocketLab Keyboard Host")) {
        keyboardState = KeyboardState::Failed;
        keyboardMessage = F("Bluetooth initialization failed");
        drawKeyboardScreen();
        return;
    }
    keyboardBleReady = true;
    NimBLEDevice::setPower(3);
    NimBLEDevice::setSecurityAuth(true, true, false);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    startKeyboardScan();
}

void connectKeyboardCandidate() {
    keyboardCandidateReady = false;
    if (!keyboardCandidate || !keyboardMode) return;
    keyboardState = KeyboardState::Connecting;
    drawKeyboardScreen();

    keyboardClient = NimBLEDevice::createClient();
    if (!keyboardClient) {
        keyboardState = KeyboardState::Failed;
        keyboardMessage = F("Could not allocate BLE client");
        drawKeyboardScreen();
        return;
    }
    keyboardClient->setClientCallbacks(&keyboardClientCallbacks, false);
    keyboardClient->setConnectionParams(12, 24, 0, 400);
    keyboardClient->setConnectTimeout(10000);
    if (!keyboardClient->connect(keyboardCandidate)) {
        keyboardState = KeyboardState::Failed;
        keyboardMessage = F("Keyboard connection failed");
        drawKeyboardScreen();
        return;
    }
    if (!keyboardClient->secureConnection()) {
        keyboardState = KeyboardState::Failed;
        keyboardMessage = F("Passkey authentication failed");
        drawKeyboardScreen();
        return;
    }

    NimBLERemoteService *hid = keyboardClient->getService(NimBLEUUID("1812"));
    if (!hid) {
        keyboardState = KeyboardState::Failed;
        keyboardMessage = F("HID service not found");
        drawKeyboardScreen();
        return;
    }

    NimBLERemoteCharacteristic *protocol = hid->getCharacteristic(NimBLEUUID("2A4E"));
    NimBLERemoteCharacteristic *bootInput = hid->getCharacteristic(NimBLEUUID("2A22"));
    auto subscribeInput = [](NimBLERemoteCharacteristic *characteristic, bool notifications) {
        bool enabled = characteristic->subscribe(notifications, keyboardNotification, true);
        if (!enabled) enabled = characteristic->subscribe(notifications, keyboardNotification, false);
        return enabled;
    };

    size_t subscribed = 0;
    if (bootInput && bootInput->canNotify()) {
        if (protocol && (protocol->canWrite() || protocol->canWriteNoResponse())) {
            const uint8_t bootProtocol = 0;
            protocol->writeValue(&bootProtocol, 1, protocol->canWrite());
        }
        delay(150);
        if (subscribeInput(bootInput, true)) {
            subscribed = 1;
            keyboardBootProtocol = true;
        }
    }

    if (subscribed == 0) {
        if (protocol && (protocol->canWrite() || protocol->canWriteNoResponse())) {
            const uint8_t reportProtocol = 1;
            protocol->writeValue(&reportProtocol, 1, protocol->canWrite());
        }
        delay(150);
        for (NimBLERemoteCharacteristic *characteristic : hid->getCharacteristics(true)) {
            bool enabled = false;
            if (characteristic->canNotify()) enabled = subscribeInput(characteristic, true);
            else if (characteristic->canIndicate()) enabled = subscribeInput(characteristic, false);
            if (enabled) ++subscribed;
        }
    }
    if (subscribed == 0) {
        keyboardState = KeyboardState::Failed;
        keyboardMessage = F("No HID input reports found");
        drawKeyboardScreen();
        return;
    }

    // Explicitly wake a HOGP device that may have retained a suspend state.
    if (NimBLERemoteCharacteristic *controlPoint = hid->getCharacteristic(NimBLEUUID("2A4C"))) {
        const uint8_t exitSuspend = 1;
        if (controlPoint->canWrite() || controlPoint->canWriteNoResponse()) {
            controlPoint->writeValue(&exitSuspend, 1, controlPoint->canWrite());
        }
    }

    NimBLEDevice::getScan()->clearResults();
    keyboardCandidate = nullptr;
    keyboardState = KeyboardState::Connected;
    keyboardSubscribedCount = subscribed;
    keyboardMessage = "Ready; " + String(subscribed) + F(" input report(s)");
    drawKeyboardScreen();
}

void endKeyboardMode() {
    keyboardMode = false;
    keyboardExitRequested = false;
    if (keyboardBleReady) {
        NimBLEDevice::getScan()->stop();
        if (keyboardClient && keyboardClient->isConnected()) keyboardClient->disconnect();
        NimBLEDevice::deinit(true);
    }
    keyboardBleReady = false;
    keyboardClient = nullptr;
    keyboardCandidate = nullptr;
    keyboardCandidateReady = false;
    keyboardState = KeyboardState::Off;
    applyPowerProfile();
    setDisplayMode(DisplayMode::Normal);
    startPortal();
}

void handleKeyboardModeRequest() {
    if (bleScanning || keyboardMode || keyboardModeRequested) {
        server.send(409, "text/plain", "A Bluetooth operation is already active.");
        return;
    }
    server.send(202, "text/plain", "Keyboard mode starting. Follow the StickS3 screen.");
    keyboardModeRequested = true;
    keyboardModeRequestedAt = millis();
}

void handleDisplayControl() {
    const String mode = server.arg("mode");
    if (mode == "off") setDisplayMode(DisplayMode::Off);
    else if (mode == "dim") setDisplayMode(DisplayMode::Dim);
    else {
        setDisplayMode(DisplayMode::Normal);
        drawPortalScreen();
    }
    server.send(200, "text/plain", displayStateText());
}

void handlePowerProfile() {
    powerProfile = server.arg("profile") == "performance" ? PowerProfile::Performance : PowerProfile::Eco;
    if (portalRunning && powerProfile == PowerProfile::Performance) WiFi.mode(WIFI_AP_STA);
    applyPowerProfile();
    server.send(200, "text/plain", powerProfileText());
    if (portalRunning && powerProfile == PowerProfile::Eco) restoreEcoApAt = millis() + 250;
}

void handlePortalTimeout() {
    const int minutes = server.arg("minutes").toInt();
    if (minutes == 0 || minutes == 2 || minutes == 5 || minutes == 10) {
        portalTimeoutMs = static_cast<uint32_t>(minutes) * 60UL * 1000UL;
        lastPortalClientAt = millis();
        server.send(200, "text/plain", minutes == 0 ? "Automatic stop disabled" : "Timeout updated");
    } else {
        server.send(400, "text/plain", "Unsupported timeout");
    }
}

void handleSleepRequest() {
    server.send(200, "text/plain", "Sleeping");
    sleepRequested = true;
    sleepRequestedAt = millis();
}

void sendNovaPng(const uint8_t *data, size_t length) {
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.send_P(200, "image/png", reinterpret_cast<const char *>(data), length);
}

void startPortal() {
    sleeping = false;
    portalQrVisible = false;
    const uint32_t chip = static_cast<uint32_t>(ESP.getEfuseMac());
    char suffix[9];
    snprintf(suffix, sizeof(suffix), "%08lX", static_cast<unsigned long>(chip));
    apSsid = "PocketLab-" + String(suffix + 4);
    apPassword = "lab-" + String(suffix);

    WiFi.mode(powerProfile == PowerProfile::Eco ? WIFI_AP : WIFI_AP_STA);
    WiFi.softAP(apSsid.c_str(), apPassword.c_str());
    applyPowerProfile();
    dnsServer.start(kDnsPort, "*", WiFi.softAPIP());

    if (!routesConfigured) {
        server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", kPage); });
        server.on("/assets/nova/idle.png", HTTP_GET, []() { sendNovaPng(kNovaIdlePng, kNovaIdlePngLength); });
        server.on("/assets/nova/scanning.png", HTTP_GET, []() { sendNovaPng(kNovaScanningPng, kNovaScanningPngLength); });
        server.on("/assets/nova/success.png", HTTP_GET, []() { sendNovaPng(kNovaSuccessPng, kNovaSuccessPngLength); });
        server.on("/assets/nova/warning.png", HTTP_GET, []() { sendNovaPng(kNovaWarningPng, kNovaWarningPngLength); });
        server.on("/assets/nova/device.png", HTTP_GET, []() { sendNovaPng(kNovaDevicePng, kNovaDevicePngLength); });
        server.on("/assets/nova/wifi.png", HTTP_GET, []() { sendNovaPng(kNovaWifiPng, kNovaWifiPngLength); });
        server.on("/assets/nova/ble.png", HTTP_GET, []() { sendNovaPng(kNovaBlePng, kNovaBlePngLength); });
        server.on("/assets/nova/keyboard.png", HTTP_GET, []() { sendNovaPng(kNovaKeyboardPng, kNovaKeyboardPngLength); });
        server.on("/assets/nova/vault.png", HTTP_GET, []() { sendNovaPng(kNovaVaultPng, kNovaVaultPngLength); });
        server.on("/api/status", HTTP_GET, sendStatus);
        server.on("/api/files", HTTP_GET, sendFiles);
        server.on("/api/wifi-scan", HTTP_GET, scanWifi);
        server.on("/api/save", HTTP_POST, saveSurvey);
        server.on("/api/baseline", HTTP_POST, setBaseline);
        server.on("/api/ble-scan", HTTP_GET, scanBle);
        server.on("/api/ble-save", HTTP_POST, saveBleSurvey);
        server.on("/api/ble-baseline", HTTP_POST, setBleBaseline);
        server.on("/api/keyboard-mode", HTTP_POST, handleKeyboardModeRequest);
        server.on("/api/upload", HTTP_POST, finishFileUpload, handleFileUploadData);
        server.on("/api/delete", HTTP_POST, deleteFile);
        server.on("/api/display", HTTP_POST, handleDisplayControl);
        server.on("/api/power-profile", HTTP_POST, handlePowerProfile);
        server.on("/api/portal-timeout", HTTP_POST, handlePortalTimeout);
        server.on("/api/sleep", HTTP_POST, handleSleepRequest);
        server.on("/download", HTTP_GET, downloadFile);
        server.onNotFound([]() {
            server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
            server.send(302, "text/plain", "");
        });
        routesConfigured = true;
    }
    server.begin();
    portalRunning = true;
    portalStartedAt = millis();
    lastPortalClientAt = portalStartedAt;
    drawPortalScreen();
}

void drawStoppedScreen() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 8);
    M5.Display.println("Portal stopped");
    refreshBattery(true);
    drawStoppedBattery();
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, 108);
    M5.Display.println("Press Face button (blue) to start");
    lastBatteryDraw = millis();
}

void stopPortal() {
    portalRunning = false;
    portalQrVisible = false;
    dnsServer.stop();
    server.stop();
    WiFi.scanDelete();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    drawStoppedScreen();
}

void enterSleepMode() {
    sleepRequested = false;
    if (portalRunning) stopPortal();
    sleeping = true;
    setDisplayMode(DisplayMode::Off);
    setCpuFrequencyMhz(80);
}

bool pressed(uint8_t pin) {
    static uint32_t lastPress[49] = {};
    if (digitalRead(pin) != LOW || millis() - lastPress[pin] < 350) return false;
    lastPress[pin] = millis();
    return true;
}
} // namespace

void setup() {
    Serial.begin(115200);
    auto config = M5.config();
    M5.begin(config);
    M5.Display.setRotation(3);
    pinMode(kSelectPin, INPUT_PULLUP);
    pinMode(kNextPin, INPUT_PULLUP);
    M5.Power.setExtOutput(false);
    bleResultsMutex = xSemaphoreCreateMutex();
    setDisplayMode(DisplayMode::Normal);
    showSplashScreen();
    resetReason = resetReasonName(esp_reset_reason());
    initializeBatteryEstimator();
    bootBatteryMillivolts = battery.millivolts;

    if (!LittleFS.begin(true)) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.println("LittleFS failed");
        return;
    }

    startPortal();
}

void loop() {
    if (sleeping) {
        if (pressed(kSelectPin)) {
            sleeping = false;
            applyPowerProfile();
            setDisplayMode(DisplayMode::Normal);
            drawStoppedScreen();
        } else {
            refreshBattery();
            delay(20);
        }
        return;
    }

    if (keyboardMode) {
        const bool m5Pressed = pressed(kNextPin);
        const bool facePressed = pressed(kSelectPin);
        if (m5Pressed || keyboardExitRequested) {
            endKeyboardMode();
            return;
        }
        if (facePressed && keyboardState == KeyboardState::Failed && keyboardBleReady) {
            if (keyboardClient) {
                if (keyboardClient->isConnected()) keyboardClient->disconnect();
                NimBLEDevice::deleteClient(keyboardClient);
                keyboardClient = nullptr;
            }
            keyboardDisconnected = false;
            keyboardPasskey = 100000UL + (esp_random() % 900000UL);
            startKeyboardScan();
        }
        if (keyboardCandidateReady) connectKeyboardCandidate();
        if (keyboardScreenDirty && millis() - keyboardLastScreenDraw >= 40) {
            setDisplayMode(DisplayMode::Normal);
            lastDisplayActivity = millis();
            drawKeyboardScreen();
        }
        if (displayMode == DisplayMode::Normal && millis() - lastDisplayActivity >= kAutoDimMs) {
            setDisplayMode(DisplayMode::Dim);
        }
        refreshBattery();
        delay(5);
        return;
    }

    if (portalRunning) {
        dnsServer.processNextRequest();
        server.handleClient();

        const uint32_t now = millis();
        if (WiFi.softAPgetStationNum() > 0) lastPortalClientAt = now;
        else if (!bleScanning && portalTimeoutMs > 0 && now - lastPortalClientAt >= portalTimeoutMs) stopPortal();

        if (portalRunning && restoreEcoApAt != 0 && static_cast<int32_t>(now - restoreEcoApAt) >= 0) {
            restoreEcoApAt = 0;
            if (powerProfile == PowerProfile::Eco) {
                WiFi.mode(WIFI_AP);
                applyPowerProfile();
            }
        }
    }

    if (keyboardModeRequested && millis() - keyboardModeRequestedAt >= 300) {
        beginKeyboardMode();
        return;
    }

    if (sleepRequested && millis() - sleepRequestedAt >= 250) {
        enterSleepMode();
        return;
    }

    const bool m5Pressed = pressed(kNextPin);
    const bool facePressed = pressed(kSelectPin);
    if (m5Pressed || facePressed) {
        lastDisplayActivity = millis();
        if (displayMode != DisplayMode::Normal) {
            setDisplayMode(DisplayMode::Normal);
            if (portalRunning) drawPortalScreen();
            else drawStoppedScreen();
        } else if (portalRunning && m5Pressed) {
            stopPortal();
        } else if (portalRunning && facePressed) {
            if (portalQrVisible) drawPortalScreen();
            else drawPortalQrScreen();
        } else if (!portalRunning && facePressed) {
            startPortal();
        }
    }

    if (displayMode == DisplayMode::Normal && millis() - lastDisplayActivity >= kAutoDimMs) {
        setDisplayMode(DisplayMode::Dim);
    }
    if (millis() - lastBatteryDraw >= 5000) {
        if (displayMode != DisplayMode::Off) {
            if (portalRunning) {
                if (portalQrVisible) drawPortalQrBattery();
                else drawBatteryBadge();
            }
            else drawStoppedBattery();
        } else {
            refreshBattery();
        }
        lastBatteryDraw = millis();
    }
    delay(2);
}
