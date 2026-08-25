#!/usr/bin/env python3
import asyncio
from bleak import BleakScanner, BleakClient

async def main():
    print("🔍 กำลังค้นหาอุปกรณ์ BLE Provisioning (PROV_38D0EC)...")
    device = await BleakScanner.find_device_by_name("PROV_38D0EC", timeout=10.0)
    
    if not device:
        print("⚠️ ไม่พบชื่อ PROV_38D0EC แบบเจาะจง กำลังค้นหาอุปกรณ์ PROV ทั้งหมด...")
        devices = await BleakScanner.discover(timeout=5.0)
        for d in devices:
            if d.name and "PROV" in d.name:
                device = d
                break
    
    if not device:
        print("❌ ไม่พบอุปกรณ์ ESP32 BLE Device")
        return

    print(f"✅ พบอุปกรณ์: {device.name} (UUID/MAC: {device.address})")
    print("🔗 กำลังเชื่อมต่อเข้าสู่ GATT Server...")
    
    async with BleakClient(device) as client:
        print("🎉 เชื่อมต่อสำเร็จ (GATT Connected)!\n")
        print("=" * 70)
        print("🌳 โครงสร้าง GATT SERVICES, CHARACTERISTICS & DESCRIPTORS (BLE FORENSICS)")
        print("=" * 70)
        
        for service in client.services:
            print(f"\n📁 [PRIMARY SERVICE] UUID: {service.uuid}")
            print(f"   Description: {service.description}")
            print("   " + "-" * 60)
            
            for char in service.characteristics:
                props = ",".join(char.properties)
                print(f"   ├── 📄 [CHARACTERISTIC] UUID: {char.uuid} [{props}]")
                for desc in char.descriptors:
                    try:
                        val = await client.read_gatt_descriptor(desc.handle)
                        val_str = val.decode("utf-8", errors="replace")
                        print(f"   │     └── 🏷️  [DESCRIPTOR 0x2901] Handle: {desc.handle} => \"{val_str}\"")
                    except Exception as e:
                        print(f"   │     └── 🏷️  [DESCRIPTOR] UUID: {desc.uuid} Handle: {desc.handle}")
                        
        print("\n" + "=" * 70)
        print("✨ จบการตรวจสอบโครงสร้าง GATT Forensics เรียบร้อยแล้ว")
        print("=" * 70)

if __name__ == "__main__":
    asyncio.run(main())
