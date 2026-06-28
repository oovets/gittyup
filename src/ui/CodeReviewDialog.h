#ifndef CODEREVIEWDIALOG_H
#define CODEREVIEWDIALOG_H

#include "git/Repository.h"
#include "ai/AiReviewStore.h"
#include <QDialog>
#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTextBrowser;

class CodeReviewDialog : public QDialog {
  Q_OBJECT

public:
  // Opens with a fresh review result. Saves to store automatically.
  CodeReviewDialog(const QString &reviewText, const QByteArray &diff,
                   const QString &headSha,
                   const git::Repository &repo, QWidget *parent = nullptr);

  // Opens showing existing review history (no new review needed).
  CodeReviewDialog(const git::Repository &repo, QWidget *parent = nullptr);

private slots:
  void onFixIssues();
  void onRerun();
  void onHistoryChanged(int index);

private:
  struct Fix {
    QString file;
    QString find;
    QString replace;
  };

  void initUi();
  void loadHistory();
  void showEntry(const AiReviewStore::Entry &entry, const QByteArray &diff);
  void updateFixLog(const QList<AiReviewStore::FixRecord> &fixes);
  QList<Fix> parseFixes(const QString &response) const;
  void applyFixes(const QList<Fix> &fixes);

  git::Repository mRepo;
  QString mCurrentId;
  QByteArray mCurrentDiff;

  QComboBox *mHistoryBox;
  QTextBrowser *mReviewText;
  QTextBrowser *mFixLog;
  QPushButton *mFixBtn;
  QPushButton *mRerunBtn;
  QLabel *mStatus;

  QList<AiReviewStore::Entry> mHistory;
};

#endif // CODEREVIEWDIALOG_H
