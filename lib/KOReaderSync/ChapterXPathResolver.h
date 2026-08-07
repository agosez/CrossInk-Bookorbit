#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

class ChapterXPathResolver {
 public:
  /**
   * Resolve the Nth paragraph in a spine item to its real XHTML ancestry path.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]
   *
   * An empty string means parsing failed or the paragraph index was not found.
   */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /**
   * Resolve a highlight to the precise xpointer pair that delimits it.
   *
   * Returns positions like `/body/DocFragment[8]/body/div[2]/p[4]/text()[1].96`, offsets being
   * codepoints in the whitespace-collapsed view of a single text node (the crengine convention
   * BookOrbit resolves against) -- so a highlight starting inside inline markup gets that
   * element's node, not the paragraph's.
   *
   * A paragraph-level position is not a usable substitute: a server that resolves the range and
   * reads the text back finds nothing there and flags the annotation as repaired, which reads to
   * the user as a failure. `pos1` is exclusive, matching KOReader.
   *
   * The whole chapter is streamed twice at a flat memory cost: one pass locates the text, a
   * second extracts the positions and the span's exact source text. Matching ignores all
   * whitespace and soft hyphens on both sides -- clipping text is rebuilt from rendered words,
   * whose spacing justification distorts and whose soft hyphens are already consumed. The
   * paragraph hint only disambiguates repeated occurrences: its counting drifts from the <p>
   * ordinal whenever non-<p> blocks precede the text, so it is never trusted as a position.
   *
   * @param highlightText The highlighted text, as the clipping stores it
   * @param outSourceText When non-null, receives the matched span's exact source text in the
   *        collapsed view (paragraph breaks as newlines) -- what the server reads back at
   *        these positions, non-breaking spaces included as plain spaces
   * @return false when the chapter could not be read or no longer contains the highlight;
   *         outputs are left empty and the caller should not guess a position
   */
  static bool findHighlightXPointers(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex,
                                     const std::string& highlightText, std::string& outPos0, std::string& outPos1,
                                     std::string* outSourceText = nullptr);

  /**
   * Resolve the Nth list item in a spine item to its real XHTML ancestry path.
   *
   * An empty string means parsing failed or the list item index was not found.
   */
  static std::string findXPathForListItem(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t listItemIndex);

  /**
   * Resolve intra-spine progress to a real XHTML ancestry path plus text offset.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text().96
   *
   * An empty string means parsing failed or the location could not be resolved.
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

  // Resolve a zero-based visible Unicode codepoint offset. This is the stable
  // page position stored in CrossInk's section cache, independent of layout.
  static std::string findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                   uint32_t visibleTextOffset);
};
