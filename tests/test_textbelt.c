/*
 * Textbelt helpers: phone numbers, recipient lists, form encoding, replies
 * and message text (src/textbelt/tb.c).
 */

#include "tb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

static void norm_is(const char *in, const char *want)
{
    char out[TB_PHONE_MAX], msg[96];
    int rc = tb_phone_norm(in, out, sizeof(out));

    snprintf(msg, sizeof(msg), "phone \"%s\" -> %s", in, want ? want : "invalid");
    if (want)
        CHECK(rc == 0 && strcmp(out, want) == 0, msg);
    else
        CHECK(rc != 0, msg);
}

static void test_phone(void)
{
    char out[16];

    norm_is("5558838530", "5558838530");
    norm_is("(555) 883-8530", "5558838530");
    norm_is(" 555.883.8530 ", "5558838530");
    norm_is("15558838530", "+15558838530");
    norm_is("1-555-883-8530", "+15558838530");
    norm_is("+1 555 883 8530", "+15558838530");
    norm_is("+33509758351", "+33509758351");
    norm_is("+44 7700 900123", "+447700900123");
    norm_is("", NULL);
    norm_is("12345", NULL);
    norm_is("+1234567", NULL);                  /* too short for E.164 */
    norm_is("+0123456789", NULL);
    norm_is("+1234567890123456", NULL);         /* 16 digits */
    norm_is("555-883-853a", NULL);
    norm_is("25558838530", NULL);               /* 11 digits, not +1 */
    tb_phone_mask("5558838530", out, sizeof(out));
    CHECK(strcmp(out, "***8530") == 0, "mask shows the last four");
    tb_phone_mask("12", out, sizeof(out));
    CHECK(strcmp(out, "***") == 0, "mask of a short value");
}

static void test_recipients(void)
{
    char list[4][TB_PHONE_MAX], bad[64];
    int n;

    n = tb_recipients("555-883-8530, +44 7700 900123; (555) 111-2222", list, 4,
                      bad, sizeof(bad));
    CHECK(n == 3 && strcmp(list[0], "5558838530") == 0 &&
          strcmp(list[1], "+447700900123") == 0 &&
          strcmp(list[2], "5551112222") == 0, "three recipients");
    n = tb_recipients("555-883-8530, abc", list, 4, bad, sizeof(bad));
    CHECK(n == -1 && strcmp(bad, "abc") == 0, "a bad entry is named");
    CHECK(tb_recipients("", list, 4, bad, sizeof(bad)) == 0, "empty list");
    CHECK(tb_recipients(" , ; ", list, 4, bad, sizeof(bad)) == 0, "blank entries");
    n = tb_recipients("5550000001,5550000002,5550000003", list, 2, bad, sizeof(bad));
    CHECK(n == -1 && strcmp(bad, "too many numbers") == 0, "too many");
}

static void test_encoding(void)
{
    char *s = tb_escape("a+b c/\xc3\xa9~._-");

    CHECK(s && strcmp(s, "a%2Bb%20c%2F%C3%A9~._-") == 0, "escape");
    free(s);
    s = tb_form("+15558838530", "Hi & bye", "abc_test");
    CHECK(s && strcmp(s, "phone=%2B15558838530&message=Hi%20%26%20bye&key=abc_test")
          == 0, "form body");
    free(s);
}

static void test_replies(void)
{
    tb_reply_t r;

    CHECK(tb_parse_reply("{\"success\": true, \"quotaRemaining\": 40, "
                         "\"textId\": \"12345\"}", &r) == 0 &&
          r.success && r.quota == 40 && strcmp(r.text_id, "12345") == 0 &&
          r.error[0] == '\0', "success reply");
    CHECK(tb_parse_reply("{\"success\":true,\"quotaRemaining\":39,\"textId\":678}",
                         &r) == 0 && strcmp(r.text_id, "678") == 0,
          "a numeric textId");
    CHECK(tb_parse_reply("{\"success\": false, \"quotaRemaining\": 0, "
                         "\"error\": \"Out of quota\"}", &r) == 0 &&
          !r.success && r.quota == 0 && strcmp(r.error, "Out of quota") == 0,
          "failure reply");
    CHECK(tb_parse_reply("{\"success\": false, \"error\": \"Incomplete request\"}",
                         &r) == 0 && r.quota == -1, "no quota in the reply");
    CHECK(tb_parse_reply("{\"success\":true,\"quotaRemaining\":98}", &r) == 0 &&
          r.success && r.quota == 98 && r.text_id[0] == '\0', "quota reply");
    CHECK(tb_parse_reply("<html>502</html>", &r) == -1, "not JSON");
    CHECK(tb_parse_reply("{\"ok\":1}", &r) == -1, "not a Textbelt reply");
    CHECK(tb_parse_reply(NULL, &r) == -1, "no body");
}

static void test_message(void)
{
    char out[TB_MSG_MAX + 1], big[1000];
    size_t i;

    tb_message("Moon Flare", "Battery low: 19%", out, sizeof(out));
    CHECK(strcmp(out, "Moon Flare: Battery low: 19%") == 0, "title and message");
    tb_message(NULL, "  two\n lines\t here  ", out, sizeof(out));
    CHECK(strcmp(out, "two lines here") == 0, "controls and runs of space");
    tb_message("", "just this", out, sizeof(out));
    CHECK(strcmp(out, "just this") == 0, "empty title");
    tb_message("t", "   ", out, sizeof(out));
    CHECK(out[0] == '\0', "a title alone is not a message");
    tb_message(NULL, " \n ", out, sizeof(out));
    CHECK(out[0] == '\0', "blank message");
    for (i = 0; i < sizeof(big) - 1; i++)
        big[i] = (char)('a' + i % 26);
    big[sizeof(big) - 1] = '\0';
    tb_message(NULL, big, out, sizeof(out));
    CHECK(strlen(out) == TB_MSG_MAX && strcmp(out + TB_MSG_MAX - 3, "...") == 0,
          "cut to 320 with an ellipsis");
    /* 2-byte UTF-8 characters: the cut never splits one. */
    for (i = 0; i + 2 < sizeof(big); i += 2)
    {
        big[i] = (char)0xc3;
        big[i + 1] = (char)0xa9;
    }
    big[i] = '\0';
    tb_message(NULL, big, out, sizeof(out));
    CHECK(strlen(out) <= TB_MSG_MAX && (strlen(out) - 3) % 2 == 0,
          "UTF-8 kept whole");
}

int main(void)
{
    test_phone();
    test_recipients();
    test_encoding();
    test_replies();
    test_message();
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_textbelt: ok\n");
    return 0;
}
