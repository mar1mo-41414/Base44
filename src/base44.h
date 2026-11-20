/*
 * Base44 コア変換ライブラリ
 *
 * 本プロジェクト独自の Base44 仕様:
 * - 固定文字セット 44 文字 (順序がそのまま数値 0..43 への割り当てになる)
 * - バイト列をビッグエンディアンで処理する
 * - 2 バイトブロック -> 44 進数 3 桁固定、末尾の 1 バイト -> 2 桁固定
 * - 44^2 = 1936 >= 256、44^3 = 85184 >= 65536 のため各ブロックは安全に収まる
 * - デコード時は 2 桁ブロック値 >= 256、3 桁ブロック値 >= 65536 を
 *   オーバーフローとして拒否する
 * - 有効な符号長は mod 3 が 0 か 2 のものだけ (mod 3 == 1 は不正桁数)
 *
 * このライブラリはバイト列ベースで動作し、入出力や UI には依存しない。
 * 他の C プログラムからリンクして使うことができる。
 */
#ifndef BASE44_H
#define BASE44_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定文字セット (44 文字 + 終端 NUL)。順序は変更しないこと */
extern const char base44_alphabet[45];

/* 変換結果の状態コード */
typedef enum {
    BASE44_OK = 0,             /* 成功 */
    BASE44_ERR_INVALID_LENGTH, /* 符号長が mod 3 == 1 (不正な桁数) */
    BASE44_ERR_INVALID_CHAR,   /* 文字セット外の文字が含まれている */
    BASE44_ERR_OVERFLOW,       /* ブロック値が表現範囲を超えている */
    BASE44_ERR_NO_MEMORY       /* メモリ確保に失敗した */
} base44_status;

/* エラーの詳細。成功時は status == BASE44_OK になる */
typedef struct {
    base44_status status;
    size_t position;    /* INVALID_CHAR: 文字の位置 / OVERFLOW: ブロック先頭位置 */
    size_t length;      /* INVALID_LENGTH: 入力の文字数 */
    int digits;         /* OVERFLOW: ブロックの桁数 (2 か 3) */
    unsigned int value; /* OVERFLOW: 範囲を超えたブロック値 */
    unsigned char ch;   /* INVALID_CHAR: 問題の文字 */
} base44_error;

/* status の短い説明文字列を返す */
const char *base44_status_string(base44_status status);

/* error の内容を人間可読な 1 行メッセージに整形する (buf は NUL 終端される) */
void base44_error_format(const base44_error *error, char *buf, size_t buflen);

/*
 * バイト列を Base44 文字列へエンコードする。
 * 戻り値は malloc された NUL 終端文字列で、呼び出し側が free すること。
 * 失敗時は NULL を返し、error が非 NULL なら詳細を設定する。
 * out_len が非 NULL なら文字列長 (NUL を除く) を格納する。
 */
char *base44_encode(const uint8_t *data, size_t len,
                    size_t *out_len, base44_error *error);

/*
 * Base44 文字列を厳密にデコードする。
 * 空白を含む文字セット外の文字はすべてエラーにする (補正はしない)。
 * 検証は全入力に対して行われ、失敗時は NULL を返し、error が非 NULL なら
 * 詳細を設定する。成功時は malloc されたバイト列を返し (長さ 0 の場合も
 * NULL ではない)、呼び出し側が free すること。
 * out_len が非 NULL なら復元したバイト数を格納する。
 */
uint8_t *base44_decode(const char *text, size_t len,
                       size_t *out_len, base44_error *error);

#ifdef __cplusplus
}
#endif

#endif /* BASE44_H */
