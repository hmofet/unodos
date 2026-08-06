# LOGVIEW.PY - the system log viewer, packaged as APPS\LOGVIEW.UNO.
#
# A PYAPP rather than a native .UNO module for a lane reason worth writing down:
# giving an app a desktop icon means editing pc64_uui.c in a dozen places (an
# EX_ id, two name tables, the hidden test, the icon, the launch dispatch, the
# key and frame routing), and that file belongs to the toolkits lane. A PYAPP
# needs none of it - it rides the EX_PYAPP host that already exists - so the
# whole of unolog stays inside one lane. A request for a desktop slot is filed;
# until it lands this opens from Files like DUUM.UNO does.
#
# It reads the RING, not the file: the ring is what is current and the file is
# a copy of it that lags by up to a flush interval. What the ring has already
# lost to wrapping is genuinely gone from this view, and the footer says so
# rather than presenting a partial list as a complete one.

import uno

ROW_H = 13

SEV = ("emerg", "alert", "crit", "err", "warning", "notice", "info", "debug")
FAC = ("kernel", "net", "storage", "browser", "ui", "security", "app", "remote")

# Colours picked so the eye finds the bad line without reading: scanning a log
# is looking for trouble, and a wall of one colour makes that a reading task.
C_BAD  = 0xFF3838E0        # emerg..err   (BGRA)
C_WARN = 0xFF20A0E0        # warning
C_TEXT = 0xFF303030        # notice/info
C_DIM  = 0xFF909090        # debug
C_BG   = 0xFFF8F8F8
C_BAR  = 0xFFE8E8E8


def sev_col(s):
    if s <= 3:  return C_BAD
    if s == 4:  return C_WARN
    if s >= 7:  return C_DIM
    return C_TEXT


def two(n):
    return ("0" + str(n))[-2:]


def stamp(ms, wall):
    # No clock -> uptime, and SAY it is uptime. A bare number that might be
    # either makes two machines' logs unmergeable.
    if wall:
        d = wall // 86400
        r = wall % 86400
        return "%s:%s:%s" % (two(r // 3600), two((r % 3600) // 60), two(r % 60))
    return "+%d.%03ds" % (ms // 1000, ms % 1000)


class LogView:
    def __init__(self):
        self.top = 0
        self.follow = True
        self.fac = -1              # -1 = every facility
        self.rows = 20

    # ---- the list ---------------------------------------------------------
    def draw(self, cv):
        w, h = cv.width(), cv.height()
        cv.clear(C_BG)
        self.rows = max(1, (h - 56) // ROW_H)

        first, nxt = uno.log_span()
        total = nxt - first
        if self.follow:
            self.top = max(0, total - self.rows)
        self.top = max(0, min(self.top, max(0, total - 1)))

        y = 6
        shown = 0
        seq = first + self.top
        while seq < nxt and shown < self.rows:
            r = uno.log_read(seq)
            seq += 1
            if r is None:
                continue
            sev, fac, ms, wall, text, src = r
            if self.fac >= 0 and fac != self.fac:
                continue
            line = "%s %-7s %-8s %s%s" % (
                stamp(ms, wall), SEV[sev], FAC[fac],
                ("[" + src + "] ") if src else "", text)
            cv.text(6, y, line, sev_col(sev))
            y += ROW_H
            shown += 1

        if shown == 0:
            cv.text(6, 6, "(nothing at this level yet)", C_DIM)

        # ---- the footer ---------------------------------------------------
        cv.fill_rect(0, h - 48, w, 48, C_BAR)
        lvl = uno.log_level()
        n, dropped, sent, rcvd = uno.log_stat()
        cv.text(6, h - 42,
                "keep <= %s    %s    facility: %s" % (
                    SEV[lvl],
                    "following" if self.follow else "paused",
                    "all" if self.fac < 0 else FAC[self.fac]),
                C_TEXT)
        # The ring is finite and this is a window onto it. Showing a subset
        # without saying so teaches you to trust a complete-looking list.
        cv.text(6, h - 28,
                "records %d, in memory %d, dropped %d, sent %d, received %d"
                % (n, nxt - first, dropped, sent, rcvd), C_DIM)
        cv.text(6, h - 14,
                "-/+ level   f follow   t facility   w write   "
                "arrows/PgUp/PgDn scroll", C_DIM)

    # ---- input ------------------------------------------------------------
    def key(self, uni, scan, ctrl):
        # UnoDOS scancodes: Up=1 Down=2 Right=3 Left=4 Home=5 End=6 PgUp=9 PgDn=10
        ch = chr(uni) if 32 <= uni < 127 else ""
        if scan == 1:    self.follow = False; self.top -= 1
        elif scan == 2:  self.follow = False; self.top += 1
        elif scan == 9:  self.follow = False; self.top -= self.rows
        elif scan == 10: self.follow = False; self.top += self.rows
        elif scan == 5:  self.follow = False; self.top = 0
        elif scan == 6:  self.follow = True
        elif ch == "-":
            uno.log_level(max(0, uno.log_level() - 1)); uno.log_save()
        elif ch in ("+", "="):
            uno.log_level(min(7, uno.log_level() + 1)); uno.log_save()
        elif ch in ("f", "F"):
            self.follow = not self.follow
        elif ch in ("t", "T"):
            self.fac = -1 if self.fac + 1 >= len(FAC) else self.fac + 1
        elif ch in ("w", "W"):
            uno.log_flush()
        else:
            return False
        return True

    # Redraw only while following. A reader who has scrolled back is reading,
    # and repainting under them drags the text away mid-sentence.
    def tick(self):
        if not self.follow:
            return False
        n = uno.log_span()[1]
        if n == getattr(self, "_seen", None):
            return False
        self._seen = n
        return True


app = LogView()
