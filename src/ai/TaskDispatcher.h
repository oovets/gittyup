#ifndef TASKDISPATCHER_H
#define TASKDISPATCHER_H

#include <QObject>
#include <QQueue>
#include <QMap>
#include <QSet>
#include <QPointer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <functional>

class AiService;
class QNetworkReply;

class TaskDispatcher : public QObject {
  Q_OBJECT

public:
  static TaskDispatcher *instance();

  enum class TaskType { Review, Chat, Embedding, CommitMsg };
  enum class Priority { Low, Normal, High, Critical };

  struct ModelRoute {
    QString model;
    bool prefersGpu = true;
    int maxConcurrent = 1;
  };

  struct TaskStats {
    int queued = 0;
    int active = 0;
    int completed = 0;
    int failed = 0;
    qint64 totalTokensIn = 0;
    qint64 totalTokensOut = 0;
    int cacheHits = 0;
  };

  struct TaskResult {
    QString text;
    QString error;
    int tokensIn = 0;
    int tokensOut = 0;
    qint64 durationMs = 0;
    bool fromCache = false;
  };

  using ResultCallback = std::function<void(const TaskResult &result)>;
  using StreamCallback = std::function<void(const QString &chunk)>;
  using StreamDoneCallback =
      std::function<void(const QString &text, const QString &error)>;

  // Opaque handle identifying a submitted task, usable with cancel(). 0 means
  // the task was rejected (e.g. the queue was full).
  using TaskHandle = quint64;

  TaskHandle submit(TaskType type, const QString &prompt,
                    ResultCallback callback,
                    Priority priority = Priority::Normal, int maxTokens = 4096);

  TaskHandle submitStreaming(TaskType type, const QString &prompt,
                             StreamCallback onChunk, StreamDoneCallback onDone,
                             Priority priority = Priority::Normal,
                             int maxTokens = 4096);

  // Multi-turn streaming variant: sends a full message array (role/content
  // objects) plus an optional system prompt. Used by the chat panel so it
  // shares the dispatcher's timeouts, retry, cancellation and stats.
  TaskHandle submitStreamingChat(const QJsonArray &messages,
                                 const QString &system, StreamCallback onChunk,
                                 StreamDoneCallback onDone,
                                 Priority priority = Priority::Normal,
                                 int maxTokens = 4096);

  // Abort a queued or in-flight task. The task's callback fires once with a
  // "Cancelled" result (non-streaming) or the partial text (streaming).
  void cancel(TaskHandle handle);

  void setModelRoute(TaskType type, const ModelRoute &route);
  ModelRoute modelRoute(TaskType type) const;

  TaskStats stats() const;
  void resetStats();

  // Record that a request was served from a local cache (knowledge base) so the
  // live stats reflect avoided API/GPU calls.
  void recordCacheHit();

  int queueDepth() const;
  int activeCount() const;

signals:
  void statsChanged();
  void taskCompleted(TaskType type, bool success);

private:
  struct Task {
    TaskHandle id = 0;
    TaskType type;
    Priority priority;
    QString prompt;
    QString system;       // optional system prompt (multi-turn chat)
    QJsonArray messages;  // optional role/content array (multi-turn chat)
    int maxTokens;
    int attempt = 0;
    ResultCallback callback;
    StreamCallback streamChunk;
    StreamDoneCallback streamDone;
    bool streaming = false;
    QElapsedTimer timer;

    bool operator<(const Task &other) const {
      return static_cast<int>(priority) < static_cast<int>(other.priority);
    }
  };

  void executeStreamTask(Task &task);

  explicit TaskDispatcher(QObject *parent = nullptr);
  void processQueue();
  void executeTask(Task &task);

  // Insert a task by priority (bounded by mMaxQueue). Returns its handle, or 0
  // if the queue was full and the task was rejected.
  TaskHandle enqueue(Task task);

  // Re-queue a transiently-failed task with backoff. Returns true if a retry
  // was scheduled.
  bool maybeRetry(const Task &task, QNetworkReply *reply);

  QList<Task> mQueue;
  QMap<TaskType, ModelRoute> mRoutes;
  QMap<TaskType, int> mActiveCount;
  QMap<TaskHandle, QPointer<QNetworkReply>> mActiveReplies;
  QSet<TaskHandle> mCancelled;
  TaskHandle mNextHandle = 1;
  int mMaxQueue = 200;
  TaskStats mStats;
};

#endif
