# Sistema IoT de Seguridad Doméstica

Sistema completo de control de acceso para vivienda compuesto por un microcontrolador ESP8266, un broker MQTT en la nube, un puente Node.js y una aplicación web responsive con notificaciones push.

**Proyecto académico — Instituto Tecnológico Metropolitano (ITM), Medellín**

**Integrantes:**
- Manuela Rivera Gómez
- Janier Alexander Rojas Giraldo
- Luis Alfonso Ortiz Ruiz

---

## Tabla de contenido

1. [Arquitectura del sistema](#arquitectura-del-sistema)
2. [Estructura de archivos](#estructura-de-archivos)
3. [Archivos que tú debes crear o descargar](#archivos-que-tu-debes-crear-o-descargar)
4. [Orden de instalación](#orden-de-instalación)
5. [Componentes físicos requeridos](#componentes-físicos-requeridos)
6. [Servicios en la nube utilizados](#servicios-en-la-nube-utilizados)
7. [Tabla de pines del NodeMCU](#tabla-de-pines-del-nodemcu)
8. [Tópicos MQTT](#tópicos-mqtt)
9. [Estructura de datos en Firebase](#estructura-de-datos-en-firebase)

---

## Arquitectura del sistema

El sistema tiene **cinco capas** que se comunican entre sí:

```
  [Usuario]
     |
     v
  [Aplicación web]   <--  Firebase Hosting  (HTTPS)
     |
     v
  [Firebase]         <--  Authentication + Firestore + Realtime DB + FCM
     |
     v
  [Puente Node.js]   <--  Se ejecuta en un PC o servidor
     |
     v
  [Broker MQTT]      <--  HiveMQ Cloud Serverless (TLS, puerto 8883)
     |
     v
  [ESP8266]          <--  NodeMCU V3 que controla puerta y garaje
```

Ninguna capa habla directamente con la del extremo opuesto: cada mensaje pasa por las capas intermedias. Esto permite separación de responsabilidades y seguridad por capas.

---

## Estructura de archivos

```
sistema-iot-casa-completo/
│
├── README.md                              ← Este archivo (empieza aquí)
│
├── firmware/                              ← Programa que corre en el NodeMCU
│   ├── SistemaIoT_Casa.ino                  Codigo principal del firmware
│   └── config.h                             Parametros (WiFi, MQTT, pines, tiempos)
│
├── puente/                                ← Servidor Node.js
│   ├── index.js                             Codigo principal del puente
│   ├── package.json                         Dependencias del proyecto
│   ├── .env.example                         Plantilla de credenciales
│   └── .gitignore                           Exclusiones para git
│
├── web/                                   ← Aplicacion web (Firebase Hosting)
│   ├── firebase.json                        Configuracion de Hosting
│   ├── .firebaserc                          Vinculacion con el proyecto
│   └── public/
│       ├── index.html                       HTML principal con las 3 vistas
│       ├── styles.css                       Estilos visuales
│       ├── app.js                           Logica de la aplicacion
│       ├── firebase-config.js               Credenciales de Firebase (placeholder)
│       ├── firebase-messaging-sw.js         Service Worker para notificaciones
│       ├── manifest.json                    Configuracion PWA
│       ├── icon-192.png                     Icono PWA pequeno
│       └── icon-512.png                     Icono PWA grande
│
└── firebase-reglas/                       ← Reglas de seguridad de Firebase
    ├── firestore.rules                      Reglas de la base de datos documental
    └── database.rules.json                  Reglas de la base de datos en tiempo real
```

---

## Archivos que tú debes crear o descargar

Estos archivos **no vienen en el paquete** porque contienen información sensible o son específicos de tu cuenta. Debes crearlos siguiendo las instrucciones:

| Archivo | Carpeta donde va | Cómo obtenerlo |
|---|---|---|
| `serviceAccountKey.json` | `puente/` | Firebase Console → ⚙ → Configuración del proyecto → Cuentas de servicio → Generar nueva clave privada |
| `.env` | `puente/` | Copiar `.env.example` y renombrar a `.env`. Editar con tus credenciales reales |
| `node_modules/` | `puente/` | Se genera automáticamente al ejecutar `npm install` |

**Además, debes editar estos archivos con tus credenciales reales:**

| Archivo | Qué editar |
|---|---|
| `firmware/config.h` | WIFI_SSID, WIFI_PASSWORD, MQTT_BROKER, MQTT_USER, MQTT_PASSWORD, LOCAL_DEVICE_KEY |
| `web/public/firebase-config.js` | El objeto `firebaseConfig` completo y la `vapidKey` |
| `web/public/firebase-messaging-sw.js` | Los mismos valores del bloque `firebase.initializeApp({...})` |

---

## Orden de instalación

Si vas a empezar desde cero o reconfigurar todo, sigue **EXACTAMENTE este orden**:

### Paso 1 — Crear cuenta en HiveMQ Cloud
1. Ir a [console.hivemq.cloud](https://console.hivemq.cloud) y crear cuenta.
2. Crear un cluster "Serverless" (gratuito).
3. En **Access Management** crear credenciales (usuario + contraseña).
4. Anotar el hostname del cluster.

### Paso 2 — Configurar Firebase
1. Ir a [console.firebase.google.com](https://console.firebase.google.com) y crear proyecto `sistema-iot-casa`.
2. Habilitar **Authentication** → método "Correo electrónico/contraseña".
3. Habilitar **Firestore Database** en modo producción.
4. Habilitar **Realtime Database** en modo bloqueado.
5. Aplicar las reglas de seguridad de `firebase-reglas/firestore.rules` y `firebase-reglas/database.rules.json` (copiar/pegar en la consola).
6. Crear el primer usuario admin en Authentication.
7. En Firestore crear la colección `usuarios` con un documento cuyo ID sea el UID del admin, y los campos: `nombre`, `correo`, `rol: "admin"`, `permisoPuerta: true`, `permisoGaraje: true`, `activo: true`.
8. Registrar una app web → copiar el `firebaseConfig` que aparece.
9. En **Cloud Messaging** generar el par de claves VAPID y copiarla.
10. Descargar la **cuenta de servicio**: ⚙ → Configuración → Cuentas de servicio → Generar clave privada. Esto descarga un archivo JSON que renombrarás a `serviceAccountKey.json`.

### Paso 3 — Configurar el firmware
1. Abrir `firmware/SistemaIoT_Casa.ino` en Arduino IDE.
2. Editar `firmware/config.h` con tus credenciales (WiFi, broker MQTT, clave local).
3. Instalar las librerías: PubSubClient, ArduinoJson, Servo, ESP8266 Core.
4. Compilar y subir al NodeMCU V3.

### Paso 4 — Configurar el puente Node.js
1. En la carpeta `puente/`, copiar `.env.example` y renombrar a `.env`.
2. Editar `.env` con las credenciales reales.
3. Pegar el `serviceAccountKey.json` descargado en el paso 2 dentro de la carpeta `puente/`.
4. Ejecutar `npm install` para descargar las dependencias.
5. Ejecutar `npm start` para iniciar el puente.

### Paso 5 — Desplegar la web
1. Editar `web/public/firebase-config.js` con tu `firebaseConfig` y `vapidKey`.
2. Editar `web/public/firebase-messaging-sw.js` con los mismos valores (excepto la vapidKey).
3. Instalar Firebase CLI: `npm install -g firebase-tools`.
4. Login: `firebase login`.
5. Desde la carpeta `web/`, desplegar: `firebase deploy --only hosting`.
6. Abrir la URL que Firebase te da (algo como `https://sistema-iot-casa.web.app`).

---

## Componentes físicos requeridos

| Componente | Cantidad | Notas |
|---|---|---|
| NodeMCU V3 (ESP8266Mod) | 1 | Microcontrolador principal |
| Módulo sensor IR (HC-SR501 o similar) | 2 | Uno para puerta y uno para garaje |
| Módulo relé 5V (1 canal) | 2 | Para cada cerradura |
| Cerradura electromagnética P82F-5V | 2 | Una por punto de acceso |
| Buzzer KY-006 | 2 | Para alarmas locales |
| Servomotor SG92R-360 | 1 | Para el portón del garaje (rotación continua) |
| Fuente externa 5V / 2A | 1 | Para alimentar cerraduras y servo |
| Resistencia 10kΩ | 1 | Pull-down obligatorio en D8 (servo) |
| Cables, breadboard, etc. | - | Material de conexión |

---

## Servicios en la nube utilizados

| Servicio | Plan | Uso |
|---|---|---|
| HiveMQ Cloud Serverless | Gratis | Broker MQTT con TLS |
| Firebase Authentication | Gratis (Spark) | Login con correo/contraseña |
| Firebase Firestore | Gratis (Spark) | Base de datos de usuarios y eventos |
| Firebase Realtime Database | Gratis (Spark) | Estados en vivo y comandos |
| Firebase Cloud Messaging | Gratis | Notificaciones push |
| Firebase Hosting | Gratis (Spark) | Hospedaje de la web |

Todos los servicios funcionan dentro de la capa gratuita para un proyecto académico de este alcance.

---

## Tabla de pines del NodeMCU

| Componente | Etiqueta en placa | GPIO | Tipo |
|---|---|---|---|
| Sensor IR puerta | D1 | 5 | Entrada digital |
| Relé puerta | D2 | 4 | Salida digital |
| Buzzer garaje | D3 | 0 | Salida PWM |
| Sensor IR garaje | D6 | 12 | Entrada digital |
| Relé garaje | D7 | 13 | Salida digital |
| Buzzer puerta | D5 | 14 | Salida PWM |
| Servo garaje | D8 | 15 | Salida PWM (con pull-down 10kΩ) |

---

## Tópicos MQTT

| Tópico | Dirección | Propósito |
|---|---|---|
| `casa/dispositivo/disponible` | ESP8266 → puente | LWT: online/offline del dispositivo completo |
| `casa/puerta/cmd` | Puente → ESP8266 | Comandos para la puerta |
| `casa/puerta/estado` | ESP8266 → puente | Estado completo de la puerta (cada 15s) |
| `casa/puerta/evento` | ESP8266 → puente | Eventos puntuales (alarmas, autorizaciones) |
| `casa/puerta/disponible` | ESP8266 → puente | LWT: online/offline del nodo puerta |
| `casa/garaje/cmd` | Puente → ESP8266 | Comandos para el garaje |
| `casa/garaje/estado` | ESP8266 → puente | Estado completo del garaje (cada 15s) |
| `casa/garaje/evento` | ESP8266 → puente | Eventos puntuales del garaje |
| `casa/garaje/disponible` | ESP8266 → puente | LWT: online/offline del nodo garaje |

---

## Estructura de datos en Firebase

### Firestore

```
/usuarios/{uid}
   nombre:         string
   correo:         string
   rol:            "admin" | "usuario"
   permisoPuerta:  boolean
   permisoGaraje:  boolean
   activo:         boolean

/eventos/{auto_id}
   nodo:           "puerta" | "garaje"
   tipo:           "alarma" | "auth_ok" | "auth_fail" | "error"
   detalle:        string
   tsDispositivo:  number       (millis del ESP8266)
   recibidoEn:     Timestamp    (hora del servidor)
```

### Realtime Database

```
/estado/
    puerta/      (objeto JSON con cerradura, sensor_ir, alarma, rssi, uptime_s, estado)
    garaje/      (similar a puerta, mas el campo porton)

/disponibilidad/
    dispositivo: "online" | "offline"
    puerta:      "online" | "offline"
    garaje:      "online" | "offline"

/ultimoEvento/
    puerta/      (objeto JSON: tipo, detalle, recibidoEn)
    garaje/      (similar)

/comandos/
    puerta/      (lista de comandos pendientes, los borra el puente)
    garaje/      (lista de comandos pendientes, los borra el puente)

/usuarios/{uid}
    permisoPuerta: boolean   (espejo de Firestore, lo escribe el puente)
    permisoGaraje: boolean
    rol:           string
    activo:        boolean

/fcmTokens/{uid}/{deviceId}
    token:   string          (token FCM del celular del usuario)
    ua:      string          (user agent abreviado)
    updated: number          (timestamp)
```

---

## Para más detalle

Cada archivo de código tiene comentarios exhaustivos explicando su propósito, arquitectura, y decisiones técnicas. Te recomiendo leer los encabezados en este orden:

1. `firmware/SistemaIoT_Casa.ino` (cabecera y secciones 1-2)
2. `puente/index.js` (cabecera y secciones 1-3)
3. `web/public/app.js` (cabecera y secciones 1-3)

---

**Última actualización del paquete:** Mayo 2026
**Versión del firmware:** Single-NodeMCU (un microcontrolador para puerta y garaje)
**Versión del puente:** v2.0 (con notificaciones push FCM)
