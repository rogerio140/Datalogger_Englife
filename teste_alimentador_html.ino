#include <WiFi.h>
#include <FirebaseESP32.h>
#include <RTClib.h>

// Configurações do WiFi
#define WIFI_SSID "Englife"
#define WIFI_PASSWORD "kpq6f4716"

// Configurações do Firebase
#define FIREBASE_HOST "datalogger-b81e3-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "nB1yab3NVNT5iOQ22Z2jbXcGlyM1XtbBw4F17ggT"

// Objetos globais
RTC_DS3231 rtc;
FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

// Variáveis de configuração
String horarioInicio = "08:00";
String horarioFim = "18:00";
int porcoes = 5;

void setup() {
  Serial.begin(115200);
  
  // Conectar ao WiFi
  Serial.println("\nConectando ao WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nConectado com sucesso!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // Inicializar Firebase
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("Firebase inicializado");

  // Inicializar RTC
  if (!rtc.begin()) {
    Serial.println("Erro ao iniciar RTC!");
    while(1);
  }
  Serial.println("RTC inicializado");
}

void loop() {
  static unsigned long ultimaLeitura = 0;
  if (millis() - ultimaLeitura > 5000) {
    ultimaLeitura = millis();
    
    // Ler valores do Firebase
    horarioInicio = lerStringFirebase("/config/horarioInicio");
    horarioFim = lerStringFirebase("/config/horarioFim");
    porcoes = lerIntFirebase("/config/porcoes");
    
    // Calcular e mostrar horários
    calcularEmostrarHorarios();
    
  }
}

void calcularEmostrarHorarios() {
  int inicioMinutos = converterHoraParaMinutos(horarioInicio);
  int fimMinutos = converterHoraParaMinutos(horarioFim);
  
  // Ajustar para período que ultrapassa a meia-noite
  if (fimMinutos <= inicioMinutos) {
    fimMinutos += 1440; // Adiciona 24h (1440 minutos)
  }

  int intervalo = (fimMinutos - inicioMinutos) / (porcoes - 1);
  
  Serial.println("\n--- Programação de Alimentação ---");
  
  for(int i = 0; i < porcoes; i++) {
    int minutosCalculados = inicioMinutos + (i * intervalo);
    
    // Ajustar para horário válido (caso ultrapasse 1440)
    minutosCalculados %= 1440; // Garante valor entre 0-1439
    
    String horario = converterMinutosParaHora(minutosCalculados);
    
    Serial.printf("Porção %d: %s\n", i+1, horario.c_str());
  }
}

// Funções auxiliares
int converterHoraParaMinutos(String hora) {
  int h = hora.substring(0,2).toInt();
  int m = hora.substring(3,5).toInt();
  return (h * 60) + m;
}

String converterMinutosParaHora(int minutos) {
  int h = minutos / 60;
  int m = minutos % 60;
  return String(h) + ":" + (m < 10 ? "0" : "") + String(m);
}

String lerStringFirebase(const String& caminho) {
  if (Firebase.getString(firebaseData, caminho)) {
    return firebaseData.stringData();
  }
  return "Erro";
}

int lerIntFirebase(const String& caminho) {
  if (Firebase.getInt(firebaseData, caminho)) {
    return firebaseData.intData();
  }
  return -1;
}