#ifndef CHATPANEL_H
#define CHATPANEL_H

#include "git/Repository.h"
#include <QWidget>
#include <QList>

class QTextEdit;
class QLineEdit;
class QPushButton;
class QLabel;
class QComboBox;

class ChatPanel : public QWidget {
  Q_OBJECT

public:
  ChatPanel(const git::Repository &repo, QWidget *parent = nullptr);

  void setDiffContext(const QString &diff);
  void setFileContext(const QString &file);
  void addTerminalContext(const QString &text);
  void clearContext();
  void clearChat();

signals:
  void visibilityToggled(bool visible);

private slots:
  void sendMessage();

private:
  struct Message {
    QString role;
    QString content;
  };

  QString buildSystemPrompt() const;
  QString gatherRepoContext() const;
  void appendMessage(const QString &role, const QString &text);
  void renderFormattedResponse();
  static QString markdownToHtml(const QString &md);
  void scrollToBottom();

  git::Repository mRepo;

  QTextEdit *mChatView;
  QLineEdit *mInput;
  QPushButton *mSendBtn;
  QPushButton *mClearBtn;
  QLabel *mStatusLabel;

  quint64 mActiveHandle = 0;

  QList<Message> mHistory;
  QString mStreamBuffer;
  int mAiBlockStart = -1; // character position where the current AI reply began

  QString mDiffContext;
  QString mFileContext;
  QString mTerminalContext;
};

#endif // CHATPANEL_H
