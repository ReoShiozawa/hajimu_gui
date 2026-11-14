#include "../src/hjp_grapheme.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_boundaries(const char *text, int length, const int *expected, int count) {
    int offset = 0;
    assert(count > 0 && expected[0] == 0 && expected[count - 1] == length);
    for (int i = 1; i < count; i++) {
        offset = hjp_grapheme_step(text, length, offset);
        assert(offset == expected[i]);
    }
    for (int position = 0; position <= length; position++) {
        int floor = 0, ceil = length, prev = 0, next = length;
        for (int i = 0; i < count; i++) {
            if (expected[i] <= position) floor = expected[i];
            if (expected[i] < position) prev = expected[i];
        }
        for (int i = count - 1; i >= 0; i--) {
            if (expected[i] >= position) ceil = expected[i];
            if (expected[i] > position) next = expected[i];
        }
        assert(hjp_grapheme_floor(text, length, position) == floor);
        assert(hjp_grapheme_ceil(text, length, position) == ceil);
        assert(hjp_grapheme_prev(text, length, position) == prev);
        assert(hjp_grapheme_next(text, length, position) == next);
    }
    assert(hjp_grapheme_floor(text, length, -100) == 0);
    assert(hjp_grapheme_ceil(text, length, -100) == 0);
    assert(hjp_grapheme_next(text, length, length + 100) == length);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!strcmp(utf8proc_version(), "2.12.0") && !strcmp(utf8proc_unicode_version(), "18.0.0"));
    FILE *file = fopen(argv[1], "r"); assert(file);
    char line[8192]; unsigned cases = 0;
    while (fgets(line, sizeof(line), file)) {
        assert(strchr(line, '\n') || feof(file));
        char *comment = strchr(line, '#'); if (comment) *comment = '\0';
        char text[4096]; int expected[1024], count = 0, length = 0;
        for (char *token = strtok(line, " \t\r\n"); token; token = strtok(NULL, " \t\r\n")) {
            if (!strcmp(token, "\xc3\xb7")) {
                assert(count < (int)(sizeof(expected) / sizeof(expected[0])));
                expected[count++] = length;
            } else if (strcmp(token, "\xc3\x97")) {
                char *end;
                unsigned long codepoint = strtoul(token, &end, 16);
                assert(*end == '\0' && codepoint <= 0x10ffff && length < (int)sizeof(text) - 4);
                int size = (int)utf8proc_encode_char((utf8proc_int32_t)codepoint, (utf8proc_uint8_t *)text + length);
                assert(size > 0); length += size;
            }
        }
        if (count) { check_boundaries(text, length, expected, count); cases++; }
    }
    assert(!ferror(file) && cases > 500); fclose(file);
    const int empty[] = {0}; check_boundaries("", 0, empty, 1);
    /* NULに依存せず、途中切れ/過長/孤立した継続バイトでも範囲内で前進する。 */
    const char invalid[] = {(char)0xf0, (char)0x9f, (char)0xc0, (char)0xaf, 'A'};
    const int invalid_boundaries[] = {0, 1, 2, 3, 4, 5};
    check_boundaries(invalid, sizeof(invalid), invalid_boundaries, 6);
    printf("grapheme passed: Unicode 18.0.0 %u cases, all byte positions, invalid UTF-8\n", cases);
}
