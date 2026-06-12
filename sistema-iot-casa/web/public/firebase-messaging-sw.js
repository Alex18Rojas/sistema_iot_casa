/*
 * firebase-messaging-sw.js
 * Service Worker para Firebase Cloud Messaging
 *
 * Recibe notificaciones push cuando la web esta cerrada o en segundo plano.
 */

importScripts('https://www.gstatic.com/firebasejs/10.13.2/firebase-app-compat.js');
importScripts('https://www.gstatic.com/firebasejs/10.13.2/firebase-messaging-compat.js');

// IMPORTANTE: estos valores deben coincidir con los de firebase-config.js
// (no se pueden importar modulos en un service worker antiguo, por eso se duplican)
firebase.initializeApp({
  apiKey: "AIzaSyAI9BmZs3yQyqSNywgcwkYNozCqN6U0fAg",
  authDomain: "sistema-iot-casa.firebaseapp.com",
  databaseURL: "https://sistema-iot-casa-default-rtdb.firebaseio.com",
  projectId: "sistema-iot-casa",
  storageBucket: "sistema-iot-casa.firebasestorage.app",
  messagingSenderId: "831457756009",
  appId: "1:831457756009:web:5457dc38e976c75db0b3f8"
});

const messaging = firebase.messaging();

// Cuando llega una notificacion en segundo plano
messaging.onBackgroundMessage((payload) => {
  const notif = payload.notification || {};
  const title = notif.title || 'Alerta del sistema';
  const options = {
    body: notif.body || 'Hay una novedad en tu casa',
    icon: '/icon-192.png',
    badge: '/icon-192.png',
    tag: 'casa-segura-alarma',
    requireInteraction: true,
    data: payload.data || {}
  };
  self.registration.showNotification(title, options);
});

// Cuando el usuario hace clic en la notificacion -> abrir la web
self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  event.waitUntil(
    clients.matchAll({ type: 'window', includeUncontrolled: true }).then((windows) => {
      for (const w of windows) {
        if (w.url.includes(self.location.host) && 'focus' in w) return w.focus();
      }
      if (clients.openWindow) return clients.openWindow('/');
    })
  );
});
