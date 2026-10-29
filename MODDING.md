# Making mods

Xerenge looks for mods in a `mods` folder inside the game's files - the folder
that holds `default.xex`, `fe`, `language` and the rest (`game` in an installed
copy, `game_eu`/`game_us` in a checkout; the installer makes it). Each mod is a
folder of its own with a `mod.cfg` in it:

```
game/
  default.xex
  language/
  mods/
    locale_rus/
      mod.cfg
      locale_rus.txt
      Big.fnt
      Big.png
      ...
```

`mod.cfg` is a list of `Key = value` lines; everything after a `#` is a
comment. `Type` says what the mod is; the rest depends on the type.

So far there is one type: **localisation**.

## Localisation mods

A localisation mod puts its own text in place of the game's, and its own fonts
in place of the game's fonts - for a language the game was never released in.

```
Version = 0.1
Type = Localization
Name = Russian

Localization = locale_rus.txt

FontBig = recipe:font_recipe.txt
FontBigDistressed = recipe:font_recipe.txt
FontSmall = recipe:font_recipe.txt
# FontDigital = Digital.fnt
```

This is `rexglue/mods/locale_rus` in the repository, which the releases carry
as `Xerenge-mod-locale_rus.zip`.

| Key | What it is |
|---|---|
| `Version` | The mod's own version, shown in the log. |
| `Type` | `Localization`. |
| `Name` | The language's name as it appears in **Settings > LANGUAGE**. |
| `Localization` | The text file (below). |
| `FontBig`, `FontBigDistressed`, `FontSmall`, `FontDigital` | Fonts in place of the game's (below). A font left out stays the game's. |

### Turning it on

Every localisation mod found appears in **Settings > LANGUAGE** after the
game's own languages, under its `Name`. It is written to `xerenge.conf` as
`language = <the mod's folder>`, where a language of the game's is a number;
without a `language` line at all, the first mod found (by folder name) is used.

Under a mod the game runs in English: any string the mod does not have reads
in English. Switching between the mod and English takes effect at once;
another language of the game's, after the restart the game offers.

A mod's fonts are loaded whatever the language, as long as a mod with fonts is
there: they hold the game's letters as well as the mod's, so names in the
mod's alphabet - your own music's, for one - read as they are in English too.

### The text file

UTF-8, one string a line:

```
# A comment.
FESONGMANAGER = МЕНЕДЖЕР ПЕСЕН
FEPREVIEWTRACK = Прослушать
crashTip19 = Playing Crash\n\nSmashing up street furniture earns\nsmall amounts of $.
0x80303571 = ЛУЧШИЙ СЧЁТ
```

- The key is the string's name, as the game's interface asks for it (in the
  menus it is written `$FESONGMANAGER`; the `$` may be left out).
- `\n` is a new line, `\\` a backslash.
- The game keeps its strings by a hash of the name, not by the name, and not
  every name could be found again. A string whose name is not known is written
  with its hash, `0x80303571`; such a key works the same.
- Keep the game's placeholders (`%1`, `%s` and the like) where they are in the
  English text.

**Start from the game's own text.** `localeexport` writes every string of a
language file into this format, with its name where it is known and its hash
where it is not:

```bash
tools/localeexport/localeexport game/language/strings/MainUK.bin strings_template.txt game/fe/*.bin
```

(`MainUK.bin` on the European disc, `MainUS.bin` on the American one; the
`fe/*.bin` files are where names are looked for. Adding the game's decrypted
executable finds a few more.) Translate the lines you want and keep them in
your text file; the template need not be shipped.

### Fonts

The game draws all of its text with four fonts:

| Key | The game's file | Where it is used |
|---|---|---|
| `FontBig` | `language/fonts/Big.bin` | Most of the menus: lists, buttons, hints (Univers 59 Ultra Condensed) |
| `FontBigDistressed` | `language/fonts/BigDistressed.bin` | Titles: SONG MANAGER, [EA TRAX]... - the same letters, worn |
| `FontSmall` | `language/fonts/small.bin` | Smaller text (Univers 47 Condensed Light) |
| `FontDigital` | `language/fonts/digital.bin` | The LCD-style numbers - digits only |

A font may be given in one of four forms:

- **A recipe** - `recipe:font_recipe.txt`. The new letters made of the game's
  own glyphs, on the player's machine, from their copy of the game: nothing of
  the game's is in the mod, so it can be shared freely. See *Font recipes*
  below. `auto` is Xerenge's own Cyrillic recipe.
- **A BMFont** - `.fnt` (the text format) and its `.png` page(s). Pictures of
  each letter, drawn as you like: this is how to make a font look like the
  game's, wear and all.
- **A TrueType or OpenType font** - `.ttf`, `.otf`. Simplest: it is drawn at the
  start, sized so its capitals are as tall as the game's. Clean letters only -
  no wear for `FontBigDistressed`.

**Start from the game's own fonts.** `fontgen export` writes a game font as a
BMFont, with Cyrillic already put together out of its own Latin letters (Я from
a mirrored R, Б from E over B, И from a mirrored N, and so on) - a draft to
redraw letter by letter in any image editor:

```bash
tools/fontgen/fontgen export game/language/fonts/Big.bin Big mods/locale_rus
tools/fontgen/fontgen export game/language/fonts/BigDistressed.bin BigDistressed mods/locale_rus
tools/fontgen/fontgen export game/language/fonts/small.bin Small mods/locale_rus
```

`fontgen fnt <game font> <your .fnt> <out.bin>` (or `fontgen ttf ...`) makes the
game's font of yours the way the game will, and writes `<out.bin>.pam`: a line
of text drawn with it, to look at before starting the game.

What to keep in mind when drawing:

- **The atlas keeps the game's size**: 512x512 for Big and BigDistressed,
  256x256 for Small. The game sizes a letter on the screen by its share of
  that atlas, so a bigger one would draw everything smaller. Your page may be
  any size - its letters are packed anew - but they must fit the game's.
  When they do not, the letters that matter least are left out first: the
  accented Latin ones (À-ÿ), then the rarer signs. ASCII, the quotes, ©, ® and
  ™, and Cyrillic are always kept.
- **Keep the game's proportions.** A letter's `yoffset` is measured from the
  top of the line and `base` is the baseline; the exported files have the
  game's own values - new letters should sit on the same baseline and be as
  tall as their Latin neighbours.
- **White letters** on a transparent background: the game colours them.
- Every character your text uses needs a glyph. A character the font does not
  have is drawn as nothing.

### Font recipes

A recipe adds letters to a game font by putting them together out of the
font's own glyphs - with the font's own strokes, wear and proportions - when
the game starts on the player's machine. A letter a line, `#` a comment:

```
Я = flipH R
Г = flipV L
П = bar top (hollow H)
Б = over (flipV L) (belowMiddle B)
Ф = stem center 0.1 O
б = small Б
```

On the right, a glyph is one character (or `U+0411`): a letter made earlier in
the recipe, otherwise the game font's own. Operations take their arguments
after them; parentheses make one an argument of another.

| Operation | Makes |
|---|---|
| `flipH g`, `flipV g`, `rot180 g` | g mirrored, upside down, turned round |
| `cap g` | g as tall as the capitals (H) |
| `small g` | g as a small capital, as tall as `x` |
| `closer g` | g's mark (dots, a breve) moved down near the letter |
| `wide <f> g` | g f times as wide |
| `hollow g` | g emptied between its stems, under the top |
| `bar top\|middle\|bottom [<f>] [left\|right] g` | E's bar across g - f of its width, at a side |
| `stem [center\|left\|right] [<f>] g` | I's stem down g, f of the capitals' height past it above and below |
| `stemSpan a b g` | a stem through g from a's top to b's foot (ф: `stemSpan b p o`) |
| `tail g` | a stub below g's right corner (Ц, Щ) |
| `legs g` | g on a bar a stem wider each side, its ends going down (Д) |
| `belowMiddle g` | g from its middle bar down (B, for Б) |
| `over a b` | b drawn over a, each where its own height puts it |
| `mark g n` | n's mark (Ñ's tilde) above g (Й) |
| `arm g` | a bar out to the left of g's top (Ъ) |
| `beside [<gap>] a b` | b to the right of a, gap in stems (Ы) |
| `join [<gap>] a b` | a, a bar at mid height, b (Ю) |

A letter made by an operation gets the capitals' side bearings; one taken as
it is, or with `small` or `closer`, keeps its own advance. A letter the font
has already is left alone, and a line that cannot be read is skipped (the log
says which).

`fontgen default-recipe` prints Xerenge's Cyrillic recipe - the start of one
of your own - and `fontgen recipe <game font> <recipe.txt> <out.bin>` makes
the font as the game will, with a line of text drawn in `<out.bin>.pam`.

### What happens at the start

Before the game's files are opened, Xerenge makes each of the mod's fonts into
the game's own format and keeps the result in the mod's `.cache` folder. It is
made again only when the font, its pages, `mod.cfg` or the text file are newer
than it - so editing a `.png` and starting the game is enough. Delete `.cache`
to have everything made again.

`burnout.log` says what was found and done - lines starting with `mods:`:

```
mods: locale_rus (Russian, version 0.1)
mods: 412 strings from .../mods/locale_rus/locale_rus.txt
mods: locale_rus: Big made of Big.fnt (1060864 bytes)
mods: the game's Language/Fonts/Big.bin is mods/locale_rus/.cache/Big.bin ...
```

and why a font was not used, if it was not.

### Your own music

Not a mod, but close: the songs of a `music` folder (beside the game, or
`music_dir = <folder>` in `xerenge.conf`) can take EA Trax's place - **Driver
Details > Song Manager**, LT. MP3 and WMA; the names come from the files' tags
(or from `Artist - Title.mp3`). With a localisation mod whose fonts have the
letters, the names are shown as they are; without one, Cyrillic in them is
written in Latin letters. Which list is chosen is kept in `xerenge.conf` as
`music_source` (`custom` or `eatrax`), so a restart plays the same one.

### Sharing a mod

A mod is shared as its folder, `.cache` left out. Do not put the game's own
files in it: a font exported from the game and only touched up is still the
game's font, and the game's text is the game's. Fonts of your own drawing, or
fonts whose licence allows it (SIL Open Font License and the like), and your
own translation, are fine - and a recipe, which carries none of the game's
pixels, is the way to give a mod the game's own look.
