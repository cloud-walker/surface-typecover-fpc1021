/*
 * FPC1021 protocol constants and pure decoding helpers.
 *
 * This header holds only facts about the wire protocol, as documented in
 * ../PROTOCOL.md -- no I/O, no state. Both the capture tool and the
 * diagnostic probe include it so there is exactly one place where a
 * protocol fact lives.
 */

#ifndef FPC_PROTO_H
#define FPC_PROTO_H

#define FPC_VID 0x045e
#define FPC_PID 0x09c2
#define FPC_IFACE 1
#define FPC_EP_OUT 0x04
#define FPC_EP_IN 0x83
#define FPC_MAX_PACKET 64

#define CMD_GET_CHIP_ID 0x0001
#define CMD_UNKNOWN_0005 0x0005
#define CMD_CAPTURE 0x0007
#define CMD_RESET 0x0008

/* Replies echo the command they acknowledge as 0x1000 | opcode. */
#define FPC_STATUS_TO_OPCODE(status) ((unsigned short)((status) & 0x0fff))
#define FPC_OPCODE_TO_STATUS(opcode) ((unsigned short)(0x1000 | (opcode)))

/* substatus 5 is a transient "not ready yet" -- retry, don't fail. */
#define FPC_SUBSTATUS_OK 0
#define FPC_SUBSTATUS_NOT_READY 5

typedef struct {
    unsigned short width;
    unsigned short height;
    const char *name;
} fpc_chip_info;

static inline const char *fpc_opcode_name(unsigned short opcode) {
    switch (opcode) {
        case CMD_GET_CHIP_ID:  return "get_chip_id";
        case CMD_UNKNOWN_0005: return "unknown_0005";
        case CMD_CAPTURE:      return "capture";
        case CMD_RESET:        return "reset";
        default:               return "unknown";
    }
}

/* Looks up chip identity/resolution from the Get-Chip-ID word. See PROTOCOL.md. */
static inline int fpc_identify_chip(unsigned short chip_id, fpc_chip_info *out) {
    unsigned short masked = chip_id & 0xfff0;
    if (masked == 0x0200) { out->width = 192; out->height = 192; out->name = "FPC1020"; return 0; }
    if (masked == 0x0210) { out->width = 160; out->height = 160; out->name = "FPC1021"; return 0; }
    if (masked == 0x1400) { out->width = 192; out->height =  56; out->name = "FPC1140"; return 0; }
    if (masked == 0x1500) { out->width = 208; out->height =  80; out->name = "FPC1150"; return 0; }
    if ((chip_id & 0xff0f) == 0x0101) { out->width = 88; out->height = 112; out->name = "FPC1022"; return 0; }
    return -1;
}

#endif /* FPC_PROTO_H */
