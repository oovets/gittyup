#ifndef STATUSBAR_H
#define STATUSBAR_H

#include <QFrame>
#include <QLabel>
#include <QTimer>

class RepoView;

class StatusBar : public QFrame {
  Q_OBJECT

public:
  StatusBar(QWidget *parent = nullptr);

  void updateRepo(RepoView *view);
  void setAiStatus(const QString &model, const QString &provider);

private:
  void checkOllamaStatus();

  QLabel *mBranchLabel;
  QLabel *mTrackingLabel;
  QLabel *mStateLabel;
  QLabel *mDirtyLabel;
  QLabel *mAiModelLabel;
  QLabel *mOllamaLabel;
  QLabel *mQueueLabel;
  QLabel *mTokenLabel;
  QTimer mOllamaTimer;
  QString mOllamaUrl;
};

#endif
