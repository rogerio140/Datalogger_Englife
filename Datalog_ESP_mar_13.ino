#include <WiFi.h>
#include <FirebaseESP32.h>
#include <RTClib.h>

#define FIREBASE_HOST "https://datalogeer-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "AIzaSyAMAf30UG0YOqaxF-KgD5gfAhWhddN-5Qg"

// Configurações do WiFi
#define WIFI_SSID "Englife"
#define WIFI_PASSWORD "kpq6f4716"

// Definição dos pinos
#define LED_AGUA_PIN     19
#define LED_ESTUFA_PIN   18
#define MOTOR_PIN        25  // Nome alterado

// Objetos globais
RTC_DS3231 rtc;
FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

// Estrutura para armazenar limites
struct Limites {
  float max;
  float min;
};

// Variáveis de configuração
String horarioInicio = "08:00";
String horarioFim = "18:00";
int porcoes = 5;


Limites agua_limites = {30.0, 20.0};
Limites estufa_limites = {35.0, 25.0};
Limites externa_limites = {40.0, 15.0};


// Variáveis do motor
int motor_state = 0;
unsigned long motor_ligado_ate = 0;  // Timer para controle de tempo

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_AGUA_PIN, OUTPUT);
  pinMode(LED_ESTUFA_PIN, OUTPUT);
  pinMode(MOTOR_PIN, OUTPUT);  // Nome alterado
  
  // Conectar ao WiFi (mesmo código)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  // Configurar Firebase (mesmo código)
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);

  // Criar variável do motor se não existir
  if (!Firebase.get(firebaseData, "/motor")) {
    Firebase.setInt(firebaseData, "/motor", 0);
  }
  // Inicializar RTC
  if (!rtc.begin()) {
    Serial.println("Erro ao iniciar RTC!");
    while(1);
  }

  // Sincronizar RTC com Firebase na inicialização
  syncRTCWithFirebase();
  
  // Verificar se o RTC está funcionando
  if(!rtc.begin()) {
    Serial.println("RTC não encontrado!");
    while(1);
  }
  if(rtc.lostPower()) {
    Serial.println("RTC perdeu energia, sincronizando com Firebase!");
    syncRTCWithFirebase();
  }
}

void loop() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 2000) {
    lastUpdate = millis();
    
    fetchLimites();
    checkSensores();
    checkMotor();
    verificarTempoMotor();  // Nova verificação do timer
  }
  handleCalibracao(); // Nova chamada
  delay(100); // Reduzir carga da CPU
  static unsigned long ultimaLeitura = 0;
  if (millis() - ultimaLeitura > 5000) {
    ultimaLeitura = millis();
    
    // Ler valores do Firebase
    horarioInicio = lerStringFirebase("/config/horarioInicio");
    horarioFim = lerStringFirebase("/config/horarioFim");
    porcoes = lerIntFirebase("/config/porcoes");
    
    // Calcular e mostrar horários
    calcularEmostrarHorarios();
    static unsigned long lastRTCcheck = 0;
  if(millis() - lastRTCcheck > 1000) { // Verificar a cada 1 segundo
    lastRTCcheck = millis();
    
    checkAgendamento();
    exibirHoraAtual();
  }
  }
}
// Função para sincronizar horário com Firebase (opcional)
void syncRTCWithFirebase() {
  if(Firebase.getString(firebaseData, "/config/rtcTime")) {
    String ftime = firebaseData.stringData();
    int h = ftime.substring(0,2).toInt();
    int m = ftime.substring(3,5).toInt();
    rtc.adjust(DateTime(2020, 1, 1, h, m, 0)); // Data fixa, hora e minuto ajustáveis
  }
}
void exibirHoraAtual() {
  DateTime now = rtc.now();
  Serial.printf("\nHora RTC: %02d:%02d:%02d", now.hour(), now.minute(), now.second());
}
// Função para acionar o motor por um tempo determinado (em segundos)
void acionarMotorPorTempo(int tempoSegundos) {
  digitalWrite(MOTOR_PIN, HIGH);
  motor_state = 1;
  motor_ligado_ate = millis() + (tempoSegundos * 1000);
  Firebase.setInt(firebaseData, "/motor", 1);  // Atualiza Firebase
  
  Serial.print("Motor ligado por ");
  Serial.print(tempoSegundos);
  Serial.println(" segundos");
}

void verificarTempoMotor() {
  if (motor_ligado_ate > 0 && millis() >= motor_ligado_ate) {
    digitalWrite(MOTOR_PIN, LOW);
    motor_state = 0;
    motor_ligado_ate = 0;
    Firebase.setInt(firebaseData, "/motor", 0);  // Atualiza Firebase
    Serial.println("Motor desligado por tempo");
  }
}

// Função modificada com novo nome
void checkMotor() {
  if (Firebase.getInt(firebaseData, "/motor")) {
    int novo_estado = firebaseData.intData();
    
    // Só altera se não estiver controlado por tempo
    if(novo_estado != motor_state && motor_ligado_ate == 0) {
      motor_state = novo_estado;
      digitalWrite(MOTOR_PIN, motor_state);
      //Serial.print("Motor: ");
      Serial.println(motor_state ? "LIGADO" : "DESLIGADO");
    }
  }
}

// fetchLimites() e checkSensores() permanecem iguais
void fetchLimites() {
  // Limites Água
  if (Firebase.getFloat(firebaseData, "/alert_thresholds/agua/max")) {
    agua_limites.max = firebaseData.floatData();
    //Serial.print("Limite Máximo Água: ");
    //Serial.println(agua_limites.max);
  }
  if (Firebase.getFloat(firebaseData, "/alert_thresholds/agua/min")) {
    agua_limites.min = firebaseData.floatData();
    //Serial.print("Limite Mínimo Água: ");
    //Serial.println(agua_limites.min);
  }

  // Limites Estufa
  if (Firebase.getFloat(firebaseData, "/alert_thresholds/estufa/max")) {
    estufa_limites.max = firebaseData.floatData();
    //Serial.print("Limite Máximo Estufa: ");
    //Serial.println(estufa_limites.max);
  }
  if (Firebase.getFloat(firebaseData, "/alert_thresholds/estufa/min")) {
    estufa_limites.min = firebaseData.floatData();
    //Serial.print("Limite Mínimo Estufa: ");
    //Serial.println(estufa_limites.min);
  }

  // Limites Externa
  if (Firebase.getFloat(firebaseData, "/alert_thresholds/externa/max")) {
    externa_limites.max = firebaseData.floatData();
    //Serial.print("Limite Máximo Externa: ");
    //Serial.println(externa_limites.max);
  }
  if (Firebase.getFloat(firebaseData, "/alert_thresholds/externa/min")) {
    externa_limites.min = firebaseData.floatData();
    //Serial.print("Limite Mínimo Externa: ");
    //Serial.println(externa_limites.min);
  }
}

void checkSensores() {
  float temp_agua, temp_estufa, temp_externa;
  
  // Verificar Água
  if (Firebase.getFloat(firebaseData, "/sensores/temperatura_3")) {
    temp_agua = firebaseData.floatData();
    bool alerta_agua = (temp_agua > agua_limites.max) || (temp_agua < agua_limites.min);
    digitalWrite(LED_AGUA_PIN, alerta_agua ? HIGH : LOW);
    //Serial.print("Água: ");
    //Serial.print(temp_agua);
    //Serial.println(alerta_agua ? " - ALERTA!" : " - Normal");
  }

  // Verificar Estufa
  if (Firebase.getFloat(firebaseData, "/sensores/temperatura_2")) {
    temp_estufa = firebaseData.floatData();
    bool alerta_estufa = (temp_estufa > estufa_limites.max) || (temp_estufa < estufa_limites.min);
    digitalWrite(LED_ESTUFA_PIN, alerta_estufa ? HIGH : LOW);
    //Serial.print("Estufa: ");
    //Serial.print(temp_estufa);
   // Serial.println(alerta_estufa ? " - ALERTA!" : " - Normal");
  }

  // Verificar Externa (sem atuador)
  if (Firebase.getFloat(firebaseData, "/sensores/temperatura_1")) {
    temp_externa = firebaseData.floatData();
    bool alerta_externa = (temp_externa > externa_limites.max) || (temp_externa < externa_limites.min);
    //Serial.print("Externa: ");
    //Serial.print(temp_externa);
    //Serial.println(alerta_externa ? " - ALERTA!" : " - Normal");
  }
}

// Variáveis globais adicionadas
bool calibracaoAtiva = false;
unsigned long calibracaoDelay = 0;

void handleCalibracao() {
  static int etapa = 0;
  static float peso1 = 0, peso2 = 0;
  static int tempo1 = 0, tempo2 = 0;
  
  if (Firebase.getBool(firebaseData, "/calibracao/ativa")) {
    bool novaAtiva = firebaseData.boolData();
    
    // Iniciar calibração
    if (!calibracaoAtiva && novaAtiva) {
      calibracaoAtiva = true;
      etapa = 0;
      peso1 = 0;
      peso2 = 0;
      
      Serial.println("Iniciando calibração...");
    }
    // Cancelar calibração
    else if (calibracaoAtiva && !novaAtiva) {
      calibracaoAtiva = false;
      Serial.println("Calibração cancelada");
    }
  }

  if (!calibracaoAtiva) return;

  // Máquina de estados da calibração
  switch (etapa) {
    case 0: // Preparação
      Firebase.setInt(firebaseData, "/calibracao/etapa", 1);
      etapa = 1;
      break;

    case 1: // Executar tempo1 e aguardar peso1
      if (Firebase.getInt(firebaseData, "/calibracao/tempo1")) {
        tempo1 = firebaseData.intData();
        acionarMotorPorTempo(tempo1);
        calibracaoDelay = millis() + (tempo1 * 1000) + 2000; // Tempo + margem
        etapa = 2;
      }
      break;

    case 2: // Aguardar peso1
      if (millis() > calibracaoDelay) {
        if (Firebase.getFloat(firebaseData, "/calibracao/peso1") && firebaseData.floatData() > 0) {
          peso1 = firebaseData.floatData();
          etapa = 3;
          Firebase.setInt(firebaseData, "/calibracao/etapa", 2);
        }
      }
      break;

    case 3: // Executar tempo2 e aguardar peso2
      if (Firebase.getInt(firebaseData, "/calibracao/tempo2")) {
        tempo2 = firebaseData.intData();
        acionarMotorPorTempo(tempo2);
        calibracaoDelay = millis() + (tempo2 * 1000) + 2000;
        etapa = 4;
      }
      break;

    case 4: // Aguardar peso2
      if (millis() > calibracaoDelay) {
        if (Firebase.getFloat(firebaseData, "/calibracao/peso2") && firebaseData.floatData() > 0) {
          peso2 = firebaseData.floatData();
          etapa = 5;
        }
      }
      break;

    case 5: // Calcular constantes
      if (tempo2 != tempo1) { // Evitar divisão por zero
        float constanteA = (peso2 - peso1) / (tempo2 - tempo1);
        float constanteB = peso1 - (constanteA * tempo1);
        
        Firebase.setFloat(firebaseData, "/calibracao/constanteA", constanteA);
        Firebase.setFloat(firebaseData, "/calibracao/constanteB", constanteB);
        
        Serial.print("Constantes calculadas - A: ");
        Serial.print(constanteA);
        Serial.print(" B: ");
        Serial.println(constanteB);
      }
      
      // Resetar valores
      Firebase.setBool(firebaseData, "/calibracao/ativa", false);
      Firebase.setFloat(firebaseData, "/calibracao/peso1", 0);
      Firebase.setFloat(firebaseData, "/calibracao/peso2", 0);
      Firebase.setInt(firebaseData, "/calibracao/etapa", 0);
      
      calibracaoAtiva = false;
      Serial.println("Calibração concluída!");
      break;
  }
}

//configuração de horarios e pesos
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
    // Armazenar os horários calculados em variáveis globais
  for(int i = 0; i < porcoes; i++) {
    // ... (cálculo existente)
    horariosPorcoes[i] = horario; // Array global de strings
  }
  }
}
void checkAgendamento() {
  static String ultimoHorarioAcionado = "";
  String horarioAtual = getRTCTime();

  for(int i = 0; i < porcoes; i++) {
    if(horariosPorcoes[i] == horarioAtual && horarioAtual != ultimoHorarioAcionado) {
      acionarMotorPorTempo(3); // 3 segundos por exemplo
      ultimoHorarioAcionado = horarioAtual;
      Serial.println("Alimentação acionada por agendamento!");
    }
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



// ... (outras funções se mantêm inalteradas)