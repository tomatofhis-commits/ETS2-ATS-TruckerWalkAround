# TruckerWalkAround (v1.0-beta)

**Next-Gen Free Walk & Physical Roam Plugin for American Truck Simulator (ATS) & Euro Truck Simulator 2 (ETS2)**

[**日本語のREADMEはこちら (Japanese README)**](README_JA.md)

---

## 🌟 Overview

**TruckerWalkAround** allows you to get out of your truck cabin and freely explore the game world on foot with realistic physics, collision detection, and step-climbing.

Unlike traditional mods, this tool uses a dedicated external manager (`TruckerWalkAround.exe`) that installs the telemetry plugin into your game directory **only while running**, and automatically deletes it when closed. This ensures zero risk of game crashes after SCS game updates!

---

## ✨ Key Features

- **PhysX Real-Time Physics Engine Integration**: Direct raycasting against game world colliders for realistic ground elevation, step-climbing (up to 0.45m), and gravity.
- **Dedicated Binaries for ATS & ETS2**: Distinct 64-bit plugins (`TruckerWalkAround_ATS.dll` and `TruckerWalkAround_ETS2.dll`) optimized specifically for each game's memory layouts.
- **Automatic `config.cfg` Configuration**: Automatically detects and sets `uset g_developer "1"` and `uset g_console "1"` on launch. No manual file editing required!
- **Vehicle Input Lock**: Automatically blanks vehicle controls (WASD, throttle, steering, handbrake) while walking to prevent accidental vehicle movement.
- **HUD & Notification Auto-Hiding**: Seamlessly hides Route Advisor and pop-up notifications during walk mode for maximum immersion.
- **Emergency Unstuck (Noclip Mode)**: Stuck in a barrier or ditch? Press `[Alt]` to enter Noclip mode, fly out, and resume walking.

---

## 🎮 Controls & Keybindings

| Key | Action | Description |
| :--- | :--- | :--- |
| **`[F9]`** | **Toggle Walk Mode** | Switch between truck cabin view and physical walk mode. |
| **`[BackSpace]`** | **Emergency Exit** | Instantly abort walk mode and return to the driver seat. |
| **`[W] [A] [S] [D]`** | **Movement** | Walk forward, backward, left, and right. |
| **`[Shift]`** | **Sprint** | Increase walking/running speed. |
| **`[Alt]`** | **Toggle Noclip** | Fly and pass through walls (useful for unstucking). |
| **`[Space]`** | **Ascend** | Move upward while in Noclip mode. |
| **`[Ctrl]`** | **Descend** | Move downward while in Noclip mode. |
| **`[F10]`** | **Debug Monitor** | Toggle real-time placement and collider debug stats. |
| **Mouse** | **Look / Turn** | Freely look around and steer viewing direction. |

---

## 🚀 How to Use

1. Download the latest installer `TruckerWalkAround_Setup_v1.0_beta.exe` from [**Releases**](../../releases).
2. Run the installer and launch **TruckerWalkAround**.
   - Your ATS / ETS2 installation directories and `config.cfg` files will be automatically detected and configured.
3. Launch **American Truck Simulator** or **Euro Truck Simulator 2** via Steam as usual.
4. In-game, press **`[F9]`** to step out of your truck and start walking!
5. After finishing your session, simply close the **TruckerWalkAround** window; the plugin will be cleaned up automatically.

---

## ⚠️ Notes & Tips

1. **Collision on Static Parked Cars**:
   - In SCS games, some pre-placed scenery background vehicles (especially in parking lots / rest stops) lack physical colliders by game engine design. Walking through them is expected behavior and not a bug.
2. **Getting Stuck**:
   - If your character gets trapped in tight gaps, guardrails, or deep terrain trenches, simply press **`[Alt]`** to toggle Noclip, fly to clear ground, and press **`[Alt]`** again to return to normal walking. You can also press **`[BackSpace]`** at any time to return to the cabin.

---

## 🛡️ Security & VirusTotal Verification

Because this tool reads and writes memory addresses (PhysX, camera, CVars) and manages `.dll` files in game directories, some aggressive heuristic antivirus scanners may flag it with generic warnings.

The build is completely clean and verified safe by over 70 security engines on VirusTotal:
- **VirusTotal Scan Report**: [Clean Scan Result (v1.0-beta)](https://www.virustotal.com/gui/file/9053e54dba6868b5638388e58e0c6b8974dda52b26e924f77f25e7a9a2402576)
- *Major vendors (Microsoft Defender, Kaspersky, Symantec, Bitdefender, etc.) report 100% clean.*

---

## 🛠️ Building from Source

### Prerequisites
- Visual Studio 2022 / Build Tools (MSVC x64 compiler `cl.exe`)
- .NET Framework 4.0+ (`csc.exe`)
- [Inno Setup 6](https://jrsoftware.org/isdl.php) (optional, for building the installer)

### Build Commands
Run from Developer Command Prompt for VS:
```cmd
# 1. Compile both ATS and ETS2 native DLLs
build_dll.bat

# 2. Compile C# WinForms GUI Manager and Installer
build_all.bat
```

---

## 📄 License
This project is licensed under the [MIT License](LICENSE).
