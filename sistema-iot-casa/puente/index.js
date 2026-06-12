/* =====================================================================
 * ARCHIVO   : index.js
 * PROYECTO  : Sistema IoT de seguridad doméstica
 * COMPONENTE: Puente Node.js entre MQTT y Firebase
 *
 * PROPÓSITO
 * ---------
 * Este programa actúa como "traductor" entre dos mundos que no se hablan
 * directamente: el broker MQTT (donde vive el firmware del ESP8266) y
 * Firebase (donde vive la aplicación web del usuario).
 *
 *
 * UBICACIÓN EN LA ARQUITECTURA
 * ----------------------------
 *
 *       [App web]                                       [ESP8266]
 *           ↑↓                                              ↑↓
 *       [Firebase]    ←————   ESTE PUENTE   ————→    [Broker MQTT]
 *
 * Sin este puente, la app web no podría enterarse de lo que pasa en el
 * dispositivo, ni el dispositivo recibiría las órdenes del usuario.
 *
 *
 * CUATRO FUNCIONES PRINCIPALES
 * ----------------------------
 *
 *   FUNCIÓN 1 — MQTT → Firebase (estados):
 *     Cuando el ESP8266 publica su estado (cada 15s o tras un cambio),
 *     este puente lo recibe y lo escribe en Realtime Database. La web
 *     escucha esa base de datos y se actualiza al instante.
 *
 *   FUNCIÓN 2 — Firebase → MQTT (comandos):
 *     Cuando un usuario presiona "Abrir puerta" en la web, esa orden
 *     se escribe en Realtime Database. Este puente la detecta, le agrega
 *     la clave local de seguridad, y la publica al broker MQTT para que
 *     el ESP8266 la ejecute.
 *
 *   FUNCIÓN 3 — Sincronización de permisos:
 *     Los usuarios y sus permisos viven en Firestore. Las reglas de
 *     seguridad de Realtime Database necesitan saber esos permisos
 *     pero no pueden consultar Firestore directamente. Este puente
 *     copia los permisos de cada usuario a Realtime Database cada vez
 *     que cambian.
 *
 *   FUNCIÓN 4 — Notificaciones push:
 *     Cuando el ESP8266 publica un evento de alarma, este puente busca
 *     los tokens FCM (Firebase Cloud Messaging) de los usuarios con
 *     permiso sobre ese punto de acceso, y les envía una notificación
 *     push a sus celulares.
 *
 *
 * REQUISITOS PARA EJECUTAR
 * ------------------------
 *   1. Node.js instalado (verificar con `node --version`).
 *   2. Archivo `serviceAccountKey.json` en la misma carpeta (descargado
 *      de Firebase → Cuentas de servicio).
 *   3. Archivo `.env` con las credenciales (ver `.env.example`).
 *   4. Ejecutar `npm install` para descargar las dependencias.
 *   5. Ejecutar `npm start` para iniciar el puente.
 *
 *
 * AUTORES
 * -------
 *   Manuela Rivera Gómez
 *   Janier Alexander Rojas Giraldo
 *   Luis Alfonso Ortiz Ruiz
 *
 *   Instituto Tecnológico Metropolitano · Medellín, Colombia
 * ===================================================================== */


/* =====================================================================
 *  SECCIÓN 1 — IMPORTACIÓN DE LIBRERÍAS Y CARGA DE CONFIGURACIÓN
 * ===================================================================== */

// Lee las variables del archivo .env y las pone en process.env.
// Es el equivalente a config.h del firmware: separa credenciales del
// código para que se puedan cambiar sin tocar la lógica.
require('dotenv').config();

// Cliente MQTT para conectarnos al broker (HiveMQ Cloud en este caso).
const mqtt = require('mqtt');

// SDK de administrador de Firebase. Con esta librería el puente puede
// hacer CUALQUIER cosa en Firebase (es como ser superusuario), por eso
// la clave de cuenta de servicio es tan sensible.
const admin = require('firebase-admin');

// Las credenciales privadas que descargamos de Firebase. Este archivo
// NUNCA debe subirse a un repositorio público.
const serviceAccount = require('./serviceAccountKey.json');


/* =====================================================================
 *  SECCIÓN 2 — INICIALIZACIÓN DE FIREBASE
 *
 *  Aquí se configura el SDK con las credenciales del proyecto. Después
 *  de esta línea, podemos hacer admin.database(), admin.firestore(),
 *  admin.messaging(), etc.
 * ===================================================================== */

admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),  // Quién soy
  databaseURL: process.env.FIREBASE_DATABASE_URL      // A qué base de datos en tiempo real apunto
});

// Tres referencias a los servicios de Firebase que vamos a usar.
const rtdb     = admin.database();    // Realtime Database (estados en vivo)
const firestore = admin.firestore();  // Firestore (usuarios e historial de eventos)
const fcm      = admin.messaging();   // Firebase Cloud Messaging (notificaciones push)

console.log('[Firebase] Conectado al proyecto:', serviceAccount.project_id);


/* =====================================================================
 *  SECCIÓN 3 — CONEXIÓN AL BROKER MQTT
 *
 *  Aquí establecemos la conexión persistente con el broker. Usamos
 *  mqtts:// (con cifrado TLS) y las credenciales del archivo .env.
 * ===================================================================== */

const mqttClient = mqtt.connect(process.env.MQTT_BROKER_URL, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASSWORD,

  // Si la conexión se cae, reintenta cada 5 segundos.
  reconnectPeriod: 5000

  // NOTA: el client_id se genera automáticamente y es único, por eso
  // no choca con el del ESP8266 (que es "esp8266_casa_iot"). Si dos
  // clientes se conectaran al mismo broker con el mismo client_id,
  // el broker desconectaría al primero.
});

// Lista de tópicos a los que este puente debe SUSCRIBIRSE para enterarse
// de lo que publica el ESP8266.
const TOPICOS_ESCUCHAR = [
  'casa/dispositivo/disponible',   // El dispositivo está online/offline
  'casa/puerta/disponible',        // El nodo puerta está online/offline
  'casa/garaje/disponible',        // El nodo garaje está online/offline
  'casa/puerta/estado',            // Estado completo de la puerta (cada 15s)
  'casa/garaje/estado',            // Estado completo del garaje (cada 15s)
  'casa/puerta/evento',            // Eventos puntuales de la puerta (alarmas, autorizaciones)
  'casa/garaje/evento'             // Eventos puntuales del garaje
];

// Evento "connect": se dispara cuando logramos conectar al broker.
mqttClient.on('connect', () => {
  console.log('[MQTT] Conectado al broker');

  // Suscribirse a todos los tópicos definidos arriba.
  TOPICOS_ESCUCHAR.forEach((topico) => {
    mqttClient.subscribe(topico, (err) => {
      if (err) {
        console.error('[MQTT] Error al suscribirse a', topico, '-', err.message);
      } else {
        console.log('[MQTT] Suscrito a', topico);
      }
    });
  });
});

// Manejadores de eventos para depuración.
mqttClient.on('error',     (err) => console.error('[MQTT] Error:', err.message));
mqttClient.on('reconnect', ()    => console.log('[MQTT] Reintentando conexión...'));
mqttClient.on('offline',   ()    => console.warn('[MQTT] Cliente desconectado del broker'));


/* =====================================================================
 *  SECCIÓN 4 — FUNCIÓN 1: MQTT → FIREBASE
 *
 *  El evento "message" se dispara CADA VEZ que llega un mensaje a alguno
 *  de los tópicos suscritos. La función decide qué hacer según el tópico.
 *
 *  La función es asincrónica porque las escrituras a Firebase son
 *  asincrónicas (toman tiempo en completarse).
 * ===================================================================== */

mqttClient.on('message', async (topico, payload) => {

  // Los mensajes MQTT vienen como bytes crudos. Los convertimos a string.
  const mensaje = payload.toString();
  console.log(`[MQTT->FB] ${topico}: ${mensaje}`);

  try {

    /* ---- Mensajes de disponibilidad (online/offline) ----
     * Vienen como texto plano ("online" u "offline"), no JSON.
     * Se escriben directamente en /disponibilidad/{nodo} de Realtime DB.
     */
    if (topico === 'casa/dispositivo/disponible') {
      await rtdb.ref('disponibilidad/dispositivo').set(mensaje);
      return;
    }
    if (topico === 'casa/puerta/disponible') {
      await rtdb.ref('disponibilidad/puerta').set(mensaje);
      return;
    }
    if (topico === 'casa/garaje/disponible') {
      await rtdb.ref('disponibilidad/garaje').set(mensaje);
      return;
    }

    /* ---- Mensajes de estado completo ----
     * Vienen como JSON con todos los campos del nodo:
     *   - cerradura: "abierta" | "cerrada"
     *   - sensor_ir: "activo"  | "inactivo"
     *   - alarma:    "activa"  | "inactiva"
     *   - modo:      "ausente" | "presente"  (modo de operación)
     *   - estado:    "reposo" | "apertura_autorizada" | "alarma"
     *   - porton:    "abierto" | "cerrado"  (solo en garaje)
     *   - rssi, uptime_s
     * Se parsean y se escriben tal cual a /estado/{nodo} en RTDB.
     */
    if (topico === 'casa/puerta/estado') {
      await rtdb.ref('estado/puerta').set(JSON.parse(mensaje));
      return;
    }
    if (topico === 'casa/garaje/estado') {
      await rtdb.ref('estado/garaje').set(JSON.parse(mensaje));
      return;
    }

    /* ---- Mensajes de eventos puntuales ----
     * Son eventos discretos: alarmas, intentos de autenticación, errores.
     * Se hacen DOS cosas con ellos:
     *   1. Se guardan en Firestore /eventos/ como historial completo.
     *   2. Se escribe el último evento en RTDB /ultimoEvento/{nodo}
     *      para que la web pueda mostrar alertas en tiempo real.
     */
    if (topico === 'casa/puerta/evento' || topico === 'casa/garaje/evento') {
      const evento = JSON.parse(mensaje);
      const nodo = evento.nodo || (topico.includes('puerta') ? 'puerta' : 'garaje');

      // 1. Guardar en historial permanente (Firestore)
      await firestore.collection('eventos').add({
        nodo:           nodo,
        tipo:           evento.tipo || 'desconocido',
        detalle:        evento.detalle || '',
        tsDispositivo:  evento.ts || null,  // millis() del dispositivo
        recibidoEn:     admin.firestore.FieldValue.serverTimestamp()  // Hora del servidor
      });

      // 2. Publicar último evento (Realtime Database) para que la web
      //    lo detecte y muestre la alerta inmediatamente.
      await rtdb.ref(`ultimoEvento/${nodo}`).set({
        tipo:        evento.tipo || 'desconocido',
        detalle:     evento.detalle || '',
        recibidoEn:  Date.now()
      });

      // 3. Si es una alarma, además enviar notificación push a los celulares.
      //    (Se ejecuta sin await porque no queremos bloquear el procesamiento
      //    del siguiente mensaje esperando a que termine el envío push.)
      if (evento.tipo === 'alarma') {
        enviarNotificacionAlarma(nodo, evento);
      }
      return;
    }

    console.warn('[MQTT->FB] Tópico no reconocido:', topico);

  } catch (error) {
    console.error('[MQTT->FB] Error procesando mensaje:', error.message);
  }
});


/* =====================================================================
 *  SECCIÓN 5 — FUNCIÓN 4: NOTIFICACIONES PUSH (FCM)
 *
 *  Cuando llega una alarma, esta función:
 *    1. Lee todos los tokens FCM registrados en /fcmTokens/.
 *    2. Filtra para enviar solo a usuarios que tienen permiso sobre
 *       el punto de acceso donde ocurrió la alarma.
 *    3. Envía la notificación push a todos los celulares de esos usuarios.
 *    4. Limpia tokens inválidos (celulares que ya no están).
 * ===================================================================== */

/**
 * Envía una notificación push a todos los usuarios autorizados.
 *
 * @param {string} nodo    Punto de acceso donde se disparó la alarma ("puerta" o "garaje")
 * @param {object} evento  Objeto con los datos del evento de alarma
 */
async function enviarNotificacionAlarma(nodo, evento) {
  try {

    // PASO 1: Leer la lista completa de tokens FCM registrados.
    // Estructura en RTDB: /fcmTokens/{uid}/{deviceId}/{token, ua, updated}
    const tokensSnap = await rtdb.ref('fcmTokens').once('value');
    const tokensPorUsuario = tokensSnap.val() || {};

    // PASO 2: Leer la lista de usuarios para verificar permisos.
    // Solo los usuarios con permiso sobre el nodo afectado deben recibir
    // la notificación. (Ejemplo: alguien sin permiso de garaje no
    // necesita enterarse de la alarma del garaje.)
    const usuariosSnap = await rtdb.ref('usuarios').once('value');
    const usuarios = usuariosSnap.val() || {};

    // Determinar qué campo de permiso revisar según el nodo.
    const campoPermiso = nodo === 'puerta' ? 'permisoPuerta' : 'permisoGaraje';

    // PASO 3: Construir la lista final de tokens a notificar.
    const tokens = [];
    for (const [uid, devices] of Object.entries(tokensPorUsuario)) {
      const u = usuarios[uid];
      if (!u) continue;                       // Usuario no existe en RTDB
      if (u.activo === false) continue;       // Usuario desactivado
      if (!u[campoPermiso]) continue;         // No tiene permiso sobre este nodo

      // Recolectar todos los tokens de este usuario (puede tener varios
      // dispositivos: celular, tablet, computador, etc.).
      for (const tokenObj of Object.values(devices)) {
        if (tokenObj && tokenObj.token) tokens.push(tokenObj.token);
      }
    }

    if (tokens.length === 0) {
      console.log('[FCM] No hay tokens registrados para notificar la alarma');
      return;
    }

    // PASO 4: Construir el mensaje de notificación.
    const message = {
      notification: {
        title: `Alarma activa en ${nodo}`,
        body:  'Se detectó movimiento sin autorización. Revisa la app inmediatamente.'
      },
      data: {
        // Estos datos van en el payload pero no se muestran en pantalla.
        // La app los lee para tomar acciones específicas si el usuario
        // toca la notificación.
        nodo:    nodo,
        tipo:    'alarma',
        detalle: evento.detalle || '',
        ts:      String(Date.now())   // FCM exige que todos los valores de data sean strings
      },
      tokens: tokens
    };

    // PASO 5: Enviar la notificación a todos los tokens a la vez.
    // sendEachForMulticast envía hasta 500 destinatarios por llamada.
    const response = await fcm.sendEachForMulticast(message);
    console.log(`[FCM] Notificación enviada a ${response.successCount} de ${tokens.length} dispositivos`);

    // PASO 6: Limpiar tokens inválidos (celulares que desinstalaron
    // la app, cambiaron de dispositivo, etc.).
    if (response.failureCount > 0) {
      response.responses.forEach((r, i) => {
        if (!r.success && (
            r.error.code === 'messaging/invalid-registration-token' ||
            r.error.code === 'messaging/registration-token-not-registered'
        )) {
          console.log('[FCM] Token inválido, limpiando:', tokens[i].slice(-12));
          limpiarTokenInvalido(tokens[i]);
        }
      });
    }

  } catch (error) {
    console.error('[FCM] Error enviando notificación:', error.message);
  }
}

/**
 * Borra un token inválido de Realtime Database.
 *
 * Recorre /fcmTokens/ buscando el token específico y lo elimina cuando
 * lo encuentra. Mantiene la base de datos limpia para que las próximas
 * notificaciones no intenten enviarse a destinos muertos.
 *
 * @param {string} tokenInvalido  Token FCM que ya no funciona
 */
async function limpiarTokenInvalido(tokenInvalido) {
  try {
    const all = (await rtdb.ref('fcmTokens').once('value')).val() || {};
    for (const [uid, devices] of Object.entries(all)) {
      for (const [deviceId, obj] of Object.entries(devices)) {
        if (obj.token === tokenInvalido) {
          await rtdb.ref(`fcmTokens/${uid}/${deviceId}`).remove();
        }
      }
    }
  } catch (e) {
    console.error('[FCM] Error limpiando token:', e.message);
  }
}


/* =====================================================================
 *  SECCIÓN 6 — FUNCIÓN 2: FIREBASE → MQTT (comandos de la web)
 *
 *  Cuando un usuario presiona "Abrir puerta" en la web, esta sección
 *  detecta el comando y lo envía al ESP8266 por MQTT.
 *
 *  COMANDOS QUE EL FIRMWARE ACEPTA (en el campo action):
 *    - "abrir"          → libera la cerradura (10s por defecto)
 *    - "cerrar"         → bloquea la cerradura manualmente
 *    - "silenciar"      → detiene la alarma sonora
 *    - "estado"         → pide al firmware re-publicar su estado
 *    - "modo_ausente"   → activa la vigilancia del sensor IR
 *    - "modo_presente"  → desactiva el sensor IR (gente en casa)
 *
 *  El puente NO valida qué action es válido: simplemente reenvía lo que
 *  llegue de Firebase al broker MQTT. Esto mantiene el puente simple y
 *  permite agregar nuevas acciones modificando solo el firmware y la app.
 * ===================================================================== */

/**
 * Escucha la rama /comandos/{nodo} de Realtime Database y publica
 * cualquier comando nuevo al broker MQTT.
 *
 * El evento 'child_added' de Firebase se dispara cada vez que la web
 * hace push() de un nuevo comando bajo /comandos/{nodo}.
 *
 * @param {string} nodo  "puerta" o "garaje"
 */
function escucharComandos(nodo) {
  const ref = rtdb.ref(`comandos/${nodo}`);

  ref.on('child_added', async (snapshot) => {
    const comando = snapshot.val();
    console.log(`[FB->MQTT] Comando recibido para ${nodo}:`, JSON.stringify(comando));

    try {
      // Construir el mensaje MQTT que entiende el firmware.
      // Le agregamos la clave local (LOCAL_DEVICE_KEY), que es la segunda
      // barrera de seguridad. El firmware la verifica antes de ejecutar.
      const mensajeMqtt = {
        action: comando.action,
        key:    process.env.LOCAL_DEVICE_KEY,
        user:   comando.user || 'desconocido'
      };

      mqttClient.publish(`casa/${nodo}/cmd`, JSON.stringify(mensajeMqtt));
      console.log(`[FB->MQTT] Publicado en casa/${nodo}/cmd`);

      // Eliminar el comando de Firebase después de procesarlo para que
      // no se vuelva a ejecutar si el puente se reinicia.
      await snapshot.ref.remove();

    } catch (error) {
      console.error('[FB->MQTT] Error procesando comando:', error.message);
    }
  });
}

// Activar el listener para ambos puntos de acceso.
escucharComandos('puerta');
escucharComandos('garaje');
console.log('[FB] Escuchando comandos en /comandos/puerta y /comandos/garaje');


/* =====================================================================
 *  SECCIÓN 7 — FUNCIÓN 3: SINCRONIZACIÓN DE USUARIOS Firestore → RTDB
 *
 *  POR QUÉ EXISTE ESTA FUNCIÓN:
 *  Los usuarios y sus permisos viven en Firestore (es donde mejor encajan
 *  porque son datos relativamente estáticos con consultas complejas).
 *  Pero las REGLAS DE SEGURIDAD de Realtime Database también necesitan
 *  verificar esos permisos cuando un usuario hace una operación.
 *
 *  El problema: las reglas de Realtime DB NO pueden consultar Firestore.
 *  Solo pueden mirar datos dentro de la misma Realtime DB.
 *
 *  La solución: este puente copia automáticamente los permisos de cada
 *  usuario desde Firestore a Realtime DB cada vez que cambian. Las
 *  reglas de RTDB usan esa copia local.
 * ===================================================================== */

// onSnapshot() es un "listener" en tiempo real. Se dispara una vez al
// inicio (con todos los usuarios actuales) y después cada vez que algún
// documento de /usuarios/ cambia.
firestore.collection('usuarios').onSnapshot(
  (snapshot) => {

    // snapshot.docChanges() solo trae los documentos QUE CAMBIARON
    // (no todos cada vez), lo cual es más eficiente.
    snapshot.docChanges().forEach(async (change) => {
      const uid = change.doc.id;
      const datos = change.doc.data();

      try {
        if (change.type === 'removed') {
          // El usuario fue eliminado de Firestore → eliminarlo también
          // de RTDB y de la lista de tokens FCM.
          await rtdb.ref(`usuarios/${uid}`).remove();
          await rtdb.ref(`fcmTokens/${uid}`).remove();
          console.log(`[Sync] Usuario ${uid} eliminado de RTDB`);
        } else {
          // El usuario fue agregado o modificado → copiar sus permisos
          // a la versión "espejo" en RTDB.
          await rtdb.ref(`usuarios/${uid}`).set({
            permisoPuerta: datos.permisoPuerta === true,
            permisoGaraje: datos.permisoGaraje === true,
            rol:           datos.rol || 'usuario',
            activo:        datos.activo === true
          });
          console.log(`[Sync] Usuario ${uid} sincronizado a RTDB`);
        }
      } catch (error) {
        console.error('[Sync] Error sincronizando usuario', uid, '-', error.message);
      }
    });
  },
  (error) => console.error('[Sync] Error en el listener de usuarios:', error.message)
);

console.log('[Sync] Sincronización de usuarios Firestore → RTDB activa');


/* =====================================================================
 *  SECCIÓN 8 — MANEJO DE CIERRE LIMPIO
 *
 *  Cuando alguien presiona Ctrl+C en la terminal, Node.js recibe la
 *  señal SIGINT. Antes de cerrar, queremos desconectarnos del broker
 *  MQTT de manera ordenada (para que el Last Will Testament se active
 *  correctamente y la web sepa que estamos offline).
 * ===================================================================== */

process.on('SIGINT', () => {
  console.log('\n[Sistema] Cerrando puente...');
  mqttClient.end();   // Desconexión limpia del broker
  process.exit(0);
});


/* =====================================================================
 *  FIN DEL ARCHIVO — Banner de arranque
 * ===================================================================== */

console.log('==========================================');
console.log(' Puente IoT Casa v2 - EN EJECUCIÓN');
console.log(' (con notificaciones push)');
console.log(' Presiona Ctrl+C para detener');
console.log('==========================================');