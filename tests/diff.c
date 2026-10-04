#include "document.h"

#include <assert.h>

int main(void) {
  const char *text =
      "diff --git a/file b/file\nindex abc..def 100644\n--- a/file\n+++ b/file\n"
      "@@ -1,3 +1,3 @@\n same\n-old\n+new\n---code\n+++code\n"
      "\\ No newline at end of file\n"
      "@@ -10 +10 @@\n-last\n+next\n"
      "diff --cc conflict\nindex abc,def..ghi\n--- a/conflict\n+++ b/conflict\n"
      "@@@ -1,2 -1,2 +1,3 @@@\n  same\n- old\n -other\n++new\n +newer\n"
      "diff --git a/binary b/binary\nBinary files a/binary and b/binary differ\n";
  const diff_line_kind expected[] = {
      DIFF_METADATA, DIFF_METADATA, DIFF_METADATA, DIFF_METADATA,
      DIFF_HUNK, DIFF_CONTEXT, DIFF_REMOVED, DIFF_ADDED, DIFF_REMOVED, DIFF_ADDED,
      DIFF_METADATA, DIFF_HUNK, DIFF_REMOVED, DIFF_ADDED,
      DIFF_METADATA, DIFF_METADATA, DIFF_METADATA, DIFF_METADATA,
      DIFF_HUNK, DIFF_CONTEXT, DIFF_REMOVED, DIFF_REMOVED, DIFF_ADDED, DIFF_ADDED,
      DIFF_METADATA, DIFF_METADATA
  };
  document *doc = document_preview("Diff", text);
  assert(doc && doc->readonly && doc->diff_lines);
  assert(doc->buf.nlines == (int)(sizeof expected / sizeof *expected));
  for (int i = 0; i < doc->buf.nlines; i++)
    assert(doc->diff_lines[i] == expected[i]);
  document_retain(doc);
  document_retain(doc);
  document_release(doc);
  assert(doc->diff_lines[7] == DIFF_ADDED);
  document_release(doc);

  doc = document_preview("Diff", "No differences.\n");
  assert(doc && doc->diff_lines[0] == DIFF_METADATA);
  document_retain(doc);
  document_release(doc);
  return 0;
}
