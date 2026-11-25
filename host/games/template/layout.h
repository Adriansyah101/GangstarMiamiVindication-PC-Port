#ifndef GMV_LAYOUT_H
#define GMV_LAYOUT_H

#define GMV_EXE_BASE        0x00010000
#define GMV_RESERVE_START   0x00011000
#define GMV_LOW_START       0x00001000

#define GMV_MACHO_END       0x00C68000
#define GMV_LOWCOPY_BASE    0x00C70000
#define GMV_LOWCOPY_CAP     0x00020000
#define GMV_THUNK_BASE      0x00C90000
#define GMV_THUNK_SIZE      0x00008000
#define GMV_DATAGUARD_BASE  0x00C98000
#define GMV_DATAGUARD_SIZE  0x00008000
#define GMV_RESERVE_END     0x00D00000

#define GMV_LOW_DELTA       (GMV_LOWCOPY_BASE - GMV_LOW_START)

#define GMV_GAME_NAME   "Template"
#define GMV_GAME_TITLE  "Template"
#define GMV_GAME_DEV_APP "Template.app"

#define GMV_CLASS_APP       "GameApplication"
#define GMV_CLASS_DELEGATE  "GameDelegate"
#define GMV_CLASS_VIEW      "EAGLView"
#define GMV_CLASS_WINDOW    "GameWindow"
#define GMV_VIEW_FRAME_W    1024.0f
#define GMV_VIEW_FRAME_H    768.0f
#define GMV_KEYWINDOW_FLAG_ADDR     0u
#define GMV_STRINGBUF_VTABLE_SLOT   0u

#define GMV_TEXT_LO     0x00003000u
#define GMV_TEXT_HI     0x00600000u

#endif
