#ifndef DITHER_H
#define DITHER_H

// Dithering modes — controlled by the 3rd filename segment
// Filename format: DD_MM_X_name.ext  where X is:
//   F = Floyd-Steinberg,  H = Halftone,  O = Ordered,
//   P = Pop-art,  N = None (nearest palette colour)
enum DitherMode { DITHER_NONE, DITHER_FLOYD, DITHER_HALFTONE, DITHER_ORDERED, DITHER_POPART };

#endif
