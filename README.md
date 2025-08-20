# 🏴‍☠️ Scavenger — ESP32 Pirate Hunt Kiosk

Welcome to **Scavenger**, a playful offline-first scavenger hunt kiosk powered by ESP32.  
This little device spins up its own Wi-Fi network and becomes a treasure chest of clues.  
Players join the network, get pulled into a captive portal, and race to solve riddles, enter phrases, or (optionally) upload QR codes.  

Organizers get a hidden setup mode to manage the hunt. Once the game is live, the kiosk becomes a pirate's map hub!

---

## ✨ Features

- **Two Personalities (Modes)**
  - ⚙️ **Setup Mode**  
    - Password-protected Wi-Fi AP for organizers only.  
    - Slick web UI to add, edit, or delete hunt items.  
    - All changes saved into LittleFS for persistence.  
  - 🏃 **Game Mode**  
    - Open Wi-Fi AP (no password).  
    - Captive portal = no typing URLs.  
    - Players drop straight into the player portal.  
    - They can enter secret phrases or upload QR codes.  

- **Smart Storage**
  - Hunt data is saved in **LittleFS**.  
  - Safe across reboots.  
  - Automatic reset when firmware version changes (so organizers can start fresh).  

- **Organizer Tools**
  - Factory reset endpoint (wipes storage + reboots).  
  - Admin passwords stored as salted hashes (mbedtls).  
  - Easy USB/serial log output for debugging.  

- **Player Experience**
  - Works on **any phone or laptop** (no HTTPS camera issues).  
  - File-input QR decoding (optional) OR text fallback.  
  - Captive portal magic: connect, play, done.  

---

## 🎮 How to Play

1. **Organizers**  
   - Power up the ESP32.  
   - Connect to `SCAVENGER-SETUP` Wi-Fi (password in serial log).  
   - Visit `192.168.4.1/admin`.  
   - Add hunt items: riddles, secret words, QR triggers.  
   - When ready, switch to Game Mode.  

2. **Players**  
   - Find and join the `SCAVENGER` Wi-Fi.  
   - Device will **redirect automatically** to the hunt portal.  
   - Solve the riddle, enter the phrase, or upload a QR.  
   - Successes (and fails) get playful feedback messages.  
   - Continue until treasure is found!  

3. **Reset Between Hunts**  
   - Hit the hidden `/factoryreset` endpoint to clear the board.  
   - Or flash a new firmware with bumped `FW_VERSION`.  

---

## 📂 Project Layout

```
/data           # HTML, CSS, and JS for player/admin portals
/src            # ESP32 firmware
platformio.ini  # PlatformIO build config
README.md       # This file
```

---

## 🛠️ Requirements

- [PlatformIO](https://platformio.org/) (Arduino framework)  
- ESP32 board (tested on ESP32-DevKit + ESP32-CAM)  
- Libraries:  
  - `ESP Async WebServer`  
  - `AsyncTCP`  
  - `ArduinoJson`  
  - `DNSServer`  
  - `LittleFS`  

---

## 🚀 Setup & Build

1. Clone this repo:  
   ```bash
   git clone https://github.com/yourusername/scavenger.git
   cd scavenger
   ```

2. Build & upload:  
   ```bash
   pio run -t upload
   ```

3. Upload the web assets:  
   ```bash
   pio run -t uploadfs
   ```

4. Connect to the new Wi-Fi AP and start hunting!  

---

## 🧭 Example Hunts

- **Treasure Map Riddle**  
  Each station holds a riddle. Solve it to unlock the next clue.  

- **QR Tagging**  
  Hide QR codes around a park. Players scan and upload via the kiosk.  

- **Secret Phrases**  
  Players shout pirate phrases like “shiver me timbers” into the portal.  

---

## 🔧 Configuration

- `FW_VERSION` → bump to reset admin storage.  
- Default SSIDs:  
  - Setup mode → `SCAVENGER-SETUP`  
  - Game mode → `SCAVENGER`  

---

## 📸 Screenshots (coming soon)

- Admin UI mockup  
- Player portal flow  

---

## 📜 License

MIT License — free to use, remix, and deploy in your events.  

---

## 🙌 Credits

- Built with [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)  
- Pirate inspiration from classic treasure hunts, escape rooms, and adventure games.  
