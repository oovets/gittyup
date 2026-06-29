#include "KnowledgeBase.h"
#include "EmbeddingClient.h"
#include "VectorMath.h"
#include "conf/Settings.h"
#include "conf/Setting.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>

KnowledgeBase *KnowledgeBase::instance() {
  static KnowledgeBase sInstance;
  return &sInstance;
}

KnowledgeBase::KnowledgeBase(QObject *parent) : QObject(parent) {
  mEmbeddingClient = new EmbeddingClient(this);
  initDatabase();
}

bool KnowledgeBase::isEnabled() const {
  return Settings::instance()
      ->value(Setting::Id::AiKnowledgeBaseEnabled)
      .toBool();
}

double KnowledgeBase::similarityThreshold() const {
  QVariant v = Settings::instance()->value(
      Setting::Id::AiKnowledgeBaseSimilarityThreshold);
  if (!v.isValid())
    return 0.85;
  double val = v.toDouble();
  return (val >= 0.5 && val <= 1.0) ? val : 0.85;
}

QString KnowledgeBase::embeddingModel() const {
  QString m = Settings::instance()
                  ->value(Setting::Id::AiEmbeddingModel)
                  .toString();
  return m.isEmpty() ? QStringLiteral("nomic-embed-text") : m;
}

// ---------------------------------------------------------------------------
// Database setup
// ---------------------------------------------------------------------------
void KnowledgeBase::initDatabase() {
  QString dbPath = Settings::userDir().filePath("knowledge.db");
  mDb = QSqlDatabase::addDatabase("QSQLITE", "knowledge");
  mDb.setDatabaseName(dbPath);

  if (!mDb.open())
    return;

  QSqlQuery q(mDb);
  q.exec("PRAGMA journal_mode=WAL");

  q.exec(
      "CREATE TABLE IF NOT EXISTS findings ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  category TEXT NOT NULL DEFAULT '',"
      "  severity TEXT NOT NULL DEFAULT 'MEDIUM',"
      "  description TEXT NOT NULL,"
      "  language TEXT DEFAULT '',"
      "  code_pattern_hash BLOB,"
      "  embedding BLOB,"
      "  embedding_model TEXT DEFAULT '',"
      "  fix_template TEXT DEFAULT '',"
      "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
      "  hit_count INTEGER NOT NULL DEFAULT 0,"
      "  last_hit TEXT"
      ")");

  q.exec(
      "CREATE TABLE IF NOT EXISTS reviews ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  diff_hash BLOB NOT NULL,"
      "  repo_path TEXT NOT NULL,"
      "  timestamp TEXT NOT NULL DEFAULT (datetime('now')),"
      "  full_response TEXT NOT NULL"
      ")");
  q.exec("CREATE INDEX IF NOT EXISTS idx_reviews_diff_hash "
         "ON reviews(diff_hash)");
  q.exec("CREATE INDEX IF NOT EXISTS idx_findings_model "
         "ON findings(embedding_model)");

  q.exec(
      "CREATE TABLE IF NOT EXISTS review_findings ("
      "  review_id INTEGER NOT NULL REFERENCES reviews(id) ON DELETE CASCADE,"
      "  finding_id INTEGER NOT NULL REFERENCES findings(id) ON DELETE CASCADE,"
      "  PRIMARY KEY (review_id, finding_id)"
      ")");

  q.exec(
      "CREATE TABLE IF NOT EXISTS fix_recipes ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  finding_id INTEGER NOT NULL REFERENCES findings(id) ON DELETE CASCADE,"
      "  search_pattern TEXT NOT NULL,"
      "  replace_pattern TEXT NOT NULL,"
      "  file_pattern_glob TEXT DEFAULT '*'"
      ")");

  q.exec(
      "CREATE TABLE IF NOT EXISTS stats ("
      "  key TEXT PRIMARY KEY,"
      "  value INTEGER NOT NULL DEFAULT 0"
      ")");
  q.exec("INSERT OR IGNORE INTO stats(key, value) "
         "VALUES ('cache_hits', 0)");
  q.exec("INSERT OR IGNORE INTO stats(key, value) "
         "VALUES ('cache_misses', 0)");
}

// ---------------------------------------------------------------------------
// Step 1: Exact diff hash lookup
// ---------------------------------------------------------------------------
QString KnowledgeBase::findExactReview(const QByteArray &diffBytes) const {
  if (!mDb.isOpen())
    return {};

  QByteArray hash =
      QCryptographicHash::hash(diffBytes, QCryptographicHash::Sha256);

  QSqlQuery q(mDb);
  q.prepare("SELECT full_response FROM reviews WHERE diff_hash = ? "
            "ORDER BY timestamp DESC LIMIT 1");
  q.addBindValue(hash);
  if (q.exec() && q.next())
    return q.value(0).toString();
  return {};
}

// ---------------------------------------------------------------------------
// Step 2: Semantic similarity search
// ---------------------------------------------------------------------------
void KnowledgeBase::findSimilarFindings(
    const QStringList &hunkTexts,
    std::function<void(MatchResult)> callback) {

  if (!mDb.isOpen() || hunkTexts.isEmpty()) {
    callback({false, {}, 0.0});
    return;
  }

  QString model = embeddingModel();
  double threshold = similarityThreshold();

  mEmbeddingClient->setModelName(model);
  mEmbeddingClient->embedBatch(hunkTexts,
      [this, callback, model, threshold](
          const QList<QVector<float>> &embeddings, const QString &error) {

        if (!error.isEmpty() || embeddings.isEmpty()) {
          callback({false, {}, 0.0});
          return;
        }

        // Load all findings with matching embedding model
        QSqlQuery q(mDb);
        q.prepare("SELECT id, category, severity, description, language, "
                  "embedding, fix_template, hit_count "
                  "FROM findings WHERE embedding IS NOT NULL "
                  "AND embedding_model = ?");
        q.addBindValue(model);
        if (!q.exec()) {
          callback({false, {}, 0.0});
          return;
        }

        QList<Finding> allFindings;
        while (q.next()) {
          Finding f;
          f.id = q.value(0).toLongLong();
          f.category = q.value(1).toString();
          f.severity = q.value(2).toString();
          f.description = q.value(3).toString();
          f.language = q.value(4).toString();
          f.embedding = deserializeEmbedding(q.value(5).toByteArray());
          f.fixTemplate = q.value(6).toString();
          f.hitCount = q.value(7).toInt();
          allFindings.append(f);
        }

        if (allFindings.isEmpty()) {
          callback({false, {}, 0.0});
          return;
        }

        // For each input embedding, find the best matching finding
        QList<Finding> matched;
        double totalSim = 0;

        for (const auto &queryEmb : embeddings) {
          float bestSim = 0;
          int bestIdx = -1;
          for (int i = 0; i < allFindings.size(); ++i) {
            float sim = VectorMath::cosineSimilarity(queryEmb,
                                                     allFindings[i].embedding);
            if (sim > bestSim) {
              bestSim = sim;
              bestIdx = i;
            }
          }
          if (bestIdx >= 0 && bestSim >= threshold) {
            bool alreadyMatched = false;
            for (const auto &m : matched) {
              if (m.id == allFindings[bestIdx].id) {
                alreadyMatched = true;
                break;
              }
            }
            if (!alreadyMatched) {
              matched.append(allFindings[bestIdx]);
              totalSim += bestSim;
            }
          }
        }

        double avgSim = matched.isEmpty() ? 0.0 : totalSim / matched.size();
        bool sufficient = matched.size() >= 2;

        callback({sufficient, matched, avgSim});
      });
}

// ---------------------------------------------------------------------------
// Step 3: Store review results
// ---------------------------------------------------------------------------
QList<qint64> KnowledgeBase::storeReview(const QByteArray &diffBytes,
                                         const QString &repoPath,
                                         const QString &fullResponse,
                                         const QList<Finding> &findings) {
  QList<qint64> findingIds;
  if (!mDb.isOpen())
    return findingIds;

  QByteArray hash =
      QCryptographicHash::hash(diffBytes, QCryptographicHash::Sha256);

  QSqlQuery q(mDb);
  q.prepare("INSERT INTO reviews (diff_hash, repo_path, full_response) "
            "VALUES (?, ?, ?)");
  q.addBindValue(hash);
  q.addBindValue(repoPath);
  q.addBindValue(fullResponse);
  if (!q.exec())
    return findingIds;

  qint64 reviewId = q.lastInsertId().toLongLong();

  for (const Finding &f : findings) {
    QSqlQuery fq(mDb);
    fq.prepare("INSERT INTO findings "
               "(category, severity, description, language, "
               "code_pattern_hash, embedding, embedding_model, fix_template) "
               "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    fq.addBindValue(f.category);
    fq.addBindValue(f.severity);
    fq.addBindValue(f.description);
    fq.addBindValue(f.language);
    fq.addBindValue(f.codePatternHash);
    fq.addBindValue(serializeEmbedding(f.embedding));
    fq.addBindValue(embeddingModel());
    fq.addBindValue(f.fixTemplate);
    if (!fq.exec()) {
      findingIds.append(-1);
      continue;
    }

    qint64 findingId = fq.lastInsertId().toLongLong();
    findingIds.append(findingId);

    QSqlQuery jq(mDb);
    jq.prepare("INSERT INTO review_findings (review_id, finding_id) "
               "VALUES (?, ?)");
    jq.addBindValue(reviewId);
    jq.addBindValue(findingId);
    jq.exec();
  }

  emit statsChanged();
  return findingIds;
}

void KnowledgeBase::storeFixRecipe(qint64 findingId,
                                   const QString &searchPattern,
                                   const QString &replacePattern,
                                   const QString &fileGlob) {
  if (!mDb.isOpen())
    return;
  QSqlQuery q(mDb);
  q.prepare("INSERT INTO fix_recipes "
            "(finding_id, search_pattern, replace_pattern, file_pattern_glob) "
            "VALUES (?, ?, ?, ?)");
  q.addBindValue(findingId);
  q.addBindValue(searchPattern);
  q.addBindValue(replacePattern);
  q.addBindValue(fileGlob);
  q.exec();
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
void KnowledgeBase::incrementCacheHits() {
  if (!mDb.isOpen())
    return;
  QSqlQuery q(mDb);
  q.exec("UPDATE stats SET value = value + 1 WHERE key = 'cache_hits'");
  emit statsChanged();
}

void KnowledgeBase::incrementCacheMisses() {
  if (!mDb.isOpen())
    return;
  QSqlQuery q(mDb);
  q.exec("UPDATE stats SET value = value + 1 WHERE key = 'cache_misses'");
  emit statsChanged();
}

KnowledgeBase::Stats KnowledgeBase::stats() const {
  Stats s = {0, 0, 0, 0};
  if (!mDb.isOpen())
    return s;

  QSqlQuery q(mDb);
  q.exec("SELECT COUNT(*) FROM findings");
  if (q.next())
    s.totalFindings = q.value(0).toInt();

  q.exec("SELECT COUNT(*) FROM reviews");
  if (q.next())
    s.totalReviews = q.value(0).toInt();

  q.exec("SELECT value FROM stats WHERE key = 'cache_hits'");
  if (q.next())
    s.cacheHits = q.value(0).toInt();

  q.exec("SELECT value FROM stats WHERE key = 'cache_misses'");
  if (q.next())
    s.cacheMisses = q.value(0).toInt();

  return s;
}

void KnowledgeBase::clearAll() {
  if (!mDb.isOpen())
    return;
  QSqlQuery q(mDb);
  q.exec("DELETE FROM review_findings");
  q.exec("DELETE FROM fix_recipes");
  q.exec("DELETE FROM findings");
  q.exec("DELETE FROM reviews");
  q.exec("UPDATE stats SET value = 0");
  emit statsChanged();
}

// ---------------------------------------------------------------------------
// Embedding serialization
// ---------------------------------------------------------------------------
QByteArray KnowledgeBase::serializeEmbedding(const QVector<float> &v) {
  if (v.isEmpty())
    return {};
  // Store unit-length vectors so similarity comparisons stay stable.
  QVector<float> n = VectorMath::normalized(v);
  return QByteArray(reinterpret_cast<const char *>(n.constData()),
                    n.size() * sizeof(float));
}

QVector<float> KnowledgeBase::deserializeEmbedding(const QByteArray &blob) {
  if (blob.isEmpty())
    return {};
  int count = blob.size() / sizeof(float);
  QVector<float> v(count);
  memcpy(v.data(), blob.constData(), count * sizeof(float));
  return v;
}
