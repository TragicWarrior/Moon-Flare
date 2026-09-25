#ifndef MF_TB_H
#define MF_TB_H

/*
 * Textbelt SMS (https://textbelt.com): the plugin's plain data handling,
 * kept apart so it can be unit-tested.
 *
 *   POST {base}/text          phone, message, key (form-encoded)
 *        -> {"success":true,"quotaRemaining":40,"textId":"12345"}
 *        -> {"success":false,"quotaRemaining":0,"error":"Out of quota"}
 *   GET  {base}/quota/{key}   -> {"success":true,"quotaRemaining":98}
 */

#include <stddef.h>

#define TB_PHONE_MAX 20         /* "+", up to 15 digits, NUL */
#define TB_MSG_MAX   320        /* two SMS segments */

/* A phone number as Textbelt takes it: a 10-digit US/Canada number, or
 * "+" and 8-15 digits (E.164).  Spaces, dashes, dots and parentheses are
 * dropped, and "1" plus 10 digits becomes "+1...".  0, or -1 if in is
 * not a phone number. */
int  tb_phone_norm(const char *in, char *out, size_t cap);

/* The last four digits, for display: "***1234". */
void tb_phone_mask(const char *phone, char *out, size_t cap);

/* Numbers from a list separated by commas or semicolons (a number may
 * contain spaces: "(555) 883-8530"), each normalized, at most max of
 * them.  The count, or -1 with the entry that is not a number in bad. */
int  tb_recipients(const char *list, char (*out)[TB_PHONE_MAX], int max,
                   char *bad, size_t badcap);

/* s percent-encoded (everything but A-Z a-z 0-9 - . _ ~), malloc'd. */
char *tb_escape(const char *s);

/* The form body of a send, "phone=..&message=..&key=..", malloc'd. */
char *tb_form(const char *phone, const char *message, const char *key);

typedef struct {
    int  success;
    long quota;                 /* quotaRemaining; -1 when absent */
    char text_id[32];
    char error[96];
} tb_reply_t;

/* A /text or /quota reply.  -1 if body is not one. */
int  tb_parse_reply(const char *body, tb_reply_t *r);

/* A notification's text: "title: message" (or just the message), with
 * control characters as spaces, runs of space collapsed, trimmed, and cut
 * to TB_MSG_MAX with "...".  "" when the message itself is blank. */
void tb_message(const char *title, const char *message, char *out, size_t cap);

#endif
