#include "ChatPanel.h"
#include "ai/AiService.h"
#include "conf/Settings.h"
#include "git/Diff.h"
#include "git/Reference.h"
#include "git/Signature.h"
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QVBoxLayout>

class ChatInput : public QLineEdit {
public:
  using QLineEdit::QLineEdit;

protected:
  void keyPressEvent(QKeyEvent *e) override {
    if (e->key() == Qt::Key_Up && text().isEmpty()) {
      emit returnPressed(); // reuse signal to indicate "recall"
      return;
    }
    QLineEdit::keyPressEvent(e);
  }
};

ChatPanel::ChatPanel(const git::Repository &repo, QWidget *parent)
    : QWidget(parent), mRepo(repo) {
  mNet = new QNetworkAccessManager(this);

  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPointSize(12);

  mChatView = new QTextEdit(this);
  mChatView->setReadOnly(true);
  mChatView->setFont(mono);
  mChatView->setPlaceholderText(
      tr("Chat about this repository. Ask questions about code, diffs, "
         "branches, or anything else."));

  mInput = new ChatInput(this);
  mInput->setFont(mono);
  mInput->setPlaceholderText(tr("Ask about this repo…"));
  connect(mInput, &QLineEdit::returnPressed, this, &ChatPanel::sendMessage);

  mSendBtn = new QPushButton(tr("Send"), this);
  connect(mSendBtn, &QPushButton::clicked, this, &ChatPanel::sendMessage);

  mClearBtn = new QPushButton(tr("Clear"), this);
  mClearBtn->setToolTip(tr("Clear conversation history"));
  connect(mClearBtn, &QPushButton::clicked, this, &ChatPanel::clearChat);

  mStatusLabel = new QLabel(this);
  mStatusLabel->setStyleSheet("color: gray; font-size: 11px;");

  QHBoxLayout *inputRow = new QHBoxLayout;
  inputRow->setContentsMargins(0, 0, 0, 0);
  inputRow->addWidget(mInput, 1);
  inputRow->addWidget(mSendBtn);
  inputRow->addWidget(mClearBtn);

  QHBoxLayout *statusRow = new QHBoxLayout;
  statusRow->setContentsMargins(0, 0, 0, 0);
  statusRow->addWidget(mStatusLabel, 1);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->addWidget(mChatView, 1);
  layout->addLayout(inputRow);
  layout->addLayout(statusRow);
}

void ChatPanel::setDiffContext(const QString &diff) { mDiffContext = diff; }

void ChatPanel::setFileContext(const QString &file) { mFileContext = file; }

void ChatPanel::clearContext() {
  mDiffContext.clear();
  mFileContext.clear();
}

void ChatPanel::clearChat() {
  mHistory.clear();
  mChatView->clear();
  mStatusLabel->clear();
}

QString ChatPanel::buildSystemPrompt() const {
  QString prompt =
      QStringLiteral("You are a helpful coding assistant embedded in Gittyup, "
                     "a git client. You help the user understand and work with "
                     "their git repository.\n\n");

  prompt += gatherRepoContext();

  if (!mDiffContext.isEmpty()) {
    prompt += QStringLiteral("\n\nThe user is currently viewing this diff:\n");
    prompt += mDiffContext.left(8000);
  }

  if (!mFileContext.isEmpty()) {
    prompt += QStringLiteral("\n\nThe user is currently viewing this file: ") +
              mFileContext;
  }

  return prompt;
}

QString ChatPanel::gatherRepoContext() const {
  if (!mRepo.isValid())
    return {};

  QString ctx;

  git::Reference head = mRepo.head();
  if (head.isValid())
    ctx += QStringLiteral("Current branch: ") + head.name() + "\n";

  ctx += QStringLiteral("Repository: ") + mRepo.workdir().path() + "\n";

  // Recent commit messages for context
  QProcess git;
  git.setWorkingDirectory(mRepo.workdir().path());
  git.start("git",
            {"log", "--oneline", "-10", "--no-decorate"});
  if (git.waitForFinished(3000) && git.exitCode() == 0) {
    ctx += QStringLiteral("\nRecent commits:\n");
    ctx += QString::fromUtf8(git.readAllStandardOutput());
  }

  // Current status summary
  git.start("git", {"status", "--short"});
  if (git.waitForFinished(3000) && git.exitCode() == 0) {
    QString status = QString::fromUtf8(git.readAllStandardOutput());
    if (!status.isEmpty()) {
      ctx += QStringLiteral("\nWorking tree status:\n");
      ctx += status.left(2000);
    }
  }

  return ctx;
}

void ChatPanel::appendMessage(const QString &role, const QString &text) {
  QString label = (role == "user") ? QStringLiteral("<b>You:</b> ")
                                   : QStringLiteral("<b>AI:</b> ");
  QString html = text.toHtmlEscaped().replace("\n", "<br>");
  mChatView->append(label + html);
  mChatView->append("");
  scrollToBottom();
}

void ChatPanel::scrollToBottom() {
  QScrollBar *sb = mChatView->verticalScrollBar();
  sb->setValue(sb->maximum());
}

void ChatPanel::sendMessage() {
  QString text = mInput->text().trimmed();
  if (text.isEmpty())
    return;

  if (mActiveReply) {
    mStatusLabel->setText(tr("Still waiting for response…"));
    return;
  }

  mInput->clear();
  appendMessage("user", text);
  mHistory.append({"user", text});

  // Build the request
  AiService::Config cfg = AiService::instance()->currentConfig();

  QString baseUrl = cfg.baseUrl;
  if (baseUrl.isEmpty())
    baseUrl = QStringLiteral("http://localhost:11434");

  QUrl url;
  if (cfg.provider == "ollama") {
    url = QUrl(baseUrl + "/api/chat");
  } else {
    url = QUrl("https://api.anthropic.com/v1/messages");
  }

  QNetworkRequest req(url);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  QJsonObject body;

  if (cfg.provider == "ollama") {
    body["model"] = cfg.model.isEmpty() ? "llama3" : cfg.model;
    body["stream"] = true;

    QJsonArray messages;
    // System message
    QJsonObject sys;
    sys["role"] = "system";
    sys["content"] = buildSystemPrompt();
    messages.append(sys);

    // Conversation history
    for (const auto &msg : mHistory) {
      QJsonObject m;
      m["role"] = msg.role;
      m["content"] = msg.content;
      messages.append(m);
    }
    body["messages"] = messages;
  } else {
    // Anthropic
    req.setRawHeader("x-api-key", cfg.apiKey.toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");

    body["model"] = cfg.model.isEmpty() ? "claude-sonnet-4-20250514" : cfg.model;
    body["max_tokens"] = 4096;
    body["stream"] = true;
    body["system"] = buildSystemPrompt();

    QJsonArray messages;
    for (const auto &msg : mHistory) {
      QJsonObject m;
      m["role"] = msg.role;
      m["content"] = msg.content;
      messages.append(m);
    }
    body["messages"] = messages;
  }

  mStreamBuffer.clear();
  mStatusLabel->setText(tr("Thinking…"));
  mSendBtn->setEnabled(false);

  // Start the AI response block in the chat view
  mChatView->append("<b>AI:</b> ");

  mActiveReply = mNet->post(req, QJsonDocument(body).toJson());

  connect(mActiveReply, &QNetworkReply::readyRead, this,
          &ChatPanel::onStreamData);
  connect(mActiveReply, &QNetworkReply::finished, this,
          &ChatPanel::onStreamFinished);
}

void ChatPanel::onStreamData() {
  if (!mActiveReply)
    return;

  QByteArray data = mActiveReply->readAll();
  AiService::Config cfg = AiService::instance()->currentConfig();

  // Parse streaming response line by line
  QList<QByteArray> lines = data.split('\n');
  for (const QByteArray &line : lines) {
    QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
      continue;

    if (cfg.provider == "ollama") {
      // Ollama streams JSON objects, one per line
      QJsonDocument doc = QJsonDocument::fromJson(trimmed);
      if (doc.isNull())
        continue;
      QJsonObject obj = doc.object();
      QJsonObject msg = obj["message"].toObject();
      QString content = msg["content"].toString();
      if (!content.isEmpty()) {
        mStreamBuffer += content;
        // Update the last line in chat view
        QTextCursor cursor = mChatView->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(content);
        scrollToBottom();
      }
    } else {
      // Anthropic SSE format: "data: {...}"
      if (!trimmed.startsWith("data: "))
        continue;
      QByteArray json = trimmed.mid(6);
      if (json == "[DONE]")
        continue;
      QJsonDocument doc = QJsonDocument::fromJson(json);
      if (doc.isNull())
        continue;
      QJsonObject obj = doc.object();
      QString type = obj["type"].toString();
      if (type == "content_block_delta") {
        QJsonObject delta = obj["delta"].toObject();
        QString text = delta["text"].toString();
        if (!text.isEmpty()) {
          mStreamBuffer += text;
          QTextCursor cursor = mChatView->textCursor();
          cursor.movePosition(QTextCursor::End);
          cursor.insertText(text);
          scrollToBottom();
        }
      }
    }
  }
}

void ChatPanel::onStreamFinished() {
  if (!mActiveReply)
    return;

  int status = mActiveReply->attribute(
      QNetworkRequest::HttpStatusCodeAttribute).toInt();

  if (mActiveReply->error() != QNetworkReply::NoError || status >= 400) {
    QString errText = mActiveReply->readAll();
    if (errText.isEmpty())
      errText = mActiveReply->errorString();
    mChatView->append(
        QStringLiteral("<span style='color:red;'>Error: %1</span>")
            .arg(errText.toHtmlEscaped().left(500)));
    mStatusLabel->setText(tr("Request failed"));
  } else {
    if (!mStreamBuffer.isEmpty())
      mHistory.append({"assistant", mStreamBuffer});
    mChatView->append("");
    mStatusLabel->setText(tr("Ready"));
  }

  mActiveReply->deleteLater();
  mActiveReply = nullptr;
  mSendBtn->setEnabled(true);
  mInput->setFocus();
  scrollToBottom();
}
