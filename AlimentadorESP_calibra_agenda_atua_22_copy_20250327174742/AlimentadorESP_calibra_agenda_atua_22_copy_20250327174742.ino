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
// Adicione esta declaração global no início do código
char dataHora[20]; 


Limites agua_limites = {30.0, 20.0};
Limites estufa_limites = {35.0, 25.0};
Limites externa_limites = {40.0, 15.0};


// Variáveis do motor
int motor_state = 0;
unsigned long motor_ligado_ate = 0;  // Timer para controle de tempo
// Variáveis do banco de dados
int horaInicio, minutoInicio, horaFim, minutoFim;
int intervalo, lancamentos, pesoDiario, porcoes;
bool ativa;

// Estrutura para armazenar os horários das porções
struct Porcao {
    int hora;
    int minuto;
    int peso;
};
static int ultimasPorcoes = 0;  // Variável estática para controle
Porcao horariosPorcoes[50]; // Suporte para até 10 porções
static int ultimoDia = -1;
static bool porcoesFeitas[50] = {false};
// Protótipos de funções
void fetchLimites();
void checkSensores();
void checkMotor();
void verificarTempoMotor();
void handleCalibracao();
void verificarPorcoes();
bool checkHorario(DateTime atual, Porcao programado);
void registrarPorcaoFirebase(int index, DateTime atual);
void acionarMotorPorcao(int index);
void acionarMotorPorTempo(int segundos);
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
  Serial.println("RTC inicializado");
  // Carregar os valores do Firebase e calcular os horários
    atualizarVariaveis();
    
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
  verificarTempoMotor();
  atualizarVariaveis();
  handleCalibracao();
  verificarPorcoes();
  delay(100);
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
    
    int pesoPorcao = pesoDiario / porcoes;

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

        // Atualizar Firebase
        String path = "/agendamento/porcoes/" + String(i);
        Firebase.setInt(firebaseData, path + "/hora", horariosPorcoes[i].hora);
        Firebase.setInt(firebaseData, path + "/minuto", horariosPorcoes[i].minuto);
        Firebase.setInt(firebaseData, path + "/peso", horariosPorcoes[i].peso);
    }

    // Atualizar número total de porções no Firebase
    Firebase.setInt(firebaseData, "/agendamento/numPorcoes", porcoes);

    // Definir horário final do ciclo
    endHour = horariosPorcoes[porcoes - 1].hora;
    endMinute = horariosPorcoes[porcoes - 1].minuto;

    Serial.printf("Agendamento concluído! %d porções. Última: %02d:%02d\n", 
                 porcoes, endHour, endMinute);
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
void verificarPorcoes() {
    DateTime agora = rtc.now();
    int currentDay = agora.day();
    int currentHour = agora.hour();
    int currentMinute = agora.minute();

    // Debug: Exibir horário atual
    //Serial.printf("\nHorário atual: %02d:%02d (Dia %d)", currentHour, currentMinute, currentDay);

    // 1. Verificar mudança de dia
    if (ultimoDia != currentDay) {
        memset(porcoesFeitas, false, sizeof(porcoesFeitas)); // Resetar todas as porções
        ultimoDia = currentDay; // Atualizar último dia registrado
        
        Serial.println("\n--- NOVO DIA DETECTADO ---");
        Serial.printf("Resetando %d porcoes!\n", porcoes);
        
        // Atualizar Firebase com novo ciclo
        char dataAtual[11];
        sprintf(dataAtual, "%02d-%02d-%04d", agora.day(), agora.month(), agora.year());
        Firebase.setString(firebaseData, "/status/ultimoReset", dataAtual);
    }

    // 2. Verificar se está dentro do horário de operação
    bool dentroHorario = true;
    if (horaFim < horaInicio) { // Período que cruza a meia-noite
        dentroHorario = !(currentHour >= horaFim && currentHour < horaInicio);
    } else {
        dentroHorario = (currentHour >= horaInicio && currentHour <= horaFim);
    }

    

    // 3. Verificar motor ocupado
    if (motor_ligado_ate > 0) {
        Serial.printf("Motor ocupado por mais %d segundos", 
                     (motor_ligado_ate - millis())/1000);
        return;
    }

    // 4. Verificar cada porção
    for (int i = 0; i < porcoes; i++) {
        bool horarioIgual = (currentHour == horariosPorcoes[i].hora) && 
                           (currentMinute == horariosPorcoes[i].minuto);
        
        if (horarioIgual && !porcoesFeitas[i]) {
            // Calcular tempo de acionamento
            float constanteA = Firebase.getFloat(firebaseData, "/calibracao/constanteA") ? 
                              firebaseData.floatData() : 2.5;
            float constanteB = Firebase.getFloat(firebaseData, "/calibracao/constanteB") ? 
                              firebaseData.floatData() : 10.0;
            
            int segundos = max(1, static_cast<int>(
                (horariosPorcoes[i].peso - constanteB) / constanteA
            ));

            // Executar
            acionarMotorPorTempo(segundos);
            porcoesFeitas[i] = true;
            if (i == porcoes-1) { // Última porção
            atualizarVariaveis(); // Força recarregar configuração
            Serial.println("[Sistema] Última porção! Agendamento recarregado.");
        }
            // Registrar no Firebase
            char timestamp[30];
            sprintf(timestamp, "%02d-%02d_%02d-%02d", 
                    agora.day(), agora.month(), currentHour, currentMinute);
            
            FirebaseJson registro;
            registro.set("peso", horariosPorcoes[i].peso);
            registro.set("tempo", segundos);
            registro.set("timestamp", timestamp);
            
            // Gerar timestamp formatado
        
        sprintf(timestamp, "%02d-%02d-%02d_%02d-%02d-%02d", 
                agora.year()%100, // Últimos 2 dígitos do ano
                agora.month(), 
                agora.day(),
                currentHour, 
                currentMinute,
                agora.second());

        // Criar caminho estruturado
        String firebasePath = "/historico/";
        firebasePath += timestamp;
        
        // Registrar com chave personalizada
        Firebase.setJSON(firebaseData, firebasePath, registro);
            
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
  const unsigned long intervalo = 30000; // 30 segundos
  
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

void acionarMotorPorcao(int index) {
  if (Firebase.ready() && 
      Firebase.getFloat(firebaseData, "/calibracao/constanteA") &&
      Firebase.getFloat(firebaseData, "/calibracao/constanteB")) {
        
    float constanteA = firebaseData.floatData();
    Firebase.getFloat(firebaseData, "/calibracao/constanteB");
    float constanteB = firebaseData.floatData();

    if (constanteA > 0) {
      int segundos = (horariosPorcoes[index].peso - constanteB) / constanteA;
      if (segundos > 0) {
        acionarMotorPorTempo(segundos);
        Firebase.setInt(firebaseData, "/historico/" + String(dataHora) + "/tempo_acionamento", segundos);
      }
    }
  }
}





// ... (outras funções se mantêm inalteradas)