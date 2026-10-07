#ifndef UNDO_H
#define UNDO_H
#include "buffer.h"
#include <stdint.h>
struct editor;
struct document;
struct module;
typedef struct undo_stats {
  size_t undo_actions, redo_actions, records;
  uint64_t journal_bytes;
} undo_stats;
void undo_status(const struct document *doc, undo_stats *stats);
#ifdef ADM_TEST_ALLOC
void undo_test_io_failure(int fail);
#endif
struct module *undo_module(void);
void undo_attach(struct document *doc);
void undo_clear(struct document *doc);
void undo_saved(struct document *doc);
void undo_begin(struct editor *e, int key);
void undo_end(struct editor *e);
void undo_group_begin(struct editor *e);
void undo_group_end(struct editor *e);
void undo_note_edit(buffer *b, const buffer_edit *edit);
struct undo_transaction;
struct undo_transaction *undo_prepare(struct editor *e, struct document *doc,
                                      const buffer_edit *edit, const char *text,
                                      size_t bytes, int after_lines,
                                      const buffer_edit *events,
                                      size_t event_count);
void undo_commit_prepared(struct editor *e,
                          struct undo_transaction *transaction);
void undo_discard_prepared(struct undo_transaction *transaction);
#endif
