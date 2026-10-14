// dawn_format.h - Word-style formatting commands on the Markdown in the gap buffer
//
// One call per ribbon button or shortcut. format_apply edits the note and moves the cursor and
// selection with the text; format_state says whether the button for a style shows pressed. The
// module is pure (no app global, no backend), so tests/test_format.c links it with dawn_gap.c
// alone. The toggle rules are described at the top of dawn_format.c.

#ifndef DAWN_FORMAT_H
#define DAWN_FORMAT_H

#include "dawn_types.h"

//! A formatting command
DAWN_ENUM(uint8_t) {
    FORMAT_BOLD, //!< **text**
    FORMAT_ITALIC, //!< *text*
    FORMAT_STRIKE, //!< ~~text~~
    FORMAT_CODE, //!< `text`
    FORMAT_H1, //!< "# " line prefix
    FORMAT_H2, //!< "## " line prefix
    FORMAT_H3, //!< "### " line prefix
    FORMAT_BULLET, //!< "- " list item
    FORMAT_NUMBERED, //!< "1. " list item
    FORMAT_TASK, //!< "[ ] " box on a list item
    FORMAT_QUOTE, //!< "> " line prefix
    FORMAT_LINK, //!< [text](url)
    FORMAT_IMAGE, //!< ![alt](path)
    FORMAT_TABLE, //!< two-column table with a header row
    FORMAT_RULE, //!< --- thematic break
    FORMAT_CODE_BLOCK, //!< ``` fenced block
    FORMAT_COUNT //!< Number of commands
} FormatOp;

//! Cursor and selection as byte offsets, the way App keeps them (app.cursor, app.sel_anchor,
//! app.selecting). The selection is the range between anchor and cursor in either order.
typedef struct {
    size_t cursor; //!< Cursor byte offset
    size_t anchor; //!< Selection anchor byte offset
    bool selecting; //!< Whether anchor..cursor is a selection
} FormatCursor;

//! Apply a formatting command at the cursor or to the selection
//! @param g text to edit
//! @param op command
//! @param c cursor and selection; updated to follow the text, always within the buffer
//! @return false when the text did not change
bool format_apply(GapBuffer* g, FormatOp op, FormatCursor* c);

//! Whether a style is on at the cursor or over the whole selection (a pressed button). Always
//! false for the inserts (link, image, table, rule); true for a code block when the cursor's line
//! is inside a fenced block.
//! @param g text to inspect
//! @param c cursor and selection
//! @param op command
//! @return true when applying op would take the style off
bool format_state(const GapBuffer* g, const FormatCursor* c, FormatOp op);

#endif // DAWN_FORMAT_H
