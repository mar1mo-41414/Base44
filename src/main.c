/*
 * base44 — GNU base64 / base32 と同形式の Base44 CLI
 *
 * Usage: base44 [OPTION]... [FILE]
 * 入力は全てメモリに読み込んでから処理する。
 * デコードは検証が全て通ってから一括で出力する。
 */
#define _GNU_SOURCE /* getopt_long を -std=c11 でも使うため */

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base44.h"

#define BASE44_VERSION "1.0.0"
#define BASE44_DEFAULT_WRAP 76

static const char BASE44_HELP[] =
    "Usage: base44 [OPTION]... [FILE]\n"
    "Base44 encode or decode FILE, or standard input, to standard output.\n"
    "\n"
    "With no FILE, or when FILE is -, read standard input.\n"
    "\n"
    "  -d, --decode          decode data\n"
    "  -i, --ignore-garbage  when decoding, ignore non-alphabet characters\n"
    "  -w, --wrap=COLS       wrap encoded lines after COLS character (default 76).\n"
    "                          Use 0 to disable line wrapping\n"
    "      --help            display this help and exit\n"
    "      --version         output version information and exit\n"
    "\n"
    "The data is encoded/decoded using this project's own Base44 alphabet:\n"
    "  " "ABCDEFGHJKMNPQRTUYabcdefghjkmnpqrtuy23456789" "\n"
    "This Base44 is not a standard; see README.md for the specification.\n";

/* エラーを "base44: <理由>" 形式で stderr に出して exit 1 する */
static void fail(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "base44: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* --wrap の値を検証する (10 進数字のみ、ULLONG_MAX まで受理し、
 * それを超える値はエラーにする。旧実装はオーバーフロー判定式が
 * 厳しすぎて ULLONG_MAX ちょうども弾いていたため、桁ごとの
 * 判定に直した) */
static unsigned long long parse_wrap(const char *value)
{
    unsigned long long n = 0;
    const char *p;

    if (value == NULL || *value == '\0')
        fail("invalid wrap size: '%s'", value ? value : "");
    for (p = value; *p != '\0'; p++) {
        unsigned long long digit;
        if (*p < '0' || *p > '9')
            fail("invalid wrap size: '%s'", value);
        digit = (unsigned long long)(*p - '0');
        if (n > (~0ULL - digit) / 10u)
            fail("invalid wrap size: '%s'", value);
        n = n * 10u + digit;
    }
    return n;
}

/* 入力全体を動的バッファに読み込む。戻り値は malloc された領域 */
static uint8_t *read_all(FILE *in, size_t *out_len)
{
    size_t cap = 8192, len = 0;
    uint8_t *buf = malloc(cap);

    if (buf == NULL)
        fail("out of memory");
    for (;;) {
        size_t n;
        if (len == cap) {
            uint8_t *nb;
            cap *= 2;
            nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                fail("out of memory");
            }
            buf = nb;
        }
        n = fread(buf + len, 1, cap - len, in);
        len += n;
        if (n == 0) {
            if (ferror(in)) {
                free(buf);
                fail("error reading input");
            }
            break; /* EOF */
        }
    }
    *out_len = len;
    return buf;
}

static int is_framing_whitespace(unsigned char c)
{
    /* JS 版と同じく ASCII 空白 6 種を折り返しフレーミングとして扱う */
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

static int in_alphabet(unsigned char c)
{
    if (c == '\0')
        return 0; /* strchr は終端 NUL にもマッチしてしまうため */
    return strchr(base44_alphabet, (int)c) != NULL;
}

enum {
    OPT_HELP = 1000,
    OPT_VERSION
};

static const struct option base44_long_options[] = {
    {"decode", no_argument, NULL, 'd'},
    {"ignore-garbage", no_argument, NULL, 'i'},
    {"wrap", required_argument, NULL, 'w'},
    {"help", no_argument, NULL, OPT_HELP},
    {"version", no_argument, NULL, OPT_VERSION},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv)
{
    int decode = 0;
    int ignore_garbage = 0;
    unsigned long long wrap = BASE44_DEFAULT_WRAP;
    const char *file = NULL;
    FILE *in;
    uint8_t *input;
    size_t input_len = 0;
    int c;

    /* 先頭の ':' で「引数不足」を '?' と区別し、getopt 自身の
     * エラー表示を止めて文言を自前で統一する */
    while ((c = getopt_long(argc, argv, ":diw:", base44_long_options,
                            NULL)) != -1) {
        switch (c) {
        case 'd':
            decode = 1;
            break;
        case 'i':
            ignore_garbage = 1;
            break;
        case 'w':
            wrap = parse_wrap(optarg);
            break;
        case OPT_HELP:
            fputs(BASE44_HELP, stdout);
            return 0;
        case OPT_VERSION:
            printf("base44 %s\n", BASE44_VERSION);
            return 0;
        case ':': {
            const char *arg = argv[optind - 1];
            if (arg != NULL && strncmp(arg, "--", 2) == 0)
                fail("option '--wrap' requires an argument");
            else
                fail("option requires an argument -- 'w'");
            break;
        }
        case '?':
        default:
            if (optopt != 0)
                fail("invalid option -- '%c'", optopt);
            else
                fail("unrecognized option '%s'", argv[optind - 1]);
            break;
        }
    }

    if (argc - optind >= 2)
        fail("extra operand '%s'", argv[optind + 1]);
    if (argc - optind == 1)
        file = argv[optind];

    if (file == NULL || strcmp(file, "-") == 0) {
        in = stdin;
    } else {
        in = fopen(file, "rb");
        if (in == NULL)
            fail("%s: %s", file, strerror(errno));
    }
    input = read_all(in, &input_len);
    if (in != stdin)
        fclose(in);

    if (!decode) {
        size_t encoded_len = 0;
        char *encoded =
            base44_encode(input, input_len, &encoded_len, NULL);
        if (encoded == NULL)
            fail("out of memory");
        if (input_len > 0) {
            if (wrap == 0) {
                /* GNU base64 -w 0 と同じ: 改行なしでそのまま出す */
                if (fwrite(encoded, 1, encoded_len, stdout) != encoded_len)
                    fail("write error");
            } else {
                size_t step = (size_t)wrap;
                size_t p;
                for (p = 0; p < encoded_len; p += step) {
                    size_t chunk = encoded_len - p < step
                                       ? encoded_len - p
                                       : step;
                    if (fwrite(encoded + p, 1, chunk, stdout) != chunk)
                        fail("write error");
                    if (fputc('\n', stdout) == EOF)
                        fail("write error");
                }
            }
        }
        free(encoded);
        free(input);
        if (fflush(stdout) != 0)
            fail("write error");
        return 0;
    }

    /* デコード: フレーミング/ゴミを処理してから厳密に検証する */
    {
        char *text = malloc(input_len + 1);
        size_t text_len = 0;
        size_t i;
        uint8_t *bytes;
        size_t bytes_len = 0;
        base44_error error;

        if (text == NULL)
            fail("out of memory");
        for (i = 0; i < input_len; i++) {
            unsigned char ch = input[i];
            if (ignore_garbage) {
                if (in_alphabet(ch))
                    text[text_len++] = (char)ch;
            } else {
                if (!is_framing_whitespace(ch))
                    text[text_len++] = (char)ch;
            }
        }
        text[text_len] = '\0';
        free(input);

        bytes = base44_decode(text, text_len, &bytes_len, &error);
        if (bytes == NULL) {
            char msg[256];
            base44_error_format(&error, msg, sizeof(msg));
            free(text);
            fail("%s", msg);
        }
        free(text);
        if (bytes_len > 0 &&
            fwrite(bytes, 1, bytes_len, stdout) != bytes_len)
            fail("write error");
        free(bytes);
        if (fflush(stdout) != 0)
            fail("write error");
        return 0;
    }
}
