/*
 * firebase-config.js
 *
 * REEMPLAZA los valores con los TUYOS del proyecto Firebase
 * (los que guardaste en tu archivo firebase-config.txt).
 *
 * La clave VAPID se obtiene en:
 *   Firebase Console -> Configuracion del proyecto -> Cloud Messaging
 *   -> Web Push certificates -> Generar par de claves
 */

export const firebaseConfig = {
  apiKey: "AIzaSyAI9BmZs3yQyqSNywgcwkYNozCqN6U0fAg",
  authDomain: "sistema-iot-casa.firebaseapp.com",
  databaseURL: "https://sistema-iot-casa-default-rtdb.firebaseio.com",
  projectId: "sistema-iot-casa",
  storageBucket: "sistema-iot-casa.firebasestorage.app",
  messagingSenderId: "831457756009",
  appId: "1:831457756009:web:5457dc38e976c75db0b3f8"
};

// Clave VAPID para notificaciones push (Cloud Messaging)
export const vapidKey = "BNuzPmHpkSYgayZkSjqcFzlK_xg4UXGOV91O7fdb1AE8CtjtpQ9EkM5CxyTuwd4YY32Lti7fIoprrqmKnJyEheU";
