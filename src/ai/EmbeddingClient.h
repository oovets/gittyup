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
  QNetworkAccessManager mNet;
  QString mModel = QStringLiteral("nomic-embed-text");
};

#endif // EMBEDDINGCLIENT_H
