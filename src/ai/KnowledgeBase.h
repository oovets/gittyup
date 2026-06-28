#ifndef KNOWLEDGEBASE_H
#define KNOWLEDGEBASE_H

#include <QObject>
#include <QSqlDatabase>
#include <QVector>
#include <functional>

class EmbeddingClient;

class KnowledgeBase : public QObject {
  Q_OBJECT

public:
  static KnowledgeBase *instance();

  bool isEnabled() const;

  struct Finding {
    qint64 id = -1;
    QString category;
    QString severity;
    QString description;
    QString language;
    QByteArray codePatternHash;
    QVector<float> embedding;
    QString fixTemplate;
    int hitCount = 0;
  };

  struct FixRecipe {
    qint64 id = -1;
    qint64 findingId;
    QString searchPattern;
    QString replacePattern;
    QString filePatternGlob;
  };

  struct MatchResult {
    bool sufficient;
    QList<Finding> findings;
    double averageSimilarity;
  };

  struct Stats {
    int totalFindings;
    int totalReviews;
    int cacheHits;
    int cacheMisses;
  };

  QString findExactReview(const QByteArray &diffBytes) const;

  void findSimilarFindings(const QStringList &hunkTexts,
                           std::function<void(MatchResult)> callback);

  QList<qint64> storeReview(const QByteArray &diffBytes, const QString &repoPath,
                            const QString &fullResponse,
                            const QList<Finding> &findings);

  void storeFixRecipe(qint64 findingId, const QString &searchPattern,
                      const QString &replacePattern,
                      const QString &fileGlob = QStringLiteral("*"));

  void clearAll();
  Stats stats() const;
  void incrementCacheHits();
  void incrementCacheMisses();

  double similarityThreshold() const;
  QString embeddingModel() const;

signals:
  void statsChanged();

private:
  explicit KnowledgeBase(QObject *parent = nullptr);
  void initDatabase();

  static QByteArray serializeEmbedding(const QVector<float> &v);
  static QVector<float> deserializeEmbedding(const QByteArray &blob);

  QSqlDatabase mDb;
  EmbeddingClient *mEmbeddingClient;
};

#endif // KNOWLEDGEBASE_H
