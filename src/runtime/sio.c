/* SIO0: controller/memory-card serial port with a DualShock (analog controller) on port 1, at register
 * level, because the game's pad library drives the port itself and uses the ACK interrupt.
 * Memory cards are handled at BIOS level (mcd.c), so card commands on the port get no answer. */
#include "hw.h"

static struct {
    uint16_t stat, mode, ctrl, baud;
    uint8_t rx[8]; int rx_n;
    int dev;           /* 0 none, 1 pad, 2 memory card */
    int pos;           /* byte index within the current command */
    int64_t ack_time;  /* pending ACK interrupt */
    int ack_level_until;
} s;

#define CTRL_TXEN 0x0001
#define CTRL_DTR 0x0002
#define CTRL_ACKIRQ 0x1000
#define CTRL_PORT2 0x2000

/* DualShock state (psx-spx "Controllers - Configuration Commands") */
static struct {
    int analog;          /* analog mode (ID 0x73) vs digital (0x41) */
    int locked;          /* mode locked by the game: the ANALOG button does nothing */
    int config;          /* configuration mode (ID 0xF3) */
    uint8_t cmd;
    uint8_t params[8];
    uint8_t rumble_map[6];
    int pending_config;  /* -1 none, 0 leave, 1 enter */
    int logged[256];
} ds = {0, 0, 0, 0, {0}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, -1, {0}};

static int data_len(void) { return (ds.config || ds.analog) ? 6 : 2; }

static uint8_t poll_byte(int i) {
    uint16_t inv = (uint16_t)~pad_buttons;
    switch (i) {
    case 0: return (uint8_t)inv;
    case 1: return (uint8_t)(inv >> 8);
    default: return pad_axes[i - 2]; /* right X, right Y, left X, left Y */
    }
}

static void end_of_command(void) {
    if (ds.cmd == 0x42 || (ds.cmd == 0x43 && !ds.config)) {
        /* vibration from the poll parameters, through the 0x4D mapping */
        int small = 0, large = 0;
        for (int i = 0; i < 6; i++) {
            if (ds.rumble_map[i] == 0x00) small = (ds.params[i] & 1);
            if (ds.rumble_map[i] == 0x01) large = ds.params[i];
        }
        if (ds.analog || ds.config) input_rumble(small, large);
    }
    if (ds.pending_config >= 0) { ds.config = ds.pending_config; ds.pending_config = -1; }
}

static uint8_t pad_byte(uint8_t tx, int *ack) {
    *ack = 1;
    if (s.pos == 0) return 0xFF; /* reply to 0x01 */
    if (s.pos == 1) {
        ds.cmd = tx;
        memset(ds.params, 0, sizeof ds.params);
        if (!ds.logged[tx]) { ds.logged[tx] = 1; LOG("pad: command %02X\n", tx); }
        int valid = tx == 0x42 || tx == 0x43 || (ds.config && tx >= 0x44 && tx <= 0x4F);
        if (!valid) { *ack = 0; s.dev = 0; return 0xFF; }
        return ds.config ? 0xF3 : ds.analog ? 0x73 : 0x41;
    }
    if (s.pos == 2) return 0x5A;
    int i = s.pos - 3, n = data_len();
    if (i >= n) { *ack = 0; return 0xFF; }
    if (i < 8) ds.params[i] = tx;
    if (i == n - 1) *ack = 0; /* last byte: no ACK */
    uint8_t r = 0;
    switch (ds.cmd) {
    case 0x42:
        r = poll_byte(i);
        break;
    case 0x43:
        r = ds.config ? 0x00 : poll_byte(i);
        if (i == 0) ds.pending_config = tx == 1 ? 1 : 0;
        break;
    case 0x44: /* set mode and lock */
        if (i == 0 && tx <= 1) ds.analog = tx;
        if (i == 1) ds.locked = tx == 3;
        break;
    case 0x45: { /* get controller type and mode */
        static const uint8_t t[6] = {0x01, 0x02, 0, 0x02, 0x01, 0x00};
        r = i == 2 ? (uint8_t)ds.analog : t[i];
        break;
    }
    case 0x46: {
        static const uint8_t a0[6] = {0, 0, 1, 2, 0, 0x0A}, a1[6] = {0, 0, 1, 1, 1, 0x14};
        r = ds.params[0] == 1 ? a1[i] : a0[i];
        break;
    }
    case 0x47: { static const uint8_t t[6] = {0, 0, 2, 0, 1, 0}; r = t[i]; break; }
    case 0x4C: r = i == 3 ? (ds.params[0] == 1 ? 7 : 4) : 0; break;
    case 0x4D: r = ds.rumble_map[i]; ds.rumble_map[i] = tx; break;
    default: r = 0; break;
    }
    if (i == n - 1) end_of_command();
    return r;
}

void pad_vblank(void) {
    if (pad_analog_toggle && !ds.locked && !ds.config) {
        ds.analog = !ds.analog;
        LOG("pad: analog mode %s\n", ds.analog ? "on" : "off");
    }
}

void pad_set_analog(int on) { ds.analog = on; }

static void transfer(uint8_t tx) {
    uint8_t rx = 0xFF;
    int ack = 0;
    if (s.pos == 0) {
        if (!(s.ctrl & CTRL_PORT2) && tx == 0x01) s.dev = 1;
        else s.dev = 0; /* port 2 empty; memory cards (0x81) not inserted */
    }
    if (s.dev == 1) rx = pad_byte(tx, &ack);
    s.pos++;
    if (s.rx_n < 8) s.rx[s.rx_n++] = rx;
    s.stat |= 0x0002 | 0x0004; /* RX not empty, TX finished */
    if (ack) {
        s.ack_time = cpu.cycles + 1100;
        sched_at(s.ack_time);
    }
}

void sio_update(void) {
    if (s.ack_time && cpu.cycles >= s.ack_time) {
        s.ack_time = 0;
        s.stat |= 0x0080; /* /ACK low */
        if (s.ctrl & CTRL_ACKIRQ) {
            s.stat |= 0x0200;
            irq_raise(IRQ_PAD);
        }
    } else if (s.ack_time) {
        sched_at(s.ack_time);
    }
}

uint32_t sio_read(uint32_t addr, int size) {
    switch (addr & 0xF) {
    case 0x0: {
        uint32_t v = 0xFF;
        if (s.rx_n) {
            v = s.rx[0];
            memmove(s.rx, s.rx + 1, (size_t)--s.rx_n);
        }
        if (!s.rx_n) s.stat &= ~0x0002u;
        if (size > 1) v |= 0xFFFFFF00u;
        return v;
    }
    case 0x4: {
        uint32_t v = s.stat | 0x0001 | 0x0004;
        s.stat &= ~0x0080u; /* ACK level is a short pulse */
        return v;
    }
    case 0x8: return s.mode;
    case 0xA: return s.ctrl;
    case 0xE: return s.baud;
    }
    return 0;
}

void sio_write(uint32_t addr, uint32_t v, int size) {
    (void)size;
    switch (addr & 0xF) {
    case 0x0:
        if ((s.ctrl & (CTRL_TXEN | CTRL_DTR)) == (CTRL_TXEN | CTRL_DTR)) transfer((uint8_t)v);
        else if (s.rx_n < 8) { s.rx[s.rx_n++] = 0xFF; s.stat |= 0x0002; }
        break;
    case 0x8: s.mode = (uint16_t)v; break;
    case 0xA:
        if (v & 0x40) { /* reset */
            s.stat = 0; s.rx_n = 0; s.pos = 0; s.ack_time = 0;
        }
        if (v & 0x10) s.stat &= ~0x0200u; /* acknowledge IRQ */
        if (!(v & CTRL_DTR) || ((v ^ s.ctrl) & CTRL_PORT2)) { s.pos = 0; s.dev = 0; }
        s.ctrl = (uint16_t)(v & ~0x50u);
        break;
    case 0xE: s.baud = (uint16_t)v; break;
    }
}

/* ------------------------------------------------------------------ save states (state.c) */
#include "state.h"

void sio_state(StIO *io) {
    st_begin(io, "SIO", 1);
    ST(io, s); ST(io, ds);
    st_end(io);
}
