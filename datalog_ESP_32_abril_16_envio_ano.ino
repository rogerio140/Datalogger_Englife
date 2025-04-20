#include <SD.h>
#include <SPI.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include "FS.h"  // Sistema de arquivos (como SPIFFS ou LittleFS)
#include <TimeLib.h>  // Para manipular o tempo e facilitar comparações


// Configuração do Firebase
//#define FIREBASE_HOST "https://datalogeer-default-rtdb.firebaseio.com/"
//#define FIREBASE_AUTH "AIzaSyAMAf30UG0YOqaxF-KgD5gfAhWhddN-5Qg"

// Configuração do Firebase
#define FIREBASE_HOST "https://datalogger-b81e3-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "nB1yab3NVNT5iOQ22Z2jbXcGlyM1XtbBw4F17ggT"

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

const int chipSelect = 5; // Pino CS do módulo SD no ESP32
File myFile;
RTC_DS1307 rtc;
// Variável global para armazenar a última mensagem enviada que não seja um ACK
String ultimaMensagemEnviada;
// Variáveis para controle de reconexão
int tentativasReconexao = 0;
const int maxTentativasReconexao = 5;
// Defina os pinos onde os sensores DS18B20 estão conectados
#define ONE_WIRE_BUS1 14
#define ONE_WIRE_BUS2 27
#define ONE_WIRE_BUS3 26
#define LED_PIN 17  // Pino do LED no ESP32
#define LED_ERRO_SD_PIN 33 // Pino do LED de erro do SD
// Inicializando os barramentos OneWire para os sensores
OneWire oneWire1(ONE_WIRE_BUS1);
OneWire oneWire2(ONE_WIRE_BUS2);
OneWire oneWire3(ONE_WIRE_BUS3);

// Inicializando as instâncias de sensores
DallasTemperature sensor1(&oneWire1);
DallasTemperature sensor2(&oneWire2);
DallasTemperature sensor3(&oneWire3);
// Configuração do WiFi
// Variáveis globais para Wi-Fi
String wifiSSID = "";
String wifiPassword = "";
// Variáveis globais para intervalo e nome do arquivo
unsigned long intervalo = 1; // Valor padrão em minutos
String nomeArquivo = "/datalog.txt";

DateTime ultimoEnvio; // Variável global para armazenar o horário do último envio
DateTime ultimaColeta;
DateTime ultimaColetaSemanal;
DateTime ultimaColetaMensal;

int intervaloMinutos = 1440;  // envio do pacote de dados para o grafico, as últimas 60 minutos (1 hora)
int tempoPersonalizado = 0.5; // envio do estado de cada sensor

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
// Função para inicializar o cartão SD
bool inicializarSD(int tentativasMax) {
    for (int tentativa = 1; tentativa <= tentativasMax; tentativa++) {
        if (SD.begin(chipSelect)) {
            Serial.println("Cartão SD inicializado com sucesso!");
            digitalWrite(LED_ERRO_SD_PIN, LOW); // Desliga LED de erro se conseguir
            return true;
        } else {
            digitalWrite(LED_ERRO_SD_PIN, HIGH); // Liga LED de erro
            Serial.print("Erro ao inicializar o cartão SD. Tentativa ");
            
            Serial.println(tentativa);
            delay(1000); // Aguarda 1 segundo antes de tentar novamente
        }
    }
    Serial.println("Falha ao inicializar o cartão SD após múltiplas tentativas.");
    return false;
}
void mostrarRegistrosPeriodo(const String& nomeArquivo, const String& periodo) {
    if (!SD.exists(nomeArquivo)) {
        Serial.println("Arquivo não encontrado!");
        return;
    }
    
    File file = SD.open(nomeArquivo, FILE_READ);
    if (!file) {
        Serial.println("Erro ao abrir o arquivo.");
        return;
    }
    
    DateTime agora = rtc.now();
    TimeSpan intervalo(0);

    // Define o intervalo com base no período escolhido
    if (periodo == "24h") {
        
        intervalo = TimeSpan(24 * 60 * 60); // 24 horas em segundos
    } else if (periodo == "semana") {
        intervalo = TimeSpan(7 * 24 * 60 * 60); // 7 dias
    } else if (periodo == "mes") {
        intervalo = TimeSpan(30 * 24 * 60 * 60); // 30 dias
    }else if (periodo == "ano") {
        intervalo = TimeSpan(24 * 60 * 60); // 24 h
    } else {
        Serial.println("Período inválido! Use '24h', 'semana' ou 'mes'.");
        file.close();
        return;
    }

    // Calcula o limite de tempo
    DateTime limite = agora - intervalo;
    
    Serial.print("Registros do período: ");
    Serial.println(periodo);
    
    while (file.available()) {
        String linha = file.readStringUntil('\n');
        linha.trim();
        
        // Ignora linhas em branco ou cabeçalhos
        if (linha.length() == 0 || linha.startsWith("Data")) continue;
        
        int dia, mes, ano, hora, minuto, segundo;
        float tempAgua, tempEstufa, tempExterna;
        
        // Formato de data e temperatura no arquivo
        if (sscanf(linha.c_str(), "%d/%d/%d\t%d:%d:%d\t%f\t%f\t%f", 
                   &dia, &mes, &ano, &hora, &minuto, &segundo, 
                   &tempAgua, &tempEstufa, &tempExterna) == 9) {
            
            DateTime dataRegistro(ano, mes, dia, hora, minuto, segundo);
            
            // Comparar data com o limite definido
            if (dataRegistro >= limite) {
                // Exibir os dados no serial
                Serial.println(linha);
                
                // Enviar os dados para o Firebase
                enviarDadosFirebase(linha, periodo);

            }
        }
    }
    
    file.close();
}

void carregarConfiguracoes() {
    myFile = SD.open("/conf.txt");
    if (myFile) {
        Serial.println("Arquivo de configuração encontrado!");
        while (myFile.available()) {
            String linha = myFile.readStringUntil('\n');
            linha.trim();
            if (linha.startsWith("INTERVALO=")) {
                intervalo = linha.substring(10).toInt();
            } else if (linha.startsWith("ARQUIVO=")) {
                nomeArquivo = linha.substring(8);
                nomeArquivo.trim();
                if (!nomeArquivo.endsWith(".txt")) {
                    nomeArquivo += ".txt";
                }
                if (!nomeArquivo.startsWith("/")) {
                    nomeArquivo = "/" + nomeArquivo;
                }
            } else if (linha.startsWith("WIFI_SSID=")) {
                wifiSSID = linha.substring(10);
                wifiSSID.trim();
            } else if (linha.startsWith("WIFI_PASSWORD=")) {
                wifiPassword = linha.substring(14);
                wifiPassword.trim();
            }
        }
        myFile.close();
        Serial.print("Intervalo de coleta: ");
        Serial.print(intervalo);
        Serial.println(" minutos");
        Serial.print("Arquivo de saída: ");
        Serial.println(nomeArquivo);
        Serial.print("Wi-Fi SSID: ");
        Serial.println(wifiSSID);
        Serial.print("Wi-Fi Password: ");
        Serial.println(wifiPassword);
    } else {
        Serial.println("Arquivo de configuração não encontrado. Usando valores padrão.");
    }
}
// Função para criar um arquivo
bool criarArquivo(const String& nomeArquivo) {
    if (SD.exists(nomeArquivo)) {
        Serial.println("Arquivo já existe.");
        return false; // Retorna false, indicando que não foi necessário criar
    }

    myFile = SD.open(nomeArquivo, FILE_WRITE);
    if (myFile) {
        Serial.println("Arquivo criado com sucesso!");
        myFile.close();
        return true;
    } else {
        Serial.println("Erro ao criar o arquivo.");
        return false;
    }
}

// Função para escrever no arquivo (adicionar nova linha)
bool escreverArquivo(const String& nomeArquivo, const String& conteudo) {
    myFile = SD.open(nomeArquivo, FILE_APPEND); // Modo FILE_APPEND para adicionar linha
    if (myFile) {
        myFile.println(conteudo);
        Serial.println("Conteúdo adicionado com sucesso no arquivo!");
        myFile.close();
        return true;
    } else {
        Serial.println("Erro ao abrir o arquivo para escrita.");
        return false;
    }
}

// Função para inicializar os sensores
void inicializarSensores() {
    sensor1.begin();
    sensor2.begin();
    sensor3.begin();
    pinMode(LED_PIN, OUTPUT); // Configura o LED como saída
}

// Função para obter temperaturas dos sensores DS18B20
float lerSensorTemperatura(DallasTemperature& sensor) {
    sensor.requestTemperatures();
    return sensor.getTempCByIndex(0);
}

// Função para obter data e hora formatada
String obterDataHora() {
    DateTime agora = rtc.now();
    char buffer[20];
    sprintf(buffer, "%02d/%02d/%04d\t%02d:%02d:%02d",
            agora.day(), agora.month(), agora.year(),
            agora.hour(), agora.minute(), agora.second());
    return String(buffer);
}

// Função para registrar leituras dos sensores no arquivo
void registrarLeituras() {
    // Obter data e hora formatada
    String dataHora = obterDataHora();

    // Obter as temperaturas dos sensores
    float tempAgua = lerSensorTemperatura(sensor1);
    float tempEstufa = lerSensorTemperatura(sensor2);
    float tempExterna = lerSensorTemperatura(sensor3);

    // Construir o registro no formato desejado
    char registroBuffer[50];
    sprintf(registroBuffer, "%s\t%.2f\t%.2f\t%.2f", 
            dataHora.c_str(), tempAgua, tempEstufa, tempExterna);
    String registro = String(registroBuffer);

    // Escrever o registro no arquivo
    if (escreverArquivo(nomeArquivo, registro)) {
        Serial.println("Registro gravado: " + registro);
        digitalWrite(LED_PIN, HIGH); // Acende o LED para indicar sucesso
        delay(100);
        digitalWrite(LED_PIN, LOW); // Apaga o LED
    } else {
        Serial.println("Falha ao gravar o registro no arquivo.");
    }

    // Envia os dados para o Firebase
    enviarTemperatura("/sensores/temperatura_3", tempAgua);
    enviarTemperatura("/sensores/temperatura_2", tempEstufa);
    enviarTemperatura("/sensores/temperatura_1", tempExterna);
}
// Função para registrar leituras dos sensores no arquivo
void enviarLeituras() {
    // Obter data e hora formatada
    String dataHora = obterDataHora();
    delay(10000);
    // Obter as temperaturas dos sensores
    float tempAgua = lerSensorTemperatura(sensor1);
    float tempEstufa = lerSensorTemperatura(sensor2);
    float tempExterna = lerSensorTemperatura(sensor3);
   // Envia os dados para o Firebase
    enviarTemperatura("/sensores/temperatura_3", tempAgua);
    enviarTemperatura("/sensores/temperatura_2", tempEstufa);
    enviarTemperatura("/sensores/temperatura_1", tempExterna);
}
// Função para verificar se é hora de uma nova coleta
bool verificarHorarioColeta() {
    DateTime agora = rtc.now();
    TimeSpan diferenca = agora - ultimaColeta;
    return diferenca.totalseconds() >= (intervalo * 60);
}
// Função para verificar se é hora de uma nova coleta com um tempo personalizado (em minutos)
bool verificarHorarioColetaCustom(int tempoMinutos) {
    DateTime agora = rtc.now();
    TimeSpan diferenca = agora - ultimoEnvio;
    return diferenca.totalseconds() >= (tempoMinutos * 60);
}


void conectarWiFi() {
    if (wifiSSID == "" || wifiPassword == "") {
        Serial.println("SSID ou senha do Wi-Fi não configurados. Verifique o arquivo de configuração.");
        return;
    }

    // Verifica se já está conectado
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    Serial.print("Conectando ao WiFi");
    //digitalWrite(LED_PIN, HIGH);  // Acende o LED no início da tentativa

    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    
    // Timeout de 5 tentativas
    int timeout = 5;
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        Serial.print(".");
        delay(1000);
        timeout--;
    }

    //digitalWrite(LED_PIN, LOW);  // Apaga o LED ao final da tentativa

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi conectado!");
        tentativasReconexao = 0;
    } else {
        Serial.println("\nFalha na conexão. Tentativas restantes: " + String(maxTentativasReconexao - tentativasReconexao));
    }
}

void configurarFirebase() {
    config.host = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
}
void enviarDadosFirebase(const String& linha, const String& periodo) {
    int dia, mes, ano, hora, minuto, segundo;
    float tempAgua, tempEstufa, tempExterna;
    
    // Extrair os dados da linha com sscanf
    if (sscanf(linha.c_str(), "%d/%d/%d\t%d:%d:%d\t%f\t%f\t%f", 
               &dia, &mes, &ano, &hora, &minuto, &segundo, 
               &tempAgua, &tempEstufa, &tempExterna) == 9) {
        
        // Montar a data e hora no formato desejado
        String dataHora = String(ano) + "-" + (mes < 10 ? "0" : "") + String(mes) + "-" + 
                          (dia < 10 ? "0" : "") + String(dia) + "_" + 
                          (hora < 10 ? "0" : "") + String(hora) + "-" + 
                          (minuto < 10 ? "0" : "") + String(minuto) + "-" + 
                          (segundo < 10 ? "0" : "") + String(segundo);
        
        // Definir o nó correto com base no período
        String caminhoBase;
        if (periodo == "24h") {
            caminhoBase = "/temperaturas/";
            //limparCaminho("/temperaturas/");
        } else if (periodo == "semana") {

            caminhoBase = "/temperaturas_semana/";
            // limparCaminho("/temperaturas_semana/");
        } else if (periodo == "mes") {
            caminhoBase = "/temperaturas_mes/";
            // limparCaminho("/temperaturas_mes/");
        } else if (periodo == "ano") {
            caminhoBase = "/temperaturas_ano/";
            // limparCaminho("/temperaturas_mes/");
        } else {
            Serial.println("Erro: Período inválido!");
             
            return;
        }

        // Criar os caminhos completos
        String caminhoTemperaturaAgua = caminhoBase + dataHora + "/agua";
        String caminhoTemperaturaEstufa = caminhoBase + dataHora + "/estufa";
        String caminhoTemperaturaExterna = caminhoBase + dataHora + "/externa";
       
        // Enviar os dados para o Firebase
        delay(100);
        enviarTemperatura(caminhoTemperaturaAgua.c_str(), tempAgua);
        delay(100);
        enviarTemperatura(caminhoTemperaturaEstufa.c_str(), tempEstufa);
        delay(100);
        enviarTemperatura(caminhoTemperaturaExterna.c_str(), tempExterna);
    } else {
        Serial.println("Erro ao processar a linha de dados.");
    }
}



void limparCaminho(const char* caminho) {
    if (Firebase.deleteNode(firebaseData, caminho)) {
        Serial.print("Caminho ");
        Serial.print(caminho);
        Serial.println(" apagado com sucesso!");
    } else {
        Serial.print("Erro ao apagar ");
        Serial.print(caminho);
        Serial.print(": ");
        Serial.println(firebaseData.errorReason());
    }
}

void enviarTemperatura(const char* caminho, float valor) {
    int max_tentativas = 3; // Número máximo de tentativas
    int delay_entre_tentativas = 1000; // Delay entre tentativas (5 segundos)

    for (int tentativa = 1; tentativa <= max_tentativas; tentativa++) {
        if (Firebase.setFloat(firebaseData, caminho, valor)) {
            return; // Sai da função se o envio for bem-sucedido
        } else {
            if (tentativa < max_tentativas) {
                delay(delay_entre_tentativas); // Aguarda antes de tentar novamente
            }
        }
    }
}
void enviarString(const char* caminho, const char* valor) {
    int max_tentativas = 3;
    int delay_entre_tentativas = 2000;

    for (int tentativa = 1; tentativa <= max_tentativas; tentativa++) {
        if (Firebase.setString(firebaseData, caminho, valor)) {
            return;
        } else {
            if (tentativa < max_tentativas) {
                delay(delay_entre_tentativas);
            }
        }
    }
}

void enviarInteiro(const char* caminho, int valor) {
    int max_tentativas = 3;
    int delay_entre_tentativas = 2000;

    for (int tentativa = 1; tentativa <= max_tentativas; tentativa++) {
        if (Firebase.setInt(firebaseData, caminho, valor)) {
            return;
        } else {
            if (tentativa < max_tentativas) {
                delay(delay_entre_tentativas);
            }
        }
    }
}
void setup() {
    Serial.begin(115200);
    // Configura o pino do LED de erro do SD como saída
    pinMode(LED_ERRO_SD_PIN, OUTPUT);
    digitalWrite(LED_ERRO_SD_PIN, LOW); // Inicia desligado

    // Inicializar RTC
    if (!rtc.begin()) {
        Serial.println("Não foi possível inicializar o RTC.");
        while (1);
    }
    if (!rtc.isrunning()) {
        Serial.println("O RTC não está em execução. Configurando data e hora.");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    // Inicializar o cartão SD com 3 tentativas
    if (!inicializarSD(5)) {
        Serial.println("Inicialização falhou. Verifique o cartão SD.");
        while (1); // Para o programa se não conseguir inicializar o SD
    }

    // Carregar configurações do arquivo
    carregarConfiguracoes();
    // Atualiza o RTC
    //atualizarRTCviaNTP();
    // Inicializar os sensores
    inicializarSensores();

    // Criar arquivo
    if (!SD.exists(nomeArquivo)) {
        if (criarArquivo(nomeArquivo)) {
            // Escrever cabeçalho no arquivo se foi criado
            const String cabeçalho = "Data\tHora\tTemp_Água\tTemp_Estufa\tTemp_Externa";
            if (!escreverArquivo(nomeArquivo, cabeçalho)) {
                Serial.println("Falha ao escrever o cabeçalho no arquivo.");
                while (1); // Para o programa se não conseguir escrever no arquivo
            }
        } else {
            Serial.println("Falha ao criar o arquivo.");
            while (1); // Para o programa se não conseguir criar o arquivo
        }
    }

    // Conectar ao Wi-Fi
    conectarWiFi();
    
    // Configurar o Firebase
    configurarFirebase();

    registrarLeituras();
    enviarTemperatura("intervalo/intervalo", intervalo);
    // Obter e enviar informações da rede Wi-Fi
    if (WiFi.status() == WL_CONNECTED) {
        String ssid = WiFi.SSID();
        int rssi = WiFi.RSSI();
        enviarString("rede/ssid", ssid.c_str());
        enviarInteiro("rede/rssi", rssi);
    }
    Serial.println("Enviando coleta diária!");
    //limparCaminho("/temperaturas/");
    //mostrarRegistrosPeriodo(nomeArquivo, "24h"); // Últimas 24 horas
    //limparCaminho("/temperaturas_semana/");
    //mostrarRegistrosPeriodo(nomeArquivo, "semana"); // Últimos 7 dias
    //limparCaminho("/temperaturas_mes/");
    //mostrarRegistrosPeriodo(nomeArquivo, "mes"); // Últimos 30 dias
     // Se o Wi-Fi estiver conectado, realiza as operações
        if (WiFi.status() == WL_CONNECTED) {
      mostrarRegistrosPeriodo(nomeArquivo, "ano"); // Últimos 30 dias
      Serial.println("\n coleta setup realizada!");
        }
    ultimaColeta = rtc.now();
    ultimoEnvio = rtc.now();
    ultimaColetaSemanal= rtc.now();
    ultimaColetaMensal= rtc.now();
}
void loop() {
    DateTime agora = rtc.now();
    Serial.println("\n Iniciando Loop!");
    // Verifica se é hora de uma nova coleta diária (24h)
    if (verificarHorarioColetaCustom(intervalo) && agora != ultimoEnvio) {
        Serial.println("Hora de realizar uma nova coleta diária!");
        registrarLeituras();
        digitalWrite(LED_PIN, HIGH);
        delay(1000);
        conectarWiFi(); // Força nova tentativa de conexão
        digitalWrite(LED_PIN, LOW); 

        // Se o Wi-Fi estiver conectado, realiza as operações
        if (WiFi.status() == WL_CONNECTED) {
           // registrarLeituras();
           Serial.println("\n coleta do intervalo!");
            //mostrarRegistrosPeriodo(nomeArquivo, "ano"); // Últimos 30 dias
            enviarLeituras();
            int rssi = WiFi.RSSI();
            enviarInteiro("rede/rssi", rssi);
            //limparCaminho("/temperaturas/");
            //mostrarRegistrosPeriodo(nomeArquivo, "24h"); // Últimas 24 horas
            mostrarRegistrosPeriodo(nomeArquivo, "ano"); // Últimos 30 dias
            //limparCaminho("/temperaturas_semana/");
            //mostrarRegistrosPeriodo(nomeArquivo, "semana"); // Últimos 7 dias

            //limparCaminho("/temperaturas_mes/");
            //mostrarRegistrosPeriodo(nomeArquivo, "mes"); // Últimos 30 dias
            

            //mostrarRegistrosPeriodo(nomeArquivo, "ano"); // Últimos 30 dias
        }

        
        ultimoEnvio = agora;
    }
    // Verifica se é hora de uma coleta personalizada
    if (verificarHorarioColetaCustom(tempoPersonalizado) && agora != ultimaColeta) {
        // Tenta reconectar ao Wi-Fi se não estiver conectado
         // Tenta reconectar apenas se não excedeu o limite de tentativas
         Serial.println("\n Verificando conexão!");
         
        conectarWiFi(); // Força nova tentativa de conexão
        // Obter e enviar informações da rede Wi-Fi
        
        // Se o Wi-Fi estiver conectado, realiza as operações
        if (WiFi.status() == WL_CONNECTED) {
            enviarLeituras();
            // Obter e enviar informações da rede Wi-Fi       
            //String ssid = WiFi.SSID();
            int rssi = WiFi.RSSI();
            enviarInteiro("rede/rssi", rssi);
            //enviarString("rede/ssid", ssid.c_str());
            
        } 

        ultimaColeta = agora;
    }
}