#include "EmbeddingClient.h"
#include "conf/Settings.h"
#include "conf/Setting.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

EmbeddingClient::EmbeddingClient(QObject *parent) : QObject(parent) {}

void EmbeddingClient::embed(const QString &text, SingleCallback callback) {
  Settings *settings = Settings::instance();
  QString base = settings->value(Setting::Id::AiOllamaUrl).toString();
  if (base.isEmpty())
    base = QStringLiteral("http://localhost:11434");
  base = base.trimmed().remove(QRegularExpression("/$"));

  QNetworkRequest request;
  request.setUrl(QUrl(base + "/api/embeddings"));
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  QJsonObject body;
  body["model"] = mModel;
  body["prompt"] = text;

  QNetworkReply *reply = mNet.post(request, QJsonDocument(body).toJson());
  connect(reply, &QNetworkReply::finished, this, [reply, callback] {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
      callback({}, reply->errorString());
      return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonArray arr = doc["embedding"].toArray();
    if (arr.isEmpty()) {
      callback({}, QStringLiteral("No embedding in response."));
      return;
    }

    QVector<float> embedding;
    embedding.reserve(arr.size());
    for (const auto &val : arr)
      embedding.append(static_cast<float>(val.toDouble()));

    callback(embedding, QString());
  });
}

void EmbeddingClient::embedBatch(const QStringList &texts,
                                 BatchCallback callback) {
  if (texts.isEmpty()) {
    callback({}, QString());
    return;
  }

  struct State {
    QList<QVector<float>> results;
    int remaining;
    bool failed = false;
    QString error;
    BatchCallback callback;
  };

  auto state = std::make_shared<State>();
  state->results.resize(texts.size());
  state->remaining = texts.size();
  state->callback = callback;

  for (int i = 0; i < texts.size(); ++i) {
    embed(texts[i], [state, i](QVector<float> emb, const QString &err) {
      if (state->failed)
        return;

      if (!err.isEmpty()) {
        state->failed = true;
        state->callback({}, err);
        return;
      }

      state->results[i] = emb;
      state->remaining--;

      if (state->remaining == 0)
        state->callback(state->results, QString());
    });
  }
}
