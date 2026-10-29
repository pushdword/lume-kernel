#ifndef LUME_VIDEO_CONTROL_H
#define LUME_VIDEO_CONTROL_H

/* Pixel and cell dimensions pack width in bits 0..15 and height in bits 16..31.
 * Catalogue indices are local to the active controller and are not persistent IDs.
 * Queries take zero unless an index is required. The final syscall argument is zero.
 * Font upload consumes 4096 bytes containing 256 MSB-first 8x16 glyphs.
 * Glyph indices use Latin-1 with the euro sign in slot 0x80. */
enum lume_video_action
{
    LUME_VIDEO_LEGACY_MODES = 0,
    LUME_VIDEO_LEGACY_SELECT = 1,
    LUME_VIDEO_UPLOAD = 2,
    LUME_VIDEO_FRAMES = 3,
    LUME_VIDEO_PIXELS = 4,
    LUME_VIDEO_STATUS = 5,
    LUME_VIDEO_PITCH = 6,
    LUME_VIDEO_DEPTH = 7,
    LUME_VIDEO_MEMORY = 8,
    LUME_VIDEO_MODE_COUNT = 10,
    LUME_VIDEO_MODE_PIXELS = 11,
    LUME_VIDEO_MODE_SELECT = 13,
    LUME_VIDEO_FONT_COUNT = 14,
    LUME_VIDEO_FONT_CELL = 15,
    LUME_VIDEO_FONT_FACE = 16,
    LUME_VIDEO_FONT_SELECT = 17,
    LUME_VIDEO_FONT_CURRENT = 18,
    LUME_VIDEO_CELLS = 19,
};

enum lume_video_face
{
    LUME_VIDEO_LATIN = 0,
    LUME_VIDEO_BOLD = 1,
    LUME_VIDEO_COMPACT = 2,
    LUME_VIDEO_CUSTOM = 3,
};
#endif
