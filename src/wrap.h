#ifndef WRAP_H
#define WRAP_H

// Word wrap splits a buffer line into visual rows that fit the text area.
// A row breaks after the blanks that follow a word; a word longer than a row
// breaks between clusters. The last column of a row is kept for one trailing
// blank or for the end of the line, so the cursor always has a cell to sit on.
// Rows are only a view of the line: numbers and offsets stay per buffer line.

// Byte offset where the row after the one starting at start begins, or -1
// when the row starting at start is the last row of the line.
int wrap_next(const char *line, int start, int width);

// Rows taken by the line, always at least 1.
int wrap_rows(const char *line, int width);

// Row holding byte offset at, a row start belonging to the row it starts.
// The first byte of that row goes in *start unless start is NULL.
int wrap_row(const char *line, int width, int at, int *start);

// First byte of the given row, or of the last row when the line is shorter.
int wrap_start(const char *line, int width, int row);

// Columns from the row start up to (not including) byte offset at.
int wrap_col(const char *line, int start, int at);

// Byte offset in the row starting at start that covers column col, or the
// last position of the row when col falls past it.
int wrap_byte(const char *line, int start, int width, int col);

#endif
