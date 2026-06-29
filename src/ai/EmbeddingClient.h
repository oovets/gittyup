#ifndef EMBEDDINGCLIENT_H
#define EMBEDDINGCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QVector>
#include <functional>

class EmbeddingClient : public QObject {
  Q_OBJECT

public:
  explicit EmbeddingClient(QObject *parent = nullptr);

  using SingleCallback =
      std::function<void(QVector<float> embedding, const QString &error)>;
  using BatchCallback =
      std::function<void(QList<QVector<float>> embeddings, const QString &error)>;

  void embed(const QString &text, SingleCallback callback);
  void embedBatch(const QStringList &texts, BatchCallback callback);

  QString modelName() const { return mModel; }
  void setModelName(const QString &name) { mModel = name; }

private:
  QString ollamaBase() const;

  // Capped-concurrency fan-out of single /api/embeddings calls (the fallback
  // path when the batched endpoint is unavailable or disabled).
  void embedViaSingle(const QStringList &texts, int concurrency,
                      BatchCallback callback);

  // One /api/embed request per group of inputs (the fast path). Falls back to
  // embedViaSingle if the server returns 404 or an unexpected response shape.
  void embedViaBatchApi(const QStringList &texts, int batchSize,
                        int concurrency, BatchCallback callback);

  QNetworkAccessManager mNet;
  QString mModel = QStringLiteral("nomic-embed-text");
};

#endif // EMBEDDINGCLIENT_H
