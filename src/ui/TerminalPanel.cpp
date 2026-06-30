#include "TerminalPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSocketNotifier>
#include <QStringList>
#include <QUrl>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>

#include <vterm.h>

// Trampoline wrappers matching VTermScreenCallbacks signatures
namespace {

// Decode a screen cell's codepoints into a QString (empty for a blank cell).
QString cellChars(const VTermScreenCell &cell) {
  if (cell.chars[0] == 0)
    return QString();
  QString s = QString::fromUcs4(cell.chars, 1);
  for (int i = 1; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
    s += QString::fromUcs4(&cell.chars[i], 1);
  return s;
}

int cb_damage(VTermRect rect, void *user) {
  return TerminalView::onDamage(rect.start_row, rect.end_row, rect.start_col,
                                rect.end_col, user);
}

int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
  return TerminalView::onMoveCursor(pos.row, pos.col, oldpos.row, oldpos.col,
                                    visible, user);
}

int cb_settermprop(VTermProp prop, VTermValue *val, void *user) {
  return TerminalView::onSetTermProp(static_cast<int>(prop), val, user);
}

int cb_bell(void *user) { return TerminalView::onBell(user); }

int cb_resize(int rows, int cols, void *user) {
  return TerminalView::onResize(rows, cols, user);
}

int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
  return TerminalView::onSbPushLine(cols, cells, user);
}

int cb_sb_popline(int cols, VTermScreenCell *cells, void *user) {
  return TerminalView::onSbPopLine(cols, cells, user);
}

static const VTermScreenCallbacks kScreenCallbacks = {
    cb_damage,      nullptr,        cb_movecursor, cb_settermprop,
    cb_bell,        cb_resize,      cb_sb_pushline, cb_sb_popline,
    nullptr,        nullptr,
};

} // namespace

TerminalView::TerminalView(QWidget *parent) : QAbstractScrollArea(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_InputMethodEnabled);
  setAttribute(Qt::WA_OpaquePaintEvent);

  mFont = QFont(QStringLiteral("Menlo, Monaco, Courier New"));
  mFont.setStyleHint(QFont::Monospace);
  mFont.setPointSize(12);
  mFont.setFixedPitch(true);

  QFontMetricsF fm(mFont);
  mCellW = qRound(fm.horizontalAdvance('M'));
  mCellH = qRound(fm.height());
  mBaseline = qRound(fm.ascent());

  viewport()->setCursor(Qt::IBeamCursor);

  QPalette pal = palette();
  pal.setColor(QPalette::Window, QColor("#1e1e1e"));
  setPalette(pal);
  viewport()->setAutoFillBackground(false);

  verticalScrollBar()->setSingleStep(1);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int val) {
            int maxSb = static_cast<int>(mScrollback.size());
            mScrollOffset = maxSb - val;
            viewport()->update();
          });

  // Cursor blink
  mBlinkTimer.setInterval(530);
  connect(&mBlinkTimer, &QTimer::timeout, this, [this] {
    mBlinkState = !mBlinkState;
    viewport()->update();
  });

  // Create vterm
  mVt = vterm_new(mRows, mCols);
  vterm_set_utf8(mVt, 1);

  vterm_output_set_callback(mVt, onOutput, this);

  mVtScreen = vterm_obtain_screen(mVt);
  vterm_screen_set_callbacks(mVtScreen, &kScreenCallbacks, this);
  vterm_screen_set_damage_merge(mVtScreen, VTERM_DAMAGE_SCROLL);
  vterm_screen_enable_altscreen(mVtScreen, 1);
  vterm_screen_enable_reflow(mVtScreen, true);
  vterm_screen_reset(mVtScreen, 1);

  // Set default colors
  VTermState *state = vterm_obtain_state(mVt);
  VTermColor fg, bg;
  vterm_color_rgb(&fg, 0xd4, 0xd4, 0xd4);
  vterm_color_rgb(&bg, 0x1e, 0x1e, 0x1e);
  vterm_state_set_default_colors(state, &fg, &bg);
}

TerminalView::~TerminalView() {
  if (mNotifier) {
    mNotifier->setEnabled(false);
    delete mNotifier;
    mNotifier = nullptr;
  }
  if (mPtyFd >= 0) {
    ::close(mPtyFd);
    mPtyFd = -1;
  }
  if (mChildPid > 0) {
    kill(mChildPid, SIGHUP);
    waitpid(mChildPid, nullptr, WNOHANG);
    mChildPid = -1;
  }
  if (mVt) {
    vterm_free(mVt);
    mVt = nullptr;
  }
}

void TerminalView::setWorkingDirectory(const QString &dir) {
  mWorkDir = dir;
  if (mPtyFd < 0)
    spawnShell();
}

void TerminalView::spawnShell() {
  recalcGeometry();

  struct winsize ws;
  ws.ws_row = mRows;
  ws.ws_col = mCols;
  ws.ws_xpixel = mCols * mCellW;
  ws.ws_ypixel = mRows * mCellH;

  pid_t pid = forkpty(&mPtyFd, nullptr, nullptr, &ws);
  if (pid < 0)
    return;

  if (pid == 0) {
    if (!mWorkDir.isEmpty())
      chdir(mWorkDir.toLocal8Bit().constData());

    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "en_US.UTF-8", 1);
    unsetenv("COLUMNS");
    unsetenv("LINES");

    const char *shell = getenv("SHELL");
    if (!shell)
      shell = "/bin/zsh";

    execlp(shell, shell, "--login", nullptr);
    _exit(1);
  }

  mChildPid = pid;
  mNotifier = new QSocketNotifier(mPtyFd, QSocketNotifier::Read, this);
  connect(mNotifier, &QSocketNotifier::activated, this,
          &TerminalView::onPtyReadable);
  mBlinkTimer.start();
}

void TerminalView::onPtyReadable() {
  char buf[16384];
  ssize_t n = read(mPtyFd, buf, sizeof(buf));
  if (n <= 0) {
    if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
      mNotifier->setEnabled(false);
    }
    return;
  }

  vterm_input_write(mVt, buf, n);
  vterm_screen_flush_damage(mVtScreen);
  updateScrollbar();
  viewport()->update();
}

void TerminalView::writeToShell(const QByteArray &data) {
  if (mPtyFd >= 0)
    ::write(mPtyFd, data.constData(), data.size());
}

void TerminalView::recalcGeometry() {
  QSize vp = viewport()->size();
  int newCols = qMax(2, vp.width() / mCellW);
  int newRows = qMax(1, vp.height() / mCellH);

  if (newRows != mRows || newCols != mCols) {
    mRows = newRows;
    mCols = newCols;
    vterm_set_size(mVt, mRows, mCols);
    vterm_screen_flush_damage(mVtScreen);

    if (mPtyFd >= 0) {
      struct winsize ws;
      ws.ws_row = mRows;
      ws.ws_col = mCols;
      ws.ws_xpixel = mCols * mCellW;
      ws.ws_ypixel = mRows * mCellH;
      ioctl(mPtyFd, TIOCSWINSZ, &ws);
    }
  }
}

void TerminalView::updateScrollbar() {
  int sbSize = static_cast<int>(mScrollback.size());
  QScrollBar *sb = verticalScrollBar();
  sb->setRange(0, sbSize);
  sb->setPageStep(mRows);
  if (mScrollOffset == 0)
    sb->setValue(sbSize);
}

QColor TerminalView::vtermColorToQt(const VTermScreenCell &cell,
                                    bool foreground) const {
  VTermColor col = foreground ? cell.fg : cell.bg;

  if (VTERM_COLOR_IS_DEFAULT_FG(&col))
    return QColor(0xd4, 0xd4, 0xd4);
  if (VTERM_COLOR_IS_DEFAULT_BG(&col))
    return QColor(0x1e, 0x1e, 0x1e);

  if (VTERM_COLOR_IS_INDEXED(&col)) {
    VTermColor rgb = col;
    vterm_screen_convert_color_to_rgb(mVtScreen, &rgb);
    return QColor(rgb.rgb.red, rgb.rgb.green, rgb.rgb.blue);
  }

  return QColor(col.rgb.red, col.rgb.green, col.rgb.blue);
}

QSize TerminalView::sizeHint() const { return QSize(mCols * mCellW + 2, mRows * mCellH + 2); }

void TerminalView::paintEvent(QPaintEvent *) {
  QPainter p(viewport());
  p.setFont(mFont);

  QColor defaultBg(0x1e, 0x1e, 0x1e);
  p.fillRect(viewport()->rect(), defaultBg);

  // Selection bounds in reading order.
  QPoint selA = mSelAnchor, selB = mSelHead;
  if (selA.y() > selB.y() || (selA.y() == selB.y() && selA.x() > selB.x()))
    std::swap(selA, selB);
  const QColor selectionBg(0x33, 0x4e, 0x73);

  for (int row = 0; row < mRows; ++row) {
    int screenRow = row;
    bool fromScrollback = false;
    int sbIdx = -1;

    if (mScrollOffset > 0) {
      int sbSize = static_cast<int>(mScrollback.size());
      int sbRow = row - (mRows - mScrollOffset);
      if (sbRow < 0) {
        // This row is from scrollback
        sbIdx = sbSize - mScrollOffset + row;
        if (sbIdx < 0 || sbIdx >= sbSize)
          continue;
        fromScrollback = true;
      } else {
        screenRow = sbRow;
      }
    }

    for (int col = 0; col < mCols;) {
      VTermScreenCell cell;
      memset(&cell, 0, sizeof(cell));

      if (fromScrollback) {
        const auto &sbLine = mScrollback[sbIdx];
        if (col < static_cast<int>(sbLine.cells.size()))
          cell = sbLine.cells[col];
        else {
          cell.chars[0] = ' ';
          cell.width = 1;
          VTermColor dfg, dbg;
          vterm_color_rgb(&dfg, 0xd4, 0xd4, 0xd4);
          vterm_color_rgb(&dbg, 0x1e, 0x1e, 0x1e);
          dfg.type |= VTERM_COLOR_DEFAULT_FG;
          dbg.type |= VTERM_COLOR_DEFAULT_BG;
          cell.fg = dfg;
          cell.bg = dbg;
        }
      } else {
        VTermPos pos = {screenRow, col};
        vterm_screen_get_cell(mVtScreen, pos, &cell);
      }

      int cellWidth = cell.width;
      if (cellWidth < 1)
        cellWidth = 1;

      int x = col * mCellW;
      int y = row * mCellH;

      // Background
      QColor bg = vtermColorToQt(cell, false);
      QColor fg = vtermColorToQt(cell, true);

      if (cell.attrs.reverse)
        std::swap(fg, bg);

      if (bg != defaultBg)
        p.fillRect(x, y, cellWidth * mCellW, mCellH, bg);

      // Selection highlight.
      const bool selected =
          mHasSelection &&
          (row > selA.y() || (row == selA.y() && col >= selA.x())) &&
          (row < selB.y() || (row == selB.y() && col <= selB.x()));
      if (selected)
        p.fillRect(x, y, cellWidth * mCellW, mCellH, selectionBg);

      // Cursor
      bool isCursor = (!fromScrollback && mScrollOffset == 0 &&
                       screenRow == mCursorRow && col == mCursorCol &&
                       mCursorVisible);
      if (isCursor && (mBlinkState || !mCursorBlink)) {
        if (hasFocus()) {
          p.fillRect(x, y, cellWidth * mCellW, mCellH, QColor(0xc0, 0xc0, 0xc0));
          fg = QColor(0x1e, 0x1e, 0x1e);
        } else {
          p.setPen(QColor(0x80, 0x80, 0x80));
          p.drawRect(x, y, cellWidth * mCellW - 1, mCellH - 1);
        }
      }

      // Character
      if (cell.chars[0] != 0 && !cell.attrs.conceal) {
        QFont drawFont = mFont;
        if (cell.attrs.bold)
          drawFont.setBold(true);
        if (cell.attrs.italic)
          drawFont.setItalic(true);
        p.setFont(drawFont);
        p.setPen(fg);

        QString ch = QString::fromUcs4(cell.chars, 1);
        for (int ci = 1; ci < VTERM_MAX_CHARS_PER_CELL && cell.chars[ci]; ++ci)
          ch += QString::fromUcs4(&cell.chars[ci], 1);

        p.drawText(x, y + mBaseline, ch);

        if (cell.attrs.bold || cell.attrs.italic)
          p.setFont(mFont);
      }

      // Underline
      if (cell.attrs.underline) {
        p.setPen(fg);
        int uy = y + mCellH - 2;
        if (cell.attrs.underline == VTERM_UNDERLINE_DOUBLE) {
          p.drawLine(x, uy - 1, x + cellWidth * mCellW, uy - 1);
          p.drawLine(x, uy + 1, x + cellWidth * mCellW, uy + 1);
        } else if (cell.attrs.underline == VTERM_UNDERLINE_CURLY) {
          for (int cx = x; cx < x + cellWidth * mCellW; cx += 2)
            p.drawPoint(cx, uy + (cx / 2 % 2));
        } else {
          p.drawLine(x, uy, x + cellWidth * mCellW, uy);
        }
      }

      // Strikethrough
      if (cell.attrs.strike) {
        p.setPen(fg);
        int sy = y + mCellH / 2;
        p.drawLine(x, sy, x + cellWidth * mCellW, sy);
      }

      col += cellWidth;
    }
  }
}

void TerminalView::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  recalcGeometry();
  viewport()->update();
}

bool TerminalView::event(QEvent *event) {
  // Application shortcuts (e.g. the diff view's single-key "d"/"w" scroll
  // bindings) are WindowShortcuts, so they fire even when the terminal has
  // focus and would steal those keystrokes from the shell. Accepting the
  // ShortcutOverride tells Qt to deliver the key as a normal keyPressEvent
  // to us instead. We let Command-key combos through so macOS app/system
  // shortcuts (quit, toggle terminal) keep working — except Copy/Paste, which
  // the terminal handles itself so they reach the shell selection/clipboard.
  if (event->type() == QEvent::ShortcutOverride) {
    auto *ke = static_cast<QKeyEvent *>(event);
    if (!(ke->modifiers() & Qt::ControlModifier) ||
        ke->matches(QKeySequence::Copy) || ke->matches(QKeySequence::Paste)) {
      event->accept();
      return true;
    }
  }

  // Tab and Shift+Tab are consumed by Qt's focus navigation before they reach
  // keyPressEvent (returning false from focusNextPrevChild isn't enough), so
  // route them straight to the shell here for completion to work.
  if (event->type() == QEvent::KeyPress) {
    auto *ke = static_cast<QKeyEvent *>(event);
    if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
      keyPressEvent(ke);
      return true;
    }
  }

  return QAbstractScrollArea::event(event);
}

void TerminalView::keyPressEvent(QKeyEvent *event) {
  // Clipboard shortcuts (Cmd+C / Cmd+V on macOS, Ctrl+C / Ctrl+V elsewhere).
  // Copy is only intercepted when something is selected, so Ctrl+C still sends
  // an interrupt to the shell when there is no selection.
  if (event->matches(QKeySequence::Copy) && hasSelection()) {
    copySelection();
    return;
  }
  if (event->matches(QKeySequence::Paste)) {
    pasteClipboard();
    return;
  }

  // Any other key press dismisses the selection and returns to the bottom.
  clearSelection();
  if (mScrollOffset > 0) {
    mScrollOffset = 0;
    updateScrollbar();
  }

  VTermModifier mod = VTERM_MOD_NONE;
  Qt::KeyboardModifiers qmod = event->modifiers();
  if (qmod & Qt::ShiftModifier)
    mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
  if (qmod & Qt::AltModifier)
    mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
#ifdef Q_OS_MACOS
  // On macOS the physical Control key is Qt::MetaModifier; Qt::ControlModifier
  // is Command, reserved for app/clipboard shortcuts.
  if (qmod & Qt::MetaModifier)
    mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
#else
  if (qmod & Qt::ControlModifier)
    mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
#endif

  // Control + a letter (Ctrl+C interrupt, Ctrl+D EOF, Ctrl+Z, …): send the
  // control character explicitly, since event->text() may be empty for these.
  if ((mod & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A &&
      event->key() <= Qt::Key_Z) {
    vterm_keyboard_unichar(mVt, 'a' + (event->key() - Qt::Key_A), mod);
    return;
  }

  VTermKey vtkey = VTERM_KEY_NONE;
  switch (event->key()) {
  case Qt::Key_Return:
  case Qt::Key_Enter:
    vtkey = VTERM_KEY_ENTER;
    break;
  case Qt::Key_Tab:
  case Qt::Key_Backtab:
    vtkey = VTERM_KEY_TAB;
    break;
  case Qt::Key_Backspace:
    vtkey = VTERM_KEY_BACKSPACE;
    break;
  case Qt::Key_Escape:
    vtkey = VTERM_KEY_ESCAPE;
    break;
  case Qt::Key_Up:
    vtkey = VTERM_KEY_UP;
    break;
  case Qt::Key_Down:
    vtkey = VTERM_KEY_DOWN;
    break;
  case Qt::Key_Left:
    vtkey = VTERM_KEY_LEFT;
    break;
  case Qt::Key_Right:
    vtkey = VTERM_KEY_RIGHT;
    break;
  case Qt::Key_Insert:
    vtkey = VTERM_KEY_INS;
    break;
  case Qt::Key_Delete:
    vtkey = VTERM_KEY_DEL;
    break;
  case Qt::Key_Home:
    vtkey = VTERM_KEY_HOME;
    break;
  case Qt::Key_End:
    vtkey = VTERM_KEY_END;
    break;
  case Qt::Key_PageUp:
    vtkey = VTERM_KEY_PAGEUP;
    break;
  case Qt::Key_PageDown:
    vtkey = VTERM_KEY_PAGEDOWN;
    break;
  case Qt::Key_F1:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(1));
    break;
  case Qt::Key_F2:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(2));
    break;
  case Qt::Key_F3:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(3));
    break;
  case Qt::Key_F4:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(4));
    break;
  case Qt::Key_F5:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(5));
    break;
  case Qt::Key_F6:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(6));
    break;
  case Qt::Key_F7:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(7));
    break;
  case Qt::Key_F8:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(8));
    break;
  case Qt::Key_F9:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(9));
    break;
  case Qt::Key_F10:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(10));
    break;
  case Qt::Key_F11:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(11));
    break;
  case Qt::Key_F12:
    vtkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(12));
    break;
  default:
    break;
  }

  if (vtkey != VTERM_KEY_NONE) {
    vterm_keyboard_key(mVt, vtkey, mod);
    return;
  }

  QString text = event->text();
  if (!text.isEmpty()) {
    QByteArray utf8 = text.toUtf8();
    for (int i = 0; i < utf8.size();) {
      uint32_t codepoint;
      unsigned char c = utf8[i];
      if (c < 0x80) {
        codepoint = c;
        i += 1;
      } else if ((c & 0xE0) == 0xC0) {
        codepoint = (c & 0x1F) << 6;
        if (i + 1 < utf8.size())
          codepoint |= (utf8[i + 1] & 0x3F);
        i += 2;
      } else if ((c & 0xF0) == 0xE0) {
        codepoint = (c & 0x0F) << 12;
        if (i + 1 < utf8.size())
          codepoint |= (utf8[i + 1] & 0x3F) << 6;
        if (i + 2 < utf8.size())
          codepoint |= (utf8[i + 2] & 0x3F);
        i += 3;
      } else {
        codepoint = (c & 0x07) << 18;
        if (i + 1 < utf8.size())
          codepoint |= (utf8[i + 1] & 0x3F) << 12;
        if (i + 2 < utf8.size())
          codepoint |= (utf8[i + 2] & 0x3F) << 6;
        if (i + 3 < utf8.size())
          codepoint |= (utf8[i + 3] & 0x3F);
        i += 4;
      }
      vterm_keyboard_unichar(mVt, codepoint, mod);
    }
  }
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event) {
  if (!event->commitString().isEmpty()) {
    QByteArray utf8 = event->commitString().toUtf8();
    for (unsigned char c : utf8)
      vterm_keyboard_unichar(mVt, c, VTERM_MOD_NONE);
  }
  event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const {
  if (query == Qt::ImCursorRectangle)
    return QRect(mCursorCol * mCellW, mCursorRow * mCellH, mCellW, mCellH);
  return QAbstractScrollArea::inputMethodQuery(query);
}

void TerminalView::focusInEvent(QFocusEvent *event) {
  QAbstractScrollArea::focusInEvent(event);
  mBlinkState = true;
  mBlinkTimer.start();
  viewport()->update();
}

void TerminalView::focusOutEvent(QFocusEvent *event) {
  QAbstractScrollArea::focusOutEvent(event);
  mBlinkTimer.stop();
  mBlinkState = true;
  viewport()->update();
}

// Read the cell at a viewport (row, col), mirroring paintEvent's scrollback /
// screen mapping. Returns false for out-of-range coordinates.
bool TerminalView::cellAt(int row, int col, VTermScreenCell &out) const {
  memset(&out, 0, sizeof(out));
  if (row < 0 || row >= mRows || col < 0 || col >= mCols)
    return false;

  int screenRow = row;
  bool fromScrollback = false;
  int sbIdx = -1;
  if (mScrollOffset > 0) {
    const int sbSize = static_cast<int>(mScrollback.size());
    const int sbRow = row - (mRows - mScrollOffset);
    if (sbRow < 0) {
      sbIdx = sbSize - mScrollOffset + row;
      if (sbIdx < 0 || sbIdx >= sbSize)
        return false;
      fromScrollback = true;
    } else {
      screenRow = sbRow;
    }
  }

  if (fromScrollback) {
    const auto &line = mScrollback[sbIdx];
    if (col < static_cast<int>(line.cells.size()))
      out = line.cells[col];
    else {
      out.chars[0] = ' ';
      out.width = 1;
    }
  } else {
    VTermPos pos = {screenRow, col};
    vterm_screen_get_cell(mVtScreen, pos, &out);
  }
  return true;
}

QPoint TerminalView::cellForPos(const QPoint &pos) const {
  int col = mCellW > 0 ? pos.x() / mCellW : 0;
  int row = mCellH > 0 ? pos.y() / mCellH : 0;
  col = qBound(0, col, mCols - 1);
  row = qBound(0, row, mRows - 1);
  return QPoint(col, row);
}

QString TerminalView::lineText(int row) const {
  QString s;
  for (int col = 0; col < mCols; ++col) {
    VTermScreenCell cell;
    cellAt(row, col, cell);
    const QString ch = cellChars(cell);
    s += ch.isEmpty() ? QStringLiteral(" ") : ch;
  }
  return s;
}

QString TerminalView::urlAt(int row, int col) const {
  const QString line = lineText(row);
  static const QRegularExpression re(
      QStringLiteral(R"((?:https?://|file://|www\.)[^\s"'<>`\]]+)"));
  auto it = re.globalMatch(line);
  while (it.hasNext()) {
    const QRegularExpressionMatch m = it.next();
    if (col >= m.capturedStart() && col < m.capturedEnd()) {
      QString u = m.captured();
      while (u.endsWith('.') || u.endsWith(',') || u.endsWith(')') ||
             u.endsWith(';'))
        u.chop(1);
      if (u.startsWith(QStringLiteral("www.")))
        u.prepend(QStringLiteral("https://"));
      return u;
    }
  }
  return QString();
}

bool TerminalView::hasSelection() const {
  return mHasSelection && mSelAnchor.x() >= 0 && mSelHead.x() >= 0;
}

QString TerminalView::selectedText() const {
  if (!hasSelection())
    return QString();

  QPoint a = mSelAnchor, b = mSelHead;
  if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
    std::swap(a, b);

  QStringList lines;
  for (int row = a.y(); row <= b.y(); ++row) {
    const int c0 = (row == a.y()) ? a.x() : 0;
    const int c1 = (row == b.y()) ? b.x() : mCols - 1;
    QString line;
    for (int col = c0; col <= c1 && col < mCols; ++col) {
      VTermScreenCell cell;
      cellAt(row, col, cell);
      line += cellChars(cell);
    }
    while (line.endsWith(' '))
      line.chop(1);
    lines << line;
  }
  return lines.join('\n');
}

void TerminalView::copySelection() {
  const QString text = selectedText();
  if (!text.isEmpty())
    QApplication::clipboard()->setText(text);
}

void TerminalView::pasteClipboard() {
  const QString text = QApplication::clipboard()->text();
  if (text.isEmpty())
    return;
  if (mScrollOffset > 0) {
    mScrollOffset = 0;
    updateScrollbar();
  }
  // Normalise newlines to carriage returns the way a terminal expects.
  QString s = text;
  s.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
  s.replace('\n', '\r');
  writeToShell(s.toUtf8());
}

void TerminalView::selectAll() {
  mSelAnchor = QPoint(0, 0);
  mSelHead = QPoint(mCols - 1, mRows - 1);
  mHasSelection = true;
  viewport()->update();
}

void TerminalView::clearSelection() {
  if (mHasSelection || mSelAnchor.x() >= 0) {
    mHasSelection = false;
    mSelAnchor = mSelHead = QPoint(-1, -1);
    viewport()->update();
  }
}

void TerminalView::mousePressEvent(QMouseEvent *event) {
  setFocus();
  if (event->button() == Qt::LeftButton) {
    const QPoint cell = cellForPos(event->pos());
    // Cmd/Ctrl+click opens a URL under the cursor instead of selecting.
    if (event->modifiers() & Qt::ControlModifier) {
      const QString url = urlAt(cell.y(), cell.x());
      if (!url.isEmpty()) {
        QDesktopServices::openUrl(QUrl(url));
        return;
      }
    }
    clearSelection();
    mSelAnchor = mSelHead = cell;
    mSelecting = true;
    mHasSelection = false;
  }
  QAbstractScrollArea::mousePressEvent(event);
}

void TerminalView::mouseMoveEvent(QMouseEvent *event) {
  if (mSelecting) {
    const QPoint cell = cellForPos(event->pos());
    if (cell != mSelHead) {
      mSelHead = cell;
      if (mSelHead != mSelAnchor)
        mHasSelection = true;
      viewport()->update();
    }
  }
  QAbstractScrollArea::mouseMoveEvent(event);
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event) {
  mSelecting = false;
  QAbstractScrollArea::mouseReleaseEvent(event);
}

void TerminalView::contextMenuEvent(QContextMenuEvent *event) {
  const QPoint cell = cellForPos(event->pos());
  const QString url = urlAt(cell.y(), cell.x());

  QMenu menu(this);
  QAction *copyAct = menu.addAction(tr("Copy"));
  copyAct->setEnabled(hasSelection());
  QAction *pasteAct = menu.addAction(tr("Paste"));
  pasteAct->setEnabled(!QApplication::clipboard()->text().isEmpty());

  menu.addSeparator();
  QAction *chatAct = menu.addAction(tr("Send selection to Chat"));
  chatAct->setEnabled(hasSelection());

  QAction *linkAct = nullptr;
  if (!url.isEmpty()) {
    menu.addSeparator();
    linkAct = menu.addAction(tr("Open link"));
  }

  menu.addSeparator();
  QAction *selectAllAct = menu.addAction(tr("Select All"));

  QAction *chosen = menu.exec(event->globalPos());
  if (!chosen)
    return;
  if (chosen == copyAct)
    copySelection();
  else if (chosen == pasteAct)
    pasteClipboard();
  else if (chosen == chatAct)
    emit sendToChatRequested(selectedText());
  else if (linkAct && chosen == linkAct)
    QDesktopServices::openUrl(QUrl(url));
  else if (chosen == selectAllAct)
    selectAll();
}

bool TerminalView::focusNextPrevChild(bool) {
  // Keep Tab and Shift+Tab inside the terminal. By default Qt consumes them
  // for focus navigation before keyPressEvent runs; returning false lets the
  // key fall through so the shell receives it for completion.
  return false;
}

// --- libvterm callbacks ---

int TerminalView::onDamage(int, int, int, int, void *user) {
  auto *self = static_cast<TerminalView *>(user);
  self->mDamageAll = true;
  return 1;
}

int TerminalView::onMoveCursor(int newRow, int newCol, int, int, int visible,
                               void *user) {
  auto *self = static_cast<TerminalView *>(user);
  self->mCursorRow = newRow;
  self->mCursorCol = newCol;
  self->mCursorVisible = visible;
  self->mBlinkState = true;
  return 1;
}

int TerminalView::onSetTermProp(int prop, void *val, void *user) {
  auto *self = static_cast<TerminalView *>(user);
  auto *v = static_cast<VTermValue *>(val);

  switch (static_cast<VTermProp>(prop)) {
  case VTERM_PROP_CURSORVISIBLE:
    self->mCursorVisible = v->boolean;
    break;
  case VTERM_PROP_CURSORBLINK:
    self->mCursorBlink = v->boolean;
    if (self->mCursorBlink)
      self->mBlinkTimer.start();
    else {
      self->mBlinkTimer.stop();
      self->mBlinkState = true;
    }
    break;
  case VTERM_PROP_TITLE:
    // Could emit a signal for window title
    break;
  default:
    break;
  }
  return 1;
}

int TerminalView::onBell(void *) {
  QApplication::beep();
  return 1;
}

int TerminalView::onResize(int rows, int cols, void *user) {
  auto *self = static_cast<TerminalView *>(user);
  self->mRows = rows;
  self->mCols = cols;
  return 1;
}

int TerminalView::onSbPushLine(int cols, const VTermScreenCell *cells,
                               void *user) {
  auto *self = static_cast<TerminalView *>(user);
  ScrollbackLine line;
  line.cells.assign(cells, cells + cols);
  self->mScrollback.push_back(std::move(line));

  while (static_cast<int>(self->mScrollback.size()) > self->mScrollbackMax)
    self->mScrollback.erase(self->mScrollback.begin());

  return 1;
}

int TerminalView::onSbPopLine(int cols, VTermScreenCell *cells, void *user) {
  auto *self = static_cast<TerminalView *>(user);
  if (self->mScrollback.empty())
    return 0;

  const auto &line = self->mScrollback.back();
  int toCopy = qMin(cols, static_cast<int>(line.cells.size()));
  memcpy(cells, line.cells.data(), toCopy * sizeof(VTermScreenCell));

  for (int i = toCopy; i < cols; ++i) {
    memset(&cells[i], 0, sizeof(VTermScreenCell));
    cells[i].chars[0] = ' ';
    cells[i].width = 1;
  }

  self->mScrollback.pop_back();
  return 1;
}

void TerminalView::onOutput(const char *s, size_t len, void *user) {
  auto *self = static_cast<TerminalView *>(user);
  if (self->mPtyFd >= 0)
    ::write(self->mPtyFd, s, len);
}
