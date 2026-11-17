#pragma once
// BuildPlanner - Path of Building item text
// PobItem.h - Path of Building's raw item text (the body of an <Item> element) -> BuildItem.
// Format: Path of Building's Item:ParseRaw (src/Classes/Item.lua). Pure, no SDK, never throws.
//
// Threads: stateless; safe from any thread.
#include <string>
#include <string_view>

#include "Types.h"

namespace bp {

// Parses one item text. Rules:
//   - "Rarity: NORMAL|MAGIC|RARE|UNIQUE|RELIC" (an "Item Class:" first line and "--------" separators
//     are skipped). Rare / unique / relic: the next line is the name, the one after it the base.
//     Normal / magic: one line, kept whole in BuildItem::base (name stays "").
//   - "Quality: n", "Sockets: ...", "Rune: <name>" (None dropped), "LevelReq: n", "Item Level: n",
//     trailer "Corrupted" / "Twice Corrupted" -> the matching fields. Other header lines are ignored.
//   - "Implicits: N" starts the mod lines: the next N lines are rune / enchant / implicit lines (each
//     gets kModImplicit plus its own tag), the rest are explicit mods. Without an "Implicits:" line
//     every mod line is explicit.
//   - A mod line's {...} prefixes become ModTag bits ({rune}, {enchant}, {crafted}, {fractured},
//     {desecrated}, {mutated}, {custom}); {range:..}, {tags:..}, {variant:..} and unknown prefixes are
//     dropped. A {variant:a,b} line that does not list the item's "Selected Variant" is left out.
//     Trailing " (implicit)" / " (enchant)" / " (rune)" / " (crafted)" markers are removed.
//   - Caps of Types.h: kMaxModLines per list, kMaxRunes, kMaxModBytes per line, kMaxNameBytes;
//     BuildItem::raw keeps the first kMaxRawItemBytes of the text (cut on a character boundary).
// Returns false (and leaves `out` default) when the text has neither a rarity line nor a name line.
bool ParsePobItem(std::string_view text, BuildItem& out);

// The variant selection an <Item> element carries in its attributes (`variant`, `variantAlt`,
// `variantAlt2`..`variantAlt5`; 0 = absent). The text's own "Selected Variant" / "Selected Alt
// Variant[ Two..Five]" lines win over these. Without either, Path of Building selects the LAST
// "Variant:" line (the current version of a unique), and so does ParsePobItem.
struct PobItemVariants {
    int variant = 0;
    int alt[5] = {0, 0, 0, 0, 0};
};
bool ParsePobItem(std::string_view text, BuildItem& out, const PobItemVariants& fromXml);

// "RARE" -> Rarity::Rare (ASCII case-insensitive). false for an unknown word.
bool RarityFromPob(std::string_view word, Rarity& out);

} // namespace bp
