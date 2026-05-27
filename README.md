# 🏷️ IntelliKeep Smart Tag Beacons C++ Firmware

Ultra-low power C/C++ firmware driving the physical **IntelliKeep Smart Tag** beacon sensors. Engineered for maximum battery life on CR2032/LIR2032 coin cells, it utilizes microamp-level deep-sleep cycles, measures capacitive proximity touch, and broadcasts BLE telemetry.

---

## 🔬 Core Features & Architecture
### 🔋 Beacon Hardware Specifications
* **Microamp Deep Sleep**: Configured with custom hardware timer intervals (`TAG_DEFAULT_WAKE_PERIOD_US_VALUE`) to ensure maximum battery life.
* **Battery Chemistry Profiles**: Direct calculation algorithms adjusting state-of-charge measurements based on coin-cell chemistry (CR2032 vs LIR2032 voltage profiles).
* **Multiple Board Targets**: Supports **Seeed Studio XIAO ESP32-C3** dev boards as well as custom production PCB layouts.

---

## 🛠️ Technology Stack & Environment
* **Core Technologies**: ESP-IDF C/C++ (v5.x), Deep-Sleep Hardware Optimizers, Capacitive GPIO Sensing, Seeed Studio C3 profiles
* **Deployment Workspace**: NelsonServer private self-hosted infrastructure.

---

## 📦 Setup & Local Installation
1. Initialize target Espressif toolchain.
2. Configure hardware and behavior profiles: `idf.py menuconfig` (select target CR2032 or rechargeable LIR2032 parameters).
3. Compile Tag software: `idf.py build`
4. Flash to tag hardware: `idf.py -p PORT flash`

---

## 📡 NelsonServer Dual-Push Configuration

This repository is permanently configured with a dual-remote pipeline:
* **Local Bare Server Repository**: `/srv/git/intellikeep_tag.git`
* **GitHub Repository**: `git@github.com:TannerNelson16/intellikeep_tag.git`

### Unified Push
Whenever you make commits, a single:
```bash
git push origin main
```
instantly synchronizes your codebase with **both** your local private server and your GitHub account at the same time!

*All private configuration credentials (`.env`), databases, and large media files are completely isolated locally via `.gitignore` shields, ensuring only pristine source code reaches GitHub.*
