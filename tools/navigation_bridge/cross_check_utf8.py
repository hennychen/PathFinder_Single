#!/usr/bin/env python3
"""临时交叉验证: Kotlin utf8SafePrefix (字节规则回退) 与 Python bridge utf8_safe_prefix 等价性。"""


def kt_impl(data: bytes, max_bytes: int) -> bytes:
    """与 NavProtocol.kt utf8SafePrefix 一致的前向遍历实现 (修复后)。"""
    offset = 0
    while offset < max_bytes and offset < len(data):
        lead = data[offset]
        if 0x00 <= lead <= 0x7F:
            char_len = 1
        elif 0xC2 <= lead <= 0xDF:
            char_len = 2
        elif 0xE0 <= lead <= 0xEF:
            char_len = 3
        elif 0xF0 <= lead <= 0xF4:
            char_len = 4
        else:
            break
        if offset + char_len > max_bytes or offset + char_len > len(data):
            break
        for i in range(1, char_len):
            if (data[offset + i] & 0xC0) != 0x80:
                return data[:offset]
        offset += char_len
    return data[:offset]


def py_impl(data: bytes, max_len: int) -> bytes:
    bounded = data[:max_len]
    while bounded:
        try:
            bounded.decode("utf-8")
            return bounded
        except UnicodeDecodeError:
            bounded = bounded[:-1]
    return b""


def main() -> None:
    ok = True
    cases = [
        ("中文道路名称测试东长安街延长线", 32),
        ("Chang'an Avenue", 32),
        (" Mixed 中英结合 Road 123 ", 32),
        ("北京", 1),
        ("A", 0),
        ("", 10),
        ("长" * 40, 32),
        ("好", 2),
        ("好", 3),
    ]
    for s, m in cases:
        b = s.encode("utf-8")
        k, p = kt_impl(b, m), py_impl(b, m)
        match = k == p
        ok &= match
        print(f"{'PASS' if match else 'FAIL'} max={m:2d} kt={k!r} py={p!r}")

    # 全边界扫描
    scanned = 0
    for m in range(0, 36):
        for s in ["中文abc", "abc中", "\U0001f600emoji", "混合road", "éèê", "AB", "好路", "aé中😀"]:
            b = s.encode("utf-8")
            scanned += 1
            if kt_impl(b, m) != py_impl(b, m):
                ok = False
                print("FAIL", repr(s), m)
    print(f"scanned={scanned} boundary cases")
    print("ALL OK" if ok else "MISMATCH FOUND")


if __name__ == "__main__":
    main()
