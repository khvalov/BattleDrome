# BattleDrome: The Physical-Digital Tank Arena

## 1. Vision
BattleDrome is a hybrid tactical platform merging the physical thrill of battle bots with the strategic depth of online tank games. It is a playground where physical DIY equipment (tanks, turrets, and drones) interacts with a digital "Game Master" server to govern real-time gameplay.

## 2. Core Concept
The "BattleDrome" ecosystem consists of physical hardware units that possess digital attributes. Unlike traditional RC toys, a BattleDrome tank's performance—its speed, health, and firepower—is managed in real-time by a central server via WiFi.

### The Feedback Loop
1.  **Input:** Players steer tanks via a remote controller or web dashboard.
2.  **Physical Interaction:** Tanks fire IR beams at each other or drive over RFID tags on the floor.
3.  **Data Sync:** The tank reports the hit or the tag ID to the server.
4.  **Digital Impact:** The server calculates the result (e.g., "Tank 1 is hit: Reduce HP by 10") and sends a command back to the hardware (e.g., "Limit motor output to 50%").

---

## 3. Gameplay Mechanics & Equipment

### Physical Properties (The "Digital DNA")
Each piece of equipment has variable stats that can be influenced during play:
-   **Health (HP):** When it hits zero, the server disables the tank’s motors.
-   **Ammo:** Limited shots. Once empty, the IR blaster is disabled until a "Reload" event occurs.
-   **Armor:** Reduces the damage taken from IR hits.
-   **Speed:** The server controls the maximum PWM signal sent to the motors.

### Combat: IR Blasters & Receivers
Combat is handled via Infrared (IR) signals.
-   **Tanks:** Equipped with IR LEDs (Blasters) and 360° IR receiver arrays.
-   **Turrets:** Standalone, stationary sentries that fire at anything passing their sensors. They act as "environmental hazards" or defenders in specific missions.

### Artifacts: The RFID System
The battlefield is seeded with RFID tags acting as "Digital Artefacts." Each tank has a bottom-mounted RFID reader.
-   **Positive Buffs:** Ammo crates, Repair kits (Health), or Nitro boosts (Speed upgrade).
-   **Negative Debuffs:** Landmines (Instant damage), Mud (Slow down), or EMP (Temporary control loss).
-   **Programming:** RFID effects are defined server-side, allowing a single physical tag to be a "Medkit" in one round and a "Flag" in the next.

### Mission Profiles
The server-side logic allows for flexible game modes:
-   **Deathmatch:** Last tank standing.
-   **Capture the Flag:** Scan a specific RFID tag at the enemy base and return to your own.
-   **Race:** Pass through a series of RFID checkpoints in order.
-   **Survival:** Players must navigate a field of autonomous IR Turrets to reach an exit point.

---

## 4. Technical Stack

-   **Client Side (The Soldiers):**
    -   **Microcontrollers:** Arduino (C++) for low-level hardware control (Motors, IR, Sensors).
    -   **The Bridge:** Raspberry Pi used to bridge the Arduino units to the main server. Handles the WiFi stack and runs a local Node.js environment or MQTT client to ensure low-latency communication.
-   **Server Side (The General):**
    -   **Environment:** Node.js.
    -   **Communication:** MQTT or WebSockets for real-time bi-directional data flow.
    -   **Dashboard:** A web-based interface to set up missions, monitor tank health, and adjust global game variables.

---

## 5. Project Structure

-   `./equipment`: Assembly manuals, wiring diagrams, and C++ firmware for tanks, turrets, and drones.
-   `./server`: Node.js source code, dashboard frontend, and MQTT broker configurations.
-   `./assets`: 3D models for chassis, arena blueprints, and branding.

---

And this will produce a flow chart:

```mermaid
graph LR
A[Tank] --> WiFi--> B((MQTT))--> D{Server}
D-->B-->WiFi-->A

---

## 6. Contributing
This is a community-driven pet project. There are no limits to creativity.
-   **Hardware:** Design new tank chassis or specialized turrets.
-   **Software:** Build new mission logic or improve the dashboard UI.
-   **Playtesting:** Build an arena and break things.

## 7. Support
BattleDrome is community-supported. Reach out via the issues tab or the community chat. Ask questions, contribute code, and help us build the ultimate battle drome!
