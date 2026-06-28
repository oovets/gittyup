#pragma once

#include <QIcon>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>

// Attaches an animated spinning arc to a QPushButton while a job is running.
// Usage:
//   auto *spin = new SpinnerButton(myButton, this);
//   spin->start(tr("Working…"));   // disables button, shows spinner
//   spin->stop();                  // restores text + icon, re-enables
class SpinnerButton : public QObject {
public:
  SpinnerButton(QPushButton *btn, QObject *parent = nullptr)
      : QObject(parent), mBtn(btn) {
    mTimer = new QTimer(this);
    mTimer->setInterval(40);
    connect(mTimer, &QTimer::timeout, this, [this] { tick(); });
  }

  bool isRunning() const { return mTimer->isActive(); }

  void start(const QString &label = {}) {
    if (mTimer->isActive()) return;
    mOrigText = mBtn->text();
    mOrigIcon = mBtn->icon();
    mBtn->setEnabled(false);
    if (!label.isEmpty()) mBtn->setText(label);
    mAngle = 0;
    tick();
    mTimer->start();
  }

  void stop() {
    mTimer->stop();
    mBtn->setIcon(mOrigIcon);
    mBtn->setText(mOrigText);
    mBtn->setEnabled(true);
  }

private:
  void tick() {
    mAngle = (mAngle + 15) % 360;
    const int sz = 14;
    QPixmap px(sz, sz);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    QColor col = mBtn->palette().buttonText().color();
    // Faint track
    p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 50), 2.0,
                  Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(2, 2, sz - 4, sz - 4);
    // Spinning arc
    p.setPen(QPen(col, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(2, 2, sz - 4, sz - 4, mAngle * 16, 240 * 16);
    mBtn->setIcon(QIcon(px));
  }

private:
  QPushButton *mBtn;
  QTimer      *mTimer;
  QIcon        mOrigIcon;
  QString      mOrigText;
  int          mAngle = 0;
};
