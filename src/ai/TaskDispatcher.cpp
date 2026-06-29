#include "TaskDispatcher.h"
#include "AiService.h"
#include "conf/Settings.h"
#include "conf/Setting.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSharedPointer>

TaskDispatcher *TaskDispatcher::instance() {
  static TaskDispatcher sInstance;
  return &sInstance;
}

TaskDispatcher::TaskDispatcher(QObject *parent) : QObject(parent) {
  AiService::Config cfg = AiService::instance()->currentConfig();

  QString reviewModel = cfg.model;
  QString baseUrl = cfg.baseUrl;

  mRoutes[TaskType::Review] = {reviewModel, true, 1};
  mRoutes[TaskType::Chat] = {reviewModel, true, 2};
  mRoutes[TaskType::Embedding] = {
      QStringLiteral("nomic-embed-text"), false, 4};
  mRoutes[TaskType::CommitMsg] = {reviewModel, false, 2};

  for (auto type : {TaskType::Review, TaskType::Chat, TaskType::Embedding,
                    TaskType::CommitMsg})
    mActiveCount[type] = 0;
}

void TaskDispatcher::submit(TaskType type, const QString &prompt,
                            ResultCallback callback, Priority priority,
                            int maxTokens) {
  Task task;
  task.type = type;
  task.priority = priority;
  task.prompt = prompt;
  task.maxTokens = maxTokens;
  task.callback = callback;

  int insertPos = 0;
  for (int i = 0; i < mQueue.size(); ++i) {
    if (static_cast<int>(mQueue[i].priority) >=
        static_cast<int>(priority)) {
      insertPos = i + 1;
    } else {
      break;
    }
  }
  mQueue.insert(insertPos, task);

  mStats.queued = mQueue.size();
  emit statsChanged();

  processQueue();
}

void TaskDispatcher::submitStreaming(TaskType type, const QString &prompt,
                                     StreamCallback onChunk,
                                     StreamDoneCallback onDone,
                                     Priority priority, int maxTokens) {
  Task task;
  task.type = type;
  task.priority = priority;
  task.prompt = prompt;
  task.maxTokens = maxTokens;
  task.streaming = true;
  task.streamChunk = onChunk;
  task.streamDone = onDone;

  int insertPos = 0;
  for (int i = 0; i < mQueue.size(); ++i) {
    if (static_cast<int>(mQueue[i].priority) >=
        static_cast<int>(priority))
      insertPos = i + 1;
    else
      break;
  }
  mQueue.insert(insertPos, task);

  mStats.queued = mQueue.size();
  emit statsChanged();

  processQueue();
}

void TaskDispatcher::setModelRoute(TaskType type, const ModelRoute &route) {
  mRoutes[type] = route;
}

TaskDispatcher::ModelRoute TaskDispatcher::modelRoute(TaskType type) const {
  return mRoutes.value(type);
}

TaskDispatcher::TaskStats TaskDispatcher::stats() const { return mStats; }

void TaskDispatcher::resetStats() {
  mStats = TaskStats();
  mStats.queued = mQueue.size();
  emit statsChanged();
}

int TaskDispatcher::queueDepth() const { return mQueue.size(); }

int TaskDispatcher::activeCount() const {
  int total = 0;
  for (auto count : mActiveCount)
    total += count;
  return total;
}

void TaskDispatcher::processQueue() {
  while (!mQueue.isEmpty()) {
    bool dispatched = false;
    for (int i = 0; i < mQueue.size(); ++i) {
      Task &task = mQueue[i];
      ModelRoute route = mRoutes.value(task.type);
      if (mActiveCount[task.type] < route.maxConcurrent) {
        Task taken = mQueue.takeAt(i);
        taken.timer.start();
        mActiveCount[taken.type]++;
        mStats.queued = mQueue.size();
        mStats.active = activeCount();
        emit statsChanged();
        if (taken.streaming)
          executeStreamTask(taken);
        else
          executeTask(taken);
        dispatched = true;
        break;
      }
    }
    if (!dispatched)
      break;
  }
}

void TaskDispatcher::executeTask(Task &task) {
  AiService::Config cfg = AiService::instance()->currentConfig();
  ModelRoute route = mRoutes.value(task.type);

  QString baseUrl = cfg.baseUrl;
  if (baseUrl.isEmpty())
    baseUrl = QStringLiteral("http://localhost:11434");
  baseUrl = baseUrl.trimmed().remove(QRegularExpression("/$"));

  QNetworkRequest request;
  QJsonObject body;

  if (cfg.provider == QStringLiteral("ollama")) {
    if (task.type == TaskType::Embedding) {
      request.setUrl(QUrl(baseUrl + "/api/embeddings"));
      body["model"] = route.model;
      body["prompt"] = task.prompt;
    } else {
      request.setUrl(QUrl(baseUrl + "/api/chat"));
      body["model"] = route.model;
      body["stream"] = false;
      body["messages"] =
          QJsonArray{QJsonObject{{"role", "user"}, {"content", task.prompt}}};
    }
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  } else {
    if (cfg.apiKey.isEmpty()) {
      TaskResult result;
      result.error = QStringLiteral("No API key configured.");
      result.durationMs = task.timer.elapsed();
      mActiveCount[task.type]--;
      mStats.active = activeCount();
      mStats.failed++;
      emit statsChanged();
      emit taskCompleted(task.type, false);
      task.callback(result);
      processQueue();
      return;
    }
    request.setUrl(
        QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-api-key", cfg.apiKey.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    body["model"] = cfg.model;
    body["max_tokens"] = task.maxTokens;
    body["messages"] =
        QJsonArray{QJsonObject{{"role", "user"}, {"content", task.prompt}}};
  }

  static QNetworkAccessManager sNet;
  QNetworkReply *reply = sNet.post(request, QJsonDocument(body).toJson());

  TaskType taskType = task.type;
  ResultCallback callback = task.callback;
  QElapsedTimer timer = task.timer;

  connect(reply, &QNetworkReply::finished, this,
          [this, reply, taskType, callback, timer,
           provider = cfg.provider] {
            reply->deleteLater();

            TaskResult result;
            result.durationMs = timer.elapsed();

            if (reply->error() != QNetworkReply::NoError) {
              result.error = reply->errorString();
              mActiveCount[taskType]--;
              mStats.active = activeCount();
              mStats.failed++;
              emit statsChanged();
              emit taskCompleted(taskType, false);
              callback(result);
              processQueue();
              return;
            }

            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());

            if (provider == QStringLiteral("ollama")) {
              if (taskType == TaskType::Embedding) {
                result.text = QString::fromUtf8(
                    QJsonDocument(doc.object()).toJson(QJsonDocument::Compact));
              } else {
                result.text =
                    doc["message"]["content"].toString().trimmed();
                if (doc["prompt_eval_count"].isDouble())
                  result.tokensIn = doc["prompt_eval_count"].toInt();
                if (doc["eval_count"].isDouble())
                  result.tokensOut = doc["eval_count"].toInt();
              }
            } else {
              result.text =
                  doc["content"][0]["text"].toString().trimmed();
              QJsonObject usage = doc["usage"].toObject();
              result.tokensIn = usage["input_tokens"].toInt();
              result.tokensOut = usage["output_tokens"].toInt();
            }

            mActiveCount[taskType]--;
            mStats.active = activeCount();
            mStats.completed++;
            mStats.totalTokensIn += result.tokensIn;
            mStats.totalTokensOut += result.tokensOut;

            emit statsChanged();
            emit taskCompleted(taskType, true);
            callback(result);
            processQueue();
          });
}

void TaskDispatcher::executeStreamTask(Task &task) {
  AiService::Config cfg = AiService::instance()->currentConfig();
  ModelRoute route = mRoutes.value(task.type);

  QString baseUrl = cfg.baseUrl;
  if (baseUrl.isEmpty())
    baseUrl = QStringLiteral("http://localhost:11434");
  baseUrl = baseUrl.trimmed().remove(QRegularExpression("/$"));

  QNetworkRequest request;
  QJsonObject body;

  if (cfg.provider == QStringLiteral("ollama")) {
    request.setUrl(QUrl(baseUrl + "/api/chat"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    body["model"] = route.model;
    body["stream"] = true;
    body["messages"] =
        QJsonArray{QJsonObject{{"role", "user"}, {"content", task.prompt}}};
  } else {
    if (cfg.apiKey.isEmpty()) {
      mActiveCount[task.type]--;
      mStats.active = activeCount();
      mStats.failed++;
      emit statsChanged();
      emit taskCompleted(task.type, false);
      task.streamDone(QString(), QStringLiteral("No API key configured."));
      processQueue();
      return;
    }
    request.setUrl(
        QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-api-key", cfg.apiKey.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    body["model"] = cfg.model;
    body["max_tokens"] = task.maxTokens;
    body["stream"] = true;
    body["messages"] =
        QJsonArray{QJsonObject{{"role", "user"}, {"content", task.prompt}}};
  }

  static QNetworkAccessManager sStreamNet;
  QNetworkReply *reply =
      sStreamNet.post(request, QJsonDocument(body).toJson());

  TaskType taskType = task.type;
  StreamCallback onChunk = task.streamChunk;
  StreamDoneCallback onDone = task.streamDone;
  QElapsedTimer timer = task.timer;
  auto buffer = QSharedPointer<QString>::create();

  connect(reply, &QNetworkReply::readyRead, this,
          [reply, onChunk, buffer, provider = cfg.provider] {
            QByteArray data = reply->readAll();
            for (const QByteArray &rawLine : data.split('\n')) {
              QByteArray trimmed = rawLine.trimmed();
              if (trimmed.isEmpty())
                continue;

              if (provider == QStringLiteral("ollama")) {
                QJsonDocument doc = QJsonDocument::fromJson(trimmed);
                if (doc.isNull())
                  continue;
                QString chunk = doc["message"]["content"].toString();
                if (!chunk.isEmpty()) {
                  *buffer += chunk;
                  onChunk(chunk);
                }
              } else {
                if (!trimmed.startsWith("data: "))
                  continue;
                QByteArray json = trimmed.mid(6);
                if (json == "[DONE]")
                  continue;
                QJsonDocument doc = QJsonDocument::fromJson(json);
                if (doc.isNull())
                  continue;
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
          [this, reply, taskType, onDone, buffer, timer] {
            reply->deleteLater();

            mActiveCount[taskType]--;
            mStats.active = activeCount();

            if (reply->error() != QNetworkReply::NoError &&
                buffer->isEmpty()) {
              mStats.failed++;
              emit statsChanged();
              emit taskCompleted(taskType, false);
              onDone(QString(), reply->errorString());
            } else {
              mStats.completed++;
              emit statsChanged();
              emit taskCompleted(taskType, true);
              onDone(*buffer, QString());
            }
            processQueue();
          });
}
