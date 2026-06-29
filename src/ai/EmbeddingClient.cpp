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

QString EmbeddingClient::ollamaBase() const {
  QString base = Settings::instance()->value(Setting::Id::AiOllamaUrl).toString();
  if (base.isEmpty())
    base = QStringLiteral("http://localhost:11434");
  return base.trimmed().remove(QRegularExpression("/$"));
}

void EmbeddingClient::embed(const QString &text, SingleCallback callback) {
  const QString base = ollamaBase();

  QNetworkRequest request;
  request.setUrl(QUrl(base + "/api/embeddings"));
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  QJsonObject body;
  body["model"] = mModel;
  body["prompt"] = text;

  request.setTransferTimeout(
      Settings::instance()->value(Setting::Id::AiRequestTimeoutSeconds, 300).toInt() *
      1000);

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

  Settings *settings = Settings::instance();
  const bool useBatch =
      settings->value(Setting::Id::AiEmbeddingBatchEndpoint, true).toBool();
  int concurrency =
      settings->value(Setting::Id::AiEmbeddingConcurrency, 4).toInt();
  concurrency = qBound(1, concurrency, 16);

  if (useBatch)
    embedViaBatchApi(texts, /*batchSize=*/32, concurrency, callback);
  else
    embedViaSingle(texts, concurrency, callback);
}

void EmbeddingClient::embedViaSingle(const QStringList &texts, int concurrency,
                                     BatchCallback callback) {
  struct State {
    QStringList texts;
    QList<QVector<float>> results;
    int next = 0;
    int completed = 0;
    int inFlight = 0;
    int concurrency = 1;
    bool failed = false;
    BatchCallback callback;
    EmbeddingClient *self = nullptr;
  };

  auto state = std::make_shared<State>();
  state->texts = texts;
  state->results.resize(texts.size());
  state->concurrency = qMax(1, concurrency);
  state->callback = callback;
  state->self = this;

  auto dispatch = std::make_shared<std::function<void()>>();
  *dispatch = [state, dispatch]() {
    while (!state->failed && state->inFlight < state->concurrency &&
           state->next < state->texts.size()) {
      const int i = state->next++;
      state->inFlight++;
      state->self->embed(
          state->texts[i],
          [state, dispatch, i](QVector<float> emb, const QString &err) {
            if (state->failed)
              return;
            if (!err.isEmpty()) {
              state->failed = true;
              state->callback({}, err);
              return;
            }
            state->results[i] = emb;
            state->inFlight--;
            if (++state->completed == state->texts.size()) {
              state->callback(state->results, QString());
              return;
            }
            (*dispatch)();
          });
    }
  };
  (*dispatch)();
}

void EmbeddingClient::embedViaBatchApi(const QStringList &texts, int batchSize,
                                       int concurrency,
                                       BatchCallback callback) {
  struct Group {
    int start;
    QStringList items;
  };

  struct State {
    QStringList texts;
    QList<QVector<float>> results;
    QList<Group> groups;
    int nextGroup = 0;
    int completedGroups = 0;
    int inFlight = 0;
    int concurrency = 1;
    bool done = false; // finished, errored, or fell back
    BatchCallback callback;
    EmbeddingClient *self = nullptr;
  };

  auto state = std::make_shared<State>();
  state->texts = texts;
  state->results.resize(texts.size());
  state->concurrency = qMax(1, concurrency);
  state->callback = callback;
  state->self = this;

  const int bs = qMax(1, batchSize);
  for (int i = 0; i < texts.size(); i += bs)
    state->groups.append({i, texts.mid(i, bs)});

  const QString base = ollamaBase();
  const QString model = mModel;

  auto dispatch = std::make_shared<std::function<void()>>();
  *dispatch = [state, dispatch, base, model]() {
    while (!state->done && state->inFlight < state->concurrency &&
           state->nextGroup < state->groups.size()) {
      const Group group = state->groups[state->nextGroup++];
      state->inFlight++;

      QNetworkRequest request;
      request.setUrl(QUrl(base + "/api/embed"));
      request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

      QJsonObject body;
      body["model"] = model;
      QJsonArray input;
      for (const QString &t : group.items)
        input.append(t);
      body["input"] = input;

      request.setTransferTimeout(Settings::instance()
                                     ->value(Setting::Id::AiRequestTimeoutSeconds, 300)
                                     .toInt() *
                                 1000);

      QNetworkReply *reply =
          state->self->mNet.post(request, QJsonDocument(body).toJson());
      QObject::connect(
          reply, &QNetworkReply::finished, state->self,
          [state, dispatch, reply, group] {
            reply->deleteLater();
            if (state->done)
              return;

            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonArray rows = doc["embeddings"].toArray();

            // Endpoint unsupported (404) or unexpected shape -> fall back once
            // to the single-input endpoint for the whole set.
            const bool unsupported =
                status == 404 ||
                (reply->error() == QNetworkReply::NoError && rows.isEmpty());
            if (unsupported) {
              state->done = true;
              state->self->embedViaSingle(state->texts, state->concurrency,
                                          state->callback);
              return;
            }

            if (reply->error() != QNetworkReply::NoError) {
              state->done = true;
              state->callback({}, reply->errorString());
              return;
            }

            for (int j = 0; j < rows.size() && j < group.items.size(); ++j) {
              QJsonArray arr = rows[j].toArray();
              QVector<float> emb;
              emb.reserve(arr.size());
              for (const auto &val : arr)
                emb.append(static_cast<float>(val.toDouble()));
              state->results[group.start + j] = emb;
            }

            state->inFlight--;
            if (++state->completedGroups == state->groups.size()) {
              state->done = true;
              state->callback(state->results, QString());
              return;
            }
            (*dispatch)();
          });
    }
  };
  (*dispatch)();
}
