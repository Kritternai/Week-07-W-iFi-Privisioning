# ใบงานที่ 7.1  การศึกษากลไก Reset Provisioning 3 รูปแบบ และ NVS Memory Forensics

## 0. กล่าวนำ (Introduction)
เมื่ออุปกรณ์ ESP32 ผ่านการ Provisioning สำเร็จแล้ว ข้อมูล Wi-Fi จะถูกบันทึกไว้ใน NVS Flash Memory อย่างถาวร เมื่อเปิดเครื่องใหม่ เฟิร์มแวร์จะเข้าสู่สถานะ `Already provisioned` และข้ามขั้นตอนการรับข้อมูลใหม่ไปทันที

ในการพัฒนาผลิตภัณฑ์และการทดสอบความปลอดภัย วิศวกรจำเป็นต้องทราบวิธีการล้างค่าคอนฟิก (Factory Reset / Erase Credentials) ซึ่งในใบงานนี้นักศึกษาจะได้ทดลองและเปรียบเทียบกลไกการ Reset ครบทั้ง 3 รูปแบบ:
1. **Developer CLI Reset:** การล้างผ่านคำสั่ง Command Line บนเครื่องคอมพิวเตอร์
2. **Build-time Firmware Reset:** การกำหนดค่าผ่าน `menuconfig`
3. **Consumer Hardware Reset:** การต่อสวิตช์ปุ่มกดภายนอก (**External Pushbutton บน GPIO 18**) เพื่อใช้เป็นปุ่ม Factory Reset ทางกายภาพ เสมือนอุปกรณ์ IoT เชิงพาณิชย์จริง (หลีกเลี่ยงการใช้ปุ่ม BOOT/GPIO 0 ที่เป็น Strapping Pin)

---

## 1. วัตถุประสงค์ (Objectives)
1. ศึกษาและทำความเข้าใจสถานะ `Already provisioned` และการตัดสินใจของ `wifi_prov_mgr_is_provisioned()`
2. สามารถล้างข้อมูลการเชื่อมต่อใน Flash Memory ผ่านคำสั่ง CLI (`idf.py erase-flash` และ `esptool.py`) ได้
3. สามารถกำหนดค่า Build Configuration ใน `menuconfig` เพื่อสั่งรีเซ็ต State Machine ได้
4. เข้าใจข้อจำกัดของ **Strapping Pins (GPIO 0 / Bootloader Trap)** และสามารถต่อสวิตช์ปุ่มกดภายนอก (GPIO 18) เพื่อเขียนโปรแกรม Factory Reset ทางกายภาพได้อย่างถูกต้อง

---

## 2. อุปกรณ์ที่ใช้ในการทดลอง (Equipment)
1. บอร์ดไมโครคอนโทรลเลอร์ ESP32 พร้อมสาย USB
2. สวิตช์ปุ่มกด (Tactile Pushbutton Switch) จำนวน 1 ตัว พร้อมสายต่อ Breadboard
3. ESP-IDF Command Prompt (VS Code Terminal)

> [!IMPORTANT]
> **ทำไมจึงไม่ใช้ปุ่ม BOOT (GPIO 0) กดค้างตอนรีเซ็ตบอร์ด?**
> ขา **GPIO 0** บน ESP32 ทำหน้าที่เป็น **Strapping Pin** สำหรับเลือกโหมดการบูต หากขา GPIO 0 มีสถานะเป็น `LOW (0)` ในจังหวะที่บอร์ดถูกรีเซ็ตหรือจ่ายไฟ ชิป ESP32 จะเข้าสู่โหมด **ROM Download Bootloader** (`waiting for download`) ทันที ทำให้ตัวประมวลผลหยุดรอการแฟลชโปรแกรมและไม่รันโค้ด `app_main()` 
> 
> ดังนั้น ในการออกแบบอุปกรณ์เชิงพาณิชย์ จึงนิยมใช้ขา GPIO ทั่วไป (เช่น **GPIO 18**) ต่อร่วมกับปุ่มกดภายนอกเพื่อทำ Factory Reset แทน

---

## 3. สถาปัตยกรรมและการต่อวงจร (Hardware Wiring & Flow)

### 3.1 การต่อวงจรปุ่มกด Factory Reset (GPIO 18)
- ขาหนึ่งของสวิตช์ปุ่มกด $\rightarrow$ ต่อเข้าขา **GPIO 18** ของ ESP32
- อีกขาหนึ่งของสวิตช์ $\rightarrow$ ต่อลง **GND**
*(เปิดใช้งาน Internal Pull-up Resistor ในโค้ด จึงไม่ต้องต่อตัวต้านทานภายนอกเพิ่ม)*

```mermaid
flowchart TD
    Start["⚡ เริ่มต้นทำงาน (app_main)"] --> Check_GPIO["1. ตรวจสอบปุ่ม Factory Reset (GPIO 18)<br/>ถูกกดค้างไว้ 3 วินาทีหรือไม่?"]
    
    Check_GPIO -- "กดค้างครบ 3 วิ (Low/0)" --> HW_Reset["[Hardware Reset Mode]<br/>สั่ง nvs_flash_erase()<br/>และเข้าสู่ Provisioning"]
    Check_GPIO -- "ไม่ได้กด (High/1)" --> Check_Config["2. ตรวจสอบ Build-time Flag<br/>(#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED)"]
    
    HW_Reset --> Init_Prov["เข้าสู่โหมด Provisioning<br/>(กระจายสัญญาณ BLE / SoftAP)"]
    
    Check_Config -- "เปิดใช้งาน Flag" --> Menu_Reset["[Menuconfig Reset]<br/>เรียก wifi_prov_mgr_reset_provisioning()"]
    Menu_Reset --> Init_Prov
    
    Check_Config -- "ปิดใช้งาน Flag" --> Check_NVS["3. ตรวจสอบค่าใน NVS Flash<br/>wifi_prov_mgr_is_provisioned()"]
    
    Check_NVS -- "true (มีข้อมูลเดิม)" --> STA_Mode["[Already Provisioned]<br/>เริ่ม Wi-Fi Station ทันที"]
    Check_NVS -- "false (ว่างเปล่า/เพิ่งถูกลบด้วย CLI)" --> Init_Prov
```

---

## 4. ขั้นตอนการทดลอง (Step-by-Step Procedures)

### ตอนที่ 1 การล้าง Flash ผ่าน Command Line (Developer Level)
1. เสียบสาย USB เข้ากับคอมพิวเตอร์ ตรวจสอบหมายเลขพอร์ต COM (เช่น `COM24`)
2. เปิด Terminal ในโฟลเดอร์โปรเจกต์ `Week-07-W-iFi-Privisioning/Example_codes/Lab7-1-Reset-and-NVS-Forensics`
3. สั่งล้าง Flash Memory ทั้งหมดของชิปด้วยคำสั่ง:
   ```powershell
   idf.py -p COM24 erase-flash
   ```
4. ทำการ Flash โปรแกรมและเปิด Serial Monitor:
   ```powershell
   idf.py -p COM24 flash monitor
   ```
5. สังเกต Log ว่า ESP32 จะรายงานสถานะ `"Starting provisioning"` และสร้าง QR Code ขึ้นมาบนหน้าจอ

![CLI Full Chip Erase Flash Output](images/lab7-1-cli-erase-flash.png)

---

### ตอนที่ 2 การบังคับ Reset ผ่าน Menuconfig (Firmware Configuration Level)
1. กดปุ่ม `Ctrl + ]` เพื่อออกจาก Serial Monitor
2. เปิดหน้าต่างคอนฟิกโปรเจกต์:
   ```powershell
   idf.py menuconfig
   ```
3. ใช้ปุ่มลูกศรเลื่อนไปที่หัวข้อ **Example Configuration**
4. เลื่อนไปที่บรรทัด **`Reset Provisioned state (Erase credentials)`** แล้วกดปุ่ม `Spacebar` เพื่อเลือกให้มีเครื่องหมาย `[*]`
5. กดปุ่ม `S` เพื่อบันทึก และ `Q` เพื่อออกจากเมนู
6. สั่ง Build และ Flash โปรแกรม:
   ```powershell
   idf.py -p COM24 flash monitor
   ```
7. สังเกตผลลัพธ์ใน Log: บอร์ดจะทำการล้าง Credentials เก่าทิ้งทุกครั้งที่เปิดเครื่องใหม่

![Menuconfig Configuration Report](images/lab7-1-menuconfig.png)

---

### ตอนที่ 3 การสร้างปุ่ม Factory Reset ด้วยฮาร์ดแวร์ภายนอก (GPIO 18)

1. นำสวิตช์ปุ่มกดต่อเข้ากับขา **GPIO 18** และ **GND**
2. เพิ่มฟังก์ชันตรวจสอบปุ่ม Factory Reset ลงในไฟล์ `main/main.c`:

```c
#include "driver/gpio.h"

#define FACTORY_RESET_BUTTON_GPIO  GPIO_NUM_18   // ปุ่ม Factory Reset ภายนอก (ต่อลง GND)

static bool check_factory_reset_button(void)
{
    // กำหนดค่า GPIO 18 เป็น Input พร้อมเปิด Internal Pull-up Resistor
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI("FACTORY_RESET", "Hold GPIO 18 button for 3 seconds to trigger Factory Reset...");
    
    // ตรวจสอบสถานะปุ่มกดค้าง (Active Low / Logic 0)
    int hold_count = 0;
    while (gpio_get_level(FACTORY_RESET_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        hold_count++;
        if (hold_count % 10 == 0) {
            ESP_LOGI("FACTORY_RESET", "Holding button... %d/3 seconds", hold_count / 10);
        }
        if (hold_count >= 30) { // กดค้างครบ 3 วินาที (30 x 100ms)
            ESP_LOGW("FACTORY_RESET", "=================================================");
            ESP_LOGW("FACTORY_RESET", ">>> FACTORY RESET TRIGGERED! ERASING NVS FLASH <<<");
            ESP_LOGW("FACTORY_RESET", "=================================================");
            return true;
        }
    }
    return false;
}
```

3. เรียกใช้งานในตอนเริ่มต้นของฟังก์ชัน `app_main()`:

```c
void app_main(void)
{
    // ตรวจสอบการกดปุ่ม Factory Reset ทางกายภาพ (GPIO 18)
    if (check_factory_reset_button()) {
        ESP_ERROR_CHECK(nvs_flash_erase());
    }

    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    // ... โค้ดเดิมต่อจากนี้ ...
```

#### การทดสอบ:
1. ปล่อยให้บอร์ดทำงานปกติ $\rightarrow$ บอร์ดจะจำค่าเดิมได้ (`Already provisioned`)
2. กดปุ่มที่ต่อกับ **GPIO 21 (หรือ GPIO 18) ค้างไว้ 3 วินาที** จากนั้นกดรีเซ็ตบอร์ด หรือกดค้างขณะเปิดเครื่อง
3. สังเกต Serial Monitor: ระบบจะตรวจพบการกดค้าง 3 วินาที และสั่งล้าง NVS Flash เพื่อกลับสู่โหมด Provisioning ทันที!

![Hardware Factory Reset Triggered via GPIO 21](images/lab7-1-hardware-reset.png)

---

---

## 5. กิจกรรมถอดรหัสซอร์สโค้ดและเขียนผังงาน (Code Deconstruction & Flowchart Assignment)

ให้นักศึกษาศึกษาโค้ดใน `main/main.c` และ `main/led_indicator.c` แล้วเขียน **ผังงาน (Flowchart / State Diagram)** เพื่ออธิบายการตัดสินใจและการทำงานของระบบ:

### ภารกิจที่ 1  ผังงานการตัดสินใจช่วง Bootstrapping & Reset Decision
ให้นักศึกษาวาด Flowchart แสดงลำดับตรรกะการตรวจสอบเงื่อนไขตั้งแต่เริ่มต้นรันฟังก์ชัน `app_main()` โดยต้องครอบคลุม:
1. การตรวจสอบสถานะปุ่ม **GPIO 18 / GPIO 21** (ตรวจจับการกดค้าง 3 วินาที)
2. การทำงานของ `nvs_flash_init()` และกรณีที่ต้อง `nvs_flash_erase()`
3. การตรวจสอบ Macro `#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED`
4. การเรียกฟังก์ชัน `network_prov_mgr_is_wifi_provisioned(&provisioned)` / `wifi_prov_mgr_is_provisioned(&provisioned)`
5. จุดแยกสายการทำงานเข้าสู่โหมด **Provisioning Mode** หรือ **Station Mode**

```mermaid
flowchart TD
    Start(["⚡ เริ่มต้น app_main()"]) --> Task["สร้าง Background Task<br/>xTaskCreate(led_status_task)"]
    Task --> CheckBtn{"1. ตรวจสอบปุ่ม Factory Reset (GPIO 18/21)<br/>ถูกกดค้างครบ 3 วินาทีหรือไม่?"}
    
    CheckBtn -- "กดค้าง >= 3s (Active-Low / 0)" --> HW_Reset["[Hardware Factory Reset]<br/>สั่ง nvs_flash_erase() ล้างค่า NVS"]
    CheckBtn -- "ไม่ได้กด หรือกดไม่ถึง 3s (High / 1)" --> InitNVS["2. เริ่มต้น NVS Partition<br/>ret = nvs_flash_init()"]
    
    HW_Reset --> InitNVS
    
    InitNVS --> CheckNVSResult{"ret == NO_FREE_PAGES หรือ<br/>NEW_VERSION_FOUND?"}
    CheckNVSResult -- "Yes (มีปัญหาเนื้อที่ไม่พอ/เวอร์ชันเปลี่ยน)" --> EraseReinit["nvs_flash_erase()<br/>nvs_flash_init()"]
    CheckNVSResult -- "No (สำเร็จปกติ)" --> CheckConfigMacro{"3. ตรวจสอบ Build-time Macro<br/>#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED"}
    EraseReinit --> CheckConfigMacro
    
    CheckConfigMacro -- "เปิดใช้งาน Flag (y)" --> MenuReset["[Menuconfig Reset Mode]<br/>เรียก wifi_prov_mgr_reset_provisioning()"]
    CheckConfigMacro -- "ปิดใช้งาน Flag" --> InitNetif["4. เริ่มต้น Network Stack<br/>esp_netif_init()<br/>esp_event_loop_create_default()<br/>esp_wifi_init()"]
    MenuReset --> InitNetif
    
    InitNetif --> InitProv["เริ่มต้น Provisioning Manager<br/>wifi_prov_mgr_init(config)"]
    InitProv --> CheckProv{"5. ตรวจสอบสถานะ Provisioning<br/>wifi_prov_mgr_is_provisioned(&provisioned)"}
    
    CheckProv -- "provisioned == false<br/>(NVS ว่างเปล่า / เพิ่งถูกลบ)" --> UnprovMode["[Not Provisioned State]<br/>wifi_prov_mgr_deinit()<br/>พร้อมรับการทำ Provisioning (Lab 7-2 / Lab 7-3)"]
    CheckProv -- "provisioned == true<br/>(มี Wi-Fi Credentials เดิมใน NVS)" --> ProvMode["[Already Provisioned State]<br/>wifi_prov_mgr_deinit()<br/>esp_wifi_set_mode(WIFI_MODE_STA)<br/>esp_wifi_start() เพื่อเชื่อมต่อ Router ทันที"]
```

### ภารกิจที่ 2 ผังสถานะการเปลี่ยนจังหวะไฟ LED 1 (Wi-Fi STA Indicator)
ให้นักศึกษาวาด State Diagram แสดงการเปลี่ยนสถานะของ **LED 1 (GPIO 2)**:
- เงื่อนไขใดทำให้ LED 1 เข้าสู่สถานะ `LED_STA_MODE_DISCONNECTED` (กระพริบ 200ms Mark / 200ms Space)
- เงื่อนไขหรือ Event ใดทำให้เปลี่ยนเป็น `LED_STA_MODE_CONNECTED` (Heartbeat 200ms ทุก 1s)

```mermaid
stateDiagram-v2
    [*] --> LED_STA_OFF: บูตระบบเริ่มต้น (app_main)
    
    LED_STA_OFF --> LED_STA_DISCONNECTED: Event WIFI_EVENT_STA_START<br/>(สั่งเปิดโหมด Wi-Fi Station และเริ่มค้นหา AP)
    
    LED_STA_DISCONNECTED --> LED_STA_CONNECTED: Event IP_EVENT_STA_GOT_IP<br/>(เชื่อมต่อสำเร็จ ได้รับ IP Address จาก Router)
    
    LED_STA_CONNECTED --> LED_STA_DISCONNECTED: Event WIFI_EVENT_STA_DISCONNECTED<br/>(สัญญาณหลุด / Router ปิดตัวลง)
    
    state LED_STA_DISCONNECTED {
        [*] --> BlinkAlert: Alert Pattern
        note right of BlinkAlert
            กระพริบเตือนถี่ยิบ:
            ติด 200ms / ดับ 200ms
        end note
    }
    
    state LED_STA_CONNECTED {
        [*] --> HeartbeatPattern: Normal Heartbeat
        note right of HeartbeatPattern
            จังหวะชีพจรปกติ (Online):
            ติด 200ms / ดับ 800ms (ทุก 1 วินาที)
        end note
    }
```

---

## 6. ตารางบันทึกผลการทดลอง (Experiment Results)

| รูปแบบการ Reset                  | คำสั่ง / พฤติกรรมที่ทำ               | พฤติกรรมของ LED แต่ละดวงหลังเปิดเครื่อง | สถานะใน Serial Monitor |
| :------------------------------- | :----------------------------------- | :-------------------------------------- | :--------------------- |
| **1. CLI Erase**                 | `idf.py erase-flash`                 | LED 1 ดับสนิท (ไม่อยู่ในโหมด STA)       | `[STATUS]: Device is NOT provisioned (NVS is empty)` และรอการทำ Provisioning ใหม่ |
| **2. Menuconfig Flag**           | `CONFIG_EXAMPLE_RESET_PROVISIONED=y` | LED 1 ดับสนิท                           | แสดง Log บังคับรีเซ็ต State Machine และเข้าสู่สถานะ Not Provisioned ทุกครั้งที่บูตเครื่อง |
| **3. Hardware Button (GPIO 18/21)** | กดปุ่ม GPIO 18/21 ค้าง 3 วินาที   | ขณะกดค้าง LED 1 ดับ / หลังรีเซ็ตเข้าสู่สถานะ Unprovisioned | Log นับเวลา `Holding button... 1/3... 2/3... 3/3` $\rightarrow$ `>>> FACTORY RESET TRIGGERED! ERASING NVS FLASH <<<` $\rightarrow$ `Device is NOT provisioned` |

---

## 7. คำถามท้ายการทดลอง (Post-Lab Questions)

1. **เพราะเหตุใดการกดปุ่ม BOOT (GPIO 0) ค้างไว้ในจังหวะรีเซ็ตบอร์ด จึงทำให้โปรแกรมค้างอยู่ที่ ROM Bootloader และไม่ยอมทำงานต่อ?**
   * **คำตอบ:** ขา **GPIO 0** บน ESP32 ทำหน้าที่เป็น **Strapping Pin** สำหรับเลือกโหมดการบูตระดับฮาร์ดแวร์ หากขา GPIO 0 มีสถานะ Logic เป็น `LOW (0)` ในจังหวะที่บอร์ดถูกรีเซ็ตหรือจ่ายไฟ (Power-on Reset) ชิป ESP32 จะเข้าสู่โหมด **ROM Download Bootloader** (`waiting for download`) ทันทีเพื่อรอการแฟลชโปรแกรมผ่านสาย Serial ทำให้ซีพียูหยุดรอและไม่กระโดดไปรันเฟิร์มแวร์ Application ใน `app_main()` ดังนั้นจึงไม่สามารถใช้ปุ่ม BOOT เป็นปุ่ม Factory Reset ขณะบูตได้

2. **เพราะเหตุใดคำสั่ง `idf.py erase-flash` จึงทำให้ข้อมูลเฟิร์มแวร์ Application หายไปด้วย ในขณะที่ `nvs_flash_erase()` ไม่ทำให้เฟิร์มแวร์หาย?**
   * **คำตอบ:** คำสั่ง `idf.py erase-flash` เป็นการสั่งล้างหน่วยความจำ Flash ทั้งหมดของชิป (Full Chip Erase ตั้งแต่ Offset `0x00000` จนถึงขนาดสูงสุด) ซึ่งลบทั้ง Bootloader, Partition Table, Application Firmware และ NVS Storage ในขณะที่ฟังก์ชัน `nvs_flash_erase()` เป็นการลบข้อมูลเฉพาะใน Partition ที่ชื่อว่า **`nvs`** (ซึ่งอยู่ที่ Offset `0x9000` ขนาด 24KB ตามตารางพาร์ติชัน) เท่านั้น ทำให้โค้ด Application ที่อยู่ใน Factory Partition (`0x10000`) ยังคงอยู่ครบถ้วนและทำงานต่อได้ตามปกติ

3. **การออกแบบปุ่ม Factory Reset บนอุปกรณ์ IoT เชิงพาณิชย์ เหตุใดจึงต้องกำหนดให้ผู้ใช้กดปุ่มค้างไว้ 3-5 วินาที แทนที่จะสั่งลบข้อมูลทันทีที่แตะปุ่มเพียงเสี้ยววินาที?**
   * **คำตอบ:** เพื่อป้องกัน **Accidental Trigger (การเผลอกดโดนโดยไม่ได้ตั้งใจ)** เช่น การหยิบจับ เคลื่อนย้ายอุปกรณ์ การทำความสะอาด หรือสัญญาณรบกวนทางไฟฟ้า (Contact Bounce Noise) ซึ่งหากสั่งล้างค่าทันทีจะส่งผลให้ Wi-Fi Credentials หลุด ทำให้อุปกรณ์หลุดจากการเชื่อมต่อ Cloud ทันทีและสร้างความเสียหายต่อประสบการณ์ของผู้ใช้งาน ดังนั้นการหน่วงเวลา 3-5 วินาทีจึงเป็นการยืนยันเจตนาที่แท้จริงของผู้ใช้ (Intentional Action)

4. **หากอุปกรณ์ IoT ถูกติดตั้งอยู่บนเสาสูงหรือฝังอยู่ในผนัง วิธีการ Reset ทางกายภาพรูปแบบใดเหมาะสมที่สุด?**
   * **คำตอบ:** การใช้วิธี **Power-Cycle Pattern Reset (การตัด-ต่อไฟตามลำดับ)** เช่น การเปิด-ปิดสวิตช์ไฟหลักติดต่อกัน 5 ครั้ง (แต่ละครั้งเว้นระยะ 1-2 วินาที) เหมือนกับหลอดไฟ Smart Bulb หรือการสั่ง Reset ระยะไกลผ่าน **Remote Cloud Command / BLE Beacon Activation** เนื่องจากไม่ต้องปีนขึ้นไปหรือรื้อผนังเพื่อกดปุ่มทางกายภาพที่ตัวอุปกรณ์


