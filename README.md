# hb_mini

hb_mini is an unofficial fork of HarfBuzz that cuts out all features that are unnecessary for the rcut library.

The original HarfBuzz consisted of 120k lines of C++ code, this fork only consists of 16k lines of C code.

Only a small subset of the HarfBuzz API is implemented. See hb_mini.h what remains of it. The API that remains has not been changed in any way, hb_mini can always be replaced with the full HarfBuzz without any code changes.

Notable features that hb_mini doesn't have:
- No specific Khmer and Myanmar text shapers.
- No vertical text shaping.
- Universal Shaping Engine was removed, which means syllables in many Asian scripts (e.g. Sinhala, Tibetan, Balinese, Javanese) aren't reordered. The major Indic scripts and CJK are not affected.
- No fallback shaping: fonts need GSUB for Arabic joining, and GPOS for kerning and mark positioning.
- morx, kerx and trak tables are not parsed. This only affects some Apple specific fonts.
- Legacy kern tables are not parsed. This only affects some very old font files.
- No support for CFF2 (outlined glyphs for variable fonts).
- No support for SVG glyphs. (Emoji via COLR v0/COLR v1 and PNG glyphs remain.)

## Clanker Use and Copyright
This fork was primarily created using an LLM to perform the API surface reduction and the translation to C. No original work went into creating it. As such I do not claim ownership, authorship or copyright of any of the code. In effect this means the original Harfbuzz license terms (as of 21st October 2026) applies to all of hb_mini.