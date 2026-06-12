/* =====================================================================
 * ARCHIVO   : config.h
 * PROYECTO  : Sistema IoT de seguridad doméstica
 * COMPAÑERO : SistemaIoT_Casa.ino
 *
 * PROPÓSITO
 * ---------
 * Este archivo NO contiene lógica. Es el "panel de configuración" del
 * firmware: aquí están todos los valores que el programa principal usa
 * y que es probable que haya que cambiar según el entorno (red WiFi
 * diferente, broker MQTT diferente, ajuste de tiempos, etc.).
 *
 * Tenerlo separado del programa principal tiene tres ventajas:
 *   1. Para cambiar una clave de WiFi no hay que tocar la lógica.
 *   2. Se puede compartir el código fuente del programa sin compartir
 *      las credenciales (basta con borrar este archivo).
 *   3. Diferentes instalaciones del sistema pueden usar el mismo
 *      programa con diferentes config.h.
 *
 * CÓMO FUNCIONA TÉCNICAMENTE
 * --------------------------
 * Cada #define es un "buscar y reemplazar" que el compilador hace ANTES
 * de generar el código final. Por ejemplo, cuando el programa principal
 * escribe WIFI_SSID, el compilador lo reemplaza por el texto que esté
 * definido aquí. Los #define no son variables: son constantes en tiempo
 * de compilación.
 *
 * Para cambiar un valor en un sistema ya programado, hay que editar este
 * archivo y VOLVER A SUBIR el firmware al NodeMCU.
 * ===================================================================== */

#ifndef CONFIG_H
#define CONFIG_H

/* =====================================================================
 *  SECCIÓN 1 — CREDENCIALES DE LA RED WiFi
 *
 *  El dispositivo se conectará a esta red para alcanzar el broker MQTT
 *  en internet. Debe ser una red 2.4 GHz (el ESP8266 NO soporta 5 GHz).
 * ===================================================================== */

#define WIFI_SSID       "Alex_Rojas"
#define WIFI_PASSWORD   "Alex18rojas"


/* =====================================================================
 *  SECCIÓN 2 — CREDENCIALES DEL BROKER MQTT
 *
 *  El broker MQTT es el servidor intermedio (en la nube) por donde van
 *  y vienen los mensajes entre este dispositivo y la aplicación web.
 *  En este proyecto se usa HiveMQ Cloud Serverless (plan gratuito).
 *
 *  El hostname y las credenciales se obtienen al crear el cluster en
 *  console.hivemq.cloud.
 * ===================================================================== */

#define MQTT_BROKER     "192.168.80.245"                                        
#define MQTT_USER       "esp8266_casa"                                         
#define MQTT_PASSWORD   "*#AlexIot2026*#_"                                      

// Identificador único de ESTE cliente MQTT en el broker. Si tuvieras
// más de un dispositivo conectado al mismo broker, cada uno necesitaría
// un MQTT_CLIENT_ID diferente para no chocar.
#define MQTT_CLIENT_ID  "esp8266_casa_iot"

// Activar cifrado TLS para que los mensajes viajen protegidos.
// 1 = sí (recomendado, usa puerto 8883)
// 0 = no (solo para depuración local, usa puerto 1883)
#define USE_TLS         0
#if USE_TLS
  #define MQTT_PORT     8883
#else
  #define MQTT_PORT     1883
#endif


/* =====================================================================
 *  SECCIÓN 3 — CLAVE LOCAL DEL DISPOSITIVO
 *
 *  Esta es una segunda barrera de autenticación que se suma a la
 *  validación de usuario que hace Firebase. La idea es defensa en
 *  profundidad: aunque un atacante comprometa el broker MQTT y pueda
 *  publicar mensajes, sin esta clave el firmware rechaza la orden.
 *
 *  La clave que se ponga aquí DEBE ser exactamente la misma que tenga
 *  el puente Node.js en su archivo .env como LOCAL_DEVICE_KEY. Si no
 *  coinciden, ningún comando funcionará.
 * ===================================================================== */

#define REQUIRE_LOCAL_KEY   1                       // 1 = exigir clave en cada comando, 0 = no
#define LOCAL_DEVICE_KEY    "*#AlexIot2026*#_"      // Reemplazar por una clave fuerte


/* =====================================================================
 *  SECCIÓN 4 — NOMBRES DE LOS TÓPICOS MQTT
 *
 *  MQTT organiza los mensajes en "tópicos" (canales). Cada tópico tiene
 *  un propósito específico. Esta jerarquía de nombres es una convención
 *  del proyecto.
 *
 *  Estructura: casa/<nodo>/<propósito>
 *
 *  El puente Node.js debe estar suscrito a los mismos tópicos para que
 *  el sistema funcione.
 * ===================================================================== */

// --- Tópicos a nivel de dispositivo (un solo NodeMCU controla todo) ---
#define TOPIC_DISPOSITIVO_DISPONIBLE  "casa/dispositivo/disponible"   // Online/offline del dispositivo completo

// --- Tópicos del punto de acceso "puerta principal" ---
#define TOPIC_PUERTA_CMD              "casa/puerta/cmd"           // Recibe comandos de la app
#define TOPIC_PUERTA_ESTADO           "casa/puerta/estado"        // Publica estado completo
#define TOPIC_PUERTA_EVENTO           "casa/puerta/evento"        // Publica eventos puntuales
#define TOPIC_PUERTA_DISPONIBLE       "casa/puerta/disponible"    // Disponibilidad de este nodo

// --- Tópicos del punto de acceso "garaje" ---
#define TOPIC_GARAJE_CMD              "casa/garaje/cmd"
#define TOPIC_GARAJE_ESTADO           "casa/garaje/estado"
#define TOPIC_GARAJE_EVENTO           "casa/garaje/evento"
#define TOPIC_GARAJE_DISPONIBLE       "casa/garaje/disponible"


/* =====================================================================
 *  SECCIÓN 5 — ASIGNACIÓN DE PINES FÍSICOS
 *
 *  Aquí se define a qué pin del NodeMCU está conectado cada componente.
 *
 *  IMPORTANTE — Nomenclatura del NodeMCU:
 *  El NodeMCU tiene etiquetas D0, D1, D2... en su placa, pero internamente
 *  el chip ESP8266 usa números de GPIO diferentes. La traducción es:
 *
 *       D0 = GPIO16     D5 = GPIO14
 *       D1 = GPIO5      D6 = GPIO12
 *       D2 = GPIO4      D7 = GPIO13
 *       D3 = GPIO0      D8 = GPIO15
 *       D4 = GPIO2
 *
 *  El código usa números de GPIO. Hay pines con restricciones (D3, D4,
 *  D8 tienen comportamientos especiales al arranque). Ver el README
 *  del proyecto para más detalles.
 *
 *  Total de pines usados: 7 (3 para la puerta + 4 para el garaje).
 * ===================================================================== */

// --- Componentes de la puerta principal ---
#define PIN_IR_PUERTA          5    // D1 — entrada digital, sensor IR
#define PIN_RELE_PUERTA        4    // D2 — salida digital, relé de la cerradura
#define PIN_BUZZER_PUERTA      14   // D5 — salida PWM, buzzer

// --- Componentes del garaje ---
#define PIN_IR_GARAJE          12   // D6 — entrada digital, sensor IR
#define PIN_RELE_GARAJE        13   // D7 — salida digital, relé de la cerradura
#define PIN_BUZZER_GARAJE      0    // D3 — salida PWM, buzzer
#define PIN_SERVO_GARAJE       15   // D8 — salida PWM, servomotor (¡requiere pull-down de 10kΩ a GND!)


/* =====================================================================
 *  SECCIÓN 6 — LÓGICA DE ACTIVACIÓN DEL RELÉ
 *
 *  Los módulos de relé para Arduino vienen en dos variantes según su
 *  electrónica interna:
 *     - "Activo bajo": el relé se ACTIVA cuando recibe un LOW (0V).
 *     - "Activo alto": el relé se ACTIVA cuando recibe un HIGH (3.3V).
 *
 *  La mayoría de módulos genéricos son activos bajos. Si el tuyo es
 *  activo alto, intercambia los valores de RELE_ON y RELE_OFF.
 *
 *  En CUALQUIER caso, RELE_OFF debe ser el estado natural del pin al
 *  arrancar el ESP8266, para que la cerradura quede cerrada por defecto
 *  (fail-secure). La P82F-5V queda bloqueada SIN energía.
 * ===================================================================== */

#define RELE_ON         LOW    // Energiza la bobina del relé → libera la cerradura
#define RELE_OFF        HIGH   // Relé desactivado → cerradura bloqueada


/* =====================================================================
 *  SECCIÓN 6b — LÓGICA DE DETECCIÓN DE LOS SENSORES IR
 *
 *  No todos los sensores IR usan el mismo nivel lógico para señalar
 *  detección. Hay dos familias comunes en proyectos Arduino/ESP:
 *
 *    - Sensores PIR de movimiento (HC-SR501, AM312...):
 *         salida = HIGH cuando detectan movimiento.
 *         salida = LOW  en reposo.
 *
 *    - Sensores IR de obstáculo / barrera (FC-51, KY-032, TCRT5000...):
 *         salida = LOW  cuando hay un obstáculo en el haz.
 *         salida = HIGH cuando el camino está libre.
 *
 *  Para que el firmware funcione igual con ambos tipos, aquí se declara
 *  cuál es el nivel que representa "DETECCIÓN" para cada nodo. Si más
 *  adelante cambias el modelo de sensor, basta ajustar este valor; la
 *  lógica del programa no se toca.
 *
 *  Valores válidos: HIGH o LOW.
 *
 *  IMPORTANTE — Comportamiento del firmware en función de este valor:
 *  La función inicializarAP() del .ino decide automáticamente el modo
 *  del pin de entrada según este valor, para que el sistema sea robusto
 *  ante una desconexión accidental del sensor:
 *
 *    - Si DETECCION == LOW (sensor de obstáculo):
 *      el pin se configura como INPUT_PULLUP. Si el sensor pierde
 *      alimentación o se desconecta, el pull-up interno mantiene el
 *      pin en HIGH ("sin detección") y NO se dispara alarma falsa.
 *
 *    - Si DETECCION == HIGH (sensor PIR):
 *      el pin se configura como INPUT simple. En este caso se DEBE
 *      colocar una resistencia externa de 10 kΩ entre el pin y GND,
 *      ya que el ESP8266 no tiene pull-down interno en GPIO5/GPIO12.
 *      Sin esa resistencia, si el sensor se desconecta el pin queda
 *      flotando y dispara alarmas falsas.
 * ===================================================================== */

#define IR_PUERTA_DETECCION    LOW    // Sensor de obstáculo: LOW = obstáculo detectado
#define IR_GARAJE_DETECCION    LOW    // Cambiar a HIGH si el sensor del garaje es PIR


/* =====================================================================
 *  SECCIÓN 7 — PARÁMETROS DEL BUZZER
 *
 *  El buzzer se controla con PWM (modulación por ancho de pulso) para
 *  generar una onda cuadrada que produce un sonido audible.
 *
 *  POR QUÉ NO USAMOS LA FUNCIÓN tone():
 *  La función tone() del framework Arduino y la librería Servo (que
 *  controla el portón del garaje) usan el mismo temporizador interno
 *  del ESP8266 (Timer 1). Usar ambas a la vez causa conflictos. Por
 *  eso usamos analogWrite() con frecuencia configurable, que usa otro
 *  mecanismo y NO choca con el servo.
 * ===================================================================== */

#define BUZZER_FRECUENCIA_HZ    2000   // 2 kHz - frecuencia bien audible para alarmas
#define BUZZER_DUTY             512    // Ciclo de trabajo 50% (rango ESP8266: 0-1023)
#define BUZZER_INTERMITENCIA_MS 250    // Cada 250ms cambia ON↔OFF para el patrón "beep beep"


/* =====================================================================
 *  SECCIÓN 8 — PARÁMETROS DEL SERVOMOTOR (solo garaje)
 *
 *  El portón usa un servo POSICIONAL: el EMAX ES08MA II.
 *  En un servo posicional, servo.write(angulo) MUEVE el eje a ese ángulo
 *  y lo mantiene ahí. En este montaje:
 *      write(180) → posicionado en 180° (portón CERRADO)
 *      write(90)  → posicionado en 90°  (portón ABIERTO)
 *  El recorrido se controla por ÁNGULO.
 *
 *  El firmware abre llevando el servo a SERVO_POS_ABIERTO y CERRARÁ
 *  llevándolo a SERVO_POS_CERRADO; en cada caso se queda en esa posición
 *  (no hace ida y vuelta). El movimiento se hace grado por grado para que
 *  sea suave y no demande picos de corriente.
 * ===================================================================== */

// ---- Ángulos del portón (CALIBRAR según tu mecanismo) ----
#define SERVO_POS_CERRADO      0    // Ángulo con el portón cerrado
#define SERVO_POS_ABIERTO      90     // Ángulo con el portón abierto

// ---- RANGO DE PULSO del servo, en microsegundos (CALIBRACIÓN CLAVE) ----
// La librería Servo, por defecto, usa 544–2400 µs para 0°–180°. Ese rango
// rara vez coincide con el servo real: hace que el servo no llegue a los
// extremos o que empuje contra sus topes internos. Aquí declaramos el rango
// real del EMAX ES08MA II para que los ángulos caigan en posiciones físicas
// correctas y alcanzables.
//
// En este montaje el portón trabaja entre 90° (abierto) y 180° (cerrado),
// así que el extremo "cerrado" usa pulsos cercanos a SERVO_PULSO_MAX_US.
//
// CÓMO CALIBRAR (ver explicación abajo):
//  - Si en 180° (cerrado) el servo zumba o pega contra el tope → BAJA SERVO_PULSO_MAX_US.
//  - Si no llega a un 180° real (cierra corto) → SUBE SERVO_PULSO_MAX_US.
//  - Si en 90° (abierto) no queda donde esperas, ajusta el ángulo SERVO_POS_ABIERTO.
// Valores típicos de arranque para este servo: 600 y 2400 (rango amplio).
#define SERVO_PULSO_MIN_US     600    // pulso (µs) que corresponde a 0°
#define SERVO_PULSO_MAX_US     2600   // pulso (µs) que corresponde a 180°

// BARRIDO GRADUAL — paso del movimiento suave (ms por grado):
// En lugar de saltar de un ángulo al otro de golpe (lo cual demanda un pico
// de corriente que hace que el servo vibre sin moverse en fuentes débiles),
// movemos el servo grado por grado con este pequeño delay entre pasos.
//
// Tiempo total ≈ |SERVO_POS_ABIERTO − SERVO_POS_CERRADO| × SERVO_PASO_MS
// Con un recorrido de 90° y 15 ms por paso → ~1.35 s de extremo a extremo.
// Súbelo para un movimiento más lento y suave; bájalo para que sea más rápido.
#define SERVO_PASO_MS            15

// Pausa breve al llegar al extremo, para que el servo asiente la posición
// antes de soltarlo o de seguir.
#define SERVO_PAUSA_EXTREMO_MS   300    // ms de espera al final del recorrido

// ¿Qué hace el servo DESPUÉS de llegar a su posición (abierto o cerrado)?
//   0 = soltar el servo (detach): bajo consumo, sin zumbido. El portón se
//       sostiene por la fricción del mecanismo. Recomendado con fuentes
//       débiles (MB102) y portones livianos.
//   1 = mantener la señal: el servo conserva su torque y sostiene la posición
//       con fuerza. Úsalo solo si el portón tiende a caerse o moverse solo;
//       consume más corriente y algunos servos analógicos zumban al sostener.
#define SERVO_MANTENER_TORQUE    0


/* =====================================================================
 *  SECCIÓN 9 — TEMPORIZADORES DEL SISTEMA
 *
 *  Todos los tiempos están en milisegundos.
 * =====================================================================*/

// Después de una apertura autorizada, ¿cuántos ms se mantiene la
// cerradura abierta antes de cerrarse sola?
#define VENTANA_APERTURA_MS    10000   // 10 segundos

// ¿Cuántos ms suena el buzzer como máximo antes de apagarse solo?
// Esto evita molestar a los vecinos indefinidamente si nadie silencia
// la alarma desde la app.
#define DURACION_MAX_ALARMA_MS 60000   // 60 segundos

// Tiempo mínimo que la señal del sensor IR debe mantenerse alta para
// considerarla un movimiento real (filtra parpadeos espurios).
#define DEBOUNCE_IR_MS         300     // 300 milisegundos

// Cada cuántos ms el dispositivo publica su estado completo a la nube
// (heartbeat o latido). Sirve para que la app sepa que sigue vivo.
#define INTERVALO_LATIDO_MS    15000   // 15 segundos

// Tiempo mínimo entre intentos de reconectarse al broker MQTT cuando
// la conexión se cayó. Evita saturar al broker con peticiones.
#define INTENTO_RECONEXION_MS  5000    // 5 segundos

#endif // CONFIG_H