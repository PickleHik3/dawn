// dawn_title.h - Live titles: a title that keeps up with a young note (spec §08).
//
// Only titles the AI wrote are touched: a note whose frontmatter says title-source: ai, or one
// that has no title yet. The first title comes after about 160 characters; later ones only when
// the note changed shape (its first heading changed, or it grew by half), after 8 s of idle at the
// end of a sentence or paragraph, at most every 2 minutes and 5 times per note, and only when the
// model is already loaded (a title never causes a load). Each runs as a quiet job on the warm
// conversation (dawn_session.c). A new title that is nearly the old one is dropped silently.
//
// A new title says "renamed · <title>" in the status line (and the activity list), and renames
// the file too when dawn named it itself: a kebab-case slug of the title, -2, -3… on a clash,
// never over another file. Ctrl+Z right after puts the old title and file name back.

#ifndef DAWN_TITLE_H
#define DAWN_TITLE_H

#include "dawn_types.h"

#if HAS_LIBAI

//! Once per frame (from ai_tick()): ask for a title when one is due.
void title_tick(void);

//! Ctrl+Z (dawn.c undo()): when the last thing that happened to the note was a live title, put
//! the old title (and file name) back and return true; otherwise false, and undo goes on as usual.
bool title_undo(void);

//! The writer set the title themselves (the frontmatter editor, or by asking in the chat): the
//! note's title-source goes, and live titles never touch this note's title again.
void title_user_edited(void);

#endif // HAS_LIBAI

#endif // DAWN_TITLE_H
