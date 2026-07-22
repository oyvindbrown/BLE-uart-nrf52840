# BLE-uart-nrf52840

**XIAO nRF52840 BLE Smart Lock + Plain-Text HEX → UART Translator**

Firmware for the [Seeed Studio XIAO nRF52840](https://wiki.seeedstudio.com/XIAO_BLE/) (and Sense) that implements a Smart Lock GATT profile and a Nordic UART Service (NUS) based HEX command bridge to hardware UART.

Designed to be built in **VS Code** and flashed via simple **UF2 drag-and-drop** on Windows.

---

## Project Overview

This firmware turns the tiny XIAO nRF52840 into a BLE-to-UART gateway with a ready-to-use smart lock service:

### 1. Smart Lock GATT (matches common smart-lock examples)

| Service UUID                          | Characteristic UUID                   | Purpose                     | Properties                |
|---------------------------------------|---------------------------------------|-----------------------------|---------------------------|
| `0000FF00-0000-1000-8000-00805F9B34FB` | `0000FF01-0000-1000-8000-00805F9B34FB` | Lock / Unlock Command       | Write, Write No Response  |
| `0000FF00-0000-1000-8000-00805F9B34FB` | `0000FF02-0000-1000-8000-00805F9B34FB` | Lock Status (Open/Closed)   | Read, Notify              |
| `0000180F-...` (Battery Service)      | `00002A19-...`                        | Battery Level               | Read                      |
| `0000180A-...` (Device Information)   | `00002A29-...`                        | Manufacturer Name           | Read                      |

**Lock protocol (write 1 byte to 0xFF01):**

| Written value | Action          | UART packet | Status (0xFF02) |
|---------------|-----------------|-------------|-----------------|
| `0x00`        | Unlock / Open   | `55 AA 00`  | 0 (Open)        |
| `0x01`        | Lock / Close    | `55 AA 01`  | 1 (Closed)      |
| any other     | Raw bytes → UART| the payload | unchanged       |

Status is readable and notifies on every change (and on connect).

### 2. Nordic UART Service (NUS) – free-form HEX bridge

- Send plain-text HEX strings over BLE (e.g. `AA BB 01 FF` or `AABB01FF`)
- Firmware parses them and writes the binary bytes to **hardware UART** (Serial1)
- Binary data received on UART RX is sent back over BLE as space-separated HEX
- The same HEX commands are also accepted on the USB Serial port (for easy testing)

### Hardware pins

| Function | XIAO Pin | Notes |
|----------|----------|-------|
| UART TX  | **D6**   | P1.11 → connect to external device RX |
| UART RX  | **D7**   | P1.12 → connect to external device TX |
| GND      | GND      | Common ground |

Default baud rate: **115200 8N1** (change `UART_BAUD` in the sketch if needed).

Device name: **`XIAO-SmartLock`**

---

## Build & Flash with VS Code (Windows) + UF2

### Prerequisites

1. **VS Code**  
   https://code.visualstudio.com/

2. **Arduino extension for VS Code**  
   - Open Extensions (`Ctrl+Shift+X`)
   - Search “Arduino” by Microsoft and install it

3. **Board package**  
   - Open Command Palette (`Ctrl+Shift+P`) → `Arduino: Board Manager`
   - Add Additional Boards Manager URL:  
     `https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json`
   - Search for **seeed nrf52** and install **Seeed nRF52 Boards**

4. **Python 3** (for UF2 conversion)  
   https://www.python.org/downloads/  
   During install check “Add Python to PATH”.

5. **uf2conv.py**  
   ```powershell
   pip install adafruit-nrfutil
   # or
   git clone https://github.com/adafruit/Adafruit_nRF52_Bootloader
   # uf2conv.py lives in Adafruit_nRF52_Bootloader\lib\uf2\utils\
   ```

### Open the project in VS Code

1. Clone this repository or copy the files.
2. Open the folder that contains `XIAO_nRF52840_BLE_to_UART_HEX.ino` in VS Code.
3. Select the board:
   - `Ctrl+Shift+P` → `Arduino: Board Config`
   - Board: **Seeed XIAO nRF52840** (Seeed nRF52 Boards)
4. (Optional) Select the COM port for Serial Monitor.

### Compile / Export binary

1. `Ctrl+Shift+P` → `Arduino: Verify` (or click the ✓ check-mark).  
   This compiles the sketch.
2. To obtain the `.hex` file:
   - `Ctrl+Shift+P` → `Arduino: Export Compiled Binary`  
   The `.hex` appears next to the `.ino` (or inside a build folder).

### Convert .hex → .uf2 (Windows PowerShell / CMD)

```powershell
python uf2conv.py  XIAO_nRF52840_BLE_to_UART_HEX.ino.hex  -c  -f 0xADA52840  -o  XIAO_SmartLock.uf2
```

Expected output:
```
Converting to uf2, output size: ......, start address: 0x27000
Wrote ...... bytes to XIAO_SmartLock.uf2
```

### Flash by drag-and-drop (UF2)

1. Connect the XIAO nRF52840 with a USB-C cable.
2. **Double-click** the RESET button quickly.  
   A new drive appears in File Explorer (usually `XIAO` or `NRF52BOOT`).
3. Copy `XIAO_SmartLock.uf2` onto that drive.
4. The board reboots automatically and runs the firmware.  
   The drive disappears – this is normal.

> Tip: You can also use the Arduino extension’s Upload button (serial DFU). UF2 is preferred for pure file-copy flashing on Windows.

---

## Testing

1. Power the board – it advertises as **`XIAO-SmartLock`**.
2. Open **nRF Connect** (mobile or desktop).
3. Connect and explore the services.
4. Write `00` (Unlock) or `01` (Lock) to characteristic `0xFF01`.
5. Enable notifications on Lock Status (`0xFF02`).
6. Or use the Nordic UART Service and send HEX strings such as `AA BB CC DD`.
7. Open the Serial Monitor in VS Code (115200 baud) to see debug messages.

---

## File structure

```
BLE-uart-nrf52840/
├── README.md
└── XIAO_nRF52840_BLE_to_UART_HEX.ino   ← main firmware
```

---

## Customisation

- Change the UART action packets inside `sendLockPacket()`.
- Change initial lock state: `uint8_t lockState = 1;`
- Change device name: `Bluefruit.setName("YourName");`
- Change battery value: `blebas.write(xx);`
- Change UART baud rate: `#define UART_BAUD 9600`

---

## License

MIT – based on Adafruit Bluefruit nRF52 examples.  
Feel free to use, modify and distribute.
