#!/usr/bin/env python3
"""Base44 CLI (C 版) の自動テスト。

Python 3 標準ライブラリのみを使用し、ビルド済みバイナリ ./base44 を
subprocess で叩いて検証する。`make test` から実行される。
コア API の全数テストは test/core_test.c (C ハーネス) が担当し、
こちらは CLI の振る舞いに集中する二段構成になっている。

検証の観点:
- 検証済みベクトル (独立な Python 参照実装と相互検証した値) と
  長さ 1..6 の段階列 (RFC 4648 のベクトル列に相当するもの)
- ラウンドトリップ (長さ 0..257 + 10,000 + 100,000 + 1MiB、
  決定的乱数、全 0 / 全 FF / 先頭 0 / 末尾 0、全バイト値 0..255)
- 独立参照実装との照合、符号の性質 (使用文字と符号長の公式)
- 不正入力 (文字セット外・不正桁数・オーバーフロー境界・不正な -w・
  未知オプション) とエラー時の exit コード / stderr / 無出力保証
- 折り返し (デフォルト 76、幅 0..7, 76, 100, 1000000、境界値)
- デコード時の空白の扱い (種類・位置・空白のみ・CRLF)
- -i (ゴミの位置・ゴミのみ・-i なしとの対照)
- オプションの結合・省略形・-- 終端・ファイルの後ろのオプション
- ファイル系エラー (存在しない・ディレクトリ・2 つ指定・空ファイル)
- パイプ連結の往復、GNU base64 との exit コード符号の一致
"""

import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "base44"
GNU_BASE64 = shutil.which("base64")

ALPHABET = "ABCDEFGHJKMNPQRTUYabcdefghjkmnpqrtuy23456789"
EXCLUDED = "01IiLlOoSsVvWwXxZz"


def run(args, data=b""):
    return subprocess.run(
        [str(BIN), *args], input=data, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def ref_encode(data: bytes) -> str:
    """コア (C) とは独立な参照実装: ブロック毎に int 変換する。"""
    out = []
    for i in range(0, len(data), 2):
        block = data[i:i + 2]
        v = int.from_bytes(block, "big")
        digits = 3 if len(block) == 2 else 2
        chars = [""] * digits
        for k in range(digits - 1, -1, -1):
            chars[k] = ALPHABET[v % 44]
            v //= 44
        assert v == 0, "参照実装: 桁あふれ"
        out.append("".join(chars))
    return "".join(out)


def digits_of(v: int, n: int) -> str:
    chars = [""] * n
    for k in range(n - 1, -1, -1):
        chars[k] = ALPHABET[v % 44]
        v //= 44
    assert v == 0
    return "".join(chars)


def rand_bytes(rng: random.Random, n: int) -> bytes:
    return bytes(rng.randrange(256) for _ in range(n))


# 検証済みベクトル (hex, Base44)
VECTORS = [
    ("", ""),
    ("00", "AA"),
    ("ff", "Fy"),
    ("0000", "AAA"),
    ("ffff", "t3b"),
    ("01", "AB"),
    ("41", "Bd"),                                # 'A'
    ("4869", "KhQ"),                             # 'Hi'
    ("48656c6c6f", "KhKRR2Cf"),                  # 'Hello'
    ("68656c6c6f", "QyYRR2Cf"),                  # 'hello'
    ("426173653434", "JuKTNYG5r"),               # 'Base44'
    ("e38193e38293e381abe381a1e381af",
     "pDnbgbYNqpDnerDYGKpDnD9"),                 # こんにちは (UTF-8)
    ("f09f9880", "qy9cHP"),                      # 😀 (UTF-8)
    ("610a62", "P2jCM"),                         # 'a\nb'
]


class TestVectors(unittest.TestCase):
    def test_alphabet_shape(self):
        # 文字セットそのものはベクトル群経由で固定する。
        # 除外文字が混入していないことは参照実装の定義でも確認する。
        self.assertEqual(len(ALPHABET), 44)
        self.assertEqual(len(set(ALPHABET)), 44)
        for ch in EXCLUDED:
            self.assertNotIn(ch, ALPHABET)

    def test_encode_vectors(self):
        for hexs, expected in VECTORS:
            data = bytes.fromhex(hexs)
            self.assertEqual(ref_encode(data), expected, f"参照実装 hex={hexs}")
            r = run(["-w", "0"], data)
            self.assertEqual(r.returncode, 0, f"hex={hexs}")
            self.assertEqual(r.stdout.decode(), expected, f"hex={hexs}")

    def test_decode_vectors(self):
        for hexs, encoded in VECTORS:
            r = run(["-d"], encoded.encode())
            self.assertEqual(r.returncode, 0, f"encoded={encoded}")
            self.assertEqual(r.stdout, bytes.fromhex(hexs),
                             f"encoded={encoded}")

    def test_text_roundtrip_via_bytes(self):
        texts = [
            "",
            "Hello, World!",
            "abc XYZ 123 !@#",
            "こんにちは、世界",
            "改行を\n含む\nテキスト",
            "😀🎉🚀 絵文字",
            "日本語と English と 😀 の混在テキスト",
            "長いテキスト。" * 500,
        ]
        for t in texts:
            data = t.encode("utf-8")
            enc = run(["-w", "0"], data)
            self.assertEqual(enc.returncode, 0)
            self.assertEqual(enc.stdout.decode(), ref_encode(data))
            dec = run(["-d"], enc.stdout)
            self.assertEqual(dec.returncode, 0)
            self.assertEqual(dec.stdout.decode("utf-8"), t)


class TestRoundTrip(unittest.TestCase):
    def test_basic_patterns(self):
        cases = [
            b"",
            bytes([0x42]),
            bytes([0x42, 0x43]),
            bytes([0x42, 0x43, 0x44]),
            bytes([0x42, 0x43, 0x44, 0x45]),
            bytes([1, 2, 3, 4, 5]),
            bytes(16),                    # 全 0x00
            bytes([0xFF] * 16),           # 全 0xFF
            bytes([0, 0, 0, 1, 2, 3]),    # 先頭 0
            bytes([1, 2, 3, 0, 0, 0]),    # 末尾 0
            bytes([0, 0xFF, 0, 0xFF, 0]),
            bytes([0xFF, 0xFE]),          # UTF-8 としては不正でもバイナリ透過
            bytes([0x80]),
            bytes([0xC3, 0x28]),
        ]
        for data in cases:
            enc = run([], data)
            self.assertEqual(enc.returncode, 0)
            self.assertEqual(enc.stdout.decode().replace("\n", ""),
                             ref_encode(data))
            dec = run(["-d"], enc.stdout)
            self.assertEqual(dec.returncode, 0)
            self.assertEqual(dec.stdout, data)

    def test_random_lengths_0_to_257(self):
        rng = random.Random(123456789)
        for n in range(0, 258):
            data = rand_bytes(rng, n)
            enc = run(["-w", "0"], data)
            self.assertEqual(enc.returncode, 0, f"n={n}")
            self.assertEqual(enc.stdout.decode(), ref_encode(data), f"n={n}")
            dec = run(["-d"], enc.stdout)
            self.assertEqual(dec.returncode, 0, f"n={n}")
            self.assertEqual(dec.stdout, data, f"n={n}")

    def test_large_data(self):
        rng = random.Random(987654321)
        for n in (10000, 100000):
            data = rand_bytes(rng, n)
            enc = run([], data)
            self.assertEqual(enc.returncode, 0, f"n={n}")
            self.assertEqual(enc.stdout.decode().replace("\n", ""),
                             ref_encode(data), f"n={n}")
            dec = run(["-d"], enc.stdout)
            self.assertEqual(dec.returncode, 0, f"n={n}")
            self.assertEqual(dec.stdout, data, f"n={n}")


class TestInvalidInput(unittest.TestCase):
    def assert_decode_error(self, text):
        r = run(["-d"], text.encode())
        self.assertNotEqual(r.returncode, 0, f"input={text!r}")
        self.assertTrue(r.stderr.startswith(b"base44: "),
                        f"input={text!r} stderr={r.stderr!r}")
        self.assertEqual(r.stdout, b"", "失敗時に途中まで出力しない")

    def test_invalid_characters(self):
        for bad in ["0", "1", "I", "l", "O", "S", "V", "W", "X", "Z", "!"]:
            self.assert_decode_error(f"A{bad}A")
        self.assert_decode_error("A!A")
        self.assert_decode_error("A\x00A")  # NUL バイトも文字セット外

    def test_invalid_lengths(self):
        for s in ["A", "AAAA", "AAAAAAA", "AAAAAAAAAA"]:
            self.assertEqual(len(s) % 3, 1)
            self.assert_decode_error(s)
        # 正常な桁数は通る
        self.assertEqual(run(["-d"], b"AA").stdout, bytes([0]))
        self.assertEqual(run(["-d"], b"AAA").stdout, bytes([0, 0]))
        self.assertEqual(len(run(["-d"], b"AAAAA").stdout), 3)

    def test_overflow_boundaries(self):
        # 3 桁ブロック: 65535 は正常、65536 はオーバーフロー
        self.assertEqual(digits_of(65535, 3), "t3b")
        self.assertEqual(run(["-d"], b"t3b").stdout, bytes.fromhex("ffff"))
        self.assertEqual(digits_of(65536, 3), "t3c")
        self.assert_decode_error("t3c")
        self.assert_decode_error(digits_of(70000, 3))
        self.assert_decode_error("999")  # 85183
        # 2 桁ブロック: 255 は正常、256 はオーバーフロー
        self.assertEqual(digits_of(255, 2), "Fy")
        self.assertEqual(run(["-d"], b"Fy").stdout, bytes.fromhex("ff"))
        self.assertEqual(digits_of(256, 2), "F2")
        self.assert_decode_error("F2")
        self.assert_decode_error("99")  # 1935


class TestCli(unittest.TestCase):
    def test_stdin_encode_matches_reference(self):
        r = run([], b"Hello")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"KhKRR2Cf\n")

    def test_file_argument(self):
        data = bytes([0, 1, 2, 250, 251, 252])
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "in.bin"
            path.write_bytes(data)
            r = run([str(path)])
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stdout.decode(), ref_encode(data) + "\n")
            dec = run(["-d", "-"], r.stdout)
            self.assertEqual(dec.returncode, 0)
            self.assertEqual(dec.stdout, data)
            # ファイルに書かれた符号をファイル引数でデコードする
            b44 = Path(d) / "in.b44"
            b44.write_bytes(r.stdout)
            dec2 = run(["-d", str(b44)])
            self.assertEqual(dec2.returncode, 0)
            self.assertEqual(dec2.stdout, data)

    def test_default_wrap_76(self):
        rng = random.Random(7)
        data = rand_bytes(rng, 500)
        r = run([], data)
        self.assertEqual(r.returncode, 0)
        lines = r.stdout.decode().split("\n")
        self.assertEqual(lines[-1], "")  # 最終行にも改行
        for line in lines[:-1]:
            self.assertLessEqual(len(line), 76)
        self.assertEqual("".join(lines), ref_encode(data))

    def test_wrap_zero_no_newline(self):
        r = run(["-w", "0"], b"Hello")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"KhKRR2Cf")  # 末尾改行もなし

    def test_wrap_forms(self):
        data = bytes([0x61] * 60)
        for args in (["--wrap=10"], ["--wrap", "10"], ["-w10"], ["-w", "10"]):
            r = run(args, data)
            self.assertEqual(r.returncode, 0, " ".join(args))
            for line in r.stdout.decode().rstrip("\n").split("\n"):
                self.assertLessEqual(len(line), 10, " ".join(args))

    def test_empty_input_no_output(self):
        r = run([], b"")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"")

    def test_decode_accepts_whitespace(self):
        enc = ref_encode("Hello, Base44!".encode())
        wrapped = (enc[:4] + "\n" + enc[4:] + " \t\r\n").encode()
        r = run(["--decode"], wrapped)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"Hello, Base44!")

    def test_ignore_garbage(self):
        enc = ref_encode(b"hi")
        dirty = f"!{enc[:2]} 0{enc[2:]}!".encode()
        ng = run(["-d"], dirty)
        self.assertNotEqual(ng.returncode, 0)
        ok = run(["-d", "-i"], dirty)
        self.assertEqual(ok.returncode, 0)
        self.assertEqual(ok.stdout, b"hi")
        combined = run(["-di"], dirty)
        self.assertEqual(combined.returncode, 0)
        self.assertEqual(combined.stdout, b"hi")
        long_form = run(["--decode", "--ignore-garbage"], dirty)
        self.assertEqual(long_form.returncode, 0)

    def test_help(self):
        r = run(["--help"])
        self.assertEqual(r.returncode, 0)
        self.assertIn("Usage: base44 [OPTION]... [FILE]",
                      r.stdout.decode())
        self.assertIn(ALPHABET[:10], r.stdout.decode())

    def test_version(self):
        r = run(["--version"])
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"base44 1.0.0\n")

    def test_option_errors(self):
        for args in (
            ["--bogus"],
            ["-z"],
            ["-w", "abc"],
            ["-w", "-1"],
            ["-w"],
            ["--wrap"],
            ["--wrap="],
            ["a.txt", "b.txt"],
            ["/nonexistent-base44-file-xyz"],
        ):
            r = run(args, b"")
            self.assertNotEqual(r.returncode, 0, " ".join(args))
            self.assertTrue(r.stderr.startswith(b"base44: "),
                            f"{' '.join(args)} stderr={r.stderr!r}")


class TestGraduatedLengths(unittest.TestCase):
    def test_rfc4648_style_series(self):
        # RFC 4648 のベクトル列 (f, fo, foo, ...) の Base44 版
        words = [b"", b"f", b"fo", b"foo", b"foob", b"fooba", b"foobar"]
        for data in words:
            enc = run(["-w", "0"], data)
            self.assertEqual(enc.returncode, 0, f"data={data!r}")
            self.assertEqual(enc.stdout.decode(), ref_encode(data),
                             f"data={data!r}")
            dec = run(["-d"], enc.stdout)
            self.assertEqual(dec.returncode, 0, f"data={data!r}")
            self.assertEqual(dec.stdout, data, f"data={data!r}")

    def test_lengths_1_to_6_patterns(self):
        for n in range(1, 7):
            for data in (bytes(range(n)), bytes([0xFF] * n), bytes(n),
                         b"abcdef"[:n]):
                enc = run(["-w", "0"], data)
                self.assertEqual(enc.returncode, 0, f"data={data!r}")
                self.assertEqual(enc.stdout.decode(), ref_encode(data),
                                 f"data={data!r}")
                dec = run(["-d"], enc.stdout)
                self.assertEqual(dec.stdout, data, f"data={data!r}")


class TestAllBytesAndPatterns(unittest.TestCase):
    def test_all_byte_values(self):
        data = bytes(range(256))
        enc = run(["-w", "0"], data)
        self.assertEqual(enc.returncode, 0)
        self.assertEqual(enc.stdout.decode(), ref_encode(data))
        dec = run(["-d"], enc.stdout)
        self.assertEqual(dec.returncode, 0)
        self.assertEqual(dec.stdout, data)

    def test_repeated_byte_range_pattern(self):
        data = bytes(range(256)) * 64  # 16,384 バイト
        enc = run([], data)
        self.assertEqual(enc.returncode, 0)
        self.assertEqual(enc.stdout.decode().replace("\n", ""),
                         ref_encode(data))
        dec = run(["-d"], enc.stdout)
        self.assertEqual(dec.returncode, 0)
        self.assertEqual(dec.stdout, data)

    def test_1mib_random_roundtrip(self):
        rng = random.Random(20261120)
        data = rng.randbytes(1024 * 1024)
        enc = run([], data)
        self.assertEqual(enc.returncode, 0)
        dec = run(["-d"], enc.stdout)
        self.assertEqual(dec.returncode, 0)
        self.assertEqual(dec.stdout, data)
        enc0 = run(["-w", "0"], data)
        self.assertEqual(enc0.returncode, 0)
        self.assertNotIn(b"\n", enc0.stdout)
        dec0 = run(["-d"], enc0.stdout)
        self.assertEqual(dec0.stdout, data)


class TestEncodedProperties(unittest.TestCase):
    def test_charset_and_length_formula(self):
        # 符号は 44 文字 (+ 折り返しの改行) だけから成り、長さは
        # n バイト -> (n/2)*3 + (n%2)*2 文字という公式に従う
        rng = random.Random(4242)
        for n in (0, 1, 2, 3, 4, 5, 6, 7, 10, 100, 1000):
            data = rand_bytes(rng, n)
            r = run(["-w", "0"], data)
            self.assertEqual(r.returncode, 0, f"n={n}")
            out = r.stdout.decode()
            self.assertTrue(set(out) <= set(ALPHABET), f"n={n}")
            self.assertEqual(len(out), (n // 2) * 3 + (n % 2) * 2,
                             f"n={n}")
            r2 = run([], data)
            self.assertEqual(r2.returncode, 0, f"n={n}")
            self.assertTrue(set(r2.stdout.decode()) <= set(ALPHABET + "\n"),
                            f"n={n} (折り返しあり)")


class TestWrapWidths(unittest.TestCase):
    def test_wrap_widths_matrix(self):
        rng = random.Random(31337)
        data = rand_bytes(rng, 100)  # 符号は 150 文字
        base = run(["-w", "0"], data).stdout
        self.assertEqual(len(base), 150)
        for w in (1, 2, 3, 4, 5, 6, 7, 76, 100, 1000000):
            r = run(["-w", str(w)], data)
            self.assertEqual(r.returncode, 0, f"w={w}")
            self.assertTrue(r.stdout.endswith(b"\n"), f"w={w}")
            lines = r.stdout.decode().split("\n")[:-1]
            for line in lines:
                self.assertGreater(len(line), 0, f"w={w}")
                self.assertLessEqual(len(line), w, f"w={w}")
            self.assertEqual("".join(lines).encode(), base, f"w={w}")
            dec = run(["-d"], r.stdout)
            self.assertEqual(dec.returncode, 0, f"w={w}")
            self.assertEqual(dec.stdout, data, f"w={w}")

    def test_wrap_divisible_and_indivisible(self):
        data = bytes(range(8))  # 符号は 12 文字
        base = run(["-w", "0"], data).stdout.decode()
        self.assertEqual(len(base), 12)
        for w, expected_lines in ((3, 4), (4, 3), (6, 2), (12, 1),
                                  (5, 3), (7, 2)):
            r = run(["-w", str(w)], data)
            self.assertEqual(r.returncode, 0, f"w={w}")
            lines = r.stdout.decode().split("\n")[:-1]
            self.assertEqual(len(lines), expected_lines, f"w={w}")
            if 12 % w == 0:
                self.assertTrue(all(len(x) == w for x in lines),
                                f"w={w} (割り切れる場合は全行が幅ちょうど)")
            self.assertEqual("".join(lines), base, f"w={w}")


class TestWrapValues(unittest.TestCase):
    def assert_wrap_error(self, args):
        r = run(args, b"Hello")
        self.assertNotEqual(r.returncode, 0, f"args={args}")
        self.assertTrue(r.stderr.startswith(b"base44: "),
                        f"args={args} stderr={r.stderr!r}")
        self.assertEqual(r.stdout, b"", f"args={args}")

    def test_wrap_invalid_values(self):
        for v in ("abc", "-1", "-76", "1.5", "+10", " 10", "10 ",
                  "0x10", ""):
            self.assert_wrap_error(["-w", v])
        self.assert_wrap_error(["--wrap="])
        self.assert_wrap_error(["--wrap=abc"])

    def test_wrap_overflow_rejected(self):
        # GNU base64 は巨大値を飽和させて受理するが、base44 は
        # ULLONG_MAX を超える値を明示的にエラーにする (HISTORY 参照)
        for v in ("18446744073709551616",
                  "99999999999999999999999999999",
                  "340282366920938463463374607431768211456"):
            self.assert_wrap_error(["-w", v])

    def test_wrap_max_accepted(self):
        # ULLONG_MAX ちょうどまでは受理する (旧実装の境界バグの回帰防止)
        for v in ("18446744073709551615", "18446744073709551614",
                  "9223372036854775807"):
            r = run(["-w", v], b"Hello")
            self.assertEqual(r.returncode, 0, f"w={v}")
            self.assertEqual(r.stdout, b"KhKRR2Cf\n", f"w={v}")

    def test_wrap_leading_zeros(self):
        r = run(["-w", "0076"], b"Hello")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"KhKRR2Cf\n")
        r = run(["-w", "000"], b"Hello")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"KhKRR2Cf")  # 0 扱いで末尾改行もなし


class TestDecodeWhitespace(unittest.TestCase):
    DATA = b"Hello, Base44 whitespace!"

    def test_whitespace_kinds_and_positions(self):
        enc = ref_encode(self.DATA)
        variants = []
        for ws in (" ", "\t", "\r", "\n", "\v", "\f", "\r\n", " \t\r\n"):
            variants.append(ws + enc)                    # 先頭
            variants.append(enc + ws)                    # 末尾
            variants.append(enc[:3] + ws + enc[3:])      # ブロック境界
            variants.append(enc[:1] + ws + enc[1:])      # ブロック途中
            blocks = [enc[i:i + 3] for i in range(0, len(enc), 3)]
            variants.append(ws.join(blocks))             # 各ブロック間
        variants.append(" ".join(enc))                   # 全文字間
        for v in variants:
            r = run(["-d"], v.encode())
            self.assertEqual(r.returncode, 0, f"input={v!r}")
            self.assertEqual(r.stdout, self.DATA, f"input={v!r}")

    def test_whitespace_only_input(self):
        for data in (b"", b"\n", b"\r\n", b" ", b" \t\r\n\v\f", b"\n\n\n"):
            r = run(["-d"], data)
            self.assertEqual(r.returncode, 0, f"input={data!r}")
            self.assertEqual(r.stdout, b"", f"input={data!r}")

    def test_crlf_wrapped_input(self):
        rng = random.Random(5)
        data = rand_bytes(rng, 100)
        enc = run(["-w", "4"], data).stdout.decode()
        r = run(["-d"], enc.replace("\n", "\r\n").encode())
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, data)
        # 末尾改行がない形でも復号できる
        r2 = run(["-d"], enc.rstrip("\n").encode())
        self.assertEqual(r2.returncode, 0)
        self.assertEqual(r2.stdout, data)


class TestIgnoreGarbageExpanded(unittest.TestCase):
    def test_garbage_positions(self):
        data = b"garbage test data"
        enc = ref_encode(data).encode()
        garbages = (b"!", b"@#", b"0", b"1", b"IiLl", b"OoSs",
                    b"VvWwXxZz", b"\x00", b"\x80\xff", b"!=?")
        for g in garbages:
            dirties = (g + enc, enc + g, enc[:3] + g + enc[3:],
                       enc[:1] + g + enc[1:])
            for dirty in dirties:
                r = run(["-di"], dirty)
                self.assertEqual(r.returncode, 0, f"dirty={dirty!r}")
                self.assertEqual(r.stdout, data, f"dirty={dirty!r}")

    def test_garbage_only_input(self):
        for dirty in (b"!!!", b"01IiLlOoSsVvWwXxZz", b"\x00\x01\x02",
                      b"!@#$%^&*()"):
            r = run(["-d", "-i"], dirty)
            self.assertEqual(r.returncode, 0, f"dirty={dirty!r}")
            self.assertEqual(r.stdout, b"", f"dirty={dirty!r}")
            # 対照: -i なしでは同じ入力がエラーになる
            ng = run(["-d"], dirty)
            self.assertNotEqual(ng.returncode, 0, f"dirty={dirty!r}")
            self.assertEqual(ng.stdout, b"", f"dirty={dirty!r}")

    def test_garbage_contrast_without_i(self):
        enc = ref_encode(b"hi").encode()
        for dirty in (b"!" + enc + b"!", enc[:1] + b"\x80" + enc[1:],
                      b"0" + enc):
            ng = run(["-d"], dirty)
            self.assertNotEqual(ng.returncode, 0, f"dirty={dirty!r}")
            self.assertEqual(ng.stdout, b"", f"dirty={dirty!r}")
            ok = run(["-d", "-i"], dirty)
            self.assertEqual(ok.returncode, 0, f"dirty={dirty!r}")
            self.assertEqual(ok.stdout, b"hi", f"dirty={dirty!r}")


class TestOptionForms(unittest.TestCase):
    def test_combined_short_options(self):
        enc = ref_encode(b"hi").encode()
        dirty = b"!" + enc + b"!"
        for args in (["-di"], ["-id"], ["-d", "-i"],
                     ["--decode", "--ignore-garbage"], ["--decode", "-i"],
                     ["-d", "--ignore-garbage"]):
            r = run(args, dirty)
            self.assertEqual(r.returncode, 0, f"args={args}")
            self.assertEqual(r.stdout, b"hi", f"args={args}")

    def test_abbreviated_long_options(self):
        # getopt_long の一意な省略形 (GNU coreutils と同じ挙動)
        r = run(["--dec"], b"KhKRR2Cf")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"Hello")
        r = run(["--ver"])
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"base44 1.0.0\n")
        r = run(["--wr=5"], b"Hello")
        self.assertEqual(r.returncode, 0)
        for line in r.stdout.decode().split("\n")[:-1]:
            self.assertLessEqual(len(line), 5)
        r = run(["-d", "--ig"], b"!KhKRR2Cf!")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, b"Hello")
        r = run(["--h"])
        self.assertEqual(r.returncode, 0)
        self.assertIn(b"Usage: base44", r.stdout)

    def test_double_dash_terminator(self):
        data = b"dash test"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "in.b44"
            path.write_bytes(ref_encode(data).encode() + b"\n")
            r = run(["-d", "--", str(path)])
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stdout, data)
            r = run(["--", str(path)])
            self.assertEqual(r.returncode, 0)  # -- の後ろはファイル扱い
            r = run(["--", "-"], data)
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stdout.decode().replace("\n", ""),
                             ref_encode(data))

    def test_wrap_ignored_when_decoding(self):
        for args in (["-d", "-w", "5"], ["-d", "--wrap=0"],
                     ["-w", "5", "-d"]):
            r = run(args, b"KhKRR2Cf")
            self.assertEqual(r.returncode, 0, f"args={args}")
            self.assertEqual(r.stdout, b"Hello", f"args={args}")

    def test_option_after_file_permutation(self):
        # glibc getopt は引数の順序を入れ替えるため、ファイルの後ろに
        # 置いたオプションも効く (GNU base64 でも同じ)
        data = b"permutation"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "in.b44"
            path.write_bytes(ref_encode(data).encode() + b"\n")
            r = run([str(path), "-d"])
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stdout, data)


class TestFileErrors(unittest.TestCase):
    MISSING = "/nonexistent-base44-file-xyz"

    def assert_file_error(self, args):
        r = run(args)
        self.assertNotEqual(r.returncode, 0, f"args={args}")
        self.assertTrue(r.stderr.startswith(b"base44: "),
                        f"args={args} stderr={r.stderr!r}")
        self.assertEqual(r.stdout, b"", f"args={args}")

    def test_nonexistent_file(self):
        self.assert_file_error([self.MISSING])
        self.assert_file_error(["-d", self.MISSING])
        self.assert_file_error(["-d", "-i", self.MISSING])

    def test_directory_as_file(self):
        with tempfile.TemporaryDirectory() as d:
            self.assert_file_error([d])
            self.assert_file_error(["-d", d])

    def test_two_files_stdout_empty(self):
        self.assert_file_error(["a.txt", "b.txt"])
        self.assert_file_error(["-d", "a.txt", "b.txt"])

    def test_empty_and_newline_only_files(self):
        with tempfile.TemporaryDirectory() as d:
            empty = Path(d) / "empty"
            empty.write_bytes(b"")
            for args in ([str(empty)], ["-d", str(empty)]):
                r = run(args)
                self.assertEqual(r.returncode, 0, f"args={args}")
                self.assertEqual(r.stdout, b"", f"args={args}")
            nl = Path(d) / "nl.b44"
            nl.write_bytes(b"\n")
            r = run(["-d", str(nl)])
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stdout, b"")
            ws = Path(d) / "ws.b44"
            ws.write_bytes(b" \t\r\n")
            r = run(["-d", str(ws)])
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stdout, b"")


class TestNoOutputOnError(unittest.TestCase):
    def test_no_output_on_late_errors(self):
        # 途中まで正常でも、最後に不正があれば stdout は空のまま
        rng = random.Random(777)
        data = rand_bytes(rng, 100)
        enc = ref_encode(data)  # 150 文字
        enc101 = ref_encode(rand_bytes(rng, 101))  # 152 文字
        patterns = [
            enc + "!",            # 末尾の不正文字
            enc[:75] + "!" + enc[76:],  # 途中の不正文字
            enc + "999",          # 末尾の 3 桁オーバーフロー
            enc + "F2",           # 末尾の 2 桁オーバーフロー
            enc + "A",            # 長さ mod 3 == 1 になる
            enc101[:-1],          # 最終ブロックで切れた形 (mod 3 == 1)
        ]
        for p in patterns:
            r = run(["-d"], p.encode())
            self.assertNotEqual(r.returncode, 0, f"input={p[:24]}...")
            self.assertEqual(r.stdout, b"", f"input={p[:24]}...")


class TestTruncatedDecode(unittest.TestCase):
    def test_truncated_final_block(self):
        enc = "KhKRR2Cf"  # 'Hello' の符号 (8 文字)
        # mod 3 == 1 の長さで切れた接頭辞はエラーになる
        for cut in (1, 4, 7):
            r = run(["-d"], enc[:cut].encode())
            self.assertNotEqual(r.returncode, 0, f"cut={cut}")
            self.assertEqual(r.stdout, b"", f"cut={cut}")
        # ブロック境界 (mod 3 == 0) の接頭辞はそこまでのバイトになる
        self.assertEqual(run(["-d"], b"KhK").stdout, b"He")
        self.assertEqual(run(["-d"], b"KhKRR2").stdout, b"Hell")
        self.assertEqual(run(["-d"], enc.encode()).stdout, b"Hello")


class TestPipeRoundTrip(unittest.TestCase):
    def test_pipe_roundtrip(self):
        # base44 file | base44 -d のパイプ連結で元に戻る
        rng = random.Random(60606)
        data = rand_bytes(rng, 100000)
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "in.bin"
            src.write_bytes(data)
            enc_p = subprocess.Popen([str(BIN), str(src)],
                                     stdout=subprocess.PIPE)
            dec_p = subprocess.Popen([str(BIN), "-d"],
                                     stdin=enc_p.stdout,
                                     stdout=subprocess.PIPE)
            enc_p.stdout.close()
            out = dec_p.communicate()[0]
            self.assertEqual(dec_p.returncode, 0)
            self.assertEqual(enc_p.wait(), 0)
            self.assertEqual(out, data)


@unittest.skipUnless(GNU_BASE64, "GNU base64 がない環境ではスキップ")
class TestGnuParity(unittest.TestCase):
    def test_exit_sign_parity(self):
        # 同じ操作で GNU base64 と base44 の exit コードの符号
        # (0 か非 0 か) が一致することを確認する。文面は比較しない。
        # 対象は「どちらも受理/どちらも拒否」で合意できる操作に限る
        # (巨大 -w や +付き数値など、意図的に異なるものは除く。
        #  HISTORY の「GNU 実測で見つけた挙動差」参照)
        with tempfile.TemporaryDirectory() as d:
            cases = [
                ["/nonexistent-base44-probe-xyz"],
                ["-d", "/nonexistent-base44-probe-xyz"],
                [d],
                ["-d", d],
                ["a.txt", "b.txt"],
                ["--bogus"],
                ["-z"],
                ["-w", "abc"],
                ["-w", "-1"],
                ["--wrap="],
                ["-w"],
                ["--wrap"],
                ["--help"],
                ["--version"],
                ["-w", "0"],
                ["-w", "76"],
                ["--dec"],
            ]
            for args in cases:
                g = subprocess.run([GNU_BASE64, *args], input=b"",
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
                b = run(args, b"")
                self.assertEqual(
                    g.returncode == 0, b.returncode == 0,
                    f"args={args} gnu={g.returncode} base44={b.returncode}")


if __name__ == "__main__":
    if not BIN.exists():
        print(f"base44: バイナリ {BIN} がありません。先に make してください",
              file=sys.stderr)
        sys.exit(1)
    unittest.main(verbosity=2)
