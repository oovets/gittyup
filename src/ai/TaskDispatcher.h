#ifndef TASKDISPATCHER_H
#define TASKDISPATCHER_H

#include <QObject>
#include <QQueue>
#include <QMap>
#include <QElapsedTimer>
#include <functional>

class AiService;

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

  void submit(TaskType type, const QString &prompt, ResultCallback callback,
              Priority priority = Priority::Normal, int maxTokens = 4096);

  void submitStreaming(TaskType type, const QString &prompt,
                       StreamCallback onChunk, StreamDoneCallback onDone,
                       Priority priority = Priority::Normal,
                       int maxTokens = 4096);

  void setModelRoute(TaskType type, const ModelRoute &route);
  ModelRoute modelRoute(TaskType type) const;

  TaskStats stats() const;
  void resetStats();

  int queueDepth() const;
  int activeCount() const;

signals:
  void statsChanged();
  void taskCompleted(TaskType type, bool success);

private:
  struct Task {
    TaskType type;
    Priority priority;
    QString prompt;
    int maxTokens;
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

  QList<Task> mQueue;
  QMap<TaskType, ModelRoute> mRoutes;
  QMap<TaskType, int> mActiveCount;
  TaskStats mStats;
};

#endif
