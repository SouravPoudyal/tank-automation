// BHARAT IOT Pump Bridge
// ------------------------------------------------------------------
// Sits on Render, reachable from anywhere. The ESP32 (on your home
// WiFi, behind NAT) calls OUT to this server every few seconds to
// report its status and pick up any pending command. The dashboard
// (also served from here) reads the latest status and queues
// commands for the device to pick up on its next check-in.
//
// This does NOT replace the firmware's local dashboard at
// 192.168.4.1 — that keeps working on the local network exactly as
// before. This is an additional relay for remote access.
//
// public/index.html is now a straight port of the device's own full
// dashboard (Home / Settings / Report / Plots), so this server also
// mirrors the device's CSV history logs and accepts the same
// clearLog / clearAllData / alerts commands. See README.md for the
// two small optional hooks in the firmware that make clearAllData's
// counter reset and the alerts toggle persist on-device.
// ------------------------------------------------------------------

const express = require('express');
const path = require('path');

const app = express();
app.use(express.json({ limit: '2mb' })); // logs relayed from the device can be a few hundred KB
app.use(express.urlencoded({ extended: false })); // for the alerts toggle's form-encoded POST

const PORT = process.env.PORT || 3000;

// Shared secrets. Set these as environment variables in the Render
// dashboard — never commit real values to source control.
//   DEVICE_TOKEN     -> the ESP32 sends this to prove it's your pump, not a stranger's POST
//   DASHBOARD_TOKEN   -> required to send start/stop/reset/threshold/etc commands from the web UI
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || '';
const DASHBOARD_TOKEN = process.env.DASHBOARD_TOKEN || '';

if (!DEVICE_TOKEN || !DASHBOARD_TOKEN) {
  console.warn(
    '[WARN] DEVICE_TOKEN and/or DASHBOARD_TOKEN are not set. ' +
    'Anyone who finds this URL could control your pump. ' +
    'Set them as environment variables before going live.'
  );
}

// How long since the last check-in before we consider the device offline.
const OFFLINE_AFTER_MS = 20_000;

// ------------------------------------------------------------------
// In-memory state (MVP). Render's free-tier disk is ephemeral and the
// instance can spin down when idle, so this resets on restart/deploy —
// that's fine, because the ESP32 remains the source of truth. Its own
// LittleFS logs are untouched by any of this; the fields below are
// just the latest mirror of them, refreshed on every device check-in.
// ------------------------------------------------------------------
let lastStatus = null;        // most recent /status JSON reported by the device
let lastCheckinAt = 0;        // Date.now() of last check-in
let pendingCommand = null;    // single-slot queue: { action, value, queuedAt }

// Mirrored CSV history logs, uploaded by the device alongside its
// regular status check-ins (see firmware_patch/cloud_bridge.ino).
let lastLogs = { full: '', fault: '', pzem: '' };
let lastLogsUpdatedAt = { full: 0, fault: 0, pzem: 0 };

function isOnline() {
  return lastCheckinAt > 0 && (Date.now() - lastCheckinAt) < OFFLINE_AFTER_MS;
}

function requireToken(expected) {
  return (req, res, next) => {
    if (!expected) return next(); // no token configured -> skip check (dev convenience only)
    const auth = req.get('authorization') || '';
    const token = auth.startsWith('Bearer ') ? auth.slice(7) : '';
    if (token !== expected) {
      return res.status(401).json({ error: 'unauthorized' });
    }
    next();
  };
}

// ------------------------------------------------------------------
// Device-facing routes (called by the ESP32)
// ------------------------------------------------------------------

// The ESP32 calls this every few seconds with its current /status
// payload. The response carries back any command that's waiting.
app.post('/api/device/checkin', requireToken(DEVICE_TOKEN), (req, res) => {
  lastStatus = req.body || {};
  lastCheckinAt = Date.now();

  const toSend = pendingCommand;
  pendingCommand = null; // one-shot: cleared as soon as it's handed off

  res.json({ ok: true, command: toSend });
});

// The ESP32 calls this on a slower cadence to mirror its CSV history
// logs, so the Report/Plots tabs work remotely too. Body is
// { full?, fault?, pzem? } — any subset of the three, plain CSV text.
app.post('/api/device/logs', requireToken(DEVICE_TOKEN), (req, res) => {
  const body = req.body || {};
  ['full', 'fault', 'pzem'].forEach((key) => {
    if (typeof body[key] === 'string') {
      lastLogs[key] = body[key];
      lastLogsUpdatedAt[key] = Date.now();
    }
  });
  res.json({ ok: true });
});

// ------------------------------------------------------------------
// Dashboard-facing routes (called by the browser)
// ------------------------------------------------------------------

// Shaped exactly like the device's own local /status response, so the
// ported dashboard's polling code needs zero changes: 200 + the raw
// status JSON while the device is checked in and online, otherwise a
// non-2xx so the dashboard's existing "Reconnecting…" / "Waiting for
// device…" handling kicks in unchanged.
app.get('/status', (req, res) => {
  if (!lastStatus || !isOnline()) {
    return res.status(503).json({ error: 'device offline' });
  }
  res.json(lastStatus);
});

// Kept for anything that still wants the richer, bridge-specific shape
// (online flag, age, pending command) alongside the raw status.
app.get('/api/status', (req, res) => {
  res.json({
    online: isOnline(),
    lastCheckinAt: lastCheckinAt || null,
    ageMs: lastCheckinAt ? Date.now() - lastCheckinAt : null,
    status: lastStatus,
    pendingCommand,
  });
});

function sendLog(key) {
  return (req, res) => {
    res.type('text/csv').send(lastLogs[key] || '');
  };
}
app.get('/fulllog.csv', sendLog('full'));
app.get('/faultlog.csv', sendLog('fault'));
app.get('/pzemlog.csv', sendLog('pzem'));

// opts.valueFrom: 'query' | 'body' (omit for commands with no value)
// opts.numeric: coerce to a real JS number so it round-trips to the
// device as an unquoted JSON number (setThreshold/setTime etc expect
// this — the firmware's small hand-rolled parser reads numeric values
// with toFloat()/strtoul(), not JSON string parsing).
function queueCommand(action, opts) {
  opts = opts || {};
  return (req, res) => {
    let raw;
    if (opts.valueFrom === 'query') raw = req.query[opts.queryKey || 'value'];
    else if (opts.valueFrom === 'body') raw = req.body ? req.body[opts.bodyKey || 'value'] : undefined;

    let value = raw;
    if (opts.numeric && raw !== undefined && raw !== '') {
      const n = Number(raw);
      value = Number.isFinite(n) ? n : raw;
    }

    pendingCommand = { action, value, queuedAt: Date.now() };
    res.json({ ok: true, queued: pendingCommand });
  };
}

app.post('/api/start', requireToken(DASHBOARD_TOKEN), queueCommand('start'));
app.post('/api/stop', requireToken(DASHBOARD_TOKEN), queueCommand('stop'));
app.post('/api/reset', requireToken(DASHBOARD_TOKEN), queueCommand('reset'));
app.post('/api/setThreshold', requireToken(DASHBOARD_TOKEN), queueCommand('setThreshold', { valueFrom: 'query', queryKey: 'value', numeric: true }));
app.post('/api/setThresholdDefault', requireToken(DASHBOARD_TOKEN), queueCommand('setThresholdDefault', { valueFrom: 'query', queryKey: 'value', numeric: true }));
app.post('/api/applyThresholdDefault', requireToken(DASHBOARD_TOKEN), queueCommand('applyThresholdDefault'));
app.post('/api/setTime', requireToken(DASHBOARD_TOKEN), queueCommand('setTime', { valueFrom: 'query', queryKey: 'epoch', numeric: true }));
app.post('/api/clearLog', requireToken(DASHBOARD_TOKEN), queueCommand('clearLog', { valueFrom: 'query', queryKey: 'log' }));
app.post('/api/clearAllData', requireToken(DASHBOARD_TOKEN), queueCommand('clearAllData'));
app.post('/api/alerts', requireToken(DASHBOARD_TOKEN), queueCommand('alerts', { valueFrom: 'body', bodyKey: 'on' }));

app.get('/healthz', (req, res) => res.send('ok'));

// Static dashboard
app.use(express.static(path.join(__dirname, 'public')));

app.listen(PORT, () => {
  console.log(`Pump bridge listening on port ${PORT}`);
});
