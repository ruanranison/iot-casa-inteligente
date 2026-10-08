/*
  SMART HOME - ESP32
  Alinhado ao artigo (monitoramento, LCD, interface web e comandos de voz)
  e à fiação pedida, com duas correções elétricas.

  Fiação desta montagem
    RGB vermelho   GPIO 13
    RGB verde      GPIO 12   (strapping: na gravação, se não iniciar, solte o LED)
    RGB azul       GPIO 14
    HC-SR04 TRIG   GPIO 27
    HC-SR04 ECHO   GPIO 26   (ECHO é 5 V: divisor 1k até o GPIO e 2k até o GND)
    DHT11 DATA     GPIO 4    (pedido no 34: corrigido para 4 pois DHT exige I/O)
    LCD I2C SDA    GPIO 32
    LCD I2C SCL    GPIO 33   (pedido no 32: corrigido pois I2C exige dois pinos)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>

// ===================== CONFIGURAÇÃO =====================
const char* WIFI_SSID = "Rede IFAL_Arapiraca";
const char* WIFI_PASS = "redeifal";
const char* AP_SSID   = "SmartHome-ESP32";

#define PIN_R     13
#define PIN_G     12
#define PIN_B     14
#define PIN_TRIG  27
#define PIN_ECHO  26
#define PIN_DHT   4
#define PIN_SDA   32
#define PIN_SCL   33

#define DHTTYPE   DHT11
#define LCD_ADDR  0x27   // se a varredura achar só 0x3F, troque aqui

// ===================== OBJETOS E ESTADO =====================
DHT dht(PIN_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
WebServer server(80);

float temperatura = NAN;
float umidade = NAN;
float distanciaCm = -1;
bool luzOn = false;
String corNome = "OFF";
uint8_t cr = 0, cg = 0, cb = 0;

unsigned long tDHT = 0, tUS = 0, tLCD = 0;
uint8_t telaLCD = 0;

// ===================== PWM RGB =====================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define PWM_INIT(pin, ch) ledcAttach(pin, 5000, 8)
  #define PWM_WRITE(pin, ch, v) ledcWrite(pin, v)
#else
  #define PWM_INIT(pin, ch) do { ledcSetup(ch, 5000, 8); ledcAttachPin(pin, ch); } while (0)
  #define PWM_WRITE(pin, ch, v) ledcWrite(ch, v)
#endif

void aplicarRGB(uint8_t r, uint8_t g, uint8_t b, const String& nome) {
  cr = r;
  cg = g;
  cb = b;
  corNome = nome;
  luzOn = (r | g | b) != 0;
  PWM_WRITE(PIN_R, 0, r);
  PWM_WRITE(PIN_G, 1, g);
  PWM_WRITE(PIN_B, 2, b); 
}

uint8_t clamp8(long v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

// ===================== SENSORES =====================
float lerDistancia() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracao = pulseIn(PIN_ECHO, HIGH, 30000); 
  if (duracao == 0) return -1;

  float cm = duracao * 0.0343f / 2.0f; 
  if (cm < 2.0f || cm > 400.0f) return -1;
  return cm;
}

void atualizarSensores() {
  unsigned long agora = millis();

  if (agora - tDHT >= 2000) { 
    tDHT = agora;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperatura = t;
    if (!isnan(h)) umidade = h;
  }

  if (agora - tUS >= 400) {
    tUS = agora;
    distanciaCm = lerDistancia();
  }
}

// ===================== LCD =====================
void linhaLCD(uint8_t linha, const char* txt) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16.16s", txt);
  lcd.setCursor(0, linha);
  lcd.print(buf);
}

void textoTemp(char* saida, size_t n) {
  if (isnan(temperatura)) {
    snprintf(saida, n, "--");
    return;
  }
  snprintf(saida, n, "%.1f", temperatura);
  for (char* p = saida; *p; ++p) {
    if (*p == '.') *p = ',';
  }
}

void atualizarLCD() {
  if (millis() - tLCD < 3000) return;
  tLCD = millis();

  char l0[17], l1[17], num[8];
  switch (telaLCD) {
    case 0: // Tela 1: TEMP e UMID
      textoTemp(num, sizeof(num));
      snprintf(l0, sizeof(l0), "TEMP: %s C", num);
      if (isnan(umidade)) snprintf(l1, sizeof(l1), "UMID: --");
      else snprintf(l1, sizeof(l1), "UMID: %.0f%%", umidade);
      break;
    case 1: // Tela 2: LUZ e RGB
      snprintf(l0, sizeof(l0), "LUZ: %s", luzOn ? "ON" : "OFF");
      snprintf(l1, sizeof(l1), "RGB: %s", corNome.c_str());
      break;
    case 2: // Tela 3: PRESENÇA e VENTILADOR
      bool presenca = (distanciaCm > 0 && distanciaCm < 100); // Simulação de presença via HC-SR04
      snprintf(l0, sizeof(l0), "PRESENCA: %s", presenca ? "SIM" : "NAO");
      snprintf(l1, sizeof(l1), "VENT: OFF");
      break;
  }
  linhaLCD(0, l0);
  linhaLCD(1, l1);
  telaLCD = (telaLCD + 1) % 3;
}

void varrerI2C() {
  Serial.println("Varredura I2C (SDA 32, SCL 33):");
  int achou = 0;
  for (uint8_t endereco = 1; endereco < 127; ++endereco) {
    Wire.beginTransmission(endereco);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  encontrado 0x%02X\n", endereco);
      if (endereco == 0x3F && LCD_ADDR != 0x3F) {
        Serial.println("  LCD parece 0x3F: altere LCD_ADDR e grave de novo.");
      }
      achou++;
    }
  }
  if (!achou) Serial.println("  nenhum dispositivo");
}

// ===================== WEB =====================
const char PAGINA[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="pt-BR"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Home ESP32</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--tx:#1b1f23;--ac:#2563eb;--bd:#d9dee3}
@media (prefers-color-scheme:dark){:root{--bg:#14171a;--card:#1f2428;--tx:#e8ecef;--bd:#323940}}
*{box-sizing:border-box}body{margin:0 auto;max-width:760px;padding:16px;font-family:system-ui,sans-serif;background:var(--bg);color:var(--tx)}
h1{font-size:1.3rem;margin:0 0 4px}p.nota{margin:0 0 14px;font-size:.9rem;opacity:.75}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:12px}
.card small{opacity:.7}.v{font-size:1.5rem;font-weight:600}
button{background:var(--ac);color:#fff;border:0;border-radius:8px;padding:10px 12px;font-size:.95rem;cursor:pointer}
button.sec{background:transparent;color:var(--tx);border:1px solid var(--bd)}
.row{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap}
input[type=color]{width:100%;height:40px;border:0;background:none;padding:0}
input[type=text]{flex:1;min-width:0;padding:10px;border-radius:8px;border:1px solid var(--bd);background:var(--card);color:var(--tx)}
#log,#net{font-size:.9rem;margin-top:8px;min-height:1.2em}
#net{color:#b45309}
</style></head><body>
<h1>Smart Home ESP32</h1>
<p class="nota">Luz acende o RGB em branco. Ventilador, presença e LDR não têm pino nesta fiação. O HC-SR04 está nos GPIO 26 e 27.</p>
<div class="grid">
 <div class="card"><small>Temperatura</small><div class="v" id="t">--</div></div>
 <div class="card"><small>Umidade</small><div class="v" id="h">--</div></div>
 <div class="card"><small>Distância</small><div class="v" id="d">--</div></div>
 <div class="card"><small>Luz / RGB</small><div class="v" id="rgb">--</div></div>
</div>
<h3>Controle</h3>
<div class="grid">
 <div class="card"><small>Luz: <b id="luz">--</b></small>
  <div class="row"><button onclick="c('ligar a luz')">Ligar</button><button class="sec" onclick="c('desligar a luz')">Desligar</button></div>
 </div>
 <div class="card"><small>Cor do RGB</small>
  <input type="color" id="cor" value="#0000ff" onchange="rgbHex(this.value)">
  <button class="sec" onclick="c('desligar rgb')">Apagar</button>
 </div>
</div>
<h3>Voz / texto</h3>
<div class="row">
 <input type="text" id="txt" placeholder="ex.: acender RGB azul" onkeydown="if(event.key==='Enter')c(txt.value)">
 <button onclick="c(txt.value)">Enviar</button>
 <button id="mic" onclick="voz()">Falar</button>
</div>
<div id="log"></div>
<div id="net"></div>
<script>
const $=id=>document.getElementById(id);
const CORES=[
 ['vermelh','VERMELHO',255,0,0],
 ['verde','VERDE',0,255,0],
 ['azul','AZUL',0,0,255],
 ['amarel','AMARELO',255,200,0],
 ['branc','BRANCO',255,255,255],
 ['roxo','ROXO',128,0,255],
 ['violet','ROXO',128,0,255],
 ['laranja','LARANJA',255,90,0],
 ['ciano','CIANO',0,255,255],
 ['rosa','ROSA',255,40,120]
];
const tem=(s,p)=>s.indexOf(p)>=0;

function interpretar(bruto){
 const s=bruto.normalize('NFD').replace(/[\u0300-\u036f]/g,'').toLowerCase().trim();
 if(!s) return {erro:'Comando vazio'};
 const off=tem(s,'deslig')||tem(s,'apag')||tem(s,'desativ')||tem(s,'desacion');
 const on=!off&&(tem(s,'lig')||tem(s,'acend')||tem(s,'acion')||tem(s,'ativ'));
 if(tem(s,'ventil')||tem(s,'ventoinha')) return {acao:off?'vent_off':'vent_on'};
 let cor=null;
 for(const c of CORES){ if(tem(s,c[0])){ cor=c; break; } }
 const querCor=tem(s,'rgb')||tem(s,'led')||tem(s,'cor');
 if(querCor||cor){
  if(cor&&!off) return {acao:'rgb',r:cor[2],g:cor[3],b:cor[4],nome:cor[1]};
  if(off) return {acao:'rgb_off'};
  if(on) return {acao:'luz_on'};
  return {erro:'Cor nao reconhecida'};
 }
 if(tem(s,'luz')||tem(s,'lampada')){
  if(off) return {acao:'luz_off'};
  if(on) return {acao:'luz_on'};
 }
 return {erro:'Comando nao reconhecido'};
}

async function c(t){
 if(!t) return;
 const i=interpretar(t);
 const t0=performance.now();
 if(!i.acao){ $('log').textContent='"'+t+'" -> '+(i.erro||'Comando nao reconhecido'); return; }
 try{
  const q=new URLSearchParams();
  q.set('acao',i.acao);
  if(i.nome){ q.set('r',i.r); q.set('g',i.g); q.set('b',i.b); q.set('nome',i.nome); }
  const r=await fetch('/cmd?'+q.toString());
  const x=await r.text();
  $('log').textContent='"'+t+'" -> '+x+' | ida e volta: '+(performance.now()-t0).toFixed(0)+' ms';
 }catch(e){ $('log').textContent='Erro de comunicacao'; }
 est();
}

async function rgbHex(h){
 const t0=performance.now();
 try{
  const r=await fetch('/rgb?hex='+encodeURIComponent(h.slice(1)));
  const x=await r.text();
  $('log').textContent=x+' | ida e volta: '+(performance.now()-t0).toFixed(0)+' ms';
 }catch(e){ $('log').textContent='Erro de comunicacao'; }
 est();
}

async function est(){
 try{
  const r=await fetch('/estado');
  if(!r.ok) throw new Error();
  const j=await r.json();
  $('t').textContent=j.temp==null?'--':Number(j.temp).toFixed(1).replace('.',',')+' °C';
  $('h').textContent=j.umid==null?'--':Number(j.umid).toFixed(0).replace('.',',')+' %';
  $('d').textContent=j.dist<0?'--':Number(j.dist).toFixed(0)+' cm';
  $('luz').textContent=j.luz?'ON':'OFF';
  $('rgb').textContent=j.luz?j.rgb:'OFF';
  $('net').textContent='';
 }catch(e){ $('net').textContent='Sem resposta do ESP32.'; }
}

function voz(){
 const SR=window.SpeechRecognition||window.webkitSpeechRecognition;
 if(!SR||!window.isSecureContext){
  $('log').textContent='Microfone bloqueado em http://IP. Digite a frase: a interpretacao e a mesma da voz.';
  return;
 }
 const r=new SR();
 r.lang='pt-BR';
 r.interimResults=false;
 r.onstart=()=>$('mic').textContent='...';
 r.onend=()=>$('mic').textContent='Falar';
 r.onresult=e=>{ const t=e.results[0][0].transcript; $('txt').value=t; c(t); };
 r.onerror=e=>$('log').textContent='Erro de voz: '+e.error;
 r.start();
}
est();
setInterval(est,2000);
</script></body></html>
)HTML";

String jsonEstado() {
  char temp[16], umid[16], dist[16];
  if (isnan(temperatura)) snprintf(temp, sizeof(temp), "null");
  else snprintf(temp, sizeof(temp), "%.1f", temperatura);
  if (isnan(umidade)) snprintf(umid, sizeof(umid), "null");
  else snprintf(umid, sizeof(umid), "%.0f", umidade);
  if (distanciaCm < 0) snprintf(dist, sizeof(dist), "-1");
  else snprintf(dist, sizeof(dist), "%.0f", distanciaCm);

  char json[160];
  snprintf(json, sizeof(json),
           "{\"temp\":%s,\"umid\":%s,\"dist\":%s,\"luz\":%s,\"rgb\":\"%s\"}",
           temp, umid, dist, luzOn ? "true" : "false", corNome.c_str());
  return String(json);
}

void responder(const char* msg, unsigned long t0) {
  unsigned long dt = micros() - t0;
  char body[96];
  snprintf(body, sizeof(body), "%s [%lu us]", msg, dt);
  Serial.printf("[RESP] %s | %lu us\n", msg, dt);
  server.send(200, "text/plain; charset=utf-8", body);
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGINA);
}

void handleEstado() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", jsonEstado());
}

bool nomeSeguro(const String& nome);

void handleCmd() {
  unsigned long t0 = micros();
  if (!server.hasArg("acao")) {
    server.send(400, "text/plain; charset=utf-8", "Parametro 'acao' ausente");
    return;
  }
  String acao = server.arg("acao");

  if (acao == "luz_on") {
    aplicarRGB(255, 255, 255, "BRANCO");
    responder("Luz ligada", t0);
  } else if (acao == "luz_off" || acao == "rgb_off") {
    aplicarRGB(0, 0, 0, "OFF");
    responder(acao == "luz_off" ? "Luz desligada" : "RGB desligado", t0);
  } else if (acao == "rgb") {
    if (!server.hasArg("r") || !server.hasArg("g") || !server.hasArg("b")) {
      server.send(400, "text/plain; charset=utf-8", "RGB incompleto");
      return;
    }
    String nome = server.arg("nome");
    if (!nomeSeguro(nome)) nome = "RGB";
    aplicarRGB(clamp8(server.arg("r").toInt()),
               clamp8(server.arg("g").toInt()),
               clamp8(server.arg("b").toInt()),
               nome);
    char msg[40];
    snprintf(msg, sizeof(msg), "RGB %s", corNome.c_str());
    responder(msg, t0);
  } else if (acao == "vent_on" || acao == "vent_off") {
    responder("Ventilador fora desta montagem", t0);
  } else {
    server.send(400, "text/plain; charset=utf-8", "Acao nao reconhecida");
  }
}

bool nomeSeguro(const String& nome) {
  if (nome.length() == 0 || nome.length() > 16) return false;
  for (unsigned i = 0; i < nome.length(); ++i) {
    char c = nome[i];
    if (!(isalnum((unsigned char)c) || c == '#')) return false;
  }
  return true;
}

bool hexValido(const String& hex) {
  if (hex.length() != 6) return false;
  for (unsigned i = 0; i < hex.length(); ++i) {
    if (!isxdigit(hex[i])) return false;
  }
  return true;
}

void handleRGB() {
  unsigned long t0 = micros();
  if (!server.hasArg("hex") || !hexValido(server.arg("hex"))) {
    server.send(400, "text/plain; charset=utf-8", "hex invalido");
    return;
  }
  String hex = server.arg("hex");
  hex.toLowerCase();
  long v = strtol(hex.c_str(), nullptr, 16);
  String nome = "#" + hex;
  aplicarRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, nome);
  char msg[24];
  snprintf(msg, sizeof(msg), "RGB %s", nome.c_str());
  responder(msg, t0);
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Smart Home ESP32");

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  digitalWrite(PIN_G, LOW); 
  PWM_INIT(PIN_R, 0);
  PWM_INIT(PIN_G, 1);
  PWM_INIT(PIN_B, 2);
  aplicarRGB(0, 0, 0, "OFF");

  dht.begin();

  Wire.setPins(PIN_SDA, PIN_SCL);
  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  varrerI2C();
  linhaLCD(0, "Smart Home");
  linhaLCD(1, "Conectando WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("IP: " + WiFi.localIP().toString());
    linhaLCD(0, "IP:");
    linhaLCD(1, WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    Serial.println("Wi-Fi da casa indisponivel. AP: " + WiFi.softAPIP().toString());
    linhaLCD(0, "AP SmartHome");
    linhaLCD(1, WiFi.softAPIP().toString().c_str());
  }
  delay(2500);
  tLCD = millis() - 3000;

  server.on("/", handleRoot);
  server.on("/estado", handleEstado);
  server.on("/cmd", handleCmd);
  server.on("/rgb", handleRGB);
  server.begin();
}

void loop() {
  server.handleClient(); // Mantém o servidor web ouvindo os comandos e requisições
  atualizarSensores();   // Lê periodicamente os valores do DHT11 e do HC-SR04
  atualizarLCD();        // Atualiza as informações no display I2C
}
