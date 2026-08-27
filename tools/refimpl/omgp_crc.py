"""Independent reference: CRC-16/CCITT-FALSE. Must agree with link/crc16.hpp."""
def crc16_ccitt_false(data: bytes, crc: int = 0xFFFF) -> int:
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

if __name__ == "__main__":
    assert crc16_ccitt_false(b"123456789") == 0x29B1, "published check value failed"
    assert crc16_ccitt_false(b"") == 0xFFFF
    print("refimpl: CRC vectors ok")
