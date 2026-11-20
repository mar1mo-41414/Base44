/*
 * Base44 コア (src/base44.c) の全数テストハーネス。
 *
 * CLI 経由では件数が多すぎて現実的でない「全数」を、コア API を
 * 直接叩いて網羅する:
 * - エンコード全数: 1 バイト全 256 値、2 バイト全 65536 値
 * - デコード全数: 2 文字全 1936 通り、3 文字全 85184 通り
 *   (範囲内なら成功してその値、範囲外ならオーバーフローで失敗)
 * - 全 2 バイト値の符号を連結した長い入力の一括デコード往復
 * - 不正桁数・不正文字 (各位置)・エラー詳細 (位置/桁数/値) の検証
 * - 文字セット定数そのものの検証 (44 文字・重複なし・除外文字なし)
 *
 * 期待値はテスト側に置いた独立な文字セット定義と除算ループで
 * 算出し、ライブラリ側の計算とは別コードで照合する。
 *
 * 実行: make test から呼ばれる。成功時はアサーション総数を表示する。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/base44.h"

/* テスト側の独立な文字セット定義 (ライブラリの定数と一致すべきもの) */
static const char TEST_ALPHABET[] =
    "ABCDEFGHJKMNPQRTUYabcdefghjkmnpqrtuy23456789";
static const char TEST_EXCLUDED[] = "01IiLlOoSsVvWwXxZz";

static unsigned long long g_checks = 0;

static void check_fail(const char *file, int line, const char *expr,
                       const char *detail)
{
    fprintf(stderr, "core_test: FAIL %s:%d: %s (%s)\n", file, line, expr,
            detail);
    exit(1);
}

#define CHECKF(cond, ...)                                                \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            char _buf[512];                                              \
            snprintf(_buf, sizeof(_buf), __VA_ARGS__);                   \
            check_fail(__FILE__, __LINE__, #cond, _buf);                 \
        }                                                                \
    } while (0)

#define CHECK(cond) CHECKF(cond, "-")

/* 値 v を 44 進 n 桁で表した文字列を除算ループで独立に算出する */
static void expected_digits(unsigned int v, int n, char *out)
{
    int k;
    for (k = n - 1; k >= 0; k--) {
        out[k] = TEST_ALPHABET[v % 44u];
        v /= 44u;
    }
    out[n] = '\0';
    if (v != 0) {
        fprintf(stderr, "core_test: 参照計算の桁あふれ\n");
        exit(1);
    }
}

/* エンコード結果が期待文字列と一致し、デコードで元に戻ることを確認 */
static void check_encode(const uint8_t *data, size_t len,
                         const char *expected, const char *what)
{
    base44_error err;
    size_t out_len = 0, dec_len = 0;
    char *enc = base44_encode(data, len, &out_len, &err);
    uint8_t *dec;

    CHECKF(enc != NULL, "%s: encode が NULL", what);
    CHECKF(err.status == BASE44_OK, "%s: encode status=%d", what,
           (int)err.status);
    CHECKF(out_len == strlen(expected), "%s: out_len=%zu expected=%zu",
           what, out_len, strlen(expected));
    CHECKF(strcmp(enc, expected) == 0, "%s: got=%s expected=%s", what,
           enc, expected);

    dec = base44_decode(enc, out_len, &dec_len, &err);
    CHECKF(dec != NULL, "%s: decode(encoded) が NULL", what);
    CHECKF(dec_len == len, "%s: dec_len=%zu len=%zu", what, dec_len, len);
    CHECKF(len == 0 || memcmp(dec, data, len) == 0,
           "%s: roundtrip 不一致", what);
    free(enc);
    free(dec);
}

static void test_alphabet(void)
{
    int seen[256];
    size_t i;

    CHECK(strlen(base44_alphabet) == 44);
    CHECK(strcmp(base44_alphabet, TEST_ALPHABET) == 0);
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < 44; i++) {
        unsigned char c = (unsigned char)base44_alphabet[i];
        CHECKF(!seen[c], "文字セットに重複: '%c'", c);
        seen[c] = 1;
    }
    for (i = 0; TEST_EXCLUDED[i] != '\0'; i++) {
        unsigned char c = (unsigned char)TEST_EXCLUDED[i];
        CHECKF(!seen[c], "除外文字 '%c' が文字セットに含まれている", c);
    }
}

static void test_encode_exhaustive_1byte(void)
{
    unsigned int v;
    for (v = 0; v < 256; v++) {
        uint8_t data[1];
        char expected[3], what[64];
        data[0] = (uint8_t)v;
        expected_digits(v, 2, expected);
        snprintf(what, sizeof(what), "1byte v=%u", v);
        check_encode(data, 1, expected, what);
    }
}

static void test_encode_exhaustive_2byte(void)
{
    unsigned int v;
    for (v = 0; v < 65536; v++) {
        uint8_t data[2];
        char expected[4], what[64];
        data[0] = (uint8_t)(v >> 8);
        data[1] = (uint8_t)(v & 0xffu);
        expected_digits(v, 3, expected);
        snprintf(what, sizeof(what), "2byte v=%u", v);
        check_encode(data, 2, expected, what);
    }
}

static void test_decode_exhaustive_2char(void)
{
    unsigned int a, b;
    for (a = 0; a < 44; a++) {
        for (b = 0; b < 44; b++) {
            char text[2];
            unsigned int v = a * 44u + b;
            base44_error err;
            size_t out_len = 0;
            uint8_t *dec;
            text[0] = TEST_ALPHABET[a];
            text[1] = TEST_ALPHABET[b];
            dec = base44_decode(text, 2, &out_len, &err);
            if (v < 256) {
                CHECKF(dec != NULL, "2char v=%u: 成功するはずが失敗", v);
                CHECKF(err.status == BASE44_OK, "2char v=%u: status", v);
                CHECKF(out_len == 1, "2char v=%u: out_len=%zu", v,
                       out_len);
                CHECKF(dec[0] == (uint8_t)v, "2char v=%u: byte=%u", v,
                       dec[0]);
            } else {
                CHECKF(dec == NULL, "2char v=%u: 失敗するはずが成功", v);
                CHECKF(err.status == BASE44_ERR_OVERFLOW,
                       "2char v=%u: status=%d", v, (int)err.status);
                CHECKF(err.digits == 2, "2char v=%u: digits=%d", v,
                       err.digits);
                CHECKF(err.value == v, "2char v=%u: value=%u", v,
                       err.value);
                CHECKF(err.position == 0, "2char v=%u: position=%zu", v,
                       err.position);
            }
            free(dec);
        }
    }
}

static void test_decode_exhaustive_3char(void)
{
    unsigned int a, b, c;
    for (a = 0; a < 44; a++) {
        for (b = 0; b < 44; b++) {
            for (c = 0; c < 44; c++) {
                char text[3];
                unsigned int v = a * 1936u + b * 44u + c;
                base44_error err;
                size_t out_len = 0;
                uint8_t *dec;
                text[0] = TEST_ALPHABET[a];
                text[1] = TEST_ALPHABET[b];
                text[2] = TEST_ALPHABET[c];
                dec = base44_decode(text, 3, &out_len, &err);
                if (v < 65536) {
                    CHECKF(dec != NULL, "3char v=%u: 成功するはずが失敗",
                           v);
                    CHECKF(out_len == 2, "3char v=%u: out_len=%zu", v,
                           out_len);
                    CHECKF(dec[0] == (uint8_t)(v >> 8),
                           "3char v=%u: hi=%u", v, dec[0]);
                    CHECKF(dec[1] == (uint8_t)(v & 0xffu),
                           "3char v=%u: lo=%u", v, dec[1]);
                } else {
                    CHECKF(dec == NULL, "3char v=%u: 失敗するはずが成功",
                           v);
                    CHECKF(err.status == BASE44_ERR_OVERFLOW,
                           "3char v=%u: status=%d", v, (int)err.status);
                    CHECKF(err.digits == 3, "3char v=%u: digits=%d", v,
                           err.digits);
                    CHECKF(err.value == v, "3char v=%u: value=%u", v,
                           err.value);
                    CHECKF(err.position == 0, "3char v=%u: position=%zu",
                           v, err.position);
                }
                free(dec);
            }
        }
    }
}

static void test_concatenated_all_2byte(void)
{
    /* 全 2 バイト値の符号を連結した 196,608 文字を一括デコードする */
    const size_t total_chars = 65536u * 3u;
    const size_t total_bytes = 65536u * 2u;
    char *big = malloc(total_chars + 3 + 1);
    uint8_t *expected = malloc(total_bytes + 1);
    base44_error err;
    size_t out_len = 0, i;
    uint8_t *dec;
    char *enc;

    CHECK(big != NULL && expected != NULL);
    for (i = 0; i < 65536u; i++) {
        expected_digits((unsigned int)i, 3, big + i * 3);
        expected[i * 2] = (uint8_t)(i >> 8);
        expected[i * 2 + 1] = (uint8_t)(i & 0xffu);
    }

    dec = base44_decode(big, total_chars, &out_len, &err);
    CHECKF(dec != NULL, "連結: decode 失敗 status=%d", (int)err.status);
    CHECKF(out_len == total_bytes, "連結: out_len=%zu", out_len);
    for (i = 0; i < total_bytes; i++)
        CHECKF(dec[i] == expected[i], "連結: byte %zu が不一致", i);

    /* 復元したバイト列を再エンコードすると元の連結文字列に戻る */
    enc = base44_encode(dec, out_len, NULL, &err);
    CHECK(enc != NULL);
    CHECK(strcmp(enc, big) == 0);
    free(enc);
    free(dec);

    /* 末尾に 1 バイト分の 2 文字ブロックを足した混合長も往復する */
    expected_digits(0xabu, 2, big + total_chars);
    expected[total_bytes] = 0xab;
    dec = base44_decode(big, total_chars + 2, &out_len, &err);
    CHECKF(dec != NULL, "連結+末尾: decode 失敗");
    CHECKF(out_len == total_bytes + 1, "連結+末尾: out_len=%zu", out_len);
    CHECK(dec[total_bytes] == 0xab);
    CHECK(memcmp(dec, expected, total_bytes) == 0);
    free(dec);
    free(big);
    free(expected);
}

static void test_invalid_lengths(void)
{
    static const size_t bad_lens[] = {1, 4, 7, 10, 13, 100, 1000, 10000};
    static const size_t good_lens[] = {0, 2, 3, 5, 6, 8, 9, 11, 12};
    char *buf = malloc(10001);
    size_t i;

    CHECK(buf != NULL);
    memset(buf, 'A', 10000);

    for (i = 0; i < sizeof(bad_lens) / sizeof(bad_lens[0]); i++) {
        size_t len = bad_lens[i];
        base44_error err;
        uint8_t *dec = base44_decode(buf, len, NULL, &err);
        CHECKF(dec == NULL, "長さ %zu: 失敗するはずが成功", len);
        CHECKF(err.status == BASE44_ERR_INVALID_LENGTH,
               "長さ %zu: status=%d", len, (int)err.status);
        CHECKF(err.length == len, "長さ %zu: err.length=%zu", len,
               err.length);
    }
    for (i = 0; i < sizeof(good_lens) / sizeof(good_lens[0]); i++) {
        size_t len = good_lens[i];
        size_t out_len = 0;
        base44_error err;
        uint8_t *dec = base44_decode(buf, len, &out_len, &err);
        CHECKF(dec != NULL, "長さ %zu: 成功するはずが失敗", len);
        CHECKF(out_len == (len / 3) * 2 + (len % 3 == 2 ? 1 : 0),
               "長さ %zu: out_len=%zu", len, out_len);
        free(dec);
    }
    free(buf);
}

static void test_invalid_chars_and_error_details(void)
{
    static const char bad_chars[] = {
        '0', '1', 'I', 'i', 'L', 'l', 'O', 'o', 'S', 's', 'V', 'v',
        'W', 'w', 'X', 'x', 'Z', 'z', '!', ' ', '\t', '\n',
        '\0', 0x7f, (char)0x80, (char)0xff,
    };
    size_t bi;
    int pos;

    /* 3 文字ブロックの各位置に不正文字を置く */
    for (pos = 0; pos < 3; pos++) {
        for (bi = 0; bi < sizeof(bad_chars); bi++) {
            char text[3] = {'A', 'A', 'A'};
            base44_error err;
            uint8_t *dec;
            text[pos] = bad_chars[bi];
            dec = base44_decode(text, 3, NULL, &err);
            CHECKF(dec == NULL, "不正文字 pos=%d ch=%d: 成功してしまった",
                   pos, (unsigned char)bad_chars[bi]);
            CHECKF(err.status == BASE44_ERR_INVALID_CHAR,
                   "不正文字 pos=%d: status=%d", pos, (int)err.status);
            CHECKF(err.position == (size_t)pos,
                   "不正文字 pos=%d: err.position=%zu", pos, err.position);
            CHECKF(err.ch == (unsigned char)bad_chars[bi],
                   "不正文字 pos=%d: err.ch=%d", pos, err.ch);
        }
    }

    /* 5 文字 (3+2) の各位置でも位置が正しく報告される */
    for (pos = 0; pos < 5; pos++) {
        char text[5] = {'A', 'A', 'A', 'A', 'A'};
        base44_error err;
        uint8_t *dec;
        text[pos] = '!';
        dec = base44_decode(text, 5, NULL, &err);
        CHECKF(dec == NULL, "5文字 pos=%d: 成功してしまった", pos);
        CHECKF(err.status == BASE44_ERR_INVALID_CHAR,
               "5文字 pos=%d: status=%d", pos, (int)err.status);
        CHECKF(err.position == (size_t)pos, "5文字 pos=%d: position=%zu",
               pos, err.position);
    }

    /* 文字エラーはオーバーフローより先に検出される (全文字を先に検証) */
    {
        base44_error err;
        CHECK(base44_decode("99!", 3, NULL, &err) == NULL);
        CHECK(err.status == BASE44_ERR_INVALID_CHAR);
        CHECK(err.position == 2);
        CHECK(base44_decode("!99", 3, NULL, &err) == NULL);
        CHECK(err.status == BASE44_ERR_INVALID_CHAR);
        CHECK(err.position == 0);
    }

    /* オーバーフローの位置: 2 ブロック目で起きれば position は 3 */
    {
        base44_error err;
        char buf[256];
        CHECK(base44_decode("AAAt3c", 6, NULL, &err) == NULL);
        CHECK(err.status == BASE44_ERR_OVERFLOW);
        CHECK(err.position == 3);
        CHECK(err.digits == 3);
        CHECK(err.value == 65536);
        base44_error_format(&err, buf, sizeof(buf));
        CHECK(strstr(buf, "overflow") != NULL);
        CHECK(base44_decode("AAAF2", 5, NULL, &err) == NULL);
        CHECK(err.status == BASE44_ERR_OVERFLOW);
        CHECK(err.position == 3);
        CHECK(err.digits == 2);
        CHECK(err.value == 256);
        CHECK(base44_decode("t3cAAA", 6, NULL, &err) == NULL);
        CHECK(err.position == 0);
    }

    /* 状態文字列の smoke テスト */
    CHECK(strcmp(base44_status_string(BASE44_OK), "no error") == 0);
    CHECK(strcmp(base44_status_string(BASE44_ERR_OVERFLOW),
                 "block value out of range") == 0);
}

static void test_empty(void)
{
    base44_error err;
    size_t out_len = 999;
    char *enc = base44_encode((const uint8_t *)"", 0, &out_len, &err);
    uint8_t *dec;

    CHECK(enc != NULL);
    CHECK(out_len == 0);
    CHECK(enc[0] == '\0');
    free(enc);

    out_len = 999;
    dec = base44_decode("", 0, &out_len, &err);
    CHECK(dec != NULL); /* 長さ 0 でも NULL ではない (API 仕様) */
    CHECK(out_len == 0);
    free(dec);
}

int main(void)
{
    test_alphabet();
    test_encode_exhaustive_1byte();
    test_encode_exhaustive_2byte();
    test_decode_exhaustive_2char();
    test_decode_exhaustive_3char();
    test_concatenated_all_2byte();
    test_invalid_lengths();
    test_invalid_chars_and_error_details();
    test_empty();
    printf("core_test: all %llu checks passed\n", g_checks);
    return 0;
}
