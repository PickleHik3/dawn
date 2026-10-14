// dawn_hit.h - Hit regions: what a tap on a screen cell stands for, registered while drawing

#ifndef DAWN_HIT_H
#define DAWN_HIT_H

#include <stdbool.h>
#include <stdint.h>

//! What a region does when tapped
typedef enum {
    HIT_KEY, //!< The same as pressing key (a DawnKey or a character such as 'h' or '\r')
    HIT_ROW, //!< Pick row arg of the screen's list, then press key (0: only pick it)
    HIT_PILL, //!< A scroll pill's track; arg is the ScrollIndPane
    HIT_INERT, //!< Swallows the tap: a box that covers what is under it, with nothing of its own
} HitKind;

//! A tapped region as hit_find() returns it
typedef struct {
    HitKind kind;
    int32_t key; //!< HIT_KEY: the key; HIT_ROW: the key pressed after picking (0 for none)
    int32_t arg; //!< HIT_ROW: the row's index; HIT_PILL: the pane
} HitTarget;

//! Most regions one frame holds; later ones are dropped silently.
#define HIT_MAX 256

//! Start a frame: forget every region. Called once at the top of render().
void hit_frame_begin(void);

//! Register a tappable rectangle of w x h cells whose top-left cell is (col, row), 1-based like
//! the rest of dawn. Later regions win where they overlap, so an overlay registers after what it
//! covers. Empty rectangles and regions past HIT_MAX are ignored.
void hit_add(int32_t col, int32_t row, int32_t w, int32_t h, HitKind kind, int32_t key, int32_t arg);

//! Register the key hints of a hint line drawn from (col, row): "[enter] new   [esc] menu" or
//! "tab:next  enter:save  esc:cancel". Each hint whose key can be named (enter, esc, tab, space,
//! <-, ->, ^S, ^N, or a single character) becomes a HIT_KEY region over the hint and its label;
//! others ("[j/k] select", "+/-:adj") are skipped. Only the first max_cols columns are covered
//! (the line may have been cut there). Single letters are skipped when letters is false, for
//! editors where a letter would be typed into a field instead of acting.
void hit_add_hints(int32_t col, int32_t row, const char* text, int32_t max_cols, bool letters);

//! The region on screen cell (col, row) as last registered, the latest one where several overlap.
//! @return false when no region covers the cell
bool hit_find(int32_t col, int32_t row, HitTarget* out);

//! How many regions the current frame holds (for tests).
int32_t hit_count(void);

#endif // DAWN_HIT_H
