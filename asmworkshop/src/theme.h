/* theme.h — бело-синяя палитра ASM Workshop (GTK3 CSS). */
#ifndef THEME_H
#define THEME_H

static const char *THEME_CSS =
"window { background:#f4f8fd; color:#0c3a66; }"
"label { color:#123c69; font-family:'Inter','Segoe UI',sans-serif; }"
".brand { font-size:22px; font-weight:800; letter-spacing:2px; color:#1d4ed8; }"
".sub  { font-size:11px; color:#5b7ba6; letter-spacing:1px; }"
".card { background:#ffffff; border-radius:14px; border:1px solid #cfe0f5;"
"  box-shadow:0 2px 8px rgba(29,78,216,0.08); }"
".cardtitle { font-size:11px; font-weight:700; letter-spacing:2px; color:#2563eb; }"
"textview, textview text { background:#f8fbff; color:#0f2f55;"
"  font-family:'JetBrains Mono','Cascadia Code','Fira Code',monospace; font-size:14px;"
"  border-radius:8px; border:1px solid #dbeafe; }"
"textview.out text { color:#1e40af; background:#eef5ff; }"
"button { background:#dbeafe; color:#1e3a8a; border:none; border-radius:10px;"
"  padding:9px 18px; font-weight:700; }"
"button:hover { background:#bfdbfe; }"
"button.primary { background:#2563eb; color:#ffffff; }"
"button.primary:hover { background:#1d4ed8; }"
"button.flat { background:transparent; color:#5b7ba6; padding:4px 10px; }"
"button.flat:hover { color:#1d4ed8; background:#eaf2fd; }"
"combobox, combobox box { background:#dbeafe; color:#1e3a8a; border-radius:8px; }"
"notebook header { background:transparent; }"
"separator { background:#cfe0f5; min-height:1px; }"
".note { color:#92400e; font-size:12px; }"
".err  { color:#b91c1c; font-size:12px; }"
".ok   { color:#15803d; font-size:12px; }"
"treeview { background:#ffffff; color:#0f2f55;"
"  font-family:'JetBrains Mono','Cascadia Code',monospace; font-size:13px; }"
"treeview header button { background:#e0ecfb; color:#1e3a8a; font-weight:700;"
"  border:1px solid #cfe0f5; padding:4px 8px; }"
"treeview:selected { background:#2563eb; color:#ffffff; }";

#endif
