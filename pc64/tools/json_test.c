/* Host test for studio_json.c - the AI client's JSON extractor.
 * Build:  gcc -O1 -o json_test tools/json_test.c apps/studio_json.c
 * Exercises the three providers' reply shapes + escapes + error paths. */
#include <stdio.h>
#include <string.h>
#include "../apps/studio_json.h"

static int pass, fail;

/* Compare emitted JSON to the exact bytes we meant to write.  A round trip
 * through the extractor proves the VALUES survive; only this proves the SHAPE
 * is what an API will accept, and shape is where "two user messages in a row"
 * lives. */
static void chkraw(const char *name, const char *want, const char *got)
{
    if (strcmp(want, got) == 0) { pass++; return; }
    printf("FAIL %-26s\n  want %s\n  got  %s\n", name, want, got);
    fail++;
}

static void chk(const char *name, const char *json, const char *path,
                const char *want)
{
    char out[4096];
    int n = jz_get_string(json, path, out, sizeof out);
    if (want == NULL) {
        if (n >= 0) { printf("FAIL %-22s expected miss, got '%s'\n", name, out); fail++; }
        else pass++;
        return;
    }
    if (n < 0) { printf("FAIL %-22s not found\n", name); fail++; return; }
    if (strcmp(out, want)) { printf("FAIL %-22s got '%s' want '%s'\n", name, out, want); fail++; return; }
    pass++;
}

int main(void)
{
    /* OpenAI chat completion */
    chk("openai", "{\"id\":\"x\",\"choices\":[{\"index\":0,\"message\":"
        "{\"role\":\"assistant\",\"content\":\"Hello there\"}}]}",
        "choices.0.message.content", "Hello there");
    /* Anthropic messages */
    chk("anthropic", "{\"id\":\"m\",\"type\":\"message\",\"content\":"
        "[{\"type\":\"text\",\"text\":\"Use uno_fill()\"}],\"model\":\"c\"}",
        "content.0.text", "Use uno_fill()");
    /* Gemini generateContent */
    chk("gemini", "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":"
        "\"const AppInterface\"}],\"role\":\"model\"}}]}",
        "candidates.0.content.parts.0.text", "const AppInterface");
    /* escapes: newline, quote, tab, backslash */
    chk("escapes", "{\"content\":\"line1\\nline2 \\\"q\\\" \\t \\\\ end\"}",
        "content", "line1\nline2 \"q\" \t \\ end");
    /* \u escape (BMP) */
    chk("uescape", "{\"content\":\"caf\\u00e9\"}", "content", "caf\xc3\xa9");
    /* code fence content with backticks + newlines */
    chk("codeblock",
        "{\"choices\":[{\"message\":{\"content\":\"```c\\nint x=1;\\n```\"}}]}",
        "choices.0.message.content", "```c\nint x=1;\n```");
    /* error shapes */
    chk("err_openai", "{\"error\":{\"message\":\"Invalid API key\",\"code\":401}}",
        "error.message", "Invalid API key");
    chk("err_anthropic", "{\"type\":\"error\",\"error\":{\"type\":\"auth\","
        "\"message\":\"x-api-key header is required\"}}",
        "error.message", "x-api-key header is required");
    /* nested arrays + object skipping (the walker must step past siblings) */
    chk("skip_siblings", "{\"a\":[1,2,3],\"b\":{\"c\":\"deep\"},\"choices\":"
        "[{\"message\":{\"content\":\"found\"}}]}",
        "choices.0.message.content", "found");
    chk("second_index", "{\"content\":[{\"text\":\"first\"},{\"text\":\"second\"}]}",
        "content.1.text", "second");
    /* misses */
    chk("miss_key", "{\"content\":\"x\"}", "nope", NULL);
    chk("miss_index", "{\"content\":[{\"text\":\"a\"}]}", "content.5.text", NULL);
    chk("empty", "", "content", NULL);

    /* emitter round-trip: escape then extract */
    {
        char buf[512]; int p = 0;
        jz_raw(buf, &p, sizeof buf, "{\"content\":");
        jz_str(buf, &p, sizeof buf, "he said \"hi\"\nand left\ttab");
        jz_raw(buf, &p, sizeof buf, "}");
        chk("emit_roundtrip", buf, "content", "he said \"hi\"\nand left\ttab");
    }

    /* One JSON string written in PIECES.  This is what merges two same-role
     * turns into a single message when the system notice between them is
     * dropped, so the escaping has to survive being applied per piece rather
     * than once over the whole thing. */
    {
        char buf[512]; int p = 0;
        jz_raw(buf, &p, sizeof buf, "{\"content\":");
        jz_open(buf, &p, sizeof buf);
        jz_more(buf, &p, sizeof buf, "first \"turn\"", 12);
        jz_more(buf, &p, sizeof buf, "\n\n", 2);
        jz_more(buf, &p, sizeof buf, "second\tturn", 11);
        jz_close(buf, &p, sizeof buf);
        jz_raw(buf, &p, sizeof buf, "}");
        chk("emit_pieces", buf, "content", "first \"turn\"\n\nsecond\tturn");
    }

    /* And that the pieced form agrees with the whole-string form, since
     * jz_strn is now defined in terms of them. */
    {
        char a[256], b[256]; int pa = 0, pb = 0;
        const char *s = "quote \" back \\ nl \n end";
        jz_str(a, &pa, sizeof a, s);
        jz_open(b, &pb, sizeof b);
        jz_more(b, &pb, sizeof b, s, (int)strlen(s));
        jz_close(b, &pb, sizeof b);
        if (pa == pb && strcmp(a, b) == 0) pass++;
        else { printf("FAIL %-22s '%s' vs '%s'\n", "pieces_match_whole", a, b);
               fail++; }
    }

    /* ---- the conversation, which is where the "it has no memory" bug was --
     * The client used to send ONE message while the pane drew a whole
     * transcript, so the assistant appeared to remember and did not. These
     * assert the three rules that turn a local transcript into a legal API
     * conversation. */
    {
        char b[1024]; int p = 0;
        jz_turn t[6];

        /* A plain two-round conversation. */
        t[0].role = JZ_USER;      t[0].text = "one";   t[0].len = 3;
        t[1].role = JZ_ASSISTANT; t[1].text = "two";   t[1].len = 3;
        t[2].role = JZ_USER;      t[2].text = "three"; t[2].len = 5;
        p = 0; jz_msgs(b, &p, sizeof b, t, 3, 0);
        chkraw("msgs_history",
            "{\"role\":\"user\",\"content\":\"one\"},"
            "{\"role\":\"assistant\",\"content\":\"two\"},"
            "{\"role\":\"user\",\"content\":\"three\"}", b);

        /* A FAILED request leaves a local notice between two user turns. The
         * notice must not be sent, and what is left must not be two user
         * messages in a row - the APIs reject that. */
        t[0].role = JZ_USER; t[0].text = "one";    t[0].len = 3;
        t[1].role = JZ_SKIP; t[1].text = "No API key."; t[1].len = 11;
        t[2].role = JZ_USER; t[2].text = "two";    t[2].len = 3;
        p = 0; jz_msgs(b, &p, sizeof b, t, 3, 0);
        chkraw("msgs_merge_after_notice",
            "{\"role\":\"user\",\"content\":\"one\\n\\ntwo\"}", b);

        /* The history has to OPEN with a user turn. */
        t[0].role = JZ_ASSISTANT; t[0].text = "hello"; t[0].len = 5;
        t[1].role = JZ_USER;      t[1].text = "hi";    t[1].len = 2;
        p = 0; jz_msgs(b, &p, sizeof b, t, 2, 0);
        chkraw("msgs_skip_leading_assistant",
            "{\"role\":\"user\",\"content\":\"hi\"}", b);

        /* Gemini's shape, and its different role word. */
        t[0].role = JZ_USER;      t[0].text = "q"; t[0].len = 1;
        t[1].role = JZ_ASSISTANT; t[1].text = "a"; t[1].len = 1;
        p = 0; jz_msgs(b, &p, sizeof b, t, 2, 1);
        chkraw("msgs_gemini",
            "{\"role\":\"user\",\"parts\":[{\"text\":\"q\"}]},"
            "{\"role\":\"model\",\"parts\":[{\"text\":\"a\"}]}", b);

        /* Nothing sendable at all must produce nothing, not a stray comma or
         * an unclosed object. */
        t[0].role = JZ_SKIP; t[0].text = "notice"; t[0].len = 6;
        p = 0; b[0] = 0; jz_msgs(b, &p, sizeof b, t, 1, 0);
        chkraw("msgs_all_skipped", "", b);

        /* And the merged text still round-trips through the extractor, which
         * is the real proof the piecewise escaping is right. */
        t[0].role = JZ_USER; t[0].text = "say \"hi\""; t[0].len = 8;
        t[1].role = JZ_SKIP; t[1].text = "x"; t[1].len = 1;
        t[2].role = JZ_USER; t[2].text = "now\tagain"; t[2].len = 9;
        p = 0;
        jz_raw(b, &p, sizeof b, "{\"messages\":[");
        jz_msgs(b, &p, sizeof b, t, 3, 0);
        jz_raw(b, &p, sizeof b, "]}");
        chk("msgs_roundtrip", b, "messages.0.content", "say \"hi\"\n\nnow\tagain");
    }

    printf("\njson_test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
