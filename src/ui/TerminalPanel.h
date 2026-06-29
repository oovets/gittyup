#ifndef TERMINALPANEL_UI_H
#define TERMINALPANEL_UI_H

#include <QAbstractScrollArea>
#include <QTimer>
#include <QWidget>
#include <vector>

#include <vterm.h>

class QSocketNotifier;

class TerminalView : public QAbstractScrollArea {
  Q_OBJECT

public:
  TerminalView(QWidget *parent = nullptr);
  ~TerminalView();

  void setWorkingDirectory(const QString &dir);
  QSize sizeHint() const override;

protected:
  bool event(QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void inputMethodEvent(QInputMethodEvent *event) override;
  QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
  void focusInEvent(QFocusEvent *event) override;
  void focusOutEvent(QFocusEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  bool focusNextPrevChild(bool next) override;

private slots:
  void onPtyReadable();

public:
  // libvterm callbacks (public for C trampoline access)
  static int onDamage(int startRow, int endRow, int startCol, int endCol,
                      void *user);
  static int onMoveCursor(int newRow, int newCol, int oldRow, int oldCol,
                          int visible, void *user);
  static int onSetTermProp(int prop, void *val, void *user);
  static int onBell(void *user);
  static int onResize(int rows, int cols, void *user);
  static int onSbPushLine(int cols, const VTermScreenCell *cells, void *user);
  static int onSbPopLine(int cols, VTermScreenCell *cells, void *user);
  static void onOutput(const char *s, size_t len, void *user);

private:
  void spawnShell();
  void writeToShell(const QByteArray &data);
  void recalcGeometry();
  void updateScrollbar();
  QColor vtermColorToQt(const VTermScreenCell &cell, bool foreground) const;
  void flushDamage();

  VTerm *mVt = nullptr;
  VTermScreen *mVtScreen = nullptr;

  int mRows = 24;
  int mCols = 80;
  int mCellW = 0;
  int mCellH = 0;
  int mBaseline = 0;
  QFont mFont;

  // Cursor
  int mCursorRow = 0;
  int mCursorCol = 0;
  bool mCursorVisible = true;
  bool mCursorBlink = false;
  QTimer mBlinkTimer;
  bool mBlinkState = true;

  // Scrollback
  struct ScrollbackLine {
    std::vector<VTermScreenCell> cells;
  };
  std::vector<ScrollbackLine> mScrollback;
  int mScrollbackMax = 5000;
  int mScrollOffset = 0;

  // Damage tracking
  bool mDamageAll = false;

  // PTY
  QSocketNotifier *mNotifier = nullptr;
  int mPtyFd = -1;
  pid_t mChildPid = -1;
  QString mWorkDir;
};

#endif // TERMINALPANEL_UI_H
