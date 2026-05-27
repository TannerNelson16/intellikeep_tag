# IntelliKeep Smart Tag beacons C++ Firmware

Low-power C/C++ Espressif ESP-IDF firmware for the IntelliKeep smart sensor beacons. Features ultra-low sleep modes, sensor polling, and beacon reporting.

---

## 🚀 Key Features
* Tailored custom software architecture optimized for NelsonServer.
* Robust configuration models with clean environment variable fallback systems.
* Seamless integration with self-hosted bare repository networks and off-site cloud backups.

---

## 🛠️ Technology Stack
* **Core**: ESP-IDF C/C++, Low-power RTC controllers, Sensor GPIO nodes
* **Environment**: Linux Server deployment compatibility.

---

## 📦 Local Installation & Setup

1. Set up ESP-IDF environment.
2. Build project: `idf.py build`
3. Flash to beacons: `idf.py -p PORT flash`

---

## 📡 NelsonServer Dual-Push Deployment Configuration

This repository is configured with **automatic local & cloud synchronization**! 

* **Local bare repository**: `/srv/git/intellikeep_tag.git`
* **GitHub remote**: `git@github.com:TannerNelson16/intellikeep_tag.git`

### Secure Dual-Push
Whenever you run `git push origin`, it instantly and securely uploads your commits to **both** your local private bare repository on NelsonServer and your off-site GitHub account in a single step!

```bash
git push origin main
```
*Your secrets, `.env` config credentials, and local databases are protected locally in your `.gitignore` shield, ensuring only clean source code reaches GitHub.*
