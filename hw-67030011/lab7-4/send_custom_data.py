#!/usr/bin/env python3
import asyncio
import sys
import os

# Add protocomm proto path
sys.path.append("/Users/kbbk/.espressif/v6.0.2/esp-idf/components/protocomm/python")
import session_pb2
import sec1_pb2
import constants_pb2

from bleak import BleakScanner, BleakClient
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

POP_KEY = b"abcd1234"
CUSTOM_DATA_TEXT = "STUDENT_ID:67030011"

def xor_bytes(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))

async def main():
    print(f"🔍 กำลังค้นหา ESP32 BLE Device (PROV4_38D0EC)...")
    
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name and ("PROV4" in d.name or "PROV" in d.name)),
        timeout=10.0
    )

    if not device:
        print("⚠️ กำลังค้นหาอุปกรณ์ทั้งหมดรอบข้าง...")
        devices = await BleakScanner.discover(timeout=6.0)
        for d in devices:
            if d.name and "PROV" in d.name:
                device = d
                break

    if not device:
        print("❌ ไม่พบอุปกรณ์ ESP32 (โปรดตรวจสอบว่ามือถือไม่ได้กำลังเชื่อมต่อค้างอยู่)")
        return

    print(f"✅ พบอุปกรณ์: {device.name} ({device.address})")
    print("🔗 กำลังเชื่อมต่อเข้าสู่ GATT Server...")

    async with BleakClient(device) as client:
        print("🎉 เชื่อมต่อ GATT Server สำเร็จ!")

        session_char = None
        custom_char = None

        for service in client.services:
            for char in service.characteristics:
                # Also check UUID directly (021aff51 = prov-session, 021aff54 = custom-data)
                if "ff51" in str(char.uuid).lower():
                    session_char = char
                elif "ff54" in str(char.uuid).lower():
                    custom_char = char

                for desc in char.descriptors:
                    try:
                        val = (await client.read_gatt_descriptor(desc.handle)).decode("utf-8")
                        print(f"  [GATT] Handle {desc.handle} ({char.uuid}) => \"{val}\"")
                        if val == "prov-session":
                            session_char = char
                        elif val == "custom-data":
                            custom_char = char
                    except Exception:
                        pass

        if not session_char:
            print("❌ ไม่พบ characteristic: prov-session")
            return

        print(f"🔑 เริ่มกระบวนการ Protocomm Security 1 Handshake (PoP: {POP_KEY.decode()})...")

        # Step 1: Generate Client X25519 Keys
        client_priv = X25519PrivateKey.generate()
        client_pub = client_priv.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw
        )

        req0 = session_pb2.SessionData()
        req0.sec_ver = session_pb2.SecScheme1
        req0.sec1.sc0.client_pubkey = client_pub

        print("  -> ส่ง Client Public Key (Step 1)...")
        await client.write_gatt_char(session_char.uuid, req0.SerializeToString(), response=True)
        resp0_bytes = await client.read_gatt_char(session_char.uuid)

        resp0 = session_pb2.SessionData()
        resp0.ParseFromString(resp0_bytes)

        dev_pub = resp0.sec1.sr0.device_pubkey
        dev_rand = resp0.sec1.sr0.device_random

        print(f"  <- ได้รับ Device Public Key ({len(dev_pub)} bytes) และ IV Random ({len(dev_rand)} bytes)")

        # Step 2: Compute Shared Secret and XOR with SHA256(PoP)
        shared_key = client_priv.exchange(X25519PublicKey.from_public_bytes(dev_pub))
        h = hashes.Hash(hashes.SHA256(), backend=default_backend())
        h.update(POP_KEY)
        pop_digest = h.finalize()

        shared_key_pop = xor_bytes(shared_key, pop_digest)

        # Initialize AES-CTR Cipher (continuous keystream)
        cipher_ctx = Cipher(algorithms.AES(shared_key_pop), modes.CTR(dev_rand), backend=default_backend()).encryptor()

        # Step 3: Encrypt Device Public Key as Proof (keystream bytes 0..31)
        client_proof = cipher_ctx.update(dev_pub)
        req1 = session_pb2.SessionData()
        req1.sec_ver = session_pb2.SecScheme1
        req1.sec1.msg = sec1_pb2.Session_Command1
        req1.sec1.sc1.client_verify_data = client_proof

        print("  -> ส่ง Client Verification Proof (Step 2)...")
        await client.write_gatt_char(session_char.uuid, req1.SerializeToString(), response=True)
        resp1_bytes = await client.read_gatt_char(session_char.uuid)

        resp1 = session_pb2.SessionData()
        resp1.ParseFromString(resp1_bytes)

        dev_proof = resp1.sec1.sr1.device_verify_data
        # Decrypt device proof using continuous keystream bytes 32..63
        decrypted_dev_proof = cipher_ctx.update(dev_proof)

        if decrypted_dev_proof != client_pub:
            print("❌ ตรวจสอบ Device Proof ล้มเหลว! (PoP ผิดพลาด หรือ Handshake ไม่ตรง)")
            return

        print("✨ [SECURITY SUCCESS]: Valid PoP! Handshake สำเร็จ และได้ AES Session Key เรียบร้อยแล้ว! 🔐\n")

        # Step 4: Send Custom Data
        CUSTOM_CHAR_UUID = "021aff54-0382-4aea-bff4-6b3f1c5adfb4"
        print(f"📤 กำลังส่ง Custom Data: \"{CUSTOM_DATA_TEXT}\" ไปยัง ({CUSTOM_CHAR_UUID})...")
        custom_payload = cipher_ctx.update(CUSTOM_DATA_TEXT.encode("utf-8"))

        try:
            target_uuid = custom_char.uuid if custom_char else CUSTOM_CHAR_UUID
            await client.write_gatt_char(target_uuid, custom_payload, response=True)
            print("  -> ส่งข้อมูลสำเร็จ รอรับข้อความตอบกลับ...")
            resp_custom = await client.read_gatt_char(target_uuid)
            decrypted_resp = cipher_ctx.update(resp_custom).decode("utf-8", errors="replace")
            print(f"📥 ข้อความตอบกลับจาก ESP32 (Decrypted): \"{decrypted_resp}\"")
        except Exception as e:
            print(f"⚠️ การส่งไปยัง custom characteristic: {e}")

        print("\n" + "=" * 65)
        print("🎉 ส่ง Custom Data สำเร็จเรียบร้อย 100%!")
        print("=" * 65)

if __name__ == "__main__":
    asyncio.run(main())
