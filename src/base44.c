#include "base44.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char base44_alphabet[45] =
    "ABCDEFGHJKMNPQRTUYabcdefghjkmnpqrtuy23456789";

#define BASE44_BASE 44u
#define BASE44_BASE2 1936u /* 44^2 */
#define BASE44_LIMIT_2DIGIT 256u
#define BASE44_LIMIT_3DIGIT 65536u

/* 文字 -> 数値 (0..43) の逆引き表。文字セット外は -1 */
static signed char base44_value_table[256];
static int base44_value_table_ready = 0;

static void base44_build_value_table(void)
{
    int i;
    if (base44_value_table_ready)
        return;
    for (i = 0; i < 256; i++)
        base44_value_table[i] = -1;
    for (i = 0; i < 44; i++)
        base44_value_table[(unsigned char)base44_alphabet[i]] = (signed char)i;
    base44_value_table_ready = 1;
}

static void base44_set_error(base44_error *error, base44_status status)
{
    if (error == NULL)
        return;
    memset(error, 0, sizeof(*error));
    error->status = status;
}

const char *base44_status_string(base44_status status)
{
    switch (status) {
    case BASE44_OK:
        return "no error";
    case BASE44_ERR_INVALID_LENGTH:
        return "invalid length";
    case BASE44_ERR_INVALID_CHAR:
        return "invalid character";
    case BASE44_ERR_OVERFLOW:
        return "block value out of range";
    case BASE44_ERR_NO_MEMORY:
        return "out of memory";
    }
    return "unknown error";
}

void base44_error_format(const base44_error *error, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0)
        return;
    if (error == NULL) {
        snprintf(buf, buflen, "no error");
        return;
    }
    switch (error->status) {
    case BASE44_OK:
        snprintf(buf, buflen, "no error");
        break;
    case BASE44_ERR_INVALID_LENGTH:
        snprintf(buf, buflen,
                 "invalid length %zu (mod 3 == 1 is not a valid Base44 length)",
                 error->length);
        break;
    case BASE44_ERR_INVALID_CHAR:
        if (isprint(error->ch)) {
            snprintf(buf, buflen,
                     "invalid character '%c' (U+%04X) at position %zu: "
                     "not in the Base44 alphabet",
                     error->ch, error->ch, error->position);
        } else {
            snprintf(buf, buflen,
                     "invalid character (U+%04X) at position %zu: "
                     "not in the Base44 alphabet",
                     error->ch, error->position);
        }
        break;
    case BASE44_ERR_OVERFLOW:
        snprintf(buf, buflen,
                 "overflow: %d-digit block at position %zu has value %u, "
                 "which is >= %u",
                 error->digits, error->position, error->value,
                 error->digits == 2 ? BASE44_LIMIT_2DIGIT
                                     : BASE44_LIMIT_3DIGIT);
        break;
    case BASE44_ERR_NO_MEMORY:
        snprintf(buf, buflen, "out of memory");
        break;
    }
}

char *base44_encode(const uint8_t *data, size_t len,
                    size_t *out_len, base44_error *error)
{
    size_t out_size = (len / 2) * 3 + (len % 2 ? 2 : 0);
    size_t i, o = 0;
    char *out = malloc(out_size + 1);

    if (out == NULL) {
        base44_set_error(error, BASE44_ERR_NO_MEMORY);
        return NULL;
    }
    for (i = 0; i < len; i += 2) {
        if (i + 1 < len) {
            unsigned int v = ((unsigned int)data[i] << 8) | data[i + 1];
            out[o++] = base44_alphabet[v / BASE44_BASE2];
            out[o++] = base44_alphabet[(v / BASE44_BASE) % BASE44_BASE];
            out[o++] = base44_alphabet[v % BASE44_BASE];
        } else {
            unsigned int v = data[i];
            out[o++] = base44_alphabet[v / BASE44_BASE];
            out[o++] = base44_alphabet[v % BASE44_BASE];
        }
    }
    out[o] = '\0';
    base44_set_error(error, BASE44_OK);
    if (out_len != NULL)
        *out_len = o;
    return out;
}

uint8_t *base44_decode(const char *text, size_t len,
                       size_t *out_len, base44_error *error)
{
    size_t i, pos, o = 0;
    size_t out_size;
    uint8_t *values;
    uint8_t *out;

    base44_build_value_table();

    if (len % 3 == 1) {
        base44_set_error(error, BASE44_ERR_INVALID_LENGTH);
        if (error != NULL)
            error->length = len;
        return NULL;
    }

    /* まず全文字を検証して数値化する (JS 版と同様、文字エラーを
     * オーバーフローより先に検出するため) */
    values = malloc(len > 0 ? len : 1);
    if (values == NULL) {
        base44_set_error(error, BASE44_ERR_NO_MEMORY);
        return NULL;
    }
    for (i = 0; i < len; i++) {
        signed char v = base44_value_table[(unsigned char)text[i]];
        if (v < 0) {
            base44_set_error(error, BASE44_ERR_INVALID_CHAR);
            if (error != NULL) {
                error->position = i;
                error->ch = (unsigned char)text[i];
            }
            free(values);
            return NULL;
        }
        values[i] = (uint8_t)v;
    }

    out_size = (len / 3) * 2 + (len % 3 == 2 ? 1 : 0);
    out = malloc(out_size > 0 ? out_size : 1);
    if (out == NULL) {
        base44_set_error(error, BASE44_ERR_NO_MEMORY);
        free(values);
        return NULL;
    }

    pos = 0;
    while (pos < len) {
        size_t remaining = len - pos;
        if (remaining == 2) {
            unsigned int v =
                (unsigned int)values[pos] * BASE44_BASE + values[pos + 1];
            if (v >= BASE44_LIMIT_2DIGIT) {
                base44_set_error(error, BASE44_ERR_OVERFLOW);
                if (error != NULL) {
                    error->position = pos;
                    error->digits = 2;
                    error->value = v;
                }
                free(values);
                free(out);
                return NULL;
            }
            out[o++] = (uint8_t)v;
            pos += 2;
        } else {
            unsigned int v = (unsigned int)values[pos] * BASE44_BASE2 +
                             (unsigned int)values[pos + 1] * BASE44_BASE +
                             values[pos + 2];
            if (v >= BASE44_LIMIT_3DIGIT) {
                base44_set_error(error, BASE44_ERR_OVERFLOW);
                if (error != NULL) {
                    error->position = pos;
                    error->digits = 3;
                    error->value = v;
                }
                free(values);
                free(out);
                return NULL;
            }
            out[o++] = (uint8_t)(v >> 8);
            out[o++] = (uint8_t)(v & 0xffu);
            pos += 3;
        }
    }

    free(values);
    base44_set_error(error, BASE44_OK);
    if (out_len != NULL)
        *out_len = o;
    return out;
}
