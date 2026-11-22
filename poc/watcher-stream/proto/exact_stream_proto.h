// The Exact stream protocol: an Exact app runs on a host (a Mac), and a thin
// client board (the SenseCAP Watcher) shows its pixels and sends back its
// inputs. Shared by the firmware and the host; plain C, no allocation.
//
// A packet on the wire, both ways:
//   A5 5A  type:u8  len:u24le  payload[len]  crc32:u32le
// The CRC-32 (zlib's) covers type, len and payload. A reader skips bytes
// until it sees the magic, so boot chatter on the same UART is harmless.
//
// Host to board:
//   HELLO  (empty)                  the board answers INFO
//   AREA   area header, then ops    paint one rectangle of the board's screen
//   LIGHT  brightness:u8 (0-100)
//   PLAN   epoch_ms:i64 utc_offset:i16 blob   run this plan on the board
//          (vm/plan_blob.h); the board answers PLAN_OK and stops showing AREAs
//          until the next AREA
//   AGENT  one agent request line for the board's running plan (rt::Host);
//          its answers come back as LOG lines (CE_AGENT …, CE_SHOT …)
// Board to host:
//   INFO   w:u16 h:u16 version:u16 flags:u16
//   ACK    seq:u16 ok:u8 decode_us:u32 push_us:u32   (ok 0: the board's copy of
//          the screen is now unknown; the host sends a keyframe)
//   INPUT  kind:u8 x:i16 y:i16 key:u8
//   LOG    text
//   PLAN_OK ok:u8 load_us:u32 boot_us:u32 internal_free:u32 psram_free:u32 text
//
// AREA payload: x:u16 y:u16 w:u16 h:u16 seq:u16 enc:u8, then pixels in row
// order over the rectangle. enc 0: w*h RGB565 pixels, big-endian. enc 1: ops,
// each an op byte `kind << 6 | n`, where n 0-62 means a count of n + 1 pixels
// and n 63 means a u16le count follows:
//   SKIP count             the board's pixels stay as they are
//   FILL count color:u16be one color
//   LIT  count pixels      count big-endian RGB565 pixels
#pragma once

#include <stdint.h>

#define XS_MAGIC0 0xA5
#define XS_MAGIC1 0x5A
#define XS_VERSION 2
#define XS_BAUD 3000000

enum {
  XS_HELLO = 0x01,
  XS_AREA = 0x10,
  XS_LIGHT = 0x11,
  XS_PLAN = 0x12,
  XS_AGENT = 0x13,
  XS_INFO = 0x81,
  XS_ACK = 0x82,
  XS_INPUT = 0x83,
  XS_LOG = 0x84,
  XS_PLAN_OK = 0x85,
};

enum { XS_ENC_RAW = 0, XS_ENC_OPS = 1 };
enum { XS_OP_SKIP = 0, XS_OP_FILL = 1, XS_OP_LIT = 2 };

enum { XS_IN_KEY = 0, XS_IN_DOWN = 1, XS_IN_MOVE = 2, XS_IN_UP = 3 };
enum { XS_KEY_LEFT = 1, XS_KEY_RIGHT = 2, XS_KEY_ENTER = 3, XS_KEY_ESCAPE = 4, XS_KEY_NEXT_APP = 5 };

#define XS_AREA_HEADER 11
#define XS_MAX_PACKET (1u << 23)
