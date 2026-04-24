# 🤖 Project Wheely
**A high-performance Mecanum-wheel robot powered by MegaPi and Raspberry Pi.**

---

## 🏗️ Core Architecture
Wheely is a versatile, omnidirectional mobile platform based on the **mBot Mega** architecture. It utilizes a **MegaPi** (ATmega2560) for low-level motor control and a **Raspberry Pi Zero** for high-level logic and processing.

* **Original Reference:** [mBot Mega Robot Car](https://www.robotshop.com/products/makeblock-mbot-mega-robot-car-bluetooth-remote-controller)
* **Drive System:** 4WD Mecanum (Omnidirectional movement)

---

## 📦 Bill of Materials

### **Electronic Components**
| Qty | Component | Description |
| :--- | :--- | :--- |
| 1 | **MegaPi** | Main microcontroller board (Arduino Mega 2560 compatible) |
| 2 | **DC Motor Drivers** | Dual-channel drivers to control all 4 motors |
| 1 | **Bluetooth Module** | For wireless remote control and telemetry |
| 4 | **Encoder DC Motors** | High-torque motors for precision movement |
| 2 | **RGB LED Modules** | Status indicators and visual effects |
| 1 | **Raspberry Pi Zero** | Secondary "Brain" for Python-based logic |

### **Building Components**
* **Chassis:** 1x Body Shell & 1x Upper Shell
* **Power:** 1x 6-AA Battery Holder (9V DC)
* **Wheels:** 2x Pairs of 60mm Mecanum Wheels (Left/Right configuration)
* **Hardware:** 68x Spacers, Standoffs, Screws, and Nuts
* **Transmission:** 4x Motor Couplings and 4x Brackets

---

## 🔌 Hardware Integration

### **MegaPi to Raspberry Pi Zero Connection**
The Raspberry Pi Zero is interfaced via the MegaPi’s dedicated RPi port. Because the Pi Zero footprint differs from the standard Pi, a custom harness was used for a secure connection.

**Connection Strategy:**
* **Interface:** MegaPi Raspberry port connected to 10 pins of the 40-pin GPIO header.
* **Hardware Used:** * 10-Pin Header ([Amazon Link](https://www.amazon.com/dp/B0F9NSTWCV))
    * Flex Flat Cable (FFC) ([Amazon Link](https://www.amazon.com/dp/B01DP55PZQ))

> [!IMPORTANT]  
> **Wiring Alert:** Ensure the **5V output** from the MegaPi pins matches the **5V Input** on the Raspberry Pi side. The 10-pin header on the MegaPi should be soldered on the upper side with the key/shroud facing the edge of the board.

**Software Guide:** Follow the [Makeblock GPIO Python Method](https://support.makeblock.com/hc/en-us/articles/1500012868722-Program-mBot-Mega-with-Raspberry-Pi-in-Python) to initialize communication.

---

## 🛠️ Assembly Notes
1.  **Motor Orientation:** Ensure the Mecanum wheels are installed in the correct "X" pattern for proper omnidirectional translation.
2.  **Wiring:** Connect the DC motors to the drivers and ensure the Bluetooth module is seated firmly in its port.
3.  **Cable Management:** Since the boards are not perfectly sized to one another, use the flexible cable to allow for better positioning inside the shell.

---

## 📊 Base Properties & Logic
These parameters define the software constraints for the robot's operation:

| Property | Value | Description |
| :--- | :--- | :--- |
| **Speed Range** | `0 - 255` | Power intensity for motor control |
| **Max Armor** | `100` | Health/Integrity threshold |
| **Ammo Capacity** | `100` | Maximum ammunition count |
| **Ammo Power** | `10` | Damage/Impact value per unit |

