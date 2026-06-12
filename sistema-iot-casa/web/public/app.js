/* =====================================================================
 * ARCHIVO   : app.js
 * PROYECTO  : Sistema IoT de seguridad doméstica
 * COMPONENTE: Aplicación web (Single Page Application)
 *
 * PROPÓSITO
 * ---------
 * Este archivo contiene TODA la lógica de la aplicación web que abre el
 * usuario en su celular o computador. Maneja desde el login hasta el
 * dashboard en vivo y el panel de administrador.
 *
 *
 * UBICACIÓN EN LA ARQUITECTURA
 * ----------------------------
 *
 *  [Usuario en celular]  ←——  ESTE ARCHIVO  ——→  [Firebase]  ←→  [Puente]  ←→  [ESP8266]
 *
 * Esta web es la "cara" del sistema. No habla con el ESP8266 directamente:
 * todas sus interacciones pasan por Firebase, y de ahí el puente Node.js
 * las traduce al lenguaje del dispositivo.
 *
 *
 * ORGANIZACIÓN DEL CÓDIGO
 * -----------------------
 * El código está dividido en SECCIONES claramente delimitadas:
 *
 *   1. Importaciones de Firebase SDK
 *   2. Inicialización de Firebase
 *   3. Estado global de la aplicación
 *   4. Utilidades de la interfaz (toasts, modales, formato)
 *   5. Autenticación (login y logout)
 *   6. Carga del perfil del usuario
 *   7. Dashboard: estados en tiempo real
 *   8. Envío de comandos al dispositivo
 *   9. Historial de eventos
 *  10. Notificaciones push (FCM)
 *  11. Panel de administrador: gestión de usuarios
 *  12. Inicialización: enlazado de eventos del DOM
 *
 *
 * DECISIONES TÉCNICAS RELEVANTES
 * -------------------------------
 *   - Vanilla JavaScript (sin React, Vue ni frameworks): mantiene la
 *     app pequeña y fácil de entender en una sustentación académica.
 *   - Firebase SDK v10 modular: solo se importan los módulos necesarios.
 *   - Event delegation en el botón principal: en vez de adjuntar muchos
 *     listeners, capturamos clicks a nivel del documento y deducimos
 *     qué acción ejecutar por los data-attributes del elemento.
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
 *  SECCIÓN 1 — IMPORTACIÓN DE LIBRERÍAS
 *
 *  Importamos los módulos de Firebase desde el CDN oficial de Google.
 *  Solo traemos las funciones específicas que vamos a usar, lo que hace
 *  la app más liviana (en vez de cargar el SDK completo).
 * ===================================================================== */

// Configuración del proyecto Firebase (apiKey, projectId, etc.) y la
// clave VAPID para notificaciones push. Estos valores son específicos
// de cada proyecto.
import { firebaseConfig, vapidKey } from './firebase-config.js';

// ----- Núcleo de Firebase -----
import { initializeApp } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-app.js";

// ----- Authentication: login, logout, gestión de usuarios -----
import {
  getAuth,                          // Acceso al servicio de auth
  signInWithEmailAndPassword,       // Iniciar sesión con correo/contraseña
  signOut,                          // Cerrar sesión
  onAuthStateChanged,               // "Escuchar" cambios en la sesión
  createUserWithEmailAndPassword    // Registrar un nuevo usuario (admin)
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-auth.js";

// ----- Firestore: base de datos de usuarios y eventos -----
import {
  getFirestore, doc, getDoc, setDoc, updateDoc, deleteDoc,
  collection, query, orderBy, limit, onSnapshot
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-firestore.js";

// ----- Realtime Database: estados en vivo y comandos -----
import {
  getDatabase, ref, onValue, push, set, remove
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-database.js";

// ----- Cloud Messaging: notificaciones push -----
import {
  getMessaging, getToken, onMessage
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-messaging.js";


/* =====================================================================
 *  SECCIÓN 2 — INICIALIZACIÓN DE FIREBASE
 *
 *  Aquí creamos las instancias de cada servicio. Estas variables son
 *  las que usaremos en todo el archivo para interactuar con Firebase.
 * ===================================================================== */

const app  = initializeApp(firebaseConfig);
const auth = getAuth(app);            // Servicio de autenticación
const db   = getFirestore(app);       // Firestore (DB documental)
const rtdb = getDatabase(app);        // Realtime Database (DB en árbol JSON)

// Messaging puede fallar en navegadores que no soportan service workers
// (por ejemplo Safari en modo privado). Lo manejamos con try/catch para
// que el resto de la app siga funcionando aunque las notificaciones no.
let messaging = null;
try {
  messaging = getMessaging(app);
} catch (e) {
  console.warn('Mensajería no disponible en este navegador');
}


/* =====================================================================
 *  SECCIÓN 3 — ESTADO GLOBAL DE LA APLICACIÓN
 *
 *  Un objeto único que contiene el estado vivo de la app: quién está
 *  logueado, sus permisos, y la lista de "listeners" activos (para
 *  poder limpiarlos al cerrar sesión).
 * ===================================================================== */

const state = {
  user:           null,    // Objeto del usuario autenticado (de Firebase Auth)
  profile:        null,    // Documento del usuario en Firestore (nombre, permisos, rol)
  listeners:      [],      // Funciones para "desuscribirse" de los listeners activos
  pendingConfirm: null     // Callback pendiente del modal de confirmación
};


/* =====================================================================
 *  SECCIÓN 4 — UTILIDADES DE LA INTERFAZ
 *
 *  Funciones helper que se usan a lo largo del código para:
 *    - Seleccionar elementos del DOM ($, $$)
 *    - Cambiar entre vistas (showView)
 *    - Mostrar mensajes flotantes (toast)
 *    - Abrir/cerrar modales
 *    - Confirmar acciones destructivas
 *    - Formatear fechas relativas
 *    - Mostrar el estado de carga en botones
 * ===================================================================== */

/**
 * Selecciona el PRIMER elemento que coincida con el selector CSS.
 * Es el equivalente abreviado de document.querySelector().
 */
function $(sel) { return document.querySelector(sel); }

/**
 * Selecciona TODOS los elementos que coincidan con el selector CSS.
 */
function $$(sel) { return document.querySelectorAll(sel); }

/**
 * Cambia la vista visible en la pantalla.
 * La app tiene tres vistas (login, dashboard, admin) y solo una está
 * visible a la vez. Esta función oculta las otras dos y muestra la
 * solicitada.
 *
 * @param {string} viewId  ID de la vista a mostrar ("view-login", "view-dashboard", "view-admin")
 */
function showView(viewId) {
  ['view-login', 'view-dashboard', 'view-admin'].forEach(id => {
    $(`#${id}`).classList.toggle('hidden', id !== viewId);
  });
  window.scrollTo(0, 0);
}

/**
 * Muestra un mensaje flotante (toast) en la esquina inferior derecha.
 * Útil para confirmar acciones ("Comando enviado") o informar errores.
 *
 * @param {string} message     Texto a mostrar
 * @param {string} type        'success' | 'error' | 'warning' | 'info'
 * @param {number} durationMs  Cuántos ms permanece visible (por defecto 3500)
 */
function toast(message, type = 'info', durationMs = 3500) {
  const container = $('#toast-container');
  const el = document.createElement('div');
  el.className = `toast toast-${type}`;

  // Cada tipo de toast tiene un ícono diferente para que se identifique
  // rápido sin leer.
  const icons = {
    success: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>',
    error:   '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>',
    warning: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>',
    info:    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>'
  };
  el.innerHTML = `${icons[type] || icons.info}<span>${message}</span>`;
  container.appendChild(el);

  // El toast desaparece solo después del tiempo configurado.
  setTimeout(() => {
    el.classList.add('toast-removing');
    setTimeout(() => el.remove(), 250);
  }, durationMs);
}

/** Abre un modal por su ID. */
function openModal(id)  { $(`#${id}`).classList.remove('hidden'); }
/** Cierra un modal por su ID. */
function closeModal(id) { $(`#${id}`).classList.add('hidden');    }

/**
 * Muestra un modal de confirmación antes de ejecutar una acción peligrosa
 * (por ejemplo, eliminar un usuario).
 *
 * @param {string}   title      Título del modal
 * @param {string}   message    Texto explicativo
 * @param {Function} onConfirm  Función a ejecutar si el usuario confirma
 */
function confirmAction(title, message, onConfirm) {
  $('#modal-confirm-title').textContent   = title;
  $('#modal-confirm-message').textContent = message;
  state.pendingConfirm = onConfirm;   // Lo guardamos para ejecutarlo cuando se haga clic en "Confirmar"
  openModal('modal-confirm');
}

/**
 * Convierte un timestamp en un texto legible relativo.
 * Ej: 1734567890 → "Hace 5 min"
 *
 * @param {number|object} ts  Timestamp en ms o Firestore Timestamp
 * @returns {string}          Texto formateado
 */
function formatTimeAgo(ts) {
  if (!ts) return '';
  const ms = typeof ts === 'number'
             ? ts
             : (ts.seconds ? ts.seconds * 1000 : Date.now());
  const diff = Date.now() - ms;
  const min = Math.floor(diff / 60000);
  if (min < 1)   return 'Ahora';
  if (min < 60)  return `Hace ${min} min`;
  const hr = Math.floor(min / 60);
  if (hr < 24)   return `Hace ${hr} h`;
  const days = Math.floor(hr / 24);
  if (days < 7)  return `Hace ${days} d`;
  const d = new Date(ms);
  return d.toLocaleDateString('es-CO', { day: '2-digit', month: 'short' });
}

/**
 * Cambia visualmente un botón a estado de "cargando" (con spinner)
 * mientras se ejecuta una operación asíncrona.
 *
 * @param {HTMLElement} btn      Botón a modificar
 * @param {boolean}     loading  true para mostrar spinner, false para volver al normal
 */
function setButtonLoading(btn, loading) {
  const text    = btn.querySelector('.btn-text');
  const spinner = btn.querySelector('.btn-spinner');
  if (loading) {
    btn.disabled = true;
    if (text)    text.classList.add('hidden');
    if (spinner) spinner.classList.remove('hidden');
  } else {
    btn.disabled = false;
    if (text)    text.classList.remove('hidden');
    if (spinner) spinner.classList.add('hidden');
  }
}


/* =====================================================================
 *  SECCIÓN 5 — AUTENTICACIÓN (LOGIN Y LOGOUT)
 * ===================================================================== */

/**
 * Maneja el envío del formulario de login.
 *
 * Intenta autenticar al usuario con Firebase. Si tiene éxito,
 * onAuthStateChanged (más abajo) detectará el cambio y cargará el
 * dashboard. Si falla, muestra un mensaje de error legible.
 *
 * @param {Event} e  Evento submit del formulario
 */
async function handleLogin(e) {
  e.preventDefault();

  const email      = $('#login-email').value.trim();
  const password   = $('#login-password').value;
  const errorEl    = $('#login-error');
  const submitBtn  = e.target.querySelector('button[type="submit"]');

  // Limpiar errores previos y mostrar spinner.
  errorEl.classList.add('hidden');
  setButtonLoading(submitBtn, true);

  try {
    await signInWithEmailAndPassword(auth, email, password);
    // Si llegamos aquí, el login fue exitoso. onAuthStateChanged se
    // disparará automáticamente y mostrará el dashboard.
  } catch (err) {
    // Traducir códigos de error de Firebase a mensajes amigables en español.
    let msg = 'Error al iniciar sesión';
    if (err.code === 'auth/invalid-credential' ||
        err.code === 'auth/wrong-password' ||
        err.code === 'auth/user-not-found') {
      msg = 'Correo o contraseña incorrectos';
    } else if (err.code === 'auth/too-many-requests') {
      msg = 'Demasiados intentos. Espera un momento.';
    } else if (err.code === 'auth/network-request-failed') {
      msg = 'Sin conexión a internet';
    }
    errorEl.textContent = msg;
    errorEl.classList.remove('hidden');
    setButtonLoading(submitBtn, false);
  }
}

/**
 * Cierra la sesión del usuario.
 *
 * Antes de hacer signOut(), limpia todos los listeners activos para
 * evitar fugas de memoria y peticiones después del logout.
 */
async function handleLogout() {
  cleanupListeners();
  await signOut(auth);
  // onAuthStateChanged se disparará y volverá a la vista de login.
}

/**
 * Cancela todos los listeners de Firebase activos.
 *
 * Firebase devuelve una función "unsubscribe" cuando creas un listener.
 * Guardamos esas funciones en state.listeners y las invocamos todas
 * cuando el usuario cierra sesión, para que no queden "fantasmas".
 */
function cleanupListeners() {
  state.listeners.forEach(unsub => {
    try { unsub(); } catch (e) { /* ignorar errores al desuscribirse */ }
  });
  state.listeners = [];
}


/* =====================================================================
 *  SECCIÓN 6 — CARGA DEL PERFIL DE USUARIO
 *
 *  Después de un login exitoso, esta función se encarga de:
 *    1. Cargar el documento del usuario desde Firestore.
 *    2. Verificar que esté activo.
 *    3. Configurar la interfaz según sus permisos.
 *    4. Activar todos los listeners en tiempo real.
 * ===================================================================== */

/**
 * Procesa el inicio de sesión: carga perfil y configura el dashboard.
 *
 * @param {object} user  Objeto del usuario provisto por Firebase Auth
 */
async function onUserSignedIn(user) {
  state.user = user;

  // PASO 1: Cargar el documento del usuario desde Firestore.
  // El documento debe existir en /usuarios/{uid}. Si no existe, el
  // usuario fue creado en Auth pero no en Firestore (inconsistencia).
  try {
    const snap = await getDoc(doc(db, 'usuarios', user.uid));
    if (!snap.exists()) {
      toast('Tu usuario no está registrado en la base de datos. Contacta al administrador.',
            'error', 6000);
      await signOut(auth);
      return;
    }
    state.profile = { uid: user.uid, ...snap.data() };

    // PASO 2: Verificar que el usuario esté activo. Permite bloquear
    // accesos sin tener que eliminar al usuario completamente.
    if (state.profile.activo === false) {
      toast('Tu cuenta está inactiva. Contacta al administrador.', 'error', 6000);
      await signOut(auth);
      return;
    }
  } catch (err) {
    console.error('Error cargando perfil:', err);
    toast('No pudimos cargar tu perfil', 'error');
    return;
  }

  // PASO 3: Configurar la interfaz con los datos del usuario.
  $('#user-name').textContent = state.profile.nombre || state.profile.correo;

  // El botón de admin solo se muestra a usuarios con rol "admin".
  $('#btn-admin').classList.toggle('hidden', state.profile.rol !== 'admin');

  applyPermissionsToCards();
  showView('view-dashboard');

  // PASO 4: Activar los listeners en tiempo real.
  // Cada listener escucha una parte específica de Firebase y actualiza
  // la interfaz cuando hay cambios.
  attachStatusListener('puerta');     // Estado de la puerta
  attachStatusListener('garaje');     // Estado del garaje
  attachAvailabilityListener();       // ESP8266 online/offline
  attachEventsListener();             // Historial de eventos
  attachAlarmListeners();             // Detectar nuevas alarmas

  // PASO 5: Configurar notificaciones push.
  setupNotifications();
}

/**
 * Aplica los permisos del usuario a las tarjetas de puerta y garaje.
 *
 * Si el usuario no tiene permiso sobre un punto de acceso, la tarjeta
 * se muestra deshabilitada (botones grises sin efecto).
 */
function applyPermissionsToCards() {
  const p = state.profile;
  $('#card-puerta').classList.toggle('disabled', !p.permisoPuerta);
  $('#card-garaje').classList.toggle('disabled', !p.permisoGaraje);
}


/* =====================================================================
 *  SECCIÓN 7 — DASHBOARD: ESTADOS EN TIEMPO REAL
 *
 *  Estos listeners escuchan los estados que el puente Node.js escribe
 *  en Realtime Database. Cada vez que el ESP8266 publica un cambio,
 *  estos listeners lo detectan y actualizan la interfaz al instante.
 * ===================================================================== */

/**
 * Activa un listener en /estado/{nodo} de Realtime DB.
 * Actualiza la tarjeta visual del punto de acceso cuando cambia el estado.
 *
 * @param {string} nodo  "puerta" o "garaje"
 */
function attachStatusListener(nodo) {
  const r = ref(rtdb, `estado/${nodo}`);

  // onValue se dispara una vez al inicio (con los datos actuales) y
  // después cada vez que el valor cambia.
  const unsub = onValue(r, (snap) => {
    const data = snap.val();
    if (!data) return;
    renderAccessCard(nodo, data);
  });

  // Guardar la función unsubscribe para limpiar al cerrar sesión.
  state.listeners.push(unsub);
}

/**
 * Renderiza visualmente los datos de un punto de acceso en su tarjeta.
 *
 * @param {string} nodo  "puerta" o "garaje"
 * @param {object} data  Objeto con cerradura, sensor_ir, alarma, etc.
 */
function renderAccessCard(nodo, data) {
  const card = $(`#card-${nodo}`);
  if (!card) return;

  // Pintar el estado general (reposo, apertura, alarma) en la "píldora"
  // de la esquina superior.
  const overall     = card.querySelector('.access-overall');
  const overallText = card.querySelector('.overall-text');
  const estado      = data.estado || 'reposo';

  // dataset se usa para que el CSS cambie colores según el estado.
  card.dataset.overall  = estado;
  overall.dataset.state = estado;
  overallText.textContent = ({
    'reposo':              'Reposo',
    'apertura_autorizada': 'Apertura',
    'alarma':              'Alarma activa'
  })[estado] || 'Reposo';

  // Pintar cada indicador específico (cerradura, sensor, alarma).
  // setIndicator también aplica clases CSS para colorear según el valor.
  setIndicator(card, 'cerradura', data.cerradura, {
    'cerrada': 'state-ok',
    'abierta': 'state-info'
  });
  setIndicator(card, 'sensor_ir', data.sensor_ir, {
    'activo':   'state-ok',
    'inactivo': 'state-warning'
  });
  setIndicator(card, 'alarma', data.alarma, {
    'inactiva': 'state-ok',
    'activa':   'state-alarm'
  });
  if (nodo === 'garaje') {
    setIndicator(card, 'porton', data.porton || '—', {
      'cerrado': 'state-ok',
      'abierto': 'state-info'
    });
  }
  if (data.rssi !== undefined) {
    const rssiEl = card.querySelector('[data-key="rssi"]');
    if (rssiEl) rssiEl.textContent = `${data.rssi} dBm`;
  }

  // Modo de vigilancia (ausente/presente): actualizar texto del indicador
  // y resaltar el botón correspondiente.
  const modo = data.modo || 'ausente';
  const modoEl = card.querySelector('[data-key="modo"]');
  if (modoEl) {
    modoEl.textContent = modo === 'ausente' ? 'Ausente' : 'Presente';
    modoEl.classList.toggle('mode-presente', modo === 'presente');
  }
  card.querySelectorAll('.mode-btn').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.mode === modo);
  });

  // El botón "Silenciar alarma" solo aparece cuando la alarma está activa.
  const silenciar = card.querySelector('[data-action="silenciar"]');
  if (silenciar) silenciar.classList.toggle('hidden', data.alarma !== 'activa');
}

/**
 * Establece el valor y la clase visual de un indicador.
 *
 * @param {HTMLElement} card      Tarjeta contenedora
 * @param {string}      key       Nombre del indicador (data-key)
 * @param {string}      value     Valor a mostrar
 * @param {object}      classMap  Mapa de valores a clases CSS (para colorear)
 */
function setIndicator(card, key, value, classMap) {
  const el = card.querySelector(`[data-key="${key}"]`);
  if (!el) return;
  el.textContent = value || '—';

  // Limpiar todas las clases de estado posibles y aplicar la correcta.
  el.classList.remove('state-ok', 'state-warning', 'state-alarm', 'state-info');
  if (classMap[value]) el.classList.add(classMap[value]);
}

/**
 * Listener que muestra el estado de conectividad del dispositivo
 * (online/offline) en la barra superior.
 */
function attachAvailabilityListener() {
  const r = ref(rtdb, 'disponibilidad/dispositivo');
  const unsub = onValue(r, (snap) => {
    const status = snap.val();
    const el = $('#device-status');
    el.classList.remove('online', 'offline');

    if (status === 'online') {
      el.classList.add('online');
      el.querySelector('.status-label').textContent = 'Dispositivo en línea';
    } else if (status === 'offline') {
      el.classList.add('offline');
      el.querySelector('.status-label').textContent = 'Dispositivo desconectado';
    } else {
      el.querySelector('.status-label').textContent = 'Sin datos';
    }
  });
  state.listeners.push(unsub);
}

/**
 * Listener que detecta NUEVAS alarmas y notifica al usuario inmediatamente.
 *
 * Usa una bandera firstLoad para ignorar el primer "snapshot" inicial
 * (que solo trae los datos que ya estaban en la base, no son alarmas
 * nuevas).
 */
function attachAlarmListeners() {
  ['puerta', 'garaje'].forEach(nodo => {
    let firstLoad = true;
    const r = ref(rtdb, `ultimoEvento/${nodo}`);

    const unsub = onValue(r, (snap) => {
      // Ignorar la primera lectura (datos preexistentes).
      if (firstLoad) {
        firstLoad = false;
        return;
      }
      const evt = snap.val();
      if (evt && evt.tipo === 'alarma') {
        showLocalNotification(nodo, evt);
      }
    });
    state.listeners.push(unsub);
  });
}

/**
 * Muestra una alerta al usuario cuando se detecta una alarma:
 *   - Un toast rojo en la esquina inferior derecha
 *   - Una notificación del navegador (si el usuario dio permiso)
 *
 * @param {string} nodo  Punto de acceso donde ocurrió
 * @param {object} evt   Datos del evento
 */
function showLocalNotification(nodo, evt) {
  toast(`Alarma en ${nodo}!`, 'error', 6000);

  if ('Notification' in window && Notification.permission === 'granted') {
    new Notification(`Alarma activa en ${nodo}`, {
      body: 'Se detectó movimiento sin autorización',
      icon: '/icon-192.png',
      tag:  `alarma-${nodo}`,           // Evita notificaciones duplicadas del mismo nodo
      requireInteraction: true          // Se queda hasta que el usuario la cierre
    });
  }
}


/* =====================================================================
 *  SECCIÓN 8 — ENVÍO DE COMANDOS AL DISPOSITIVO
 *
 *  Cuando un usuario presiona "Abrir puerta", esta sección escribe el
 *  comando en Realtime Database. El puente Node.js lo detectará y lo
 *  publicará al broker MQTT.
 * ===================================================================== */

/**
 * Helper para capitalizar la primera letra de una cadena.
 * Se usa en mensajes ("puerta" → "Puerta").
 */
function capitalize(s) { return s ? s.charAt(0).toUpperCase() + s.slice(1) : ''; }

/**
 * Envía un comando al dispositivo a través de Firebase.
 *
 * @param {string} nodo    "puerta" o "garaje"
 * @param {string} action  "abrir" | "cerrar" | "silenciar" | "modo_ausente" | "modo_presente"
 */
async function sendCommand(nodo, action) {
  if (!state.profile) return;

  // Validación de permisos en el cliente (las reglas de seguridad de
  // Realtime DB también validarán; esto solo es para feedback inmediato).
  const perm = nodo === 'puerta' ? state.profile.permisoPuerta : state.profile.permisoGaraje;
  if (!perm) {
    toast('No tienes permiso para esta acción', 'warning');
    return;
  }

  try {
    // push() crea un nuevo hijo con un ID único bajo /comandos/{nodo}.
    // El puente está escuchando esta ruta y procesará el comando.
    const cmdRef = push(ref(rtdb, `comandos/${nodo}`));
    await set(cmdRef, {
      action:    action,
      user:      state.user.email,    // Para registrar quién lo hizo
      timestamp: Date.now()
    });

    // Mensaje amigable de confirmación.
    const msg = action === 'abrir'         ? `Abriendo ${nodo}...`
              : action === 'cerrar'        ? `Cerrando ${nodo}...`
              : action === 'silenciar'     ? `Silenciando alarma...`
              : action === 'modo_ausente'  ? `${capitalize(nodo)}: modo Ausente activado`
              : action === 'modo_presente' ? `${capitalize(nodo)}: modo Presente activado`
              : 'Comando enviado';
    toast(msg, 'success');
  } catch (err) {
    console.error('Error enviando comando:', err);
    toast('No se pudo enviar el comando', 'error');
  }
}


/* =====================================================================
 *  SECCIÓN 9 — HISTORIAL DE EVENTOS
 *
 *  Carga y muestra los últimos 20 eventos del sistema desde Firestore.
 *  El listener se actualiza en tiempo real cuando llegan nuevos eventos.
 * ===================================================================== */

/**
 * Activa un listener que muestra los últimos 20 eventos del sistema.
 *
 * Los eventos se ordenan por fecha descendente (más recientes primero).
 */
function attachEventsListener() {
  // Construir la consulta: colección "eventos", ordenada por recibidoEn
  // descendente, limitada a 20 documentos.
  const q = query(
    collection(db, 'eventos'),
    orderBy('recibidoEn', 'desc'),
    limit(20)
  );

  // onSnapshot vuelve a ejecutar el callback cada vez que cambia la
  // consulta (cuando llega un evento nuevo, por ejemplo).
  const unsub = onSnapshot(q,
    (snapshot) => {
      const list = $('#events-list');

      if (snapshot.empty) {
        list.innerHTML = '<div class="events-empty">Aún no hay eventos registrados</div>';
        return;
      }

      // Limpiar la lista y volver a poblarla con los eventos actuales.
      list.innerHTML = '';
      snapshot.forEach(d => {
        const e = d.data();
        list.appendChild(renderEventRow(e));
      });
    },
    (err) => {
      console.error('Error cargando eventos:', err);
      $('#events-list').innerHTML = '<div class="events-empty">No se pudieron cargar los eventos</div>';
    }
  );
  state.listeners.push(unsub);
}

/**
 * Construye el HTML de una fila de evento.
 *
 * @param {object} e  Datos del evento (nodo, tipo, detalle, recibidoEn)
 * @returns {HTMLElement}  Elemento listo para insertar en el DOM
 */
function renderEventRow(e) {
  const row = document.createElement('div');
  row.className = 'event-row';
  row.dataset.type = e.tipo || 'error';

  // Íconos por tipo de evento.
  const icons = {
    alarma:         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>',
    auth_ok:        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 9.9-1"/></svg>',
    auth_fail:      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>',
    modo_cambiado:  '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 9l9-7 9 7v11a2 2 0 01-2 2H5a2 2 0 01-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/></svg>',
    error:          '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>'
  };

  // Títulos legibles por tipo.
  const titles = {
    alarma:         'Alarma activada',
    auth_ok:        'Apertura autorizada',
    auth_fail:      'Intento de acceso fallido',
    modo_cambiado:  'Cambio de modo',
    error:          'Evento del sistema'
  };

  const detail = e.detalle ? `<span class="tag">${e.detalle}</span>` : '';
  const time   = e.recibidoEn ? formatTimeAgo(e.recibidoEn) : '';

  row.innerHTML = `
    <div class="event-icon">${icons[e.tipo] || icons.error}</div>
    <div class="event-content">
      <div class="event-title">${titles[e.tipo] || 'Evento'}</div>
      <div class="event-meta">
        <span class="tag">${e.nodo || '?'}</span>
        ${detail}
      </div>
    </div>
    <div class="event-time">${time}</div>
  `;
  return row;
}


/* =====================================================================
 *  SECCIÓN 10 — NOTIFICACIONES PUSH (FCM)
 *
 *  Firebase Cloud Messaging (FCM) permite enviar notificaciones al
 *  celular incluso cuando la web está cerrada. Para que funcione:
 *
 *    1. El navegador del usuario debe dar permiso de notificaciones.
 *    2. Hay que generar un "token FCM" único por dispositivo.
 *    3. Hay que guardar ese token en Firebase para que el puente sepa
 *       a quién mandar las notificaciones.
 * ===================================================================== */

/**
 * Inicializa el sistema de notificaciones push.
 *
 * Se llama una vez después del login. Hace dos cosas:
 *   1. Pide permiso al navegador si nunca se ha decidido.
 *   2. Registra un listener para mensajes que lleguen mientras la
 *      web está abierta (mensajes "en primer plano").
 */
async function setupNotifications() {
  if (!messaging) return;
  if (!('Notification' in window)) return;

  // Si el usuario nunca decidió, pedimos permiso (con un retraso para
  // no asustarlo apenas entra).
  if (Notification.permission === 'default') {
    setTimeout(async () => {
      const perm = await Notification.requestPermission();
      if (perm === 'granted') {
        toast('Notificaciones activadas', 'success');
        registerFcmToken();
      }
    }, 1500);
  } else if (Notification.permission === 'granted') {
    // Ya estaba permitido; registramos el token directamente.
    registerFcmToken();
  }

  // Listener para mensajes en primer plano: cuando llega una
  // notificación push mientras la web está abierta, FCM NO la muestra
  // automáticamente (porque asume que el usuario ya la ve en la app);
  // nosotros mostramos un toast como reemplazo.
  onMessage(messaging, (payload) => {
    const notif = payload.notification || {};
    toast(notif.body || 'Nueva alerta', 'error', 6000);
  });
}

/**
 * Obtiene el token FCM del dispositivo actual y lo guarda en Firebase.
 *
 * El token es como una dirección postal: el puente lo usa para saber
 * a qué celular específico enviar la notificación.
 */
async function registerFcmToken() {
  if (!messaging || !state.user) return;
  if (!vapidKey || vapidKey.includes('REEMPLAZAR')) {
    console.warn('VAPID key no configurada. Push deshabilitado.');
    return;
  }

  try {
    // Registrar el service worker que recibirá las notificaciones
    // cuando la app esté cerrada.
    const registration = await navigator.serviceWorker.register('/firebase-messaging-sw.js');

    // Pedir el token FCM. Requiere la VAPID key configurada en firebase-config.js.
    const token = await getToken(messaging, {
      vapidKey,
      serviceWorkerRegistration: registration
    });

    if (token) {
      // Guardar el token en Realtime DB bajo /fcmTokens/{uid}/{deviceId}.
      // El deviceId es los últimos 12 caracteres del token (suficiente
      // para ser único por dispositivo).
      const tokenRef = ref(rtdb, `fcmTokens/${state.user.uid}/${token.slice(-12)}`);
      await set(tokenRef, {
        token:   token,
        ua:      navigator.userAgent.slice(0, 80),   // User agent abreviado, para identificar el dispositivo
        updated: Date.now()
      });
    }
  } catch (err) {
    console.warn('No se pudo registrar token de notificación:', err.message);
  }
}


/* =====================================================================
 *  SECCIÓN 11 — PANEL DE ADMINISTRADOR
 *
 *  Solo accesible para usuarios con rol "admin". Permite crear, editar
 *  y eliminar usuarios del sistema.
 *
 *  LIMITACIÓN CONOCIDA del SDK cliente:
 *  Cuando un admin crea un usuario nuevo con createUserWithEmailAndPassword,
 *  Firebase automáticamente cambia la sesión activa al usuario recién
 *  creado. Esto significa que el admin se desloguea y debe volver a
 *  iniciar sesión. Para evitarlo se necesitaría usar el Admin SDK desde
 *  Cloud Functions, lo cual requiere el plan de pago de Firebase.
 * ===================================================================== */

/**
 * Abre la vista de admin y carga la lista de usuarios.
 */
function openAdminView() {
  showView('view-admin');
  loadUsersList();
}

/**
 * Activa un listener que muestra la lista de usuarios en tiempo real.
 */
function loadUsersList() {
  cleanupAdminListener();

  const q = query(collection(db, 'usuarios'));
  state.adminUnsub = onSnapshot(q, (snapshot) => {
    const list = $('#users-list');

    if (snapshot.empty) {
      list.innerHTML = '<div class="events-empty">No hay usuarios registrados</div>';
      return;
    }

    list.innerHTML = '';
    snapshot.forEach(d => {
      const u = { uid: d.id, ...d.data() };
      list.appendChild(renderUserRow(u));
    });
  });
}

/**
 * Limpia el listener del panel admin (cuando se sale de esa vista).
 */
function cleanupAdminListener() {
  if (state.adminUnsub) {
    state.adminUnsub();
    state.adminUnsub = null;
  }
}

/**
 * Construye el HTML de una fila de usuario en el listado del admin.
 *
 * @param {object} u  Datos del usuario
 * @returns {HTMLElement}  Elemento listo para el DOM
 */
function renderUserRow(u) {
  const row = document.createElement('div');
  row.className = 'user-row';

  // Inicial del nombre para el avatar (ej: "Manuela" → "M").
  const initial = (u.nombre || u.correo || '?').charAt(0).toUpperCase();

  // Construir badges visuales según los atributos del usuario.
  const badges = [];
  if (u.rol === 'admin')         badges.push('<span class="badge badge-admin">Admin</span>');
  if (u.permisoPuerta)           badges.push('<span class="badge badge-puerta">Puerta</span>');
  if (u.permisoGaraje)           badges.push('<span class="badge badge-garaje">Garaje</span>');
  if (u.activo === false)        badges.push('<span class="badge badge-inactive">Inactivo</span>');

  // El admin no puede eliminar su propia cuenta (protección contra accidentes).
  const canEdit = u.uid !== state.user.uid;

  row.innerHTML = `
    <div class="user-avatar">${initial}</div>
    <div class="user-info">
      <div class="user-info-name">${u.nombre || 'Sin nombre'}</div>
      <div class="user-info-email">${u.correo || ''}</div>
    </div>
    <div class="user-badges">${badges.join('')}</div>
    <div class="user-actions">
      <button class="icon-btn" data-action="edit" data-uid="${u.uid}" aria-label="Editar">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/></svg>
      </button>
      ${canEdit ? `<button class="icon-btn" data-action="delete" data-uid="${u.uid}" aria-label="Eliminar">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-2 14a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2L5 6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
      </button>` : ''}
    </div>
  `;

  // Conectar los botones a sus acciones.
  row.querySelector('[data-action="edit"]').addEventListener('click', () => openUserForm(u));
  const delBtn = row.querySelector('[data-action="delete"]');
  if (delBtn) delBtn.addEventListener('click', () => confirmDeleteUser(u));

  return row;
}

/**
 * Abre el formulario para crear o editar un usuario.
 *
 * @param {object|null} user  Datos del usuario si es edición, null si es nuevo
 */
function openUserForm(user) {
  // Cambiar título según sea creación o edición.
  $('#modal-user-title').textContent = user ? 'Editar usuario' : 'Nuevo usuario';

  // Llenar los campos con los datos del usuario (o vacíos si es nuevo).
  $('#user-uid').value               = user ? user.uid : '';
  $('#user-name-input').value        = user ? (user.nombre || '') : '';
  $('#user-email-input').value       = user ? (user.correo || '') : '';
  $('#user-email-input').disabled    = !!user;  // No se puede cambiar el email después de crear
  $('#user-password-input').value    = '';
  $('#password-field').style.display = user ? 'none' : 'block';  // Solo se pide contraseña al crear
  $('#perm-puerta').checked          = user ? !!user.permisoPuerta : true;
  $('#perm-garaje').checked          = user ? !!user.permisoGaraje : false;
  $('#user-activo').checked          = user ? user.activo !== false : true;
  document.querySelector(`input[name="rol"][value="${user?.rol || 'usuario'}"]`).checked = true;

  $('#user-form-error').classList.add('hidden');
  openModal('modal-user');
}

/**
 * Procesa el envío del formulario de usuario (crear o editar).
 *
 * Para usuarios NUEVOS:
 *   1. Crea la cuenta en Firebase Authentication.
 *   2. Crea el documento en Firestore /usuarios/{uid}.
 *   3. Cierra la sesión del admin (limitación del SDK cliente).
 *
 * Para usuarios EXISTENTES:
 *   1. Solo actualiza el documento en Firestore.
 *   2. La cuenta en Authentication no se modifica.
 *
 * @param {Event} e  Evento submit del formulario
 */
async function handleUserFormSubmit(e) {
  e.preventDefault();
  const submitBtn = e.target.querySelector('button[type="submit"]');
  const errorEl   = $('#user-form-error');
  errorEl.classList.add('hidden');
  setButtonLoading(submitBtn, true);

  // Recoger los valores del formulario.
  const uid             = $('#user-uid').value;
  const isNew           = !uid;
  const nombre          = $('#user-name-input').value.trim();
  const correo          = $('#user-email-input').value.trim();
  const password        = $('#user-password-input').value;
  const permisoPuerta   = $('#perm-puerta').checked;
  const permisoGaraje   = $('#perm-garaje').checked;
  const activo          = $('#user-activo').checked;
  const rol             = document.querySelector('input[name="rol"]:checked').value;

  try {
    if (isNew) {
      // Validación básica de contraseña.
      if (password.length < 6) throw new Error('La contraseña debe tener al menos 6 caracteres');

      // PASO 1: Crear cuenta en Firebase Authentication.
      const cred = await createUserWithEmailAndPassword(auth, correo, password);
      const newUid = cred.user.uid;

      // PASO 2: Crear documento en Firestore con el MISMO uid.
      await setDoc(doc(db, 'usuarios', newUid), {
        nombre, correo, rol, permisoPuerta, permisoGaraje, activo
      });

      // PASO 3: createUserWithEmailAndPassword desloguea al admin actual.
      // Le avisamos y forzamos un nuevo login.
      toast('Usuario creado. Por favor vuelve a iniciar sesión.', 'success', 5000);
      await signOut(auth);
    } else {
      // Edición: solo actualizar Firestore.
      await updateDoc(doc(db, 'usuarios', uid), {
        nombre, rol, permisoPuerta, permisoGaraje, activo
      });
      toast('Usuario actualizado', 'success');
      closeModal('modal-user');
    }
  } catch (err) {
    // Traducir códigos de error a mensajes amigables.
    let msg = err.message || 'Error guardando usuario';
    if (err.code === 'auth/email-already-in-use') msg = 'Ese correo ya está registrado';
    if (err.code === 'auth/weak-password')         msg = 'Contraseña demasiado débil';
    if (err.code === 'auth/invalid-email')         msg = 'Correo inválido';
    errorEl.textContent = msg;
    errorEl.classList.remove('hidden');
  } finally {
    setButtonLoading(submitBtn, false);
  }
}

/**
 * Pide confirmación antes de eliminar un usuario y lo elimina si se confirma.
 *
 * Nota: solo elimina el documento de Firestore. La cuenta en Authentication
 * debe eliminarse manualmente desde la consola de Firebase (limitación del
 * SDK cliente).
 *
 * @param {object} user  Datos del usuario a eliminar
 */
function confirmDeleteUser(user) {
  confirmAction(
    'Eliminar usuario',
    `Vas a eliminar a ${user.nombre || user.correo}. La cuenta de inicio de sesión en Authentication deberá borrarla manualmente desde la consola de Firebase. ¿Continuar?`,
    async () => {
      try {
        // Borrar de Firestore (causa eliminación en cascada por el sync del puente).
        await deleteDoc(doc(db, 'usuarios', user.uid));

        // Borrar también de RTDB por si el sync todavía no ha corrido.
        await remove(ref(rtdb, `usuarios/${user.uid}`));
        await remove(ref(rtdb, `fcmTokens/${user.uid}`));

        toast('Usuario eliminado del sistema', 'success');
      } catch (err) {
        console.error(err);
        toast('No se pudo eliminar el usuario', 'error');
      }
    }
  );
}


/* =====================================================================
 *  SECCIÓN 12 — INICIALIZACIÓN: ENLAZADO DE EVENTOS DEL DOM
 *
 *  Esta función se ejecuta una sola vez al cargar la página. Enlaza
 *  los formularios y botones a sus funciones, y configura el listener
 *  de cambio de estado de autenticación.
 * ===================================================================== */

function init() {

  // ---- Formulario de login ----
  $('#login-form').addEventListener('submit', handleLogin);

  // ---- Botón de logout ----
  $('#btn-logout').addEventListener('click', handleLogout);

  // ---- Navegación al panel admin ----
  $('#btn-admin').addEventListener('click', openAdminView);
  $('#btn-back').addEventListener('click', () => {
    cleanupAdminListener();
    showView('view-dashboard');
  });

  // ---- Acciones del panel admin ----
  $('#btn-add-user').addEventListener('click', () => openUserForm(null));
  $('#user-form').addEventListener('submit', handleUserFormSubmit);

  // ---- Event delegation: un único listener captura todos los clicks ----
  // Esta técnica evita tener que adjuntar muchos listeners individuales.
  // Cuando ocurre un clic, miramos los data-attributes del elemento más
  // cercano para decidir qué hacer.
  document.addEventListener('click', (e) => {

    // Click en un botón de acción (abrir/cerrar/silenciar en una tarjeta)
    const actionBtn = e.target.closest('[data-action][data-node]');
    if (actionBtn) {
      const action = actionBtn.dataset.action;
      const node   = actionBtn.dataset.node;
      sendCommand(node, action);
      return;
    }

    // Click en un botón de cerrar modal (que tiene data-close-modal="id")
    const closeAttr = e.target.closest('[data-close-modal]');
    if (closeAttr) {
      closeModal(closeAttr.dataset.closeModal);
      return;
    }

    // Click en el fondo oscuro de un modal → cerrarlo
    if (e.target.classList.contains('modal-backdrop')) {
      e.target.closest('.modal').classList.add('hidden');
    }
  });

  // ---- Botón de confirmar acción genérica ----
  $('#btn-confirm-action').addEventListener('click', () => {
    if (state.pendingConfirm) {
      const fn = state.pendingConfirm;
      state.pendingConfirm = null;
      fn();   // Ejecutar el callback guardado por confirmAction()
    }
    closeModal('modal-confirm');
  });

  // ---- Listener principal: cambios en el estado de autenticación ----
  // Esta función es EL CORAZÓN del flujo de la app. Se ejecuta:
  //   - Al cargar la página (con el usuario actual o null si no hay)
  //   - Cada vez que alguien inicia o cierra sesión
  onAuthStateChanged(auth, (user) => {

    // Ocultar el loader inicial.
    $('#loader').classList.add('fade-out');
    setTimeout(() => $('#loader').classList.add('hidden'), 300);

    if (user) {
      // Hay sesión activa → cargar dashboard.
      onUserSignedIn(user);
    } else {
      // No hay sesión → mostrar login y limpiar todo.
      state.user    = null;
      state.profile = null;
      cleanupListeners();
      cleanupAdminListener();
      showView('view-login');
    }
  });
}

// Disparar la inicialización al cargar el archivo.
init();

/* =====================================================================
 *  FIN DEL ARCHIVO
 * ===================================================================== */
