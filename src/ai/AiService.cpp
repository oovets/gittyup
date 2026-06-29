#include "AiService.h"
#include "conf/Settings.h"
#include "conf/Setting.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSharedPointer>
#include <QRegularExpression>

AiService *AiService::instance() {
  static AiService sInstance;
  return &sInstance;
}

AiService::AiService(QObject *parent) : QObject(parent) {}

AiService::Config AiService::currentConfig() const {
  Settings *settings = Settings::instance();
  Config cfg;
  cfg.provider = settings->value(Setting::Id::AiProvider).toString();
  if (cfg.provider.isEmpty())
    cfg.provider = QStringLiteral("anthropic");

  if (cfg.provider == QStringLiteral("ollama")) {
    cfg.baseUrl = settings->value(Setting::Id::AiOllamaUrl).toString();
    if (cfg.baseUrl.isEmpty())
      cfg.baseUrl = QStringLiteral("http://localhost:11434");
    cfg.baseUrl = cfg.baseUrl.trimmed().remove(QRegularExpression("/$"));
    cfg.model = settings->value(Setting::Id::AiOllamaModel).toString();
    if (cfg.model.isEmpty())
      cfg.model = QStringLiteral("llama3");
  } else {
    cfg.apiKey = settings->value(Setting::Id::AiApiKey).toString();
    cfg.model = settings->value(Setting::Id::AiModel).toString();
    if (cfg.model.isEmpty())
      cfg.model = QStringLiteral("claude-haiku-4-5-20251001");
  }
  return cfg;
}

void AiService::chat(const QString &prompt, int maxTokens, Callback callback) {
  Config cfg = currentConfig();

  QNetworkRequest request;
  QJsonObject body;

  if (cfg.provider == QStringLiteral("ollama")) {
    request.setUrl(QUrl(cfg.baseUrl + "/api/chat"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    body["model"] = cfg.model;
    body["stream"] = false;
    body["messages"] =
        QJsonArray{QJsonObject{{"role", "user"}, {"content", prompt}}};
  } else {
    if (cfg.apiKey.isEmpty()) {
      callback(QString(), QStringLiteral("No API key configured."));
      return;
    }
    request.setUrl(
        QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-api-key", cfg.apiKey.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    body["model"] = cfg.model;
    body["max_tokens"] = maxTokens;
    body["messages"] =
        QJsonArray{QJsonObject{{"role", "user"}, {"content", prompt}}};
  }

  QNetworkReply *reply = mNet.post(request, QJsonDocument(body).toJson());
  connect(reply, &QNetworkReply::finished, this,
          [reply, callback, provider = cfg.provider] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
              callback(QString(), reply->errorString());
              return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QString text;
            if (provider == QStringLiteral("ollama"))
              text = doc["message"]["content"].toString().trimmed();
            else
              text = doc["content"][0]["text"].toString().trimmed();

            if (text.isEmpty()) {
              callback(QString(), QStringLiteral("Empty response from AI."));
              return;
            }
            callback(text, QString());
          });
}

QNetworkReply *AiService::chatStreaming(const QString &prompt, int maxTokens,
                                        StreamCallback onChunk,
                                        FinishCallback onDone) {
  Config cfg = currentConfig();

  QNetworkRequest request;
  QJsonObject body;

  if (cfg.provider == QStringLiteral("ollama")) {
    request.setUrl(QUrl(cfg.baseUrl + "/api/chat"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    body["model"] = cfg.model;
    body["stream"] = true;
    body["messages"] =
        QJsonArray{QJsonObject{{"role", "user"}, {"content", prompt}}};
  } else {
    if (cfg.apiKey.isEmpty()) {
      onDone(QString(), QStringLiteral("No API key configured."));
      return nullptr;
    }
    request.setUrl(
        QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-api-key", cfg.apiKey.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    body["model"] = cfg.model;
    body["max_tokens"] = maxTokens;
    body["stream"] = true;
    body["messages"] =
        QJsonArray{QJsonObject{{"role", "user"}, {"content", prompt}}};
  }

  QNetworkReply *reply = mNet.post(request, QJsonDocument(body).toJson());
  auto buffer = QSharedPointer<QString>::create();

  connect(reply, &QNetworkReply::readyRead, this,
          [reply, onChunk, buffer, provider = cfg.provider] {
            QByteArray data = reply->readAll();
            for (const QByteArray &rawLine : data.split('\n')) {
              QByteArray trimmed = rawLine.trimmed();
              if (trimmed.isEmpty()) continue;

              if (provider == QStringLiteral("ollama")) {
                QJsonDocument doc = QJsonDocument::fromJson(trimmed);
                if (doc.isNull()) continue;
                QString chunk = doc["message"]["content"].toString();
                if (!chunk.isEmpty()) {
                  *buffer += chunk;
                  onChunk(chunk);
                }
              } else {
                if (!trimmed.startsWith("data: ")) continue;
                QByteArray json = trimmed.mid(6);
                if (json == "[DONE]") continue;
                QJsonDocument doc = QJsonDocument::fromJson(json);
                if (doc.isNull()) continue;
                if (doc["type"].toString() == "content_block_delta") {
                  QString chunk = doc["delta"]["text"].toString();
                  if (!chunk.isEmpty()) {
                    *buffer += chunk;
                    onChunk(chunk);
                  }
                }
              }
            }
          });

  connect(reply, &QNetworkReply::finished, this,
          [reply, onDone, buffer] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError && buffer->isEmpty())
              onDone(QString(), reply->errorString());
            else
              onDone(*buffer, QString());
          });

  return reply;
}

QString AiService::reviewPrompt(const QByteArray &diff,
                                const QString &commitSha,
                                const QString &commitMsg,
                                const QString &codebaseContext) {
  QString prompt;

  if (!commitSha.isEmpty()) {
    prompt += QStringLiteral("Review the following git diff for commit %1.\n")
                  .arg(commitSha);
  }
  if (!commitMsg.isEmpty()) {
    prompt += QStringLiteral("Commit message: %1\n\n").arg(commitMsg);
  }

  prompt += QStringLiteral(
      "Analyze for bugs, security vulnerabilities, "
      "and code quality issues.\n"
      "For each issue found:\n"
      "- State severity: CRITICAL / HIGH / MEDIUM / LOW\n"
      "- Identify the file and approximate line\n"
      "- Explain the problem concisely\n"
      "- Suggest a fix\n\n"
      "If no issues are found, say so clearly.\n\n");

  if (!codebaseContext.isEmpty())
    prompt += codebaseContext.trimmed() + "\n";

  prompt += QString::fromUtf8(diff);
  return prompt;
}
