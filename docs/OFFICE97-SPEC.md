# OFFICE97-SPEC: what Microsoft Office 97 actually does

The conformance yardstick for the UnoOffice suite (`docs/OFFICE97-PLAN.md`).
This file enumerates the real product — Word 97 / Excel 97 / PowerPoint 97
(SR-2 behavior), from the 2026-08-01 research pass — as checkable items.
When the suite is "done", it is compared against THIS list, item by item.

**Conformance states**, recorded in the checkbox column as work lands:
- `[F]` full — looks and behaves like Office 97
- `[C]` chrome-only — the menu item / dialog exists and honestly says
  "not in this build"
- `[ ]` not yet present
- `[X]` deliberately absent (justify inline)

Target for v1 ship: every item at least `[C]`; the **bold** items `[F]`.
Items tagged *(verify)* were reconstructed from period documentation and
must be pixel-checked against a live Office 97 install (WinWorld /
archive.org image in a VM) before being marked `[F]`.

Sources: Microsoft launch press release (1997-01-16), Wikipedia Office 97,
learn.microsoft.com [MS-DOC]/[MS-XLS]/[MS-PPT]/[MS-CFB]/[MS-ODRAW], period
guides (full URL list in the plan's research provenance, §11).

---

## 1. Shared Office platform (S-OFF)

### S-OFF-01 Command bars
- [ ] **Menus are owner-drawn with 16x16 icons beside items that exist as
  toolbar buttons; full static menus (NO adaptive/personalized collapsing
  — that is Office 2000); disabled items gray, never hidden**
- [ ] **Toolbar buttons flat; raised 3D edge on hover; sunken + dither
  background when pressed/toggled**
- [ ] **Toolbars dockable top/bottom/left/right and floatable (mini title
  bar + close); drag handle (two raised bars) at docked left end**
- [ ] Right-click any bar → checklist of toolbars + Customize…
- [ ] Customize dialog (3 tabs: Toolbars / Commands / Options); drag
  commands onto bars; Alt-drag off; Options: Large icons, ScreenTips,
  shortcut keys in ScreenTips, menu animations (None/Random/Unfold/Slide)
- [ ] Combo boxes in toolbars (Style, Font, Size, Zoom) editable + dropdown
- [ ] **Split/dropdown buttons with tear-off palettes (Undo/Redo stacks;
  Border, Highlight, Font Color, Fill Color palettes float when dragged)**
- [ ] Menu keyboard: Alt+mnemonic, F10, arrows, Esc; accelerator column

### S-OFF-02 Visual identity
- [ ] **Win95/NT4 chrome: `#C0C0C0` face, `#FFFFFF` light, `#808080`
  shadow, black frame; navy `#000080` selection with white text; MS Sans
  Serif 8pt-alike dialog font**
- [ ] **16x16 toolbar icons, 16-color VGA look; Large-icons doubling**
- [ ] Word default Times New Roman 10pt / Excel Arial 10 (metric-compatible
  Liberation faces; document defaults reproduce 97 line breaks)
- [ ] MDI: documents as children in one app frame; Window menu switching
- [ ] Zoom ranges: Word 10-500% (+Page Width/Whole/Two/Many), Excel 10-400%
  (+Fit Selection), PowerPoint 25-400% (+Fit)
- [ ] Splash screens + About boxes (own artwork/name, Office-97 layout)

### S-OFF-03 Office Assistant
- [ ] Assistant frame: frameless always-on-top window, yellow balloon,
  "What would you like to do?" query + numbered blue-bullet results,
  Options/Close; lightbulb tips; animates on save/print
- [ ] F1 and the Standard-toolbar Assistant button summon it; off by
  default in UnoOffice; our own character ("Uno"), never Clippit's art
- [ ] Classic help behind it: Contents and Index viewer; What's This?
  (Shift+F1) per-control popups; "?" titlebar button on dialogs

### S-OFF-04 OfficeArt drawing layer (shared by all three apps)
- [ ] **Drawing toolbar: Draw ▾ (Group/Ungroup/Regroup, Order (6 ops),
  Grid/Snap, Nudge, Align or Distribute, Rotate or Flip, Edit Points,
  Change AutoShape, Set AutoShape Defaults), Select Objects, Free Rotate,
  AutoShapes ▾ (Lines, Basic Shapes, Block Arrows, Flowchart, Stars and
  Banners, Callouts; Connectors in Excel/PPT *(verify Word)*), Line,
  Arrow, Rectangle, Oval, Text Box, Insert WordArt, Fill Color ▾, Line
  Color ▾, Font Color ▾, Line Style, Dash Style, Arrow Style, Shadow, 3-D**
  (v1 geometry: ~20 autoshapes `[F]`, rest `[C]` placeholders)
- [ ] Fill Effects dialog: Gradient (1/2-color + presets, 6 styles × 4
  variants), Texture, Pattern (48), Picture; Semitransparent
- [ ] Shadow presets (20) + Shadow Settings mini-toolbar; 3-D presets (20)
  + 3-D Settings (tilt/depth/direction/lighting/surface) — 3-D `[C]` v1
- [ ] Text inside any shape; adjustment handles; Edit Points on
  curves/freeforms; grouping; z-order
- [ ] WordArt Gallery (30 styles) → editable shaped text; WordArt toolbar
  (shape palette, rotate, same-heights, vertical, spacing) — `[C]` minimum
- [ ] Picture toolbar: Image Control (Auto/Grayscale/B&W/Watermark),
  contrast/brightness, Crop, Set Transparent Color, Reset
- [ ] Format AutoShape/Object dialog: Colors and Lines / Size / Position /
  Picture / Text Box / Properties-Wrapping (per app)

### S-OFF-05 Common facilities
- [ ] Clip Gallery-style dialog (categories, thumbnails, Insert) over the
  OS image formats; own clip art
- [ ] **Insert → Hyperlink (Ctrl+K) in all apps; blue/underlined, purple
  followed; Web toolbar (Back/Forward/Stop/Refresh/Start Page/Search/
  Favorites/Go/Address) driving the OS browser**
- [ ] Office Open/Save dialogs: Look-in combo, list/details/preview
  views, type filter, MRU on File menu (4 default, 9 max)
- [ ] Paste Special with format list; Paste as Hyperlink; drag-and-drop
  of selections within and between the suite's apps
- [ ] Single shared spell engine + one custom dictionary; shared
  AutoCorrect replacement list (Office-wide)
- [ ] OLE embed/link BETWEEN SUITE APPS: v1 = embedded object renders as
  picture, double-click opens the source app (`[C]` for in-place
  activation with menu merging; document honestly)
- [ ] Tools → Macro (Macros… / Record New Macro… / Visual Basic Editor)
  present-and-stubbed `[C]`; macro-virus prompt N/A `[X]` (no VBA)
- [ ] Save as HTML in all three apps (export only)

### S-OFF-06 File formats (native, via unodoc)
- [ ] **`.doc` Word 97-2003 binary: read (piece table, both text
  encodings, CHPX/PAPX/sprms, STSH, sections, tables, cached fields,
  pictures) + write (accepted by real Word AND LibreOffice with no repair
  prompt)**
- [ ] **`.xls` BIFF8: read (SST incl. mid-string Continue encoding
  switches, XF/FONT/FORMAT, all cell record types, FORMULA ptg decompile,
  shared formulas, 1900/1904 epochs) + write (incl. compiled formulas
  with correct operand classes)**
- [ ] **`.ppt`: read (UserEdit chain → persist directory → live document;
  text from both homes; StyleTextPropAtom) + write (single UserEdit,
  valid PPDrawing per slide, no repair prompt)**
- [F] **CFB container: read with cycle/bounds defense; write with valid
  (length, uppercase-UTF-16) directory ordering** — `unodoc/ud_cfb.c`,
  2026-08-01. Verified by `unodoc/test/run_tests.py`: round-trip across the
  64/512/4096 boundaries, DIFAT overflow, a 7-file LibreOffice corpus read
  and rebuilt through our writer with LibreOffice agreeing the rebuilt
  container is the same document, and 28,000 fuzz mutations under
  ASan/UBSan. (Read covers v3 and v4; write emits v3, as Office 97 does.)
- [ ] Encrypted files (FILEPASS / [MS-OFFCRYPTO]) refused with a clear
  message `[F for the refusal]`
- [ ] Also: `.TXT` and `.RTF` write (Word), `.CSV`/`.TXT` (Excel)
- [X] OOXML `.docx/.xlsx/.pptx` — Office 2007, out of scope v1

---

## 2. Word 97 (S-UOW)

### S-UOW-01 Menu bar — 9 menus, exact trees
The full trees (File / Edit / View / Insert / Format / Tools / Table /
Window / Help) as inventoried in the research report are NORMATIVE,
including separators and submenus; deltas get called out here:
- [ ] **File**: New… (template tabs) · Open · Close · Save · Save As ·
  Save as HTML · Versions… · Page Setup · Print Preview · Print · Send To
  ▸ `[C]` · Properties · MRU · Exit
- [ ] **Edit**: Undo/Redo (named, with toolbar stacks) · Cut/Copy/Paste ·
  Paste Special · Paste as Hyperlink · Clear · Select All · Find ·
  Replace · Go To · Links `[C]` · Object `[C]`
- [ ] **View**: Normal · Online Layout `[C]` ok · Page Layout · Outline ·
  Master Document `[C]` · Toolbars ▸ (13 named + Customize) · Ruler ·
  Document Map · Header and Footer · Footnotes · Comments · Full Screen ·
  Zoom
- [ ] **Insert**: Break · Page Numbers · Date and Time · AutoText ▸ ·
  Field · Symbol · Comment · Footnote · Caption `[C]` · Cross-reference
  `[C]` · Index and Tables `[C]` · Picture ▸ (Clip Art / From File /
  AutoShapes / WordArt / Chart `[C]`) · Text Box · File · Object `[C]` ·
  Bookmark · Hyperlink
- [ ] **Format**: Font · Paragraph · Bullets and Numbering · Borders and
  Shading · Columns · Tabs · Drop Cap · Text Direction `[C]` · Change
  Case · AutoFormat `[C]` · Style Gallery `[C]` · Style · Background ▸
- [ ] **Tools**: Spelling and Grammar (grammar `[C]`) · Language ▸
  (Thesaurus `[C]`, Hyphenation `[C]`) · Word Count · AutoSummarize `[C]`
  · AutoCorrect · Track Changes ▸ `[C]` v1 · Protect Document `[C]` ·
  Mail Merge ▸ `[C]` v1 · Envelopes and Labels `[C]` · Letter Wizard
  `[C]` · Macro ▸ `[C]` · Customize · Options
- [ ] **Table**: Draw Table · Insert Table · Delete Cells · Merge/Split
  Cells · Select Row/Column/Table · Table AutoFormat · Distribute
  Rows/Columns · Cell Height and Width · Headings · Convert Text to
  Table · Sort · Formula `[C]` · Split Table · Gridlines toggle
- [ ] **Window**: New Window · Arrange All · Split · document list
- [ ] **Help**: Help · Contents and Index · What's This? · About

### S-UOW-02 Toolbars
- [ ] **Standard, exact order**: New Open Save | Print PrintPreview
  Spelling | Cut Copy Paste FormatPainter | Undo▾ Redo▾ | Hyperlink Web |
  TablesAndBorders InsertTable(grid) InsertExcelSheet(grid) `[C]`
  Columns(picker) Drawing | DocumentMap ShowHide¶ Zoom Assistant
- [ ] **Formatting, exact order**: Style Font Size | B I U | AlignL Center
  AlignR Justify | Numbering Bullets DecIndent IncIndent | Borders▾
  Highlight▾ FontColor▾
- [ ] Context toolbars: Header and Footer, Print Preview, Outlining,
  Tables and Borders, Picture, Text Box, Reviewing `[C]` v1

### S-UOW-03 Views and window furniture
- [ ] **View buttons at LEFT END of the horizontal scrollbar (Normal /
  Online Layout / Page Layout / Outline)**
- [ ] **Select Browse Object on the vertical scrollbar (12-way palette;
  Previous/Next arrows turn blue for non-page objects)**
- [ ] **Ruler: first-line + hanging indent triangles, left/right indent,
  tab-type selector cycling L/C/R/Decimal, click-to-set tabs, table
  column markers (Alt shows measurements), margin drag in Page Layout**
- [ ] **Status bar: Page n · Sec n · n/N · At/Ln/Col · REC TRK EXT OVR
  cells (double-click toggles) · spell-status book icon**
- [ ] Page Layout: white page on gray pasteboard, page edge + shadow;
  Normal: galley with optional style area
- [ ] Context menus: text / squiggle (suggestions bold, Ignore All, Add,
  AutoCorrect ▸, Spelling…) / table / object variants

### S-UOW-04 Text and formatting engine
- [ ] **Font dialog 3 tabs: fonts, styles, sizes; underlines (single,
  words-only, double, dotted, thick, dash, dot-dash, dot-dot-dash,
  wave); color (16+Auto); effects: strikethrough, double-strike, super/
  subscript, shadow, outline, emboss, engrave, small caps, all caps,
  hidden; Character Spacing (scale, expand/condense, raise/lower, kern);
  Animation tab `[C]`**
- [ ] **Paragraph: alignment (incl. justify), indents (+special
  first/hanging), spacing before/after, line spacing (single/1.5/double/
  at-least/exactly/multiple); Line and Page Breaks tab (widow/orphan,
  keep lines, keep with next, page break before)**
- [ ] **Tabs dialog: position, L/C/R/Decimal/Bar, leaders, default stops**
- [ ] **Borders and Shading: box/shadow/3-D/custom, line styles, Page
  Border tab (art borders `[C]`), Shading fills**
- [ ] **Named styles: paragraph + character, based-on chains, style for
  following paragraph, the built-in set (Normal, Heading 1-9, Body Text,
  TOC/Index families, List families, Header/Footer, Hyperlink...)**
- [ ] Bullets and Numbering: 7+7 preset galleries + customize, Outline
  Numbered `[C]` v1
- [ ] Highlight (15 colors); Change Case dialog; Drop Cap
- [ ] **Find/Replace/Go To: match case, whole words, wildcards, sounds-
  like `[C]`, word forms `[C]`; Format and Special search (^p ^t ^m...);
  Go To by page/section/line/bookmark/footnote/table...**
- [ ] **Undo/redo stack, effectively unbounded, with named entries**

### S-UOW-05 Page model
- [ ] **Sections: Next Page / Continuous / Even / Odd breaks; per-section
  page size, margins (+gutter, mirror), orientation, columns, vertical
  alignment**
- [ ] **Headers/footers: per-section, Different First / Different Odd and
  Even, Same-as-Previous chaining, header/footer editing mode with its
  toolbar (page # / # of pages / date / time / switch / show previous-
  next)**
- [ ] **Columns: presets + up to 12, unequal widths, line between,
  balancing on continuous break, column breaks**
- [ ] **Footnotes/endnotes: auto-number, formats, per-page bottom
  placement, separators; endnotes `[C]` v1 ok**
- [ ] Page numbers (position/format/start-at); Break dialog
- [ ] **Print Preview: multi-page grid, magnifier, Shrink to Fit `[C]`**

### S-UOW-06 Tables
- [ ] **Insert Table (dialog + grid picker) and Draw Table pencil +
  Eraser; per-cell borders/shading; merge/split; row height rules; repeat
  header rows; column drag (plain/Ctrl/Shift semantics *(verify)*);
  AutoFit; distribute evenly; vertical alignment + vertical text
  direction `[C]`; row page-break control**
- [ ] Convert Text↔Table; Sort (3 keys); Table AutoFormat gallery (the 39
  named formats — table of presets, at least 12 `[F]` *(verify roster)*)
- [ ] Table Formula (=SUM(ABOVE) family) `[C]` v1
- [X] Nested tables, wrap-around tables — Word 2000, absent by design

### S-UOW-07 Language tools and automation
- [ ] **Background spelling with red squiggles; Spelling dialog (Ignore/
  Ignore All/Add/Change/Change All/AutoCorrect)**; grammar + green
  squiggles `[C]`; readability stats `[C]`
- [ ] **AutoCorrect: two-initial-caps, capitalize first letter/day names,
  Caps-Lock fix, replace-as-you-type table (with the classic seed
  entries), exceptions lists**
- [ ] AutoFormat As You Type: quotes→smart, ordinals, fractions,
  *bold*/_italic_, auto bulleted/numbered lists, borders on ---,
  URL→hyperlink
- [ ] AutoText with AutoComplete tip (yellow tooltip, Enter accepts)
- [ ] **Fields: PAGE NUMPAGES DATE TIME FILENAME AUTHOR TITLE REF PAGEREF
  SEQ `[F]`; full ~74-type dialog with categories; F9 update, Shift+F9 /
  Alt+F9 code toggles, Ctrl+F9 insert; gray shading option; the rest of
  the types honored as cached results `[C]`**
- [ ] Mail merge (Helper, data source, merge fields, preview, merge to
  new doc) `[C]` v1; Envelopes and Labels `[C]`
- [ ] Track changes / versions / comments / protect `[C]` v1; Document
  Map `[F]`; Word Count `[F]`; AutoSummarize `[C]`
- [ ] Text boxes with linked chains `[C]` v1; wrap styles Square / Top &
  Bottom `[F]`, Tight/Through `[C]`

### S-UOW-08 Options and files
- [ ] Options dialog: View / General / Edit / Print / Save (autorecover,
  passwords `[C]`) / Spelling / User Info / Compatibility `[C]` / File
  Locations
- [ ] **`.doc` open+save (S-OFF-06); `.TXT`; `.RTF` write; UWD import
  from the existing Editor**

---

## 3. Excel 97 (S-UOC)

### S-UOC-01 Menus — 9 menus per the normative trees
- [ ] File (incl. Save Workspace `[C]`, Print Area ▸) · Edit (incl. Fill ▸
  Down/Right/Up/Left/Series/Justify, Clear ▸ All/Formats/Contents/
  Comments, Delete/Delete Sheet/Move or Copy Sheet) · View (incl. **Page
  Break Preview**, Custom Views `[C]`, Full Screen) · Insert (Cells/Rows/
  Columns/Worksheet/Chart/Page Break/Function/Name ▸ (Define/Paste/
  Create/Apply/Label `[C]`)/Comment/Picture ▸/Map `[X]` own-artwork
  stub/Object `[C]`/Hyperlink) · Format (Cells/Row ▸/Column ▸/Sheet ▸/
  AutoFormat/**Conditional Formatting**/Style) · Tools (Spelling/
  AutoCorrect/Share Workbook `[C]`/Track Changes `[C]`/Protection/Goal
  Seek/Scenarios `[C]`/Auditing/Macro `[C]`/Add-Ins `[C]`/Customize/
  Options) · Data (**Sort/Filter ▸ AutoFilter/Advanced `[C]`/Form `[C]`/
  Subtotals/Validation/Table `[C]`/Text to Columns/Consolidate `[C]`/
  Group and Outline/PivotTable `[C]`/Get External Data `[X]`/Refresh**)
  · Window (incl. Freeze Panes, Split, Hide/Unhide) · Help
- [ ] **Chart menu replaces Data while a chart is selected** (Chart Type /
  Source Data / Chart Options / Location / Add Data / Add Trendline `[C]`
  / 3-D View `[C]`)

### S-UOC-02 Toolbars
- [ ] **Standard: New Open Save | Print Preview Spelling | Cut Copy Paste
  FormatPainter | Undo▾ Redo▾ | Hyperlink Web | AutoSum(Σ)
  PasteFunction(fx) SortAsc SortDesc | ChartWizard Map`[X]` Drawing |
  Zoom Assistant**
- [ ] **Formatting: Font Size | B I U | AlignL Center AlignR
  MergeAndCenter | Currency Percent Comma IncDecimal DecDecimal |
  DecIndent IncIndent | Borders▾ FillColor▾ FontColor▾**

### S-UOC-03 Workspace anatomy
- [ ] **Formula bar: Name Box (ref display, names dropdown, type-to-
  define) · Cancel ✗ · Enter ✓ · Edit Formula (=) opening the Formula
  Palette with inline argument help**
- [ ] **In-cell editing; Range Finder (formula references color-coded
  with draggable matching borders); formula-view toggle Ctrl+`**
- [ ] **Fill handle (black square): drag series, right-drag menu,
  double-click fill-down; custom lists; AutoComplete; Pick From List**
- [ ] **Sheet tabs + scroll buttons; rename by double-click; drag
  reorder, Ctrl-drag copy; tab context menu; default 3 sheets**
- [ ] **Grid: 65,536 rows × 256 columns (A-IV); Select All corner;
  marching-ants copy marquee; heavy selection border + interior tint**
- [ ] **Status bar: mode (Ready/Enter/Edit/Point) · AutoCalculate well
  (right-click: Average/Count/Count Nums/Max/Min/Sum) · CAPS NUM SCRL
  END FIX OVR CIRC cells**
- [ ] **Freeze Panes (thin line) vs Split (thick bars); split boxes on
  the scrollbars**
- [ ] Cell context menu (Cut/Copy/Paste/Paste Special/Insert/Delete/
  Clear Contents/Insert Comment/Format Cells/Pick From List/Hyperlink);
  header menus with Height/Width/Hide/Unhide

### S-UOC-04 Cell model and formatting
- [ ] Types: IEEE double, text, bool, errors (#DIV/0! #N/A #NAME? #NULL!
  #NUM! #REF! #VALUE!); dates as serials, **1900 (with the Lotus
  leap-year bug, faithfully) and 1904 systems**
- [ ] **Format Cells 6 tabs: Number (General, Number, Currency,
  Accounting, Date, Time, Percentage, Fraction, Scientific, Text,
  Special, Custom with the FULL format-code language incl. sections,
  colors, conditions); Alignment (horizontal incl. Fill/Justify/Center
  Across, vertical, orientation dial -90..+90 + vertical stack, indent,
  wrap, shrink-to-fit, merge); Font; Border (13 styles, diagonals
  *(verify)*); Patterns (56-color palette); Protection `[C]`**
- [ ] **Conditional formatting: up to 3 conditions, Cell Value Is /
  Formula Is, font/border/pattern results**
- [ ] AutoFormat (17 named table formats); named cell styles (Normal,
  Comma, Currency, Percent + user)
- [ ] **Comments: red corner triangle, hover popup, print options `[C]`**
- [ ] Editable 56-color workbook palette (Options → Color) *(verify)*

### S-UOC-05 Formulas and calculation
- [ ] Operators incl. range `:`, union `,`, intersection ` `; R1C1
  option; 3-D refs `Sheet1:Sheet3!A1`; external refs `[C]` v1; array
  formulas (Ctrl+Shift+Enter); natural-language labels `[C]`
- [ ] **Recalc: automatic/manual, iteration settings, circular detection
  + CIRC indicator; dependency-driven partial recalc**
- [ ] **Paste Function dialog (categories + MRU) and the Formula
  Palette; AutoSum; Formula AutoCorrect `[C]`**
- [ ] Names: Define/Create/Paste/Apply; Go To Special (blanks,
  constants, formulas, last cell, precedents/dependents...)
- [ ] Auditing arrows (precedents/dependents/error) `[C]` v1

### S-UOC-06 The function set — NORMATIVE list
All names below `[F]` = implemented with Excel-97-faithful semantics and
fixture coverage. ATP functions behave as "add-in installed".
- [ ] **Financial (16)**: DB DDB FV IPMT IRR ISPMT MIRR NPER NPV PMT PPMT
  PV RATE SLN SYD VDB
- [ ] ATP financial (37): ACCRINT ACCRINTM AMORDEGRC AMORLINC COUPDAYBS
  COUPDAYS COUPDAYSNC COUPNCD COUPNUM COUPPCD CUMIPMT CUMPRINC DISC
  DOLLARDE DOLLARFR DURATION EFFECT FVSCHEDULE INTRATE MDURATION NOMINAL
  ODDFPRICE ODDFYIELD ODDLPRICE ODDLYIELD PRICE PRICEDISC PRICEMAT
  RECEIVED TBILLEQ TBILLPRICE TBILLYIELD XIRR XNPV YIELD YIELDDISC
  YIELDMAT
- [ ] **Date/Time (14)**: DATE DATEVALUE DAY DAYS360 HOUR MINUTE MONTH
  NOW SECOND TIME TIMEVALUE TODAY WEEKDAY YEAR; ATP (6): EDATE EOMONTH
  NETWORKDAYS WEEKNUM WORKDAY YEARFRAC
- [ ] **Math/Trig (51)**: ABS ACOS ACOSH ASIN ASINH ATAN ATAN2 ATANH
  CEILING COMBIN COS COSH COUNTIF DEGREES EVEN EXP FACT FLOOR INT LN LOG
  LOG10 MDETERM MINVERSE MMULT MOD ODD PI POWER PRODUCT RADIANS RAND
  ROMAN ROUND ROUNDDOWN ROUNDUP SIGN SIN SINH SQRT SUBTOTAL SUM SUMIF
  SUMPRODUCT SUMSQ SUMX2MY2 SUMX2PY2 SUMXMY2 TAN TANH TRUNC; ATP (9):
  FACTDOUBLE GCD LCM MROUND MULTINOMIAL QUOTIENT RANDBETWEEN SERIESSUM
  SQRTPI
- [ ] **Statistical (78)**: AVEDEV AVERAGE AVERAGEA BETADIST BETAINV
  BINOMDIST CHIDIST CHIINV CHITEST CONFIDENCE CORREL COUNT COUNTA
  COUNTBLANK CRITBINOM DEVSQ EXPONDIST FDIST FINV FISHER FISHERINV
  FORECAST FREQUENCY FTEST GAMMADIST GAMMAINV GAMMALN GEOMEAN GROWTH
  HARMEAN HYPGEOMDIST INTERCEPT KURT LARGE LINEST LOGEST LOGINV
  LOGNORMDIST MAX MAXA MEDIAN MIN MINA MODE NEGBINOMDIST NORMDIST
  NORMINV NORMSDIST NORMSINV PEARSON PERCENTILE PERCENTRANK PERMUT
  POISSON PROB QUARTILE RANK RSQ SKEW SLOPE SMALL STANDARDIZE STDEV
  STDEVA STDEVP STDEVPA STEYX TDIST TINV TREND TRIMMEAN TTEST VAR VARA
  VARP VARPA WEIBULL ZTEST
- [ ] **Lookup/Reference (16)**: ADDRESS AREAS CHOOSE COLUMN COLUMNS
  HLOOKUP HYPERLINK INDEX INDIRECT LOOKUP MATCH OFFSET ROW ROWS
  TRANSPOSE VLOOKUP
- [ ] **Database (12)**: DAVERAGE DCOUNT DCOUNTA DGET DMAX DMIN DPRODUCT
  DSTDEV DSTDEVP DSUM DVAR DVARP
- [ ] **Text (23)**: CHAR CLEAN CODE CONCATENATE DOLLAR EXACT FIND FIXED
  LEFT LEN LOWER MID PROPER REPLACE REPT RIGHT SEARCH SUBSTITUTE T TEXT
  TRIM UPPER VALUE
- [ ] **Logical (6)**: AND FALSE IF NOT OR TRUE
- [ ] **Information (16)**: CELL ERROR.TYPE INFO ISBLANK ISERR ISERROR
  ISLOGICAL ISNA ISNONTEXT ISNUMBER ISREF ISTEXT N NA TYPE (+ISEVEN
  ISODD via ATP)
- [ ] Engineering ATP (39): BESSELI BESSELJ BESSELK BESSELY BIN2DEC
  BIN2HEX BIN2OCT COMPLEX CONVERT DEC2BIN DEC2HEX DEC2OCT DELTA ERF ERFC
  GESTEP HEX2BIN HEX2DEC HEX2OCT IMABS IMAGINARY IMARGUMENT IMCONJUGATE
  IMCOS IMDIV IMEXP IMLN IMLOG10 IMLOG2 IMPOWER IMPRODUCT IMREAL IMSIN
  IMSQRT IMSUB IMSUM OCT2BIN OCT2DEC OCT2HEX
- [X] AVERAGEIF(S) SUMIFS COUNTIFS IFERROR GETPIVOTDATA XLOOKUP dynamic
  arrays — later Excel, absent by design (and rejected by the parser
  with #NAME?, as 97 would)

### S-UOC-07 Data features
- [ ] **Sort: 3 keys, header detection, custom orders, left-to-right;
  toolbar quick sort**
- [ ] **AutoFilter: per-column dropdowns — (All), (Top 10…), (Custom… 2
  criteria AND/OR + wildcards), values, (Blanks)/(NonBlanks); blue
  arrows + blue row numbers**
- [ ] **Data Validation: whole/decimal/list/date/time/length/custom,
  input message, error alert styles, in-cell dropdown**
- [ ] Subtotals (11 functions, nesting); Group/Outline (8 levels, auto,
  symbols); Text to Columns wizard; Data Form `[C]`; Consolidate `[C]`;
  Data Table what-if `[C]`; Goal Seek `[F]`; Scenarios `[C]`; Solver
  `[X]` add-in
- [ ] PivotTables `[C]` v1 (menu + wizard shell present, honest stub)
- [ ] Shared workbooks / track changes / merge `[C]`; protection
  (sheet/workbook, Locked/Hidden cells) `[F]` minus passwords `[C]`

### S-UOC-08 Charts
- [ ] **Chart Wizard 4 steps (Type w/ press-and-hold sample → Source
  Data w/ rows-columns switch and per-series ranges → Options tabs
  (Titles/Axes/Gridlines/Legend/Data Labels/Data Table) → Location
  (sheet object / chart sheet))**
- [ ] **v1 `[F]` types: Column, Bar, Line, Pie, XY Scatter, Area,
  Doughnut; their 97 sub-types; series formulas; drag-a-point value edit
  for simple types *(verify)* `[C]` ok**
- [ ] `[C]` v1: Radar, Surface, Bubble, Stock, Cylinder/Cone/Pyramid,
  Custom Types tab, trendlines, error bars, secondary axis, time-scale
  axes, 3-D View
- [ ] Chart toolbar (objects combo, chart type tear-off, legend, data
  table, by-row/col, angle text); right-click Format … dialogs with
  Fill Effects

### S-UOC-09 Page setup and print
- [ ] **Page (orientation, Adjust-to %, Fit-to N×M pages, paper, first
  page number) · Margins (+center on page) · Header/Footer (built-ins +
  custom 3-pane editor with &P &N &D &T &F &A codes) · Sheet (print
  area, repeat rows/cols, gridlines, B&W, draft, headings, comments,
  page order)**
- [ ] **Page Break Preview with draggable blue breaks**; Print dialog
  (selection/sheets/workbook) `[C]` until unoprint; Print Preview with
  margin drag `[F]` as render
- [ ] Options tabs: View/Calculation/Edit/General/Transition `[C]`/
  Custom Lists/Chart/Color

---

## 4. PowerPoint 97 (S-UOS)

### S-UOS-01 Menus — 9 menus per the normative trees
- [ ] File (incl. Pack and Go `[C]`, Send To ▸ Word `[C]`) · Edit (incl.
  Duplicate Ctrl+D, Delete Slide) · View (**Slide / Outline / Slide
  Sorter / Notes Page / Slide Show; Master ▸ 4 masters; Black and
  White; Slide Miniature; Speaker Notes; Ruler; Guides; Header and
  Footer**) · Insert (**New Slide Ctrl+M, Slide Number, Date and Time,
  Symbol, Slides from Files `[C]` / from Outline `[C]`, Picture ▸,
  Text Box, Movies and Sounds ▸ (WAV via unomedia; movies `[C]`),
  Chart `[C]`, Object `[C]`, Hyperlink**) · Format (**Font, Bullet,
  Alignment ▸, Line Spacing, Change Case, Replace Fonts, Slide Layout,
  Slide Color Scheme, Background, Apply Design, Colors and Lines**) ·
  Tools (Spelling, Style Checker `[C]`, AutoCorrect, Meeting Minder
  `[C]`, Expand Slide `[C]`, Macro `[C]`, Options) · **Slide Show (View
  Show F5, Rehearse Timings `[C]` v1, Record Narration `[C]`, Set Up
  Show, Action Buttons ▸ 12, Action Settings, Preset Animation ▸,
  Custom Animation, Animation Preview, Slide Transition, Hide Slide,
  Custom Shows `[C]` v1)** · Window · Help

### S-UOS-02 Toolbars and furniture
- [ ] **Standard**: New Open Save | Print Spelling | Cut Copy Paste
  FormatPainter | Undo▾ Redo▾ | Hyperlink Web | InsertWordTable `[C]`
  InsertExcelSheet `[C]` InsertChart `[C]` InsertClipArt | New Slide…
  Slide Layout… Apply Design… B&W View | Zoom Assistant
- [ ] **Formatting**: Font Size | B I U TextShadow | AlignL Center
  AlignR | Bullets | IncFontSize DecFontSize | Promote Demote |
  Animation Effects
- [ ] **Common Tasks floater (New Slide… / Slide Layout… / Apply
  Design…) shown by default**
- [ ] Drawing toolbar docked bottom (S-OFF-04); Outlining toolbar in
  Outline view; **Slide Sorter toolbar (Transition dialog btn,
  Transition Effects combo, Preset Animation combo, Hide Slide,
  Rehearse `[C]`, Summary Slide `[C]`, Show Formatting)**
- [ ] **Status bar: Slide n of N · template name (double-click = Apply
  Design) ·** view buttons lower-left
- [ ] Ruler + center-zero slide coords; Guides (draggable, position
  readout, Ctrl-drag duplicates)

### S-UOS-03 Authoring
- [ ] **24 AutoLayouts (the exact New Slide grid: Title Slide, Bulleted
  List, 2 Column Text, Table `[C]`, Text&Chart `[C]`, Chart&Text `[C]`,
  Org Chart `[C]`, Chart `[C]`, Text&ClipArt, ClipArt&Text, Title Only,
  Blank, Text&Object..., 4 Objects — object variants `[C]` where the
  object type is)**
- [ ] **Placeholders (click-to-add), text boxes, shapes, pictures;
  per-paragraph bullets (Format → Bullet: any character, color, size
  %), 5 outline indent levels, line spacing, alignment; Text Shadow /
  Emboss; Replace Fonts; Expand Slide `[C]`**
- [ ] **Masters: Slide + Title pair, Notes, Handout; per-slide "omit
  background graphics"; Header and Footer dialog (date auto/fixed,
  footer, number, not-on-title)**
- [ ] **Color schemes: 8 roles (Background, Text and lines, Shadows,
  Title text, Fills, Accent, Accent+hyperlink, Accent+followed);
  Standard + Custom tabs; Apply / Apply to All; scheme colors = the
  first 8 swatches in every color dropdown**
- [ ] **Background dialog (scheme color / Fill Effects; omit master
  graphics; Apply/Apply to All); Black and White view with per-object
  B&W override**
- [ ] Design templates: own-artwork set (≥8) presented like the 97
  Presentation Designs tab; AutoContent Wizard `[C]`
- [ ] Spelling squiggles; Style Checker `[C]`; Find/Replace;
  AutoCorrect

### S-UOS-04 Show engine
- [ ] **Set Up Show: speaker full-screen / individual window `[C]` /
  kiosk loop; loop until Esc; without narration/animation; slide
  range; advance manual vs timings; pen color**
- [ ] **Transitions v1 `[F]`: No Transition, Cut, Cut Through Black,
  Dissolve, Fade Through Black, Wipe ×4, Blinds H/V, Box In/Out,
  Checkerboard ×2, Random Bars H/V, Cover ×8, Uncover ×8, Split ×4,
  Strips ×4, Random — with Slow/Medium/Fast and the dialog's preview
  picture; per-slide sound `[C]`; advance on click / after N sec**
- [ ] **Builds (Custom Animation) v1 `[F]`: Appear, Fly From ×8, Peek
  ×4, Wipe ×4, Blinds, Box, Checkerboard, Dissolve, Random Bars,
  Split, Strips, Zoom family `[C]`, Spiral `[C]`, Swivel `[C]`,
  Stretch `[C]`, Crawl `[C]`, Flash Once; by All at once / By Word /
  By Letter; grouped by paragraph level 1-5; reverse order; After
  animation: dim color / hide / hide on click; Animation Order list;
  Timing tab (on click / auto after N); Animation Preview window;
  the Preset Animation menu + Animation Effects toolbar mapped to
  their canned combos (sounds `[C]`)**
- [ ] Chart Effects and Play Settings tabs `[C]`
- [ ] **Action Settings (Click / Mouse Over): Hyperlink to Next/
  Previous/First/Last/Last Viewed/End Show/Slide…/URL `[C]`/Custom
  Show `[C]`; Run program `[X]`; Play sound `[C]`; Highlight click**;
  Action Buttons 12-palette
- [ ] **In-show: click/Space/arrows/PgUp-PgDn advance; right-click menu
  (Next, Previous, Go ▸ Slide Navigator / By Title, Meeting Minder
  `[C]`, Speaker Notes, Pen, Pointer Options, Screen ▸ Pause/Black/
  Erase Pen, End Show); pen drawing Ctrl+P, erased on slide change;
  B black screen; Esc out; hidden slides skipped**
- [ ] Rehearse Timings `[C]` v1 → `[F]` v2; Custom Shows `[C]` v1
- [ ] **Print what: Slides / Handouts 2-3-6 / Notes Pages / Outline;
  B&W and Pure B&W; frame slides `[C]` — as page RENDERS (print
  itself gated on unoprint)**
- [ ] Page Setup: On-screen Show / Letter / A4 / 35mm / Overhead /
  Banner / Custom; numbering start; orientations
- [ ] Notes Page view + Speaker Notes floater; Send-to-Word,
  Genigraphics, Two-Screens, Presentation Conference `[X]` era
  plumbing, honest About-box note

---

## 5. Outlook 97 (S-UOM) — deferred module, spec'd for later

Recorded so the conditional scope has a yardstick when phase M runs:
Outlook Bar (Outlook/Mail/Other groups) · folder list · table/card/
timeline/day-week-month views with field chooser, grouping, sort,
filter · Inbox (AutoPreview 3 lines, flags, importance) · rich-text
compose (no HTML mail — that is Outlook 98) · Calendar (day/work-week/
week/month, date navigator with bold busy days, TaskPad, recurrence,
reminders) · Contacts (cards, many-field form, dial `[X]`) · Tasks
(due/status/%/recurrence) · Journal `[C]` · Notes (yellow stickies) ·
Categories · Import/export. Auth per plan §8: app passwords + Microsoft
device-code OAuth only.

---

## 6. Explicitly absent everywhere (Office 2000+ features)

Adaptive menus · multi-item Office Clipboard · HTML round-trip and Web
Layout · XML formats · task panes · smart tags · Ribbon · Word nested
tables/click-and-type · PivotCharts · >3 conditional formats · >65k rows
· PPT tri-pane Normal view · multiple slide masters · motion paths ·
digital signatures · activation. A clone that accidentally implements
one of these has left 1997 — file it as a fidelity bug.
