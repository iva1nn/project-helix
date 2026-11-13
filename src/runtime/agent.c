/* Playtest agent channel (tools/playtest/): `--agent` (headless) makes the port a step-by-step environment driven
 * over stdin / stdout by a script, one text line per command, one line per answer:
 *
 *   step N BTN LX LY     run N frames with that pad (BTN hex mask, sticks 0..255), then answer
 *                        "ok FRAME DISC ctl=AGE actor=A pos=x,y,z seeks=a,b,.. obs=HEX" (ctl: vblanks since the game
 *                        last updated the player, -1 never; seeks: new CD Setloc file sectors since the last answer,
 *                        obs: the bytes of the watched ranges)
 *   obs A:L,A:L,...      the RAM ranges every step answer carries (hex address, decimal length)
 *   peek A L             "ok HEX"
 *   poke A HEX           write bytes
 *   hold A HEX           those bytes written at every vblank (cheat flags); health 1: the player's health kept full
 *   save PATH            save state at the next state point ("ok FRAME" / "err ...", gives up after 300 frames)
 *   load PATH            load a state at the next vblank ("ok FRAME" / "err ...")
 *   shot PATH            the software frame as PNG
 *   ram PATH             the 2 MB RAM
 *   quit
 * A command that never answers is a hang of the game (no vblank): the script's timeout sees it. */
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "state.h"

int agent_on;
uint16_t agent_buttons;
uint8_t agent_lx = 0x80, agent_ly = 0x80;
extern int state_agent_saves, state_agent_loads;
void state_request_save(const char *path);
void state_request_load(const char *path);
void frontend_shot(const char *path);

static int64_t run_left;
static enum { IDLE, RUN, SAVE, LOAD } mode;
static int base_count;
static int64_t wait_since;
static struct { uint32_t a; int n; } obs[32];
static int nobs;
static uint32_t seeks[256];
static int nseeks;

void agent_cd_seek(uint32_t sector) {
    if (!agent_on) return;
    for (int i = 0; i < nseeks; i++) if (seeks[i] == sector) return;
    if (nseeks < 256) seeks[nseeks++] = sector;
}

/* the strings the game's text routine drew lately (prompts such as USE / TAKE, the selected item, menu and puzzle
 * texts, subtitles): subs.c's hook hands every call over */
static struct { uint64_t frame; int font, x, y; char s[64]; } texts[32];
static int text_pos;

void agent_text(uint32_t s, int font, int x, int y) {
    if (!agent_on) return;
    char t[64];
    int n = 0;
    for (int c; n < 63 && (c = mem_r8(s + (uint32_t)n)) != 0; n++) t[n] = (c < 32 || c > 126 || c == '|') ? '?' : (char)c;
    while (n && t[n - 1] == ' ') n--;
    t[n] = 0;
    if (!n) return;
    for (int i = 0; i < 32; i++)
        if (texts[i].frame && !strcmp(texts[i].s, t) && texts[i].y == y) { texts[i].frame = frame_count + 1; return; }
    int k = text_pos++ & 31;
    texts[k].frame = frame_count + 1; texts[k].font = font; texts[k].x = x; texts[k].y = y;
    memcpy(texts[k].s, t, sizeof t);
}

static void answer_step(void) {
    int controls_player_info(uint32_t *actor, int32_t pos[3]);
    uint32_t actor; int32_t pos[3];
    int age = controls_player_info(&actor, pos);
    printf("ok %llu %d ctl=%d actor=%08X pos=%d,%d,%d seeks=", (unsigned long long)frame_count, disc_current + 1, age,
           actor, pos[0], pos[1], pos[2]);
    for (int i = 0; i < nseeks; i++) printf("%s%u", i ? "," : "", seeks[i]);
    nseeks = 0;
    printf(" obs=");
    for (int i = 0; i < nobs; i++)
        for (int k = 0; k < obs[i].n; k++) printf("%02X", mem_r8(obs[i].a + (uint32_t)k));
    printf(" text=");
    for (int i = 0, k = 0; i < 32; i++)
        if (texts[i].frame && frame_count + 1 - texts[i].frame <= 8)
            printf("%s%d,%d:%s", k++ ? "|" : "", texts[i].font, texts[i].y, texts[i].s);
    printf("\n");
    fflush(stdout);
}

static struct { uint32_t a; int n; uint8_t v[16]; } holds[8];
static int nholds, keep_health;

void agent_vblank(void) {
    if (!agent_on) return;
    for (int i = 0; i < nholds; i++)
        for (int k = 0; k < holds[i].n; k++) mem_w8(holds[i].a + (uint32_t)k, holds[i].v[k]);
    if (keep_health) { /* the player's health (actor +0x68 in both games) stays full */
        int controls_player_info(uint32_t *actor, int32_t pos[3]);
        uint32_t actor; int32_t pos[3];
        controls_player_info(&actor, pos);
        if (actor) mem_w8(actor + 0x68, 0x7F);
    }
    if (mode == RUN) {
        if (--run_left > 0) return;
        answer_step();
    } else if (mode == SAVE) {
        if (state_agent_saves == base_count) {
            if ((int64_t)frame_count - wait_since < 300) return;
            printf("err no state point\n");
        } else printf("ok %llu\n", (unsigned long long)frame_count);
        fflush(stdout);
    } else if (mode == LOAD) {
        if (state_agent_loads == base_count) printf("err state not loaded\n");
        else printf("ok %llu\n", (unsigned long long)frame_count);
        fflush(stdout);
    }
    mode = IDLE;
    static char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        unsigned a, b, lx, ly;
        long long cnt;
        int len;
        if (sscanf(line, "step %lld %x %u %u", &cnt, &b, &lx, &ly) == 4) {
            agent_buttons = (uint16_t)b; agent_lx = (uint8_t)lx; agent_ly = (uint8_t)ly;
            run_left = cnt < 1 ? 1 : cnt;
            mode = RUN;
            return;
        } else if (!strncmp(line, "obs", 3)) {
            nobs = 0;
            for (char *p = line + 3; *p && nobs < 32;) {
                while (*p == ' ' || *p == ',') p++;
                if (sscanf(p, "%x:%d", &a, &len) != 2) break;
                obs[nobs].a = a; obs[nobs].n = len; nobs++;
                while (*p && *p != ',') p++;
            }
            printf("ok %d\n", nobs);
        } else if (sscanf(line, "peek %x %d", &a, &len) == 2) {
            printf("ok ");
            for (int k = 0; k < len; k++) printf("%02X", mem_r8(a + (uint32_t)k));
            printf("\n");
        } else if (sscanf(line, "poke %x %n", &a, &len) == 1) {
            const char *h = line + len;
            for (; h[0] && h[1]; h += 2, a++) {
                char t[3] = {h[0], h[1], 0};
                mem_w8(a, (uint8_t)strtoul(t, 0, 16));
            }
            printf("ok\n");
        } else if (sscanf(line, "hold %x %n", &a, &len) == 1) { /* bytes written at every vblank (cheat flags) */
            if (nholds < 8) {
                const char *h = line + len;
                holds[nholds].a = a; holds[nholds].n = 0;
                for (; h[0] && h[1] && holds[nholds].n < 16; h += 2) {
                    char t[3] = {h[0], h[1], 0};
                    holds[nholds].v[holds[nholds].n++] = (uint8_t)strtoul(t, 0, 16);
                }
                nholds++;
            }
            printf("ok\n");
        } else if (sscanf(line, "health %d", &len) == 1) {
            keep_health = len;
            printf("ok\n");
        } else if (!strncmp(line, "save ", 5)) {
            base_count = state_agent_saves; wait_since = (int64_t)frame_count;
            state_request_save(line + 5);
            mode = SAVE;
            return;
        } else if (!strncmp(line, "load ", 5)) {
            base_count = state_agent_loads;
            state_request_load(line + 5);
            mode = LOAD;
            return;
        } else if (!strncmp(line, "shot ", 5)) {
            frontend_shot(line + 5);
            printf("ok\n");
        } else if (!strncmp(line, "ram ", 4)) {
            FILE *f = fopen(line + 4, "wb");
            if (f) { fwrite(psx_ram, 1, PSX_RAM_SIZE, f); fclose(f); }
            printf(f ? "ok\n" : "err cannot write\n");
        } else if (!strcmp(line, "quit")) {
            exit(0);
        } else printf("err unknown command\n");
        fflush(stdout);
    }
    exit(0); /* the script went away */
}

/* FE2's other text routines (the HUD's: prompts, the item bar) besides the centred one subs.c hooks: the first
 * argument register that points at printable text is taken as the string */
static int fe2_text_probe(void) {
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("FE_AGENT_TXT") != 0;
    for (int r = 4; r <= 7; r++) {
        uint32_t a = cpu.r[r];
        if ((a & 0xFFE00000u) != 0x80000000u) continue;
        int n = 0, c;
        while (n < 40 && (c = mem_r8(a + (uint32_t)n)) >= 32 && c < 127) n++;
        if (n >= 2 && mem_r8(a + (uint32_t)n) == 0) {
            if (dbg) LOG("agent text: ra %08X r%d \"%s\" a0 %X a1 %X a2 %X a3 %X\n", R31, r, (char *)psx_ptr(a), R4, R5, R6, R7);
            agent_text(a, (int)(R4 & 0xFF), -1, (int)(int16_t)R7);
            break;
        }
    }
    return 0;
}

void agent_init(void) {
    if (!agent_on || strcmp(FE_GAME, "fe2")) return;
    static const uint32_t fns[] = {0x8004AA74u, 0x8004A7DCu, 0x8004A9E4u, 0x8004AC48u, 0x8004AC74u};
    for (int i = 0; i < 5; i++)
        if (hook_index(fns[i]) >= 0) hook_register(fns[i], fe2_text_probe);
}
