#include <WiFi.h>
#include <FirebaseESP32.h>
#include <RTClib.h>

//#define FIREBASE_HOST "https://datalogeer-default-rtdb.firebaseio.com/"
//#define FIREBASE_AUTH "AIzaSyAMAf30UG0YOqaxF-KgD5gfAhWhddN-5Qg"

// Configuração do Firebase
#define FIREBASE_HOST "https://datalogger-b81e3-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "nB1yab3NVNT5iOQ22Z2jbXcGlyM1XtbBw4F17ggT"

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
// Adicione esta declaração global no início do código
char dataHora[20]; 


Limites agua_limites = {30.0, 20.0};
Limites estufa_limites = {35.0, 25.0};
Limites externa_limites = {40.0, 15.0};


// Variáveis do motor
int motor_state = 0;
int tempo_acionamento=5;
unsigned long motor_ligado_ate = 0;  // Timer para controle de tempo
// Variáveis do banco de dados
int horaInicio, minutoInicio, horaFim, minutoFim;
int intervalo, lancamentos, pesoDiario, porcoes;
bool ativa;

// Estrutura para armazenar os horários das porções
struct Porcao {
    int hora;
    int minuto;
    float peso;
};
static int ultimasPorcoes = 0;  // Variável estática para controle
Porcao horariosPorcoes[150]; // Suporte para até 10 porções
static int ultimoDia = -1;
static bool porcoesFeitas[150] = {false};
// Calcular tempo de acionamento
float constanteA=0.1;
float constanteB=0; 

// --- NTP ---
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600; // Horário de Brasília
const int daylightOffset_sec = 0;




// --- Função para atualizar o RTC via NTP ---
void atualizarRTCviaNTP() {
  Serial.println("Sincronizando hora via NTP...");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;

  if (getLocalTime(&timeinfo)) {
    rtc.adjust(DateTime(
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec
    ));
    Serial.println("RTC atualizado com sucesso.");
  } else {
    Serial.println("Erro ao obter hora via NTP.");
  }
}
void atualizarVariaveis() {
  // Atualiza se a flag 'ativa' estiver true OU se chamada após a última porção
  bool precisaAtualizar = false;

  // Verificar flag 'ativa'
  if (Firebase.getBool(firebaseData, "/config/ativa") && firebaseData.boolData()) {
    precisaAtualizar = true;
    Firebase.setBool(firebaseData, "/config/ativa", false); // Reseta a flag
    Serial.println("[Firebase] Atualização manual solicitada!");
  }

  // Carregar configurações se necessário
  if (precisaAtualizar) {
    // Carregar horários
    if (Firebase.getString(firebaseData, "/config/horarioInicio")) {
      String horarioInicio = firebaseData.stringData();
      horaInicio = horarioInicio.substring(0, 2).toInt();
      minutoInicio = horarioInicio.substring(3, 5).toInt();
    }

    if (Firebase.getString(firebaseData, "/config/horarioFim")) {
      String horarioFim = firebaseData.stringData();
      horaFim = horarioFim.substring(0, 2).toInt();
      minutoFim = horarioFim.substring(3, 5).toInt();
    }

    // Carregar outros parâmetros
    Firebase.getInt(firebaseData, "/config/intervalo") ? intervalo = firebaseData.intData() : 60;
    Firebase.getInt(firebaseData, "/config/pesoDiario") ? pesoDiario = firebaseData.intData() : 1000;
    Firebase.getInt(firebaseData, "/config/porcoes") ? porcoes = firebaseData.intData() : 10;
     // Atualiza o RTC
    atualizarRTCviaNTP();
    // Resetar estados e recalcular
    memset(porcoesFeitas, false, sizeof(porcoesFeitas));
    calcularHorarios();
    Serial.println("[Sistema] Agendamento atualizado com sucesso!");
  }
}

// Variáveis globais adicionais
int endHour = 0;     // Hora final do último agendamento
int endMinute = 0;   // Minuto final do último agendamento

void calcularHorarios() {
    // Converter horários para minutos
    int minutosInicio = horaInicio * 60 + minutoInicio;
    int minutosFim = horaFim * 60 + minutoFim;

    // Ajustar para horários que ultrapassam a meia-noite
    if (minutosFim < minutosInicio) {
        minutosFim += 24 * 60; // Adiciona 1 dia em minutos
    }

    // Garantir pelo menos 1 porção
    porcoes = max(porcoes, 1); 

    // Calcular intervalo entre porções
    int tempoTotal = minutosFim - minutosInicio;
    int numIntervalos = max(porcoes - 1, 1); // Pelo menos 1 intervalo
    int intervaloPorcao = tempoTotal / numIntervalos;
    Serial.printf("peso diario %d\n", pesoDiario );
    Serial.printf("porções %d\n", porcoes );
    
    float pesoPorcao = static_cast<float>(pesoDiario) / porcoes;
    Serial.printf("peso de cada porção agendada %.2fg\n", pesoPorcao);  // Formatação corrigida
    
    // Limpar agendamento anterior
    Firebase.deleteNode(firebaseData, "/agendamento/porcoes");
    
    // Calcular horários das porções
    for (int i = 0; i < porcoes; i++) {
        int minutosAgendados;
        
        if (porcoes == 1) { // Caso única porção
            minutosAgendados = minutosInicio;
        } else { // Distribui entre início e fim
            minutosAgendados = minutosInicio + (i * intervaloPorcao);
        }
        
        // Ajustar para formato 24h
        horariosPorcoes[i].hora = (minutosAgendados / 60) % 24;
        horariosPorcoes[i].minuto = minutosAgendados % 60;
        horariosPorcoes[i].peso = pesoPorcao;
        // Depuração após atribuição
        Serial.printf("[DEBUG] Após atribuição -> Porção %d: Hora: %02d:%02d - Peso: %.2f g\n", 
               i + 1, horariosPorcoes[i].hora, horariosPorcoes[i].minuto, horariosPorcoes[i].peso);
        
        // Atualizar Firebase
        String path = "/agendamento/porcoes/" + String(i);
        Firebase.setInt(firebaseData, path + "/hora", horariosPorcoes[i].hora);
        Firebase.setInt(firebaseData, path + "/minuto", horariosPorcoes[i].minuto);
        Firebase.setFloat(firebaseData, path + "/peso", horariosPorcoes[i].peso);
    }

    // Atualizar número total de porções no Firebase
    Firebase.setInt(firebaseData, "/agendamento/numPorcoes", porcoes);

    // Definir horário final do ciclo
    endHour = horariosPorcoes[porcoes - 1].hora;
    endMinute = horariosPorcoes[porcoes - 1].minuto;

    Serial.printf("Agendamento concluído! %d porções. Última: %02d:%02d\n", 
                 porcoes, endHour, endMinute);
}
void controlarMotor(float tempoSegundos) {
  static bool em_execucao = false;
  
  if (tempoSegundos > 0 && !em_execucao) {
    em_execucao = true;
    Firebase.setInt(firebaseData, "/motor", 1);
    Serial.printf("\n[MOTOR] Ligado por %.1f segundos", tempoSegundos);
    // Ligar o motor
    digitalWrite(MOTOR_PIN, HIGH);
    
    // Bloquear execução pelo tempo especificado
    unsigned long inicio = millis();
    while(millis() - inicio < (unsigned long)(tempoSegundos * 1000)) {
      delay(1); // Manter verificação de outras tarefas
      // Insira aqui chamadas críticas que precisam executar durante a espera
      //yield(); // Manter estabilidade do WiFi/RTOS
    }

    // Desligar o motor
    digitalWrite(MOTOR_PIN, LOW);
    Firebase.setInt(firebaseData, "/motor", 0);
    Serial.println("\n[MOTOR] Desligado");
    motor_state = 0; // Resetar estado após operação
    em_execucao = false;
  }
  
}



void checkMotor() {
  if (Firebase.getInt(firebaseData, "/motor")) {
    int novo_estado = firebaseData.intData();
    
    if(novo_estado == 1 && motor_state == 0) {
      // Calcular peso equivalente usando a fórmula de calibração
      float pesoCalculado = 0;
      if(constanteA != 0) { // Evitar divisão por zero
        pesoCalculado = (tempo_acionamento - constanteB) / constanteA;
      }
      
      controlarMotor(tempo_acionamento);  
      
      
      Firebase.setIntAsync(firebaseData, "/motor", 0);
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
// Função auxiliar para resetar valores
void resetarCalibracao() {
  Firebase.setFloat(firebaseData, "/calibracao/peso1", 0);
  Firebase.setFloat(firebaseData, "/calibracao/peso2", 0);
  Firebase.setInt(firebaseData, "/calibracao/etapa", 0);
  //Firebase.setFloat(firebaseData, "/calibracao/tempo1", 0);
  //Firebase.setFloat(firebaseData, "/calibracao/tempo2", 0);
  delay(500); // Tempo para atualização do Firebase
}
// Variáveis globais adicionadas
bool calibracaoAtiva = false;
unsigned long calibracaoDelay = 100;

void handleCalibracao() {
  static int etapa = 0;
  static float peso1 = 0, peso2 = 0;
  static float tempo1 = 0, tempo2 = 0;
  
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
      if (Firebase.getFloat(firebaseData, "/calibracao/tempo1")) {
        tempo1 = firebaseData.floatData();
        controlarMotor(tempo1);
        calibracaoDelay = millis() + static_cast<unsigned long>(tempo1 * 1000.0) + 2000;
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
      if (Firebase.getFloat(firebaseData, "/calibracao/tempo2")) {
        tempo2 = firebaseData.floatData();
        controlarMotor(tempo2);
        calibracaoDelay = millis() + static_cast<unsigned long>(tempo1 * 1000.0) + 2000;
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
        float constanteA =  (tempo2 - tempo1)/(peso2 - peso1) ;
        float constanteB = tempo1 - (constanteA * peso1 );
        
        Firebase.setFloat(firebaseData, "/calibracao/constanteA", constanteA);
        Firebase.setFloat(firebaseData, "/calibracao/constanteB", constanteB);
        
        Serial.print("Constantes calculadas - A: ");
        Serial.print(constanteA);
        Serial.print(" B: ");
        Serial.println(constanteB);
        delay(1000);
      // Resetar valores
        Firebase.setFloat(firebaseData, "/calibracao/peso1", 0);
        Firebase.setFloat(firebaseData, "/calibracao/peso2", 0);
        Firebase.setInt(firebaseData, "/calibracao/etapa", 0);
        Firebase.setBool(firebaseData, "/calibracao/ativa", false);
        calibracaoAtiva = false;
      }
      
      
      
      Serial.println("Calibração concluída!");
      break;
  }
}

void verificarPorcoes() {
    DateTime agora = rtc.now();
    int currentDay = agora.day();
    int currentHour = agora.hour();
    int currentMinute = agora.minute();

    //Serial.printf("\n[DEBUG] Horário atual: %02d:%02d (Dia %d)\n", currentHour, currentMinute, currentDay);

    // 1. Verificar mudança de dia
    if (ultimoDia != currentDay) {
       // Serial.println("\n[DEBUG] Mudança de dia detectada!");
        memset(porcoesFeitas, false, sizeof(porcoesFeitas)); // Resetar todas as porções
        ultimoDia = currentDay; // Atualizar último dia registrado
        
       // Serial.printf("[DEBUG] Resetando %d porções!\n", porcoes);
        
        // Atualizar Firebase com novo ciclo
        char dataAtual[11];
       // sprintf(dataAtual, "%02d-%02d-%04d", agora.day(), agora.month(), agora.year());
        Firebase.setString(firebaseData, "/status/ultimoReset", dataAtual);
        //Serial.printf("[DEBUG] Firebase atualizado com novo reset: %s\n", dataAtual);
    }

    // 2. Verificar se está dentro do horário de operação
    bool dentroHorario = true;
    if (horaFim < horaInicio) { // Período que cruza a meia-noite
        dentroHorario = !(currentHour >= horaFim && currentHour < horaInicio);
    } else {
        dentroHorario = (currentHour >= horaInicio && currentHour <= horaFim);
    }
    //Serial.printf("[DEBUG] Dentro do horário de operação: %s\n", dentroHorario ? "SIM" : "NÃO");
    
    // 3. Verificar motor ocupado
    if (motor_ligado_ate > 0) {
        Serial.printf("[DEBUG] Motor ocupado por mais %d segundos\n", 
                     (motor_ligado_ate - millis())/1000);
        return;
    }

    // 4. Verificar cada porção
    for (int i = 0; i < porcoes; i++) {
        bool horarioIgual = (currentHour == horariosPorcoes[i].hora) && 
                           (currentMinute == horariosPorcoes[i].minuto);
        
        
        if (horarioIgual && !porcoesFeitas[i]) {
            Serial.printf("[DEBUG] Correspondência encontrada para porção %d!\n", i+1);
            float segundos = (horariosPorcoes[i].peso * constanteA) + constanteB ;
            //segundos = fmaxf(0.1, segundos); // 0.1s mínimo ao invés de 1s
            Serial.printf("[DEBUG] Peso liberado: %.1f g\n", horariosPorcoes[i].peso);
            Serial.printf("[DEBUG] Tempo calculado para acionamento do motor: %.1f s\n", segundos);
            
            // Executar
            controlarMotor(segundos);
            Serial.printf("[DEBUG] Motor acionado por %.1f segundos\n", segundos);
            
            porcoesFeitas[i] = true;
            if (i == porcoes-1) { // Última porção
                atualizarVariaveis(); // Força recarregar configuração
                Serial.println("[DEBUG] Última porção! Agendamento recarregado.");
            }
            // Registrar no Firebase
            char timestamp[30];
            sprintf(timestamp, "%02d-%02d_%02d-%02d", 
                    agora.day(), agora.month(), currentHour, currentMinute);
            Serial.printf("[DEBUG] Gerando timestamp: %s\n", timestamp);
            
            FirebaseJson registro;
            registro.set("peso", horariosPorcoes[i].peso);
            registro.set("tempo", segundos);
            registro.set("timestamp", timestamp);
            registro.set("tipo", "agendado"); // Adicionar esta linha
            sprintf(timestamp, "%02d-%02d-%02d_%02d-%02d-%02d", 
                    agora.year()%100, agora.month(), agora.day(),
                    currentHour, currentMinute, agora.second());
            
            String firebasePath = "/historico/";
            firebasePath += timestamp;
            
            Firebase.setJSON(firebaseData, firebasePath, registro);
            Serial.printf("[DEBUG] Firebase atualizado com registro em %s\n", firebasePath.c_str());
            
            break; // Evitar múltiplos acionamentos no mesmo minuto
        }
    }
}

// Funções auxiliares
bool checkHorario(DateTime atual, Porcao programado) {
  return (atual.hour() == programado.hora && 
          atual.minute() == programado.minuto);
}
void reconectarWiFi() {
  static unsigned long ultimaTentativa = 0;
  const unsigned long intervalo = 30; // 30 segundos
  
  if (WiFi.status() != WL_CONNECTED && 
      (millis() - ultimaTentativa) > intervalo) {
      
    Serial.println("Reconectando WiFi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    ultimaTentativa = millis();
    
    if (WiFi.status() == WL_CONNECTED) {
      Firebase.reconnectWiFi(true);
      Serial.println("WiFi reconectado!");
    }
  }
}
void registrarPorcaoFirebase(int index, DateTime atual) {
  char dataHora[20];
  snprintf(dataHora, sizeof(dataHora), "%02d-%02d-%04d_%02d-%02d",
           atual.day(),
           atual.month(),
           atual.year(),
           horariosPorcoes[index].hora,
           horariosPorcoes[index].minuto);

  String path = "/historico/" + String(dataHora);
  
  Firebase.setInt(firebaseData, path + "/porcao", index + 1);
  Firebase.setInt(firebaseData, path + "/peso_programado", horariosPorcoes[index].peso);
  Firebase.setInt(firebaseData, path + "/hora_execucao", atual.hour());
  Firebase.setInt(firebaseData, path + "/minuto_execucao", atual.minute());

  Serial.printf("Registro: %s | Porção: %d | Peso: %dg\n", 
               dataHora, index+1, horariosPorcoes[index].peso);
}

// Firebase e Configurações
void carregarConfiguracoesIniciais() {
  if(Firebase.getFloat(firebaseData, "/calibracao/constanteA")) 
    constanteA = firebaseData.floatData();
  if(Firebase.getFloat(firebaseData, "/calibracao/constanteB")) 
    constanteB = firebaseData.floatData();
  if(Firebase.getFloat(firebaseData, "/calibracao/tempo_acionamento")) 
    tempo_acionamento = firebaseData.floatData();
  
  Firebase.setBool(firebaseData, "/config/ativa", true);
  atualizarVariaveis();
}
void acionarPorPesoFirebase() {
  static unsigned long ultimaVerificacao = 0;
  const unsigned long intervalo = 2000; // Verifica a cada 2 segundos
  
  // Verificar periodicamente
  if(millis() - ultimaVerificacao >= intervalo) {
    if(Firebase.getFloat(firebaseData, "/controle/peso")) {
      float pesoDesejado = firebaseData.floatData();
      
      if(pesoDesejado > 0) {
        // Calcular tempo de acionamento
        float tempo = pesoDesejado  *  constanteA + constanteB;
        //tempo = fmax(tempo, 0.1f); // Tempo mínimo de 0.1 segundos
        
        Serial.printf("\n[FIREBASE] Peso recebido: %.1fg = %.1fs", pesoDesejado, tempo);
        
        // Acionar motor
        controlarMotor(tempo);
        // Adicione no final da função:
      if(pesoDesejado >= 0) { // Se peso foi especificado
        registrarHistorico(pesoDesejado, tempo);
      }
        
        // Resetar valor no Firebase
        Firebase.setFloat(firebaseData, "/controle/peso", 0);
      }
    } else {
      Serial.println("\n[ERRO] Falha ao ler peso do Firebase");
    }
    
    ultimaVerificacao = millis();
  }
}

void registrarHistorico(float peso, float tempo) {
    DateTime agora = rtc.now();
    
    // Gerar timestamp único
    char timestamp[30];
    sprintf(timestamp, "%02d-%02d-%02d_%02d-%02d-%02d", 
            agora.year()%100, agora.month(), agora.day(),
            agora.hour(), agora.minute(), agora.second());
    
    // Criar registro JSON
    FirebaseJson registro;
    registro.set("peso", peso);
    registro.set("tempo", tempo);
    registro.set("timestamp", timestamp);
    registro.set("tipo", "manual"); // Adicionar esta linha
    // Enviar para Firebase
    String firebasePath = "/historico/" + String(timestamp);
    if(Firebase.setJSON(firebaseData, firebasePath, registro)) {
        Serial.printf("\n[HISTÓRICO] Registro manual: %.2fg em %.2fs", peso, tempo);
    }
}


void setup() {
  Serial.begin(115200);
  
  pinMode(LED_AGUA_PIN, OUTPUT);
  pinMode(LED_ESTUFA_PIN, OUTPUT);
  pinMode(MOTOR_PIN, OUTPUT);  // Nome alteradoconstanteB
  
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
  if (!Firebase.get(firebaseData, "/calibracao/tempo_acionamento")) {
  Firebase.setInt(firebaseData, "/calibracao/tempo_acionamento", 5);
  }
  if (!Firebase.get(firebaseData, "/controle/peso")) {
  Firebase.setFloat(firebaseData, "/controle/peso", 0);
  }

  // Inicializar RTC
  if (!rtc.begin()) {
    Serial.println("Erro ao iniciar RTC!");
    while(1);
  }
  Serial.println("RTC inicializado");
  // Carregar os valores do Firebase e calcular os horários
    carregarConfiguracoesIniciais();
    
}

void loop() {
  static unsigned long lastUpdate = 0;
  static unsigned long lastProgramUpdate = 0;
  static unsigned long ultimaLeituraRTC = 0;
  const unsigned long intervaloRTC = 1000; // 1 segundo
  
  // Atualizar RTC a cada segundo
  if (millis() - ultimaLeituraRTC > intervaloRTC) {
    DateTime agora = rtc.now();
    //Serial.printf("Horário real: %02d:%02d (Dia %d)\n", 
      //           agora.hour(), agora.minute(), agora.day());
    ultimaLeituraRTC = millis();
  }
  
  reconectarWiFi();
  
  fetchLimites();
  checkSensores();
  checkMotor();
  
  atualizarVariaveis();
  handleCalibracao();
  verificarPorcoes();
  acionarPorPesoFirebase();  // Nova função
}
