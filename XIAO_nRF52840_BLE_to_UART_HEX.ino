/*********************************************************************
 * XIAO nRF52840 - BLE Smart Lock + Plain Text HEX → UART Translator
 *
 * Implements the Smart Lock GATT structure from the reference photo
 * PLUS the original Nordic UART Service (NUS) for free-form HEX commands.
 *
 * === Smart Lock GATT (matches photo) ===
 * Service 0000FF00-0000-1000-8000-00805F9B34FB
 *   ├─ 0000FF01  Lock/Unlock Command     Write | Write No Response
 *   └─ 0000FF02  Lock Status (Open/Closed) Read | Notify
 *
 * Service 0000180F-... (Battery Service)
 *   └─ 00002A19  Battery Level           Read
 *
 * Service 0000180A-... (Device Information)
 *   └─ 00002A29  Manufacturer Name       Read
 *
 * Lock command protocol (1 byte written to FF01):
 *   0x00 = Unlock / Open
 *   0x01 = Lock / Close
 *   other values = treated as raw payload and forwarded to UART
 *
 * On every valid lock/unlock the firmware:
 *   1. Updates internal lock state
 *   2. Notifies the new status on FF02
 *   3. Sends a short HEX packet on hardware UART (Serial1)
 *        Unlock → 55 AA 00
 *        Lock   → 55 AA 01
 *
 * Original NUS functionality is fully preserved:
 *   - Send plain-text HEX strings over BLE UART → binary on UART TX
 *   - Binary data on UART RX → HEX text back over BLE UART
 *   - Same commands also accepted on USB Serial for testing
 *
 * Hardware (XIAO nRF52840 / Sense)
 *   UART TX = D6 (P1.11)   RX = D7 (P1.12)   @ 115200 8N1
 *
 * Flash as UF2:
 *   1. Arduino IDE + Seeed nRF52 Boards package
 *   2. Board = Seeed XIAO nRF52840
 *   3. Sketch → Export compiled Binary → .hex
 *   4. uf2conv.py your.hex -c -f 0xADA52840 -o app.uf2
 *   5. Double-click RESET → copy .uf2 to the bootloader drive
 *
 * Test with nRF Connect:
 *   Connect to "XIAO-SmartLock"
 *   - Write 0x00 / 0x01 to the Lock/Unlock characteristic
 *   - Observe Notify on Lock Status
 *   - Or use the Nordic UART Service for arbitrary HEX
 *
 * MIT License – based on Adafruit Bluefruit examples
 *********************************************************************/

#include <bluefruit.h>
#include <Adafruit_TinyUSB.h>   // Needed for reliable USB Serial on Seeed nRF52 Boards

// ─── Standard services (already match the photo) ──────────────────────────────
BLEDfu  bledfu;          // OTA DFU
BLEDis  bledis;          // Device Information (Manufacturer Name = 0x2A29)
BLEBas  blebas;          // Battery Service   (Battery Level   = 0x2A19)
BLEUart bleuart;         // Nordic UART Service (kept for free HEX)

// ─── Custom Smart Lock service (0xFF00) ──────────────────────────────
BLEService        lockService   = BLEService(0xFF00);
BLECharacteristic lockCmdChar   = BLECharacteristic(0xFF01); // Write, Write No Resp
BLECharacteristic lockStatusChar= BLECharacteristic(0xFF02); // Read, Notify

// Lock state: 0 = Open/Unlocked, 1 = Closed/Locked
uint8_t lockState = 1;   // start locked

// UART
#define UART_BAUD 115200

// Command buffer for NUS / USB Serial HEX parsing
#define CMD_BUF_SIZE 256
char cmdBuffer[CMD_BUF_SIZE];
size_t cmdLen = 0;

// ─── Forward declarations ──────────────────────────────────────────
void lock_cmd_write_callback(uint16_t conn_hdl, BLECharacteristic* chr,
                             uint8_t* data, uint16_t len);
void processHexToUart(const char* str, size_t len);
void sendLockPacket(uint8_t cmd);

void setup()
{
  Serial.begin(115200);
  delay(100);

  Serial1.begin(UART_BAUD);

  Serial.println();
  Serial.println("XIAO nRF52840 – Smart Lock + HEX↔UART Translator");
  Serial.println("------------------------------------------------");
  Serial.println("UART : TX=D6  RX=D7  @ 115200");
  Serial.println("GATT : FF00 service + Battery + DIS + Nordic UART");
  Serial.println();

  Bluefruit.autoConnLed(true);
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);

  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("XIAO-SmartLock");

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  // 1. OTA DFU (recommended first)
  bledfu.begin();

  // 2. Device Information Service (Manufacturer Name matches photo)
  bledis.setManufacturer("Seeed Studio");
  bledis.setModel("XIAO nRF52840 SmartLock");
  bledis.begin();

  // 3. Battery Service (matches photo 0x180F / 0x2A19)
  blebas.begin();
  blebas.write(95);          // demo value – change as needed

  // 4. Nordic UART Service (original free-form HEX feature)
  bleuart.begin();

  // 5. Custom Smart Lock Service 0xFF00
  lockService.begin();

  // Characteristic 0xFF01 – Lock/Unlock Command
  lockCmdChar.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  lockCmdChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  lockCmdChar.setMaxLen(20);               // allow short multi-byte payloads too
  lockCmdChar.setWriteCallback(lock_cmd_write_callback);
  lockCmdChar.begin();

  // Characteristic 0xFF02 – Lock Status (Open/Closed)
  lockStatusChar.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  lockStatusChar.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  lockStatusChar.setFixedLen(1);
  lockStatusChar.begin();
  lockStatusChar.write8(lockState);        // initial value

  // Advertising
  startAdv();

  Serial.println("Advertising as \"XIAO-SmartLock\"");
  Serial.println("Write 0x00 (Unlock) or 0x01 (Lock) to FF01 characteristic");
  Serial.println("Or send plain-text HEX over Nordic UART Service");
}

void startAdv(void)
{
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();

  // Advertise the interesting services
  Bluefruit.Advertising.addService(lockService);
  Bluefruit.Advertising.addService(bleuart);
  // BAS & DIS are usually discovered after connection; optional to add them

  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);    // 20 ms / 152.5 ms
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

// ─── Lock command write handler ──────────────────────────────────
void lock_cmd_write_callback(uint16_t conn_hdl, BLECharacteristic* chr,
                             uint8_t* data, uint16_t len)
{
  (void)conn_hdl;
  (void)chr;

  if (len == 0) return;

  Serial.print("[LOCK] Write received (");
  Serial.print(len);
  Serial.print(" bytes): ");
  for (uint16_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  uint8_t cmd = data[0];

  if (cmd == 0x00) {               // Unlock / Open
    lockState = 0;
    Serial.println("  → UNLOCK / OPEN");
    sendLockPacket(0x00);
  }
  else if (cmd == 0x01) {          // Lock / Close
    lockState = 1;
    Serial.println("  → LOCK / CLOSE");
    sendLockPacket(0x01);
  }
  else {
    // Any other value: treat the whole payload as raw HEX and send to UART
    Serial.println("  → raw payload → UART");
    Serial1.write(data, len);
  }

  // Update + notify status characteristic
  lockStatusChar.write8(lockState);
  lockStatusChar.notify8(lockState);
}

// Send the short action packet on the hardware UART
void sendLockPacket(uint8_t cmd)
{
  uint8_t pkt[3] = { 0x55, 0xAA, cmd };
  Serial1.write(pkt, 3);

  Serial.print("  → UART packet: 55 AA ");
  if (cmd < 0x10) Serial.print('0');
  Serial.println(cmd, HEX);
}

// ─── Main loop (NUS + USB Serial + UART RX forwarding) ─────────────
void loop()
{
  // 1. Nordic UART Service → parse HEX text → UART
  while (bleuart.available())
  {
    char c = (char)bleuart.read();
    if (c == '\r') continue;
    if (c == '\n' || cmdLen >= CMD_BUF_SIZE - 1)
    {
      cmdBuffer[cmdLen] = '\0';
      if (cmdLen > 0) {
        Serial.print("[NUS] CMD: ");
        Serial.println(cmdBuffer);
        processHexToUart(cmdBuffer, cmdLen);
      }
      cmdLen = 0;
    }
    else
    {
      cmdBuffer[cmdLen++] = c;
    }
  }

  // 2. USB Serial also accepts the same HEX commands
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n' || cmdLen >= CMD_BUF_SIZE - 1)
    {
      cmdBuffer[cmdLen] = '\0';
      if (cmdLen > 0) {
        Serial.print("[USB] CMD: ");
        Serial.println(cmdBuffer);
        processHexToUart(cmdBuffer, cmdLen);
      }
      cmdLen = 0;
    }
    else
    {
      cmdBuffer[cmdLen++] = c;
    }
  }

  // 3. Hardware UART RX → HEX text back to BLE NUS
  if (Serial1.available())
  {
    uint8_t buf[64];
    int count = 0;
    delay(5);
    while (Serial1.available() && count < (int)sizeof(buf))
      buf[count++] = Serial1.read();

    if (count > 0)
    {
      char hexOut[3 * 64 + 2];
      size_t pos = 0;
      for (int i = 0; i < count; i++)
      {
        if (i) hexOut[pos++] = ' ';
        pos += sprintf(hexOut + pos, "%02X", buf[i]);
      }
      hexOut[pos++] = '\n';
      hexOut[pos] = '\0';

      bleuart.write((const uint8_t*)hexOut, pos);
      Serial.print("[UART→NUS] ");
      Serial.print(hexOut);
    }
  }
}

// Parse free-form HEX string and write binary to Serial1
void processHexToUart(const char* str, size_t len)
{
  uint8_t bytes[128];
  int byteCount = 0;
  int nibble = -1;

  for (size_t i = 0; i < len; i++)
  {
    char c = str[i];
    int val = -1;
    if      (c >= '0' && c <= '9') val = c - '0';
    else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
    else continue;

    if (nibble < 0) {
      nibble = val;
    } else {
      bytes[byteCount++] = (uint8_t)((nibble << 4) | val);
      nibble = -1;
      if (byteCount >= (int)sizeof(bytes)) break;
    }
  }

  if (byteCount > 0)
  {
    Serial1.write(bytes, byteCount);
    Serial.print("  → UART HEX (");
    Serial.print(byteCount);
    Serial.print(" bytes): ");
    for (int i = 0; i < byteCount; i++)
    {
      if (i) Serial.print(' ');
      if (bytes[i] < 0x10) Serial.print('0');
      Serial.print(bytes[i], HEX);
    }
    Serial.println();
  }
  else
  {
    Serial.println("  (no valid HEX bytes)");
  }
}

void connect_callback(uint16_t conn_handle)
{
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = {0};
  connection->getPeerName(central_name, sizeof(central_name));
  Serial.print("Connected to: ");
  Serial.println(central_name);

  // Push current lock status so the central sees it immediately
  lockStatusChar.notify8(lockState);
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
  (void)conn_handle;
  Serial.print("Disconnected, reason = 0x");
  Serial.println(reason, HEX);
  cmdLen = 0;
}
