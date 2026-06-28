#include "AiService.h"
#include "conf/Settings.h"
#include "conf/Setting.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
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
