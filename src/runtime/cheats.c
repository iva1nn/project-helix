/* GameShark cheats in DuckStation's .cht format, applied at the end of every frame.
 *   --cheats FILE.cht --cheat "part of a code name" (repeatable)
 * Code types: 30 (8-bit write), 80 (16-bit write), 10/11 (16-bit +/-), 20/21 (8-bit +/-),
 * D0-D3 (16-bit compare, next line), E0-E3 (8-bit compare, next line), C0 (rest of the code only if
 * equal), C2 (copy bytes). Writes into the game's code only work where the recompiler reads the
 * patched immediate from RAM (config "patchable_imm"). */
#include <stdlib.h>
#include <ctype.h>
#include "hw.h"

typedef struct { uint32_t a, v; } Line;
typedef struct { char name[160]; Line *lines; int n; int on; } Code;
static Code codes[256];
static int ncodes;

int cheats_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { LOG("cheats: cannot open %s\n", path); return 0; }
    char line[512];
    Code *cur = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '[') {
            if (ncodes >= 256) break;
            cur = &codes[ncodes++];
            memset(cur, 0, sizeof *cur);
            char *e = strchr(s, ']');
            snprintf(cur->name, sizeof cur->name, "%.*s", e ? (int)(e - s - 1) : (int)strlen(s + 1), s + 1);
            continue;
        }
        unsigned a, v;
        if (cur && isxdigit((unsigned char)s[0]) && sscanf(s, "%8x %4x", &a, &v) == 2 && strchr(s, '=') == 0) {
            cur->lines = realloc(cur->lines, sizeof(Line) * (size_t)(cur->n + 1));
            cur->lines[cur->n++] = (Line){a, v};
        }
    }
    fclose(f);
    LOG("cheats: %d codes in %s\n", ncodes, path);
    return ncodes;
}

void cheats_enable(const char *part) {
    int found = 0;
    for (int i = 0; i < ncodes; i++)
        if (strstr(codes[i].name, part)) { codes[i].on = 1; found++; LOG("cheats: on: %s\n", codes[i].name); }
    if (!found) LOG("cheats: no code matches \"%s\"\n", part);
}

static void run_code(const Code *c) {
    for (int i = 0; i < c->n; i++) {
        uint32_t t = c->lines[i].a >> 24, a = 0x80000000u | (c->lines[i].a & 0xFFFFFF), v = c->lines[i].v;
        switch (t) {
        case 0x30: mem_w8(a, v & 0xFF); break;
        case 0x80: mem_w16(a, v); break;
        case 0x10: mem_w16(a, mem_r16(a) + v); break;
        case 0x11: mem_w16(a, mem_r16(a) - v); break;
        case 0x20: mem_w8(a, mem_r8(a) + (v & 0xFF)); break;
        case 0x21: mem_w8(a, mem_r8(a) - (v & 0xFF)); break;
        case 0xD0: if (mem_r16(a) != v) i++; break;
        case 0xD1: if (mem_r16(a) == v) i++; break;
        case 0xD2: if (mem_r16(a) >= v) i++; break;
        case 0xD3: if (mem_r16(a) <= v) i++; break;
        case 0xE0: if (mem_r8(a) != (v & 0xFF)) i++; break;
        case 0xE1: if (mem_r8(a) == (v & 0xFF)) i++; break;
        case 0xE2: if (mem_r8(a) >= (v & 0xFF)) i++; break;
        case 0xE3: if (mem_r8(a) <= (v & 0xFF)) i++; break;
        case 0xC0: if (mem_r16(a) != v) return; break;
        case 0xC2:
            if (i + 1 < c->n) {
                uint32_t dst = 0x80000000u | (c->lines[i + 1].a & 0xFFFFFF);
                for (uint32_t k = 0; k < v; k++) mem_w8(dst + k, mem_r8(a + k));
                i++;
            }
            break;
        default: {
            static int warned[256];
            if (!warned[t]++) LOG("cheats: code type %02X not supported\n", t);
        }
        }
    }
}

void cheats_vblank(void) {
    for (int i = 0; i < ncodes; i++)
        if (codes[i].on) run_code(&codes[i]);
}
