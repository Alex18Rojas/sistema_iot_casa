/* =====================================================================
 * ARCHIVO   : SistemaIoT_Casa.ino
 * PROYECTO  : Sistema IoT de seguridad doméstica
 * DESTINO   : NodeMCU V3 con chip ESP8266Mod
 *
 * PROPÓSITO
 * ---------
 * Este firmware corre dentro del microcontrolador NodeMCU y se encarga de
 * controlar de manera autónoma los dos puntos de acceso de la vivienda
 * (puerta principal y garaje). El microcontrolador es el "cerebro" del
 * sistema: lee sensores, acciona actuadores y se comunica con la nube.
 *
 * ARQUITECTURA DEL SISTEMA
 * ------------------------
 * El sistema completo tiene cuatro piezas que se comunican entre sí:
 *
 *   [Celular]  <->  [Firebase]  <->  [Puente Node.js]  <->  [Broker MQTT]  <->  [ESP8266]
 *                                                                               ^^^^^^^^^
 *                                                                               ESTA PIEZA
 *
 * El ESP8266 NO habla directamente con Firebase. Publica mensajes en el
 * broker MQTT (Mosquitto en HiveMQ Cloud) y el puente Node.js los traduce
 * al lenguaje de Firebase, y viceversa con los comandos del usuario.
 *
 * RESPONSABILIDADES DE ESTE FIRMWARE
 * -----------------------------------
 *   1. Vigilar el sensor IR de cada punto de acceso para detectar
 *      movimiento sin autorización.
 *   2. Reaccionar ante una intrusión activando el buzzer (alarma local).
 *   3. Procesar comandos de apertura/cierre que llegan desde la app web,
 *      validando una clave local como segunda barrera de seguridad.
 *   4. Controlar físicamente la cerradura electromagnética (vía relé)
 *      y, en el caso del garaje, el portón (vía servomotor).
 *   5. Informar a la nube el estado actual del sistema en tiempo real.
 *
 * COMPONENTES FÍSICOS QUE CONTROLA
 * ---------------------------------
 *   Punto: Puerta principal
 *     - Sensor IR de movimiento     (entrada digital)
 *     - Cerradura electromagnética  (salida vía relé)
 *     - Buzzer KY-006 para alarma   (salida PWM)
 *
 *   Punto: Garaje
 *     - Sensor IR de movimiento     (entrada digital)
 *     - Cerradura electromagnética  (salida vía relé)
 *     - Buzzer KY-006 para alarma   (salida PWM)
 *     - Servomotor EMAX ES08MA II   (salida PWM, posicional)
 *
 * LIBRERÍAS REQUERIDAS
 * --------------------
 *   - PubSubClient   (Nick O'Leary)        — cliente MQTT
 *   - ArduinoJson    (Benoît Blanchon v6)  — parseo y construcción JSON
 *   - Servo          (incluida en core)    — control del servomotor del garaje
 *   - ESP8266WiFi    (incluida en core)    — conectividad WiFi
 *   - WiFiClientSecure (incluida en core)  — cliente TCP con cifrado TLS
 *
 * PLACA EN ARDUINO IDE
 * --------------------
 *   NodeMCU 1.0 (ESP-12E Module)
 *
 * AUTORES
 * -------
 *   Manuela Rivera Gómez
 *   Janier Alexander Rojas Giraldo
 *   Luis Alfonso Ortiz Ruiz
 *
 *   Instituto Tecnológico Metropolitano · Medellín, Colombia
 * ===================================================================== */

#include "config.h"
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Servo.h>

#if USE_TLS
  #include <WiFiClientSecure.h>
#endif

// [LAB] Servicio web de depuracion (uso exclusivo de laboratorio)
#include <ESP8266WebServer.h>


/* =====================================================================
 *  SECCIÓN 1 — DEFINICIÓN DE TIPOS Y ESTRUCTURAS DE DATOS
 *
 *  En esta sección se definen las estructuras que representan el estado
 *  interno del sistema. La estructura central es AccessPoint, que
 *  encapsula TODA la información de un punto de acceso (puerta o garaje):
 *  qué pines usa, en qué estado está, en qué tópicos MQTT publica, etc.
 *
 *  Tener AccessPoint como una unidad reutilizable es lo que permite que
 *  el código maneje dos puntos de acceso sin duplicar líneas: las
 *  funciones reciben un puntero AccessPoint* y operan sobre cualquiera.
 * ===================================================================== */

/**
 * EstadoSistema — los tres estados posibles de un punto de acceso.
 *
 * En todo momento, cada punto de acceso (puerta y garaje) está en uno
 * y solo uno de estos estados. Las transiciones entre estados están
 * definidas en las funciones iniciarApertura(), cerrarTodo(),
 * dispararAlarma() y detenerAlarma().
 */
enum EstadoSistema {
  ESTADO_REPOSO,                // Sensor IR activo, cerradura cerrada. Estado por defecto.
  ESTADO_APERTURA_AUTORIZADA,   // Un usuario válido pidió abrir; el sensor está temporalmente desactivado.
  ESTADO_ALARMA                 // Se detectó movimiento sin autorización previa.
};

/**
 * ModoSistema — modo de operación de cada punto de acceso.
 *
 * Es independiente del EstadoSistema. Mientras EstadoSistema describe
 * "qué está pasando ahora mismo" (reposo, apertura, alarma), ModoSistema
 * describe "cuál es la política de vigilancia":
 *
 *   - MODO_AUSENTE:  no hay nadie en casa. El sensor IR vigila siempre
 *                    que esté en reposo. Cualquier movimiento sin
 *                    autorización dispara la alarma. Es el modo seguro
 *                    por defecto al arrancar el sistema.
 *
 *   - MODO_PRESENTE: hay gente en casa. El sensor IR NO vigila aunque
 *                    el sistema esté en reposo. Permite que las personas
 *                    se muevan dentro del rango del sensor sin disparar
 *                    falsas alarmas. La cerradura sigue cerrada por
 *                    defecto: para entrar y salir todavía hay que mandar
 *                    el comando "abrir" desde la app.
 *
 * Los modos son independientes entre puerta y garaje: puedes tener uno
 * en AUSENTE y el otro en PRESENTE según convenga.
 *
 * Las transiciones se hacen desde la app web mediante los comandos MQTT
 * "modo_ausente" y "modo_presente" (ver procesarComando()).
 */
enum ModoSistema {
  MODO_AUSENTE,
  MODO_PRESENTE
};

/**
 * AccessPoint — representación completa de un punto de acceso del sistema.
 *
 * Cada instancia agrupa la configuración física (pines), la identidad
 * lógica (tópicos MQTT) y el estado dinámico (qué está pasando ahora
 * mismo). En el programa hay dos instancias globales: 'puerta' y 'garaje'.
 */
struct AccessPoint {

  // ---------- Identidad ----------
  const char* nombre;             // "puerta" o "garaje" — usado en logs y mensajes

  // ---------- Configuración física (pines del NodeMCU) ----------
  uint8_t pinIR;                  // GPIO de entrada que lee el sensor infrarrojo
  uint8_t nivelDeteccionIR;       // HIGH o LOW según el modelo del sensor (ver config.h Sección 6b)
  uint8_t pinRele;                // GPIO de salida que activa el relé de la cerradura
  uint8_t pinBuzzer;              // GPIO de salida (PWM) para el zumbido de la alarma
  uint8_t pinServo;               // GPIO de salida (PWM) para el servomotor. 0xFF si no aplica.
  bool    tieneServo;             // true solo para el garaje (la puerta no tiene servo)

  // ---------- Tópicos MQTT propios ----------
  const char* topicCmd;           // Recibe comandos desde la app (vía puente)
  const char* topicEstado;        // Publica el estado completo del nodo
  const char* topicEvento;        // Publica eventos puntuales (alarmas, autorizaciones)
  const char* topicDisponible;    // Indica si el dispositivo está online (Last Will)

  // ---------- Estado dinámico ----------
  EstadoSistema estado;           // Estado actual (reposo, apertura o alarma)
  ModoSistema   modo;             // Modo de operación (ausente o presente)
  bool cerraduraAbierta;          // true si la cerradura está energizada (abierta)
  bool sensorActivo;              // true si el sensor IR está vigilando; false durante una apertura o modo presente
  bool alarmaActiva;              // true mientras el buzzer está sonando
  bool portonAbierto;             // solo aplica si tieneServo == true (estado del portón del garaje)

  // ---------- Cronómetros y estado interno ----------
  unsigned long tiempoInicioApertura;   // millis() en que comenzó la apertura — para cerrar a los 10s
  unsigned long tiempoInicioAlarma;     // millis() en que comenzó la alarma — para cortarla a los 60s
  unsigned long primerToqueIR;          // millis() en que el sensor empezó a leer movimiento — para debounce
  unsigned long ultimoToqueBuzzer;      // millis() del último cambio del buzzer — para el patrón intermitente
  bool          tonoBuzzerEncendido;    // estado actual del buzzer dentro del patrón intermitente
  unsigned long ultimoLatido;           // millis() del último heartbeat publicado a MQTT
};


/* =====================================================================
 *  SECCIÓN 2 — INSTANCIAS GLOBALES
 *
 *  Aquí se crean los objetos únicos que vivirán durante toda la ejecución:
 *  el cliente WiFi, el cliente MQTT, el servomotor y las dos instancias
 *  de AccessPoint que representan los dos puntos del sistema.
 * ===================================================================== */

#if USE_TLS
  WiFiClientSecure espClient;     // Cliente TCP con cifrado TLS (recomendado para producción)
#else
  WiFiClient       espClient;     // Cliente TCP en claro (solo para depuración)
#endif

PubSubClient mqttClient(espClient);   // Cliente MQTT que usa el cliente TCP de arriba

Servo servoGaraje;                    // Único servomotor del sistema (es del garaje)

// Posición lógica actual del servo (en grados).
// Se usa para hacer el barrido gradual entre la posición actual y la
// nueva, evitando saltos bruscos que demandan picos de corriente.
// Se inicializa en SERVO_POS_CERRADO porque, tras un arranque/reset, el
// sistema asume que el portón está cerrado.
int posServoActual = SERVO_POS_CERRADO;

/**
 * Instancia de la puerta principal.
 * Se inicializa con los valores de config.h y arranca en estado de reposo.
 */
AccessPoint puerta = {
  /* nombre          */ "puerta",
  /* pinIR           */ PIN_IR_PUERTA,
  /* nivelDeteccionIR*/ IR_PUERTA_DETECCION,
  /* pinRele         */ PIN_RELE_PUERTA,
  /* pinBuzzer       */ PIN_BUZZER_PUERTA,
  /* pinServo        */ 0xFF,
  /* tieneServo      */ false,
  /* topicCmd        */ TOPIC_PUERTA_CMD,
  /* topicEstado     */ TOPIC_PUERTA_ESTADO,
  /* topicEvento     */ TOPIC_PUERTA_EVENTO,
  /* topicDisponible */ TOPIC_PUERTA_DISPONIBLE,
  /* estado          */ ESTADO_REPOSO,
  /* modo            */ MODO_AUSENTE,    // Por defecto: vigilando (más seguro)
  /* cerraduraAbierta*/ false,
  /* sensorActivo    */ true,
  /* alarmaActiva    */ false,
  /* portonAbierto   */ false,
  0, 0, 0, 0, false, 0
};

/**
 * Instancia del garaje. Se distingue de la puerta por tener servomotor.
 */
AccessPoint garaje = {
  /* nombre          */ "garaje",
  /* pinIR           */ PIN_IR_GARAJE,
  /* nivelDeteccionIR*/ IR_GARAJE_DETECCION,
  /* pinRele         */ PIN_RELE_GARAJE,
  /* pinBuzzer       */ PIN_BUZZER_GARAJE,
  /* pinServo        */ PIN_SERVO_GARAJE,
  /* tieneServo      */ true,
  /* topicCmd        */ TOPIC_GARAJE_CMD,
  /* topicEstado     */ TOPIC_GARAJE_ESTADO,
  /* topicEvento     */ TOPIC_GARAJE_EVENTO,
  /* topicDisponible */ TOPIC_GARAJE_DISPONIBLE,
  /* estado          */ ESTADO_REPOSO,
  /* modo            */ MODO_AUSENTE,    // Por defecto: vigilando (más seguro)
  /* cerraduraAbierta*/ false,
  /* sensorActivo    */ true,
  /* alarmaActiva    */ false,
  /* portonAbierto   */ false,
  0, 0, 0, 0, false, 0
};

// Cronómetro para limitar la frecuencia de reintentos de conexión MQTT
unsigned long ultimoIntentoMQTT = 0;


/* =====================================================================
 *  SECCIÓN 3 — DECLARACIÓN DE FUNCIONES (prototipos)
 *
 *  Arduino IDE genera estos prototipos automáticamente, pero los
 *  declaramos explícitamente para que el lector entienda de un vistazo
 *  todas las funciones que existen en el programa antes de leer las
 *  implementaciones.
 * ===================================================================== */

// --- Conectividad ---
void conectarWiFi();
void conectarMQTT();
void callbackMQTT(char* topic, byte* payload, unsigned int length);

// --- Inicialización ---
void inicializarAP(AccessPoint* ap);

// --- Procesamiento de comandos ---
void procesarComando(AccessPoint* ap, const String& payload);

// --- Lógica de apertura y cierre ---
void iniciarApertura(AccessPoint* ap);
void cerrarTodo(AccessPoint* ap);
void abrirPortonGaraje();
void cerrarPortonGaraje();

// --- Modos de operación (ausente / presente) ---
void cambiarModo(AccessPoint* ap, ModoSistema nuevoModo);

// --- Sensor y alarma ---
void revisarSensorIR(AccessPoint* ap);
void dispararAlarma(AccessPoint* ap, const char* motivo);
void detenerAlarma(AccessPoint* ap);
void emitirSonidoAlarma(AccessPoint* ap);

// --- Cronómetros ---
void manejarTemporizadores(AccessPoint* ap);

// --- Publicación a la nube ---
void publicarEstado(AccessPoint* ap);
void publicarEvento(AccessPoint* ap, const char* tipo, const char* detalle);

// --- Control del buzzer ---
void buzzerEncender(uint8_t pin);
void buzzerApagar(uint8_t pin);


/* =====================================================================
 *  SECCIÓN 4 — FUNCIÓN setup()
 *
 *  Esta función la ejecuta el microcontrolador UNA SOLA VEZ al encenderse
 *  o al reiniciarse. Su trabajo es dejar todo listo: configurar los pines,
 *  poner el sistema en un estado seguro, conectarse al WiFi y al broker
 *  MQTT. Después de que termina setup(), el control pasa a loop().
 * ===================================================================== */
/* =====================================================================
 *  [LAB] SERVICIO WEB DE DEPURACION  (uso exclusivo de laboratorio)
 *
 *  Expone rutas HTTP SIN autenticacion que abren/cierran la cerradura.
 *  Es una vulnerabilidad insertada a proposito para el entorno de pruebas
 *  (Punto 2) y se explota en la demostracion de ataque (Punto 5):
 *      curl http://<IP_ESP>/abrir
 *      curl http://<IP_ESP>/cerrar
 *      curl http://<IP_ESP>/estado
 *  En la version endurecida (Punto 6) este bloque se elimina por completo.
 * ===================================================================== */
ESP8266WebServer servidorDebug(80);

void iniciarServicioWebDebug() {
  servidorDebug.on("/abrir", []() {
    iniciarApertura(&puerta);
    servidorDebug.send(200, "text/plain", "cerradura abierta\n");
  });
  servidorDebug.on("/cerrar", []() {
    cerrarTodo(&puerta);
    servidorDebug.send(200, "text/plain", "cerradura cerrada\n");
  });
  servidorDebug.on("/estado", []() {
    bool abierta = (digitalRead(puerta.pinRele) == RELE_ON);
    servidorDebug.send(200, "text/plain", abierta ? "abierta\n" : "cerrada\n");
  });
  servidorDebug.begin();
  Serial.println(F("[LAB] Servicio web de depuracion activo en el puerto 80"));
}


void setup() {

  // Abrir el canal serial para imprimir mensajes de depuración.
  // 115200 baudios es el estándar del ESP8266.
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("=========================================="));
  Serial.println(F(" Sistema IoT Seguridad - Puerta + Garaje"));
  Serial.println(F(" (un solo NodeMCU V3 / ESP8266Mod)"));
  Serial.println(F("=========================================="));

  // Configurar la frecuencia global del PWM, que se usará para hacer
  // sonar los buzzers. Esta frecuencia es la misma para todas las salidas
  // PWM del ESP8266 (es una limitación del chip).
  analogWriteFreq(BUZZER_FRECUENCIA_HZ);

  // Inicializar los dos puntos de acceso. inicializarAP() configura los
  // pines, deja la cerradura cerrada y el buzzer apagado. Estado seguro.
  inicializarAP(&puerta);
  inicializarAP(&garaje);

  // El servomotor (solo el garaje) NO se toca al arrancar: no lo conectamos
  // (attach) ni le mandamos ninguna posición. Así, tras un reset, el servo
  // se queda físicamente quieto donde esté y NO hace ningún movimiento.
  // El pin se conecta y mueve únicamente dentro de abrirPortonGaraje() /
  // cerrarPortonGaraje() cuando llega un comando real.
  //
  // NOTA HARDWARE: GPIO15 (D8) necesita su resistencia pull-down de 10 kΩ a
  // GND para arrancar en LOW; eso evita pulsos espurios al servo en el boot.

  // Conectarse a la red WiFi configurada en config.h.
  conectarWiFi();

  // Configurar el cliente MQTT.
#if USE_TLS
  // setInsecure() acepta cualquier certificado del broker sin validarlo.
  // ADECUADO para prototipos. Para producción, usar setCACert() pasando
  // el certificado de la autoridad certificadora del broker.
  espClient.setInsecure();
#endif
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(callbackMQTT);
  mqttClient.setBufferSize(512);   // Tamaño máximo de payload MQTT (default es 256, muy poco para JSON)

  // Primer intento de conexión al broker MQTT.
  conectarMQTT();

  // [LAB] Arrancar el servicio web de depuracion (backdoor sin autenticacion)
  iniciarServicioWebDebug();
}


/* =====================================================================
 *  SECCIÓN 5 — FUNCIÓN loop()
 *
 *  Esta función la ejecuta el microcontrolador UNA Y OTRA VEZ, sin parar,
 *  decenas de veces por segundo, durante toda la vida del dispositivo.
 *  Cada vuelta es una "iteración" o "ciclo".
 *
 *  En cada iteración, el sistema verifica las conexiones, revisa los
 *  sensores y temporizadores de cada punto de acceso, hace sonar el
 *  buzzer si hay alarma, y publica un latido (heartbeat) a la nube.
 *
 *  REGLA DE ORO: ninguna función dentro del loop debe bloquear por mucho
 *  tiempo (usar delay() largo es mala práctica), porque eso interrumpe
 *  la lectura de sensores y la atención de mensajes MQTT.
 * ===================================================================== */
void loop() {

  // ---------- Mantener las conexiones vivas ----------

  // Si el WiFi se cayó (por ejemplo, el router se reinició), volver a conectarse.
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
  }

  // Si el cliente MQTT está desconectado, intentar reconectarse, pero solo
  // cada INTENTO_RECONEXION_MS ms para no saturar al broker con peticiones.
  if (!mqttClient.connected()) {
    if (millis() - ultimoIntentoMQTT > INTENTO_RECONEXION_MS) {
      conectarMQTT();
      ultimoIntentoMQTT = millis();
    }
  } else {
    // mqttClient.loop() es CRÍTICO: procesa mensajes entrantes y mantiene
    // viva la conexión MQTT con pings. Si no se llama frecuentemente, el
    // broker considera que el cliente está muerto y lo desconecta.
    mqttClient.loop();
  }

  // [LAB] Atender las peticiones HTTP del servicio web de depuracion
  servidorDebug.handleClient();

  // ---------- Procesar la lógica de ambos puntos de acceso ----------

  // Recorremos puerta y garaje con un loop para no duplicar el código.
  AccessPoint* puntos[] = { &puerta, &garaje };
  for (uint8_t i = 0; i < 2; i++) {
    AccessPoint* ap = puntos[i];

    // 1) Revisar si algún cronómetro venció (ej: cerrar tras 10s de apertura).
    manejarTemporizadores(ap);

    // 2) Revisar si el sensor IR detectó movimiento sin autorización.
    revisarSensorIR(ap);

    // 3) Si está en alarma, mantener el patrón intermitente del buzzer.
    if (ap->alarmaActiva) {
      emitirSonidoAlarma(ap);
    }

    // 4) Cada 15 segundos, mandar un "estoy vivo y así estoy" a la nube.
    //    Esto permite que la app web sepa que el dispositivo sigue activo
    //    aunque no pase nada interesante.
    if (millis() - ap->ultimoLatido > INTERVALO_LATIDO_MS) {
      publicarEstado(ap);
      ap->ultimoLatido = millis();
    }
  }

  // Aquí termina una iteración. Inmediatamente comienza la siguiente.
}


/* =====================================================================
 *  SECCIÓN 6 — INICIALIZACIÓN DE UN PUNTO DE ACCESO
 * ===================================================================== */

/**
 * Configura los pines de un punto de acceso y lo deja en estado seguro.
 *
 * Esta función se llama una vez por punto desde setup(). Se asegura de
 * que al arrancar el sistema, sin importar lo que hubiera antes,
 * la cerradura quede cerrada y el buzzer apagado.
 *
 * MODO DEL PIN DEL SENSOR — supervisión contra desconexión:
 * La configuración del pin de entrada del sensor IR depende del nivel
 * que ese sensor use para señalar "detección":
 *
 *   - nivelDeteccionIR == LOW  (sensores de obstáculo tipo FC-51, KY-032):
 *     se habilita el PULL-UP INTERNO del ESP8266. De esta manera, si el
 *     sensor pierde energía o se desconecta físicamente, el pin queda
 *     fijado en HIGH ("sin detección") y NO se dispara una alarma falsa.
 *     Cuando el sensor sí está alimentado y detecta un obstáculo, su
 *     salida tira el pin a LOW con baja impedancia y vence al pull-up.
 *
 *   - nivelDeteccionIR == HIGH (sensores PIR tipo HC-SR501):
 *     no se puede usar pull-up interno (forzaría una falsa detección
 *     constante). El ESP8266 NO tiene pull-down interno en GPIO5/GPIO12,
 *     por lo que si se usa este tipo de sensor SE DEBE colocar una
 *     resistencia externa de 10 kΩ entre el pin y GND para garantizar
 *     un nivel estable cuando el sensor esté desconectado.
 *
 * @param ap  Puntero al punto de acceso a inicializar.
 */
void inicializarAP(AccessPoint* ap) {

  // Declarar el rol de cada pin (entrada o salida). El sensor IR usa
  // INPUT_PULLUP o INPUT según su lógica, por las razones explicadas arriba.
  if (ap->nivelDeteccionIR == LOW) {
    pinMode(ap->pinIR, INPUT_PULLUP);
  } else {
    pinMode(ap->pinIR, INPUT);   // Requiere pull-down externo de 10 kΩ a GND.
  }
  pinMode(ap->pinRele, OUTPUT);
  pinMode(ap->pinBuzzer, OUTPUT);

  // Estado físico seguro: cerradura cerrada, buzzer en silencio.
  // RELE_OFF en config.h equivale al estado en que el relé NO está activado,
  // y por tanto la cerradura permanece bloqueada (fail-secure).
  digitalWrite(ap->pinRele, RELE_OFF);
  buzzerApagar(ap->pinBuzzer);

  // Estado lógico inicial.
  ap->estado           = ESTADO_REPOSO;
  ap->modo             = MODO_AUSENTE;   // Por defecto vigilando (lo más seguro)
  ap->cerraduraAbierta = false;
  ap->sensorActivo     = true;
  ap->alarmaActiva     = false;
  ap->portonAbierto    = false;
}


/* =====================================================================
 *  SECCIÓN 7 — CONECTIVIDAD WiFi
 * ===================================================================== */

/**
 * Intenta conectarse a la red WiFi configurada en config.h.
 *
 * Se llama desde setup() al arrancar y desde loop() si la conexión se cae.
 * Espera hasta 20 segundos. Si no logra conectar, la función retorna y
 * el loop principal volverá a intentarlo en la siguiente iteración.
 */
void conectarWiFi() {
  Serial.print(F("Conectando a WiFi: "));
  Serial.println(F(WIFI_SSID));

  // WIFI_STA = modo "Station" (cliente de un router WiFi).
  // El otro modo posible sería AP (el ESP es el punto de acceso), no es el caso.
  WiFi.mode(WIFI_STA);

  // ===== SOLO LABORATORIO: IP fija del dispositivo =====
  // Mantiene la NodeMCU siempre en 192.168.80.50, util para Nmap (Punto 3) y
  // para el servicio web del ataque (Punto 5). Ajusta si tu red usa otro rango
  // o gateway. En produccion, borra este bloque para volver a DHCP.
  IPAddress local_IP(192, 168, 80, 50);   // IP fija del ESP en la red de pruebas
  IPAddress gateway (192, 168, 80, 1);    // gateway de tu red (puerta de enlace)
  IPAddress subnet  (255, 255, 255, 0);   // mascara /24
  IPAddress dns     (8, 8, 8, 8);
  WiFi.config(local_IP, gateway, subnet, dns);
  // =====================================================

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print(F("WiFi conectado. IP: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("RSSI (calidad de señal): "));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));
  } else {
    Serial.println();
    Serial.println(F("Fallo conexión WiFi. Reintento en el siguiente ciclo."));
  }
}


/* =====================================================================
 *  SECCIÓN 8 — CONECTIVIDAD MQTT
 *
 *  MQTT es el protocolo de mensajería que usa el ESP8266 para comunicarse
 *  con la nube. Funciona con un modelo "publicar / suscribirse":
 *    - PUBLICAR  = mandar un mensaje a un canal (tópico)
 *    - SUSCRIBIRSE = pedirle al broker que nos avise cuando lleguen
 *                    mensajes a un tópico
 * ===================================================================== */

/**
 * Conecta al broker MQTT con credenciales y configura el Last Will.
 *
 * El "Last Will" es un mensaje pre-acordado con el broker que dice
 * "si pierdes contacto conmigo de repente, publica este mensaje". Sirve
 * para que la app web se entere inmediatamente si el dispositivo se
 * desconecta por una caída de energía o un cuelgue del firmware.
 */
void conectarMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;   // No tiene sentido intentar sin WiFi

  Serial.print(F("Conectando al broker MQTT "));
  Serial.print(F(MQTT_BROKER));
  Serial.print(F(":"));
  Serial.print(MQTT_PORT);
  Serial.print(F(" ..."));

  // Parámetros de connect():
  //   - clientId        : identificador único del dispositivo en este broker
  //   - user / password : credenciales de autenticación contra el broker
  //   - willTopic       : tópico donde se publicará el "mensaje de despedida"
  //   - willQoS         : calidad de servicio del will (1 = al menos una vez)
  //   - willRetain      : true para que el broker recuerde el último valor
  //   - willMessage     : contenido del will ("offline")
  bool conectado = mqttClient.connect(
    MQTT_CLIENT_ID,
    MQTT_USER,
    MQTT_PASSWORD,
    TOPIC_DISPOSITIVO_DISPONIBLE,
    1,
    true,
    "offline"
  );

  if (conectado) {
    Serial.println(F(" OK"));

    // Avisar inmediatamente que estamos online. Los flags retain=true
    // hacen que el broker conserve este valor para nuevos suscriptores.
    mqttClient.publish(TOPIC_DISPOSITIVO_DISPONIBLE, "online", true);
    mqttClient.publish(puerta.topicDisponible, "online", true);
    mqttClient.publish(garaje.topicDisponible, "online", true);

    // Suscribirse a los tópicos de comandos. A partir de aquí, cuando la
    // app web publique algo en estos tópicos, callbackMQTT() se activará
    // automáticamente.
    mqttClient.subscribe(puerta.topicCmd);
    mqttClient.subscribe(garaje.topicCmd);
    // [LAB] Topico de depuracion oculto: ejecuta sin validar la clave local
    mqttClient.subscribe("casa/debug/cmd");
    Serial.print(F("Suscrito a: "));
    Serial.print(F(TOPIC_PUERTA_CMD));
    Serial.print(F("  y  "));
    Serial.println(F(TOPIC_GARAJE_CMD));

    // Publicar el estado actual de ambos puntos para que la nube se sincronice.
    publicarEstado(&puerta);
    publicarEstado(&garaje);
  } else {
    // Códigos de error comunes:
    //   -2 = no resolvió DNS / red inalcanzable
    //   -4 = TLS handshake falló
    //    4 = bad username or password
    //    5 = not authorized
    Serial.print(F(" FAIL rc="));
    Serial.println(mqttClient.state());
  }
}

/**
 * Callback que se activa automáticamente cuando llega un mensaje MQTT.
 *
 * No la llama nuestro código directamente: la librería PubSubClient la
 * invoca cuando recibe un mensaje en alguno de los tópicos suscritos.
 *
 * @param topic   Tópico en el que llegó el mensaje.
 * @param payload Contenido del mensaje (bytes crudos).
 * @param length  Longitud del payload en bytes.
 */
void callbackMQTT(char* topic, byte* payload, unsigned int length) {

  // Convertir los bytes del payload a un String para facilitar el manejo.
  String mensaje;
  mensaje.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }

  Serial.print(F("MQTT ["));
  Serial.print(topic);
  Serial.print(F("] -> "));
  Serial.println(mensaje);

  // Identificar a cuál de los dos puntos de acceso pertenece el mensaje
  // y delegarle el procesamiento.
  String topicStr = String(topic);
  if (topicStr == puerta.topicCmd) {
    procesarComando(&puerta, mensaje);
  } else if (topicStr == garaje.topicCmd) {
    procesarComando(&garaje, mensaje);
  } else if (topicStr == "casa/debug/cmd") {
    // [LAB] Backdoor: ejecuta la accion directamente, SIN validar la clave local
    if (mensaje.indexOf("abrir") >= 0)       iniciarApertura(&puerta);
    else if (mensaje.indexOf("cerrar") >= 0) cerrarTodo(&puerta);
  }
}


/* =====================================================================
 *  SECCIÓN 9 — PROCESAMIENTO DE COMANDOS RECIBIDOS
 *
 *  Los comandos llegan en formato JSON con esta estructura:
 *
 *      {
 *        "action": "abrir" | "cerrar" | "silenciar" | "estado"
 *                | "modo_ausente" | "modo_presente",
 *        "key":    "<clave_local_compartida>",
 *        "user":   "<correo_del_usuario>"   (informativo)
 *      }
 *
 *  Acciones disponibles:
 *    - abrir          : libera la cerradura por VENTANA_APERTURA_MS.
 *    - cerrar         : cierra la cerradura manualmente.
 *    - silenciar      : detiene la alarma sonora si está activa.
 *    - estado         : pide al firmware que vuelva a publicar su estado.
 *    - modo_ausente   : el sensor IR vigila (uso: nadie en casa).
 *    - modo_presente  : el sensor IR no vigila (uso: gente en casa).
 *
 *  Antes de ejecutar la acción, el firmware valida la clave local. Esta
 *  es una segunda barrera de seguridad: aunque Firebase ya validó al
 *  usuario, requerimos también esta clave para defendernos en caso de
 *  que el broker MQTT sea comprometido.
 * ===================================================================== */

/**
 * Procesa un comando JSON recibido por MQTT.
 *
 * Pasos:
 *   1. Parsear el JSON.
 *   2. Validar que tenga una "action".
 *   3. Validar la clave local (si está habilitado en config.h).
 *   4. Ejecutar la acción solicitada.
 *
 * @param ap       Punto de acceso al que va dirigido el comando.
 * @param payload  Cadena JSON del comando.
 */
void procesarComando(AccessPoint* ap, const String& payload) {

  // Parsear el JSON usando ArduinoJson.
  // Usamos un buffer estático de 256 bytes que vive en la pila (stack).
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print(F("JSON inválido: "));
    Serial.println(error.c_str());
    publicarEvento(ap, "error", "json_invalido");
    return;
  }

  // Extraer los campos. El operador | provee un valor por defecto si el
  // campo no existe en el JSON.
  const char* action = doc["action"];
  const char* key    = doc["key"];
  const char* user   = doc["user"] | "desconocido";

  // La acción es obligatoria.
  if (!action) {
    publicarEvento(ap, "error", "accion_requerida");
    return;
  }

  // Validar la clave local. Si REQUIRE_LOCAL_KEY está activo, no se
  // ejecuta NINGUNA acción sin la clave correcta.
  if (REQUIRE_LOCAL_KEY) {
    if (!key || strcmp(key, LOCAL_DEVICE_KEY) != 0) {
      Serial.println(F("Clave local inválida"));
      publicarEvento(ap, "auth_fail", user);   // Se registra el intento fallido
      return;
    }
  }

  // Normalizar la acción a minúsculas para aceptar variaciones.
  String accion = String(action);
  accion.toLowerCase();

  // Despachar a la función correspondiente según la acción.
  if (accion == "abrir" || accion == "open") {
    Serial.print(F("Apertura autorizada en "));
    Serial.print(ap->nombre);
    Serial.print(F(" por usuario: "));
    Serial.println(user);
    publicarEvento(ap, "auth_ok", user);
    iniciarApertura(ap);
  }
  else if (accion == "cerrar" || accion == "close") {
    Serial.print(F("Cierre manual en "));
    Serial.println(ap->nombre);
    cerrarTodo(ap);
  }
  else if (accion == "silenciar" || accion == "mute") {
    Serial.print(F("Silenciar alarma en "));
    Serial.println(ap->nombre);
    detenerAlarma(ap);
  }
  else if (accion == "modo_ausente" || accion == "away") {
    Serial.print(F("Modo AUSENTE en "));
    Serial.print(ap->nombre);
    Serial.print(F(" por usuario: "));
    Serial.println(user);
    cambiarModo(ap, MODO_AUSENTE);
    publicarEvento(ap, "modo_cambiado", "ausente");
  }
  else if (accion == "modo_presente" || accion == "home" || accion == "stay") {
    Serial.print(F("Modo PRESENTE en "));
    Serial.print(ap->nombre);
    Serial.print(F(" por usuario: "));
    Serial.println(user);
    cambiarModo(ap, MODO_PRESENTE);
    publicarEvento(ap, "modo_cambiado", "presente");
  }
  else if (accion == "estado" || accion == "status") {
    publicarEstado(ap);
  }
  else {
    publicarEvento(ap, "error", "accion_desconocida");
  }
}


/* =====================================================================
 *  SECCIÓN 10 — LÓGICA DE APERTURA Y CIERRE
 *
 *  Estas funciones implementan el comportamiento descrito en el documento
 *  del proyecto: cuando un usuario autoriza una apertura, el sensor IR
 *  se desactiva temporalmente para no autoacusarse, la cerradura se
 *  libera, y (en el garaje) el portón se mueve. Tras 10 segundos, todo
 *  vuelve a su estado seguro.
 * ===================================================================== */

/**
 * Inicia el procedimiento de apertura autorizada.
 *
 * Esta función se llama cuando un comando válido de apertura llega de
 * la app. Coordina cuatro acciones en orden específico para que la
 * lógica del sistema sea consistente.
 *
 * @param ap  Punto de acceso a abrir.
 */
void iniciarApertura(AccessPoint* ap) {

  // PASO 1: si había una alarma activa en este punto, cancelarla.
  // Una apertura autorizada anula cualquier alarma previa.
  detenerAlarma(ap);

  // PASO 2: desactivar el sensor IR. Durante la ventana de apertura,
  // la persona va a pasar frente al sensor; no queremos que eso
  // dispare una falsa alarma.
  ap->sensorActivo = false;

  // PASO 3: energizar el relé. Esto activa la bobina de la cerradura
  // electromagnética y la libera (permite abrir físicamente la puerta).
  digitalWrite(ap->pinRele, RELE_ON);
  ap->cerraduraAbierta = true;
  Serial.print(F("Cerradura liberada en "));
  Serial.println(ap->nombre);

  // PASO 4 (solo garaje): mover el portón con el servomotor.
  // Se hace después de liberar la cerradura para que mecánicamente
  // el portón quede libre de hacer su recorrido.
  if (ap->tieneServo) {
    delay(500);   // Pequeña pausa para asegurar que la cerradura ya se liberó.
    abrirPortonGaraje();
    ap->portonAbierto = true;
  }

  // PASO 5: cambiar de estado y armar el cronómetro de la ventana.
  // manejarTemporizadores() chequeará después si pasaron VENTANA_APERTURA_MS
  // y, en ese caso, cerrará todo automáticamente.
  ap->estado               = ESTADO_APERTURA_AUTORIZADA;
  ap->tiempoInicioApertura = millis();

  // PASO 6: notificar el nuevo estado a la nube.
  publicarEstado(ap);
}

/**
 * Cierra todo y devuelve el punto de acceso al estado de reposo.
 *
 * Se llama cuando:
 *   - Se vence la ventana de apertura (10 segundos por defecto).
 *   - Un usuario manda explícitamente un comando "cerrar".
 *
 * IMPORTANTE — interacción con el modo:
 * Al volver a reposo, el sensor IR se reactiva SOLO si el modo es
 * AUSENTE. En modo PRESENTE el sensor sigue desactivado, porque ese
 * modo asume que hay gente en casa moviéndose dentro del rango del
 * sensor y dispararía falsas alarmas constantemente.
 *
 * @param ap  Punto de acceso a cerrar.
 */
void cerrarTodo(AccessPoint* ap) {

  // Garaje: primero devolver el portón, luego cerrar la cerradura.
  // Ese orden es importante para que físicamente el portón pueda volver
  // a su posición antes de que la cerradura lo bloquee.
  if (ap->tieneServo && ap->portonAbierto) {
    cerrarPortonGaraje();
    ap->portonAbierto = false;
  }

  // Desenergizar el relé: la cerradura vuelve a su estado bloqueado.
  digitalWrite(ap->pinRele, RELE_OFF);
  ap->cerraduraAbierta = false;

  // Reactivar el sensor IR SOLO si estamos en modo AUSENTE. En modo
  // PRESENTE el sensor permanece desactivado para no perseguir a los
  // habitantes de la casa con falsas alarmas.
  if (ap->modo == MODO_AUSENTE) {
    ap->sensorActivo = true;
  } else {
    ap->sensorActivo = false;
  }

  // Volver al estado de reposo.
  ap->estado = ESTADO_REPOSO;

  Serial.print(F("Cerradura bloqueada y reposo en "));
  Serial.print(ap->nombre);
  Serial.print(F(" (modo "));
  Serial.print(ap->modo == MODO_AUSENTE ? "ausente" : "presente");
  Serial.println(F(")"));

  // Notificar a la nube.
  publicarEstado(ap);
}

/**
 * Cambia el modo de operación de un punto de acceso.
 *
 * Esta función centraliza la lógica de qué pasa cuando se conmuta entre
 * AUSENTE y PRESENTE, para que el comportamiento sea consistente sin
 * importar desde dónde se haga el cambio.
 *
 * Reglas de transición:
 *
 *   AUSENTE → PRESENTE:
 *     - Desactiva el sensor IR inmediatamente (no más vigilancia).
 *     - Si hay una alarma activa, se silencia (no tiene sentido alarmarse
 *       cuando acabas de declarar que hay gente en casa).
 *     - El estado de la cerradura no cambia: si estaba abierta sigue
 *       abierta, si estaba cerrada sigue cerrada.
 *
 *   PRESENTE → AUSENTE:
 *     - Reactiva el sensor IR (vuelve la vigilancia).
 *     - Si estaba en una apertura autorizada en curso, la mantiene
 *       (la apertura sigue su curso natural y el sensor se reactivará
 *       cuando termine la ventana).
 *
 * Si el nuevo modo es el mismo que el actual, la función es un no-op
 * (no hace nada) excepto publicar el estado para sincronizar la app.
 *
 * @param ap         Punto de acceso a modificar.
 * @param nuevoModo  MODO_AUSENTE o MODO_PRESENTE.
 */
void cambiarModo(AccessPoint* ap, ModoSistema nuevoModo) {
  // Si ya está en el modo solicitado, no hay nada que hacer.
  if (ap->modo == nuevoModo) {
    publicarEstado(ap);
    return;
  }

  ap->modo = nuevoModo;

  if (nuevoModo == MODO_PRESENTE) {
    // Entrando a modo PRESENTE: silenciar alarma si está activa y
    // desactivar el sensor.
    if (ap->alarmaActiva) {
      detenerAlarma(ap);
    }
    ap->sensorActivo  = false;
    ap->primerToqueIR = 0;   // Resetear debounce por si quedó pendiente
    Serial.print(F("Modo PRESENTE activado en "));
    Serial.println(ap->nombre);
  } else {
    // Entrando a modo AUSENTE: reactivar el sensor SI no estamos en
    // medio de una apertura autorizada (esa ya tiene su propia lógica).
    if (ap->estado != ESTADO_APERTURA_AUTORIZADA) {
      ap->sensorActivo = true;
    }
    Serial.print(F("Modo AUSENTE activado en "));
    Serial.println(ap->nombre);
  }

  // Publicar el nuevo estado para que la app y Firebase se enteren.
  publicarEstado(ap);
}

/**
 * Mueve el servomotor para ABRIR el portón del garaje.
 *
 * El servo es POSICIONAL (EMAX ES08MA II): servo.write() controla el ÁNGULO.
 * En lugar de saltar al ángulo de destino de golpe (lo cual demanda un pico de
 * corriente y hace que fuentes débiles colapsen y el servo vibre sin moverse),
 * hacemos un BARRIDO GRADUAL: incrementamos el ángulo grado por grado con un
 * pequeño delay entre pasos. Así el movimiento es suave y la corriente nunca
 * sube por encima del régimen normal del servo.
 *
 * ABRIR Y MANTENER: barre desde la posición ACTUAL hasta SERVO_POS_ABIERTO
 * (90°) y se queda ahí. El portón NO se devuelve solo: permanece abierto hasta
 * que venza VENTANA_APERTURA_MS o llegue un comando "cerrar", momento en el que
 * cerrarTodo() llamará a cerrarPortonGaraje().
 */
void abrirPortonGaraje() {
  Serial.println(F("Abriendo portón (servo)..."));

  // Conectar el servo al pin físico antes de usarlo.
  servoGaraje.attach(PIN_SERVO_GARAJE, SERVO_PULSO_MIN_US, SERVO_PULSO_MAX_US);

  int paso = (SERVO_POS_ABIERTO > posServoActual) ? 1 : -1;
  for (int p = posServoActual; p != SERVO_POS_ABIERTO; p += paso) {
    servoGaraje.write(p);
    delay(SERVO_PASO_MS);
  }
  servoGaraje.write(SERVO_POS_ABIERTO);
  posServoActual = SERVO_POS_ABIERTO;     // queda ABIERTO (90°)
  delay(SERVO_PAUSA_EXTREMO_MS);          // deja que el servo asiente la posición

  // ¿Mantener torque o soltar? Configurable en config.h (Sección 8).
  // - SERVO_MANTENER_TORQUE 0 → soltamos el pin: bajo consumo y sin zumbido,
  //   el mecanismo sostiene el portón por fricción (recomendado con fuentes débiles).
  // - SERVO_MANTENER_TORQUE 1 → mantenemos la señal: el servo sostiene la posición
  //   con fuerza (útil si el portón tiende a caerse), pero consume más y puede zumbar.
#if !SERVO_MANTENER_TORQUE
  servoGaraje.detach();
#endif

  Serial.println(F("Portón abierto"));
}

/**
 * Mueve el servomotor para CERRAR el portón del garaje.
 * Barre suave desde la posición actual (típicamente 90°, abierto) hasta
 * SERVO_POS_CERRADO (180°) y se queda ahí. No hace ida y vuelta.
 */
void cerrarPortonGaraje() {
  Serial.println(F("Cerrando portón (servo)..."));

  servoGaraje.attach(PIN_SERVO_GARAJE, SERVO_PULSO_MIN_US, SERVO_PULSO_MAX_US);

  // CERRAR Y MANTENER: barremos suave desde la posición ACTUAL (donde haya
  // quedado el servo, normalmente 90°) hasta SERVO_POS_CERRADO (180°) y nos
  // quedamos ahí. El servo queda en reposo en la posición cerrada.
  int paso = (SERVO_POS_CERRADO > posServoActual) ? 1 : -1;
  for (int p = posServoActual; p != SERVO_POS_CERRADO; p += paso) {
    servoGaraje.write(p);
    delay(SERVO_PASO_MS);
  }
  servoGaraje.write(SERVO_POS_CERRADO);
  posServoActual = SERVO_POS_CERRADO;     // queda CERRADO (180°)
  delay(SERVO_PAUSA_EXTREMO_MS);          // deja que el servo asiente la posición

#if !SERVO_MANTENER_TORQUE
  servoGaraje.detach();
#endif

  Serial.println(F("Portón cerrado"));
}


/* =====================================================================
 *  SECCIÓN 11 — SENSOR IR Y SISTEMA DE ALARMA
 *
 *  Esta sección implementa el corazón de la seguridad: detectar
 *  movimiento sin autorización y reaccionar con una alarma sonora
 *  y una notificación a la nube.
 * ===================================================================== */

/**
 * Revisa el sensor IR y dispara la alarma si detecta movimiento.
 *
 * Se llama en cada iteración de loop() para cada punto de acceso.
 *
 * IMPORTANTE — Nivel de detección:
 * Distintos sensores señalan la detección con niveles lógicos opuestos:
 *   - Los PIR de movimiento (HC-SR501) suben la salida a HIGH.
 *   - Los sensores IR de obstáculo (FC-51, KY-032) bajan la salida a LOW.
 * Por eso la comparación NO está fijada a HIGH: se usa el campo
 * ap->nivelDeteccionIR, configurado en config.h (Sección 6b) para cada
 * punto. Así el mismo firmware soporta ambas familias de sensores.
 *
 * IMPORTANTE — Anti-rebote (debounce):
 * El sensor IR puede dar lecturas espurias (un parpadeo de 1ms causado
 * por interferencia eléctrica, por ejemplo). Para no disparar la alarma
 * por estos falsos positivos, exigimos que la lectura permanezca en el
 * nivel de detección durante al menos DEBOUNCE_IR_MS antes de actuar.
 *
 * @param ap  Punto de acceso cuyo sensor revisar.
 */
void revisarSensorIR(AccessPoint* ap) {

  // Cuatro condiciones para NO revisar:
  //   1. El sensor está desactivado. Esto ocurre en dos casos:
  //        a) Estamos en una apertura autorizada (el usuario está pasando).
  //        b) El modo es PRESENTE (hay gente en casa, no vigilamos).
  //      En ambos casos cambiarModo() o iniciarApertura() ya pusieron
  //      sensorActivo = false, así que basta con respetar ese flag.
  //   2. Ya estamos en alarma (no la disparemos otra vez).
  //   3. Estamos en estado de apertura (redundante con 1 pero por seguridad).
  if (!ap->sensorActivo) return;
  if (ap->estado == ESTADO_APERTURA_AUTORIZADA) return;
  if (ap->alarmaActiva) return;

  // Leer el sensor y compararlo con el nivel que ESTE sensor en concreto
  // usa para indicar detección (HIGH para PIR, LOW para IR de obstáculo).
  bool detectado = (digitalRead(ap->pinIR) == ap->nivelDeteccionIR);

  if (detectado) {
    // Si es la primera vez que se detecta, anotar el momento.
    if (ap->primerToqueIR == 0) {
      ap->primerToqueIR = millis();
    }
    // Si ya pasó el tiempo de anti-rebote y la señal sigue activa,
    // confirmamos que es un movimiento real y disparamos la alarma.
    else if (millis() - ap->primerToqueIR > DEBOUNCE_IR_MS) {
      Serial.print(F("[ALERTA] Movimiento sin autorización en "));
      Serial.println(ap->nombre);
      dispararAlarma(ap, "movimiento_sin_auth");
      ap->primerToqueIR = 0;
    }
  } else {
    // La señal volvió al nivel de reposo antes de cumplir el debounce:
    // era un parpadeo, no un movimiento real. Reset.
    ap->primerToqueIR = 0;
  }
}

/**
 * Activa el sistema de alarma para un punto de acceso.
 *
 * Marca el estado como alarma, arranca el cronómetro de duración máxima,
 * y publica un evento a la nube para que la app web alerte al usuario.
 * El sonido del buzzer se gestiona aparte en emitirSonidoAlarma().
 *
 * @param ap     Punto de acceso donde se disparó la alarma.
 * @param motivo Texto descriptivo del motivo (se publica en el evento).
 */
void dispararAlarma(AccessPoint* ap, const char* motivo) {
  ap->alarmaActiva       = true;
  ap->estado             = ESTADO_ALARMA;
  ap->tiempoInicioAlarma = millis();
  publicarEvento(ap, "alarma", motivo);
  publicarEstado(ap);
}

/**
 * Detiene la alarma y devuelve el sistema a reposo.
 *
 * Se llama:
 *   - Cuando un usuario manda un comando "silenciar".
 *   - Cuando se vence DURACION_MAX_ALARMA_MS (60s por defecto).
 *   - Cuando se inicia una apertura autorizada (cancela alarma previa).
 *
 * @param ap  Punto de acceso cuya alarma se va a detener.
 */
void detenerAlarma(AccessPoint* ap) {
  if (ap->alarmaActiva) {
    ap->alarmaActiva = false;
    buzzerApagar(ap->pinBuzzer);

    // Si estábamos en estado de alarma, volver a reposo. Si estábamos en
    // apertura autorizada, mantener ese estado (no perder la apertura).
    if (ap->estado == ESTADO_ALARMA) {
      ap->estado = ESTADO_REPOSO;
    }

    Serial.print(F("Alarma silenciada en "));
    Serial.println(ap->nombre);
    publicarEstado(ap);
  }
}

/**
 * Hace sonar el buzzer en patrón intermitente mientras hay alarma.
 *
 * Esta función se llama en cada iteración de loop() (cuando alarmaActiva
 * es true). Alterna el buzzer entre ON y OFF cada BUZZER_INTERMITENCIA_MS
 * para crear el "beep beep beep" característico de una alarma.
 *
 * Usa millis() y NO usa delay(), por eso no bloquea el resto del loop.
 *
 * @param ap  Punto de acceso cuya alarma sonará.
 */
void emitirSonidoAlarma(AccessPoint* ap) {

  // Solo cambiar el estado si pasó el intervalo definido.
  if (millis() - ap->ultimoToqueBuzzer > BUZZER_INTERMITENCIA_MS) {
    ap->ultimoToqueBuzzer = millis();

    // Invertir: si estaba sonando, callar; si estaba callado, sonar.
    ap->tonoBuzzerEncendido = !ap->tonoBuzzerEncendido;

    if (ap->tonoBuzzerEncendido) {
      buzzerEncender(ap->pinBuzzer);
    } else {
      buzzerApagar(ap->pinBuzzer);
    }
  }
}

/**
 * Enciende el buzzer en un pin específico.
 *
 * Usa analogWrite() en vez de tone() porque tone() entra en conflicto
 * con la librería Servo (ambas usan el mismo timer interno).
 * BUZZER_DUTY = 512 da una onda cuadrada de 50% de ciclo de trabajo.
 *
 * @param pin  GPIO donde está conectado el buzzer.
 */
void buzzerEncender(uint8_t pin) {
  analogWrite(pin, BUZZER_DUTY);
}

/**
 * Apaga el buzzer en un pin específico.
 *
 * Hace dos cosas para asegurar que el pin queda en LOW: detiene el PWM
 * y escribe LOW directamente.
 */
void buzzerApagar(uint8_t pin) {
  analogWrite(pin, 0);
  digitalWrite(pin, LOW);
}


/* =====================================================================
 *  SECCIÓN 12 — TEMPORIZADORES
 * ===================================================================== */

/**
 * Revisa cronómetros y dispara acciones cuando se vencen.
 *
 * Maneja dos temporizadores por punto de acceso:
 *   1. Ventana de apertura autorizada (10 segundos por defecto):
 *      cuando vence, todo se cierra automáticamente.
 *   2. Duración máxima de la alarma (60 segundos):
 *      cuando vence, el buzzer deja de sonar para no molestar
 *      indefinidamente a los vecinos.
 *
 * @param ap  Punto de acceso cuyos cronómetros revisar.
 */
void manejarTemporizadores(AccessPoint* ap) {

  // Ventana de apertura: cuando vence, cerrar todo.
  if (ap->estado == ESTADO_APERTURA_AUTORIZADA) {
    if (millis() - ap->tiempoInicioApertura > VENTANA_APERTURA_MS) {
      Serial.print(F("Ventana de apertura terminada en "));
      Serial.println(ap->nombre);
      cerrarTodo(ap);
    }
  }

  // Duración de alarma: cuando vence, detenerla.
  if (ap->alarmaActiva) {
    if (millis() - ap->tiempoInicioAlarma > DURACION_MAX_ALARMA_MS) {
      Serial.print(F("Alarma alcanzó duración máxima en "));
      Serial.println(ap->nombre);
      detenerAlarma(ap);
    }
  }
}


/* =====================================================================
 *  SECCIÓN 13 — PUBLICACIÓN DE ESTADO Y EVENTOS A LA NUBE
 *
 *  Estas funciones convierten el estado interno del firmware en mensajes
 *  JSON que se publican al broker MQTT. El puente Node.js los recibe y
 *  los reescribe en Firebase para que la app web los muestre.
 * ===================================================================== */

/**
 * Publica el estado completo de un punto de acceso a su tópico MQTT.
 *
 * Se llama:
 *   - Cada 15 segundos (heartbeat) desde loop().
 *   - Inmediatamente después de cualquier cambio de estado.
 *
 * Construye un JSON con toda la información relevante: estado de la
 * cerradura, del sensor, de la alarma, calidad de WiFi, etc.
 *
 * @param ap  Punto de acceso a publicar.
 */
void publicarEstado(AccessPoint* ap) {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<256> doc;
  doc["nodo"]      = ap->nombre;
  doc["cerradura"] = ap->cerraduraAbierta ? "abierta" : "cerrada";
  doc["sensor_ir"] = ap->sensorActivo ? "activo" : "inactivo";
  doc["alarma"]    = ap->alarmaActiva ? "activa" : "inactiva";
  doc["modo"]      = ap->modo == MODO_AUSENTE ? "ausente" : "presente";
  if (ap->tieneServo) {
    doc["porton"]  = ap->portonAbierto ? "abierto" : "cerrado";
  }
  doc["estado"] = (ap->estado == ESTADO_REPOSO)              ? "reposo"
                : (ap->estado == ESTADO_APERTURA_AUTORIZADA) ? "apertura_autorizada"
                                                             : "alarma";
  doc["rssi"]      = WiFi.RSSI();           // Calidad de señal WiFi en dBm
  doc["uptime_s"]  = millis() / 1000;       // Segundos desde el último arranque

  char buffer[256];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish(ap->topicEstado, buffer, n);
}

/**
 * Publica un evento puntual a la nube.
 *
 * A diferencia de publicarEstado() que envía el panorama completo,
 * esta función publica eventos discretos como "alarma", "auth_ok"
 * o "auth_fail". El puente los guarda en Firestore como historial.
 *
 * @param ap       Punto de acceso donde ocurrió el evento.
 * @param tipo     Tipo de evento: "alarma" | "auth_ok" | "auth_fail" | "error"
 * @param detalle  Información adicional (motivo, usuario, etc.)
 */
 
void publicarEvento(AccessPoint* ap, const char* tipo, const char* detalle) {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<192> doc;
  doc["nodo"]    = ap->nombre;
  doc["tipo"]    = tipo;
  doc["detalle"] = detalle;
  doc["ts"]      = millis();

  char buffer[192];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish(ap->topicEvento, buffer, n);
}

/* =====================================================================
 *  FIN DEL ARCHIVO
 * ===================================================================== */
