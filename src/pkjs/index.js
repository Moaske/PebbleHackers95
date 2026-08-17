var Clay = require('@rebble/clay');
var clayConfig = require('./config');

// Use in-memory storage instead of localStorage.
// localStorage is unreliable in WKWebView (iOS Pebble app) and can cause
// Clay to show a "Start configuring" error button instead of the settings page.
var memStorage = (function() {
  var store = {};
  return {
    getItem: function(key) { return store.hasOwnProperty(key) ? store[key] : null; },
    setItem: function(key, val) { store[key] = String(val); },
    removeItem: function(key) { delete store[key]; },
    clear: function() { store = {}; }
  };
})();

var clay = new Clay(clayConfig, null, { storageImpl: memStorage });