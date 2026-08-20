# BHARAT IOT Pump Bridge

A small Node.js/Express relay that lets your ESP32 pump dashboard be
reached over the internet, hosted on Render.com.

**How it fits together:** the ESP32 can't be reached directly from the
internet (home routers sit behind NAT). So instead, the ESP32 makes
*outbound* check-ins to this bridge every few seconds with its status,
and picks up any pending start/stop/reset/threshold command on that
same call. This server stores the latest snapshot and serves a
dashboard that lets you view status and queue commands from anywhere.

The device's own local dashboard at `192.168.4.1` is untouched and
keeps working exactly as before on your home WiFi.

## 1. Deploy to Render

1. Push this folder to a GitHub repo (or use Render's "Deploy from
   existing repo" / manual upload flow).
2. In Render: **New → Web Service** → connect the repo.
3. Settings:
   - **Environment**: Node
   - **Build command**: `npm install`
   - **Start command**: `npm start`
   - **Instance type**: Free is fine to start
4. Add environment variables (Render dashboard → Environment):
   - `DEVICE_TOKEN` — any long random string, e.g. generate one with
     `openssl rand -hex 24`
   - `DASHBOARD_TOKEN` — a second, different long random string
5. Deploy. Render gives you a URL like
   `https://bharat-iot-pump-bridge.onrender.com`.
6. Visit that URL — you should see the dashboard showing "Waiting for
   device…" since the ESP32 hasn't been patched yet.

**Free tier note:** Render's free web services spin down after ~15
minutes of no traffic and take a few seconds to wake back up on the
next request. That's fine for a periodic check-in relay, just expect
the dashboard to show briefly stale/offline data right after a period
of inactivity. Paid tiers avoid the spin-down if that matters to you.

## 2. Patch the firmware

Files in `firmware_patch/`:

- `cloud_bridge.ino` — add this as a **second tab** in the same sketch
  folder as `BHARAT_IOT_pump_control_curve_9.ino` (Arduino IDE: the
  `+` icon next to the existing tabs → New Tab → paste this in).

Edit the four constants at the top of `cloud_bridge.ino`:

```cpp
const char* HOME_WIFI_SSID      = "YOUR_HOME_WIFI_NAME";
const char* HOME_WIFI_PASSWORD  = "YOUR_HOME_WIFI_PASSWORD";
const char* BRIDGE_HOST         = "your-app-name.onrender.com"; // your Render URL, no https://
const char* BRIDGE_DEVICE_TOKEN = "..."; // must exactly match Render's DEVICE_TOKEN
```

Then add one line to the main sketch's `setup()`, right after the
existing `WiFi.softAP(...)` block (around line 3322 — search for
`"Dashboard:           http://"` to find the spot):

```cpp
bridge_setup();
```

That's it — no other changes needed. The main sketch's `loop()` and
`controlTask()` don't need to know the bridge exists; commands arrive
as the same `webRequestStart`/`webRequestStop`/`webRequestReset` flags
the physical button and local API already use.

Re-upload the sketch. On boot it will still create its local
`PumpControl` hotspot *and* join your home WiFi in the background
(`WIFI_AP_STA` mode) to reach the bridge.

## 3. Use it

- Open your Render URL from anywhere.
- Click "Save" under **Dashboard token** and paste the `DASHBOARD_TOKEN`
  value once — it's stored in the browser and sent with every command.
- Status updates every ~3 seconds; the Report/Plots tabs refresh their
  cached logs every ~15 seconds. Commands are picked up by the device
  on its next check-in (every ~4 seconds), so expect a few seconds of
  latency — this isn't instant control, it's a periodic relay.

## Dashboard design

`public/index.html` is now a direct, full port of your firmware's own
local dashboard — Home, Settings, **Report**, and **Plots**, same tank
sight-glass gauge, hero status ring, fault lamps, electrical meters,
CSV history tables, and canvas charts — reusing the exact same colors,
layout, and element structure, just pointed at the bridge instead of
the device directly. Two differences from the local version, both
required because this dashboard is reachable from the internet and not
just your home network:

- **Commands are queued, not instant.** Start/stop/threshold/RTC/log
  commands are picked up on the device's next check-in (a few seconds
  later), so status text says "Queued — applies on next check-in"
  instead of confirming instantly.
- **A dashboard token gate.** A new "Bridge access" panel under
  Settings holds the `DASHBOARD_TOKEN`, sent as an `Authorization:
  Bearer …` header on every command. Viewing status/logs never needs
  it; sending commands does.

The Report and Plots tabs work the same way as on the device: the
bridge now also mirrors `/fulllog.csv`, `/faultlog.csv`, and
`/pzemlog.csv`, uploaded by the firmware roughly every 20s (see
`bridge_uploadLogs()` in `cloud_bridge.ino`) — small payloads, since
the main sketch already auto-clears each log at 30 rows.

### Two optional firmware hooks

`clearAllData`, triggered remotely, resets the three log files itself
(pure LittleFS truncate, no main-sketch changes needed). Two smaller
pieces of what the *local* clearAllData/alerts routes do aren't
something `cloud_bridge.ino` can do on its own, because it doesn't
have your main sketch's internal variable/function names:

- resetting the pump cycle counter and the "last full"/"last fault"
  quick fields
- persisting the phone-alerts on/off toggle so it survives a reload

`cloud_bridge.ino` declares two `__attribute__((weak))` functions —
`resetPumpCountersForClearAllData()` and
`setAlertsEnabledForBridge(bool on)` — with harmless no-op defaults
(they just print a reminder to Serial), so everything above still
compiles and runs without touching the main sketch at all. If you want
those two effects to fully apply when triggered remotely, add real
(non-weak) definitions with those exact names to the main sketch, each
doing the same thing the corresponding local `/api/...` route already
does.

## Scope / what this does NOT do

- **State resets on Render restart/redeploy** — this is an in-memory
  MVP. The device itself remains the source of truth for anything
  that matters (thresholds, logs); the bridge only ever mirrors the
  latest live snapshot, and the device re-uploads its logs on its next
  cycle after any restart.
- **No HTTPS cert management needed** — Render terminates TLS for you
  automatically on the `.onrender.com` domain.

## Security notes

- Treat `DEVICE_TOKEN` and `DASHBOARD_TOKEN` like passwords — anyone
  with the dashboard token can start/stop your pump remotely.
- Consider a custom domain + Render's built-in TLS if you want a
  nicer URL than `*.onrender.com`.
- If you ever suspect a token leaked, just change the Render env var
  and the firmware constant together and redeploy/reflash.
