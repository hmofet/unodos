/* Host test for studio_json.c - the AI client's JSON extractor.
 * Build:  gcc -O1 -o json_test tools/json_test.c apps/studio_json.c
 * Exercises the three providers' reply shapes + escapes + error paths. */
#include <stdio.h>
#include <string.h>
#include "../apps/studio_json.h"

static int pass, fail;
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

    printf("\njson_test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
