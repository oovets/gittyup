#include "CodebaseIndex.h"
#include "EmbeddingClient.h"
#include "VectorMath.h"
#include "conf/RecentRepositories.h"
#include "conf/RecentRepository.h"
#include "conf/Settings.h"
#include "conf/Setting.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>

static const QStringList kSourceExtensions = {
    "*.cpp", "*.h",   "*.c",    "*.hpp",  "*.cc",  "*.cxx",
    "*.py",  "*.js",  "*.ts",   "*.tsx",  "*.jsx", "*.rs",
    "*.go",  "*.java", "*.rb",  "*.swift", "*.kt",
    "*.cs",  "*.lua", "*.sh",   "*.yaml", "*.yml",
    "*.toml", "*.json", "*.xml", "*.cmake", "CMakeLists.txt",
    "*.sql", "*.proto", "*.graphql"};

static const int kMaxFileSize = 100000;
static const int kChunkLines = 50;
static const int kChunkOverlap = 10;

CodebaseIndex *CodebaseIndex::instance() {
  static CodebaseIndex sInstance;
  return &sInstance;
}

CodebaseIndex::CodebaseIndex(QObject *parent) : QObject(parent) {
  mEmbeddingClient = new EmbeddingClient(this);
  initDatabase();
}

void CodebaseIndex::initDatabase() {
  QString dbPath = Settings::userDir().filePath("codebase_index.db");

  if (QSqlDatabase::contains("codebase_index"))
    mDb = QSqlDatabase::database("codebase_index", false);
  else
    mDb = QSqlDatabase::addDatabase("QSQLITE", "codebase_index");

  mDb.setDatabaseName(dbPath);
  if (!mDb.open())
    return;

  QSqlQuery q(mDb);
  q.exec("PRAGMA journal_mode=WAL");

  q.exec(
      "CREATE TABLE IF NOT EXISTS file_chunks ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  repo_path TEXT NOT NULL,"
      "  file_path TEXT NOT NULL,"
      "  start_line INTEGER NOT NULL,"
      "  end_line INTEGER NOT NULL,"
      "  content TEXT NOT NULL,"
      "  embedding BLOB,"
      "  embedding_model TEXT DEFAULT '',"
      "  file_sha TEXT NOT NULL,"
      "  indexed_at TEXT NOT NULL DEFAULT (datetime('now'))"
      ")");

  q.exec("CREATE INDEX IF NOT EXISTS idx_chunks_repo "
         "ON file_chunks(repo_path)");
  q.exec("CREATE INDEX IF NOT EXISTS idx_chunks_file "
         "ON file_chunks(repo_path, file_path, file_sha)");
}

// ---------------------------------------------------------------------------
// Collect git-tracked source files
// ---------------------------------------------------------------------------
QList<CodebaseIndex::FileEntry>
CodebaseIndex::collectSourceFiles(const QString &repoPath) const {
  QList<FileEntry> files;

  QProcess git;
  git.setWorkingDirectory(repoPath);
  git.start("git", {"ls-files", "--cached", "--others", "--exclude-standard"});
  if (!git.waitForFinished(15000) || git.exitCode() != 0)
    return files;

  QString output = QString::fromUtf8(git.readAllStandardOutput());
  QStringList trackedFiles = output.split('\n', Qt::SkipEmptyParts);

  QSet<QString> extensionSet;
  for (const QString &ext : kSourceExtensions) {
    QString e = ext;
    if (e.startsWith("*."))
      extensionSet.insert(e.mid(1)); // ".cpp", ".h", etc
    else
      extensionSet.insert(e); // "CMakeLists.txt"
  }

  for (const QString &relPath : trackedFiles) {
    bool matches = false;
    for (const QString &ext : extensionSet) {
      if (ext.startsWith('.')) {
        if (relPath.endsWith(ext, Qt::CaseInsensitive)) {
          matches = true;
          break;
        }
      } else {
        if (relPath.endsWith(ext)) {
          matches = true;
          break;
        }
      }
    }
    if (!matches)
      continue;

    QString absPath = repoPath + "/" + relPath;
    QFileInfo fi(absPath);
    if (!fi.exists() || fi.size() > kMaxFileSize)
      continue;

    // Get file's blob SHA for change detection
    QProcess shaProc;
    shaProc.setWorkingDirectory(repoPath);
    shaProc.start("git", {"hash-object", absPath});
    QString sha;
    if (shaProc.waitForFinished(3000) && shaProc.exitCode() == 0)
      sha = QString::fromUtf8(shaProc.readAllStandardOutput()).trimmed();
    else
      sha = QString::number(fi.lastModified().toSecsSinceEpoch());

    files.append({relPath, absPath, sha});
  }

  return files;
}

// ---------------------------------------------------------------------------
// Chunk a file into overlapping segments
// ---------------------------------------------------------------------------
QStringList CodebaseIndex::chunkFile(const QString &content,
                                     int maxLines) const {
  QStringList chunks;
  QStringList lines = content.split('\n');

  if (lines.size() <= maxLines) {
    chunks.append(content);
    return chunks;
  }

  int step = maxLines - kChunkOverlap;
  if (step < 1)
    step = 1;

  for (int i = 0; i < lines.size(); i += step) {
    int end = qMin(i + maxLines, lines.size());
    QStringList segment = lines.mid(i, end - i);
    chunks.append(segment.join('\n'));
    if (end >= lines.size())
      break;
  }

  return chunks;
}

// ---------------------------------------------------------------------------
// Check if a file version is already indexed
// ---------------------------------------------------------------------------
bool CodebaseIndex::isFileIndexed(const QString &repoPath,
                                   const QString &filePath,
                                   const QString &sha) const {
  if (!mDb.isOpen())
    return false;

  QSqlQuery q(mDb);
  q.prepare("SELECT 1 FROM file_chunks "
            "WHERE repo_path = ? AND file_path = ? AND file_sha = ? LIMIT 1");
  q.addBindValue(repoPath);
  q.addBindValue(filePath);
  q.addBindValue(sha);
  return q.exec() && q.next();
}

// ---------------------------------------------------------------------------
// Store a chunk with its embedding
// ---------------------------------------------------------------------------
void CodebaseIndex::storeChunk(const QString &repoPath,
                                const QString &filePath, int startLine,
                                int endLine, const QString &content,
                                const QVector<float> &embedding,
                                const QString &sha) {
  if (!mDb.isOpen())
    return;

  QSqlQuery q(mDb);
  q.prepare("INSERT INTO file_chunks "
            "(repo_path, file_path, start_line, end_line, content, "
            "embedding, embedding_model, file_sha) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  q.addBindValue(repoPath);
  q.addBindValue(filePath);
  q.addBindValue(startLine);
  q.addBindValue(endLine);
  q.addBindValue(content);
  q.addBindValue(serializeEmbedding(embedding));

  QString model = Settings::instance()
                      ->value(Setting::Id::AiEmbeddingModel)
                      .toString();
  if (model.isEmpty())
    model = QStringLiteral("nomic-embed-text");
  q.addBindValue(model);
  q.addBindValue(sha);
  q.exec();
}

// ---------------------------------------------------------------------------
// Index a single repo
// ---------------------------------------------------------------------------
void CodebaseIndex::indexRepo(
    const QString &repoPath,
    std::function<void(int, int)> progress,
    std::function<void(const QString &)> done) {

  if (mIndexing) {
    if (done)
      done("Already indexing");
    return;
  }

  mIndexing = true;
  emit indexingStarted(repoPath);

  QList<FileEntry> files = collectSourceFiles(repoPath);

  // Filter out already-indexed files
  QList<FileEntry> needsIndexing;
  for (const auto &f : files) {
    if (!isFileIndexed(repoPath, f.relativePath, f.sha)) {
      // Remove old chunks for this file
      QSqlQuery del(mDb);
      del.prepare(
          "DELETE FROM file_chunks WHERE repo_path = ? AND file_path = ?");
      del.addBindValue(repoPath);
      del.addBindValue(f.relativePath);
      del.exec();

      needsIndexing.append(f);
    }
  }

  if (needsIndexing.isEmpty()) {
    mIndexing = false;
    emit indexingFinished(repoPath, 0);
    if (done)
      done({});
    return;
  }

  // Build all chunks to embed
  struct ChunkWork {
    QString repoPath;
    QString filePath;
    int startLine;
    int endLine;
    QString content;
    QString sha;
  };

  QList<ChunkWork> allChunks;
  for (const auto &f : needsIndexing) {
    QFile file(f.absolutePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
      continue;

    QString content = QTextStream(&file).readAll();
    file.close();

    QStringList chunks = chunkFile(content, kChunkLines);
    int step = kChunkLines - kChunkOverlap;
    if (step < 1)
      step = 1;

    for (int i = 0; i < chunks.size(); ++i) {
      int startLine = i * step + 1;
      int lines = chunks[i].count('\n') + 1;
      allChunks.append(
          {repoPath, f.relativePath, startLine, startLine + lines - 1,
           // Prepend file path for better embedding context
           f.relativePath + ":\n" + chunks[i], f.sha});
    }
  }

  int totalChunks = allChunks.size();
  if (progress)
    progress(0, totalChunks);

  // Process chunks sequentially in batches of 5
  struct IndexState {
    QList<ChunkWork> chunks;
    int current = 0;
    int total;
    QString repoPath;
    std::function<void(int, int)> progress;
    std::function<void(const QString &)> done;
  };

  auto state = std::make_shared<IndexState>();
  state->chunks = allChunks;
  state->total = totalChunks;
  state->repoPath = repoPath;
  state->progress = progress;
  state->done = done;

  QString model = Settings::instance()
                      ->value(Setting::Id::AiEmbeddingModel)
                      .toString();
  if (model.isEmpty())
    model = QStringLiteral("nomic-embed-text");
  mEmbeddingClient->setModelName(model);

  // Process one chunk at a time
  std::function<void()> processNext;
  processNext = [this, state, processNext]() {
    if (state->current >= state->total) {
      mIndexing = false;
      emit indexingFinished(state->repoPath, state->total);
      if (state->done)
        state->done({});
      return;
    }

    const auto &chunk = state->chunks[state->current];
    mEmbeddingClient->embed(
        chunk.content,
        [this, state, chunk, processNext](QVector<float> emb,
                                           const QString &err) {
          if (err.isEmpty() && !emb.isEmpty()) {
            storeChunk(chunk.repoPath, chunk.filePath, chunk.startLine,
                       chunk.endLine, chunk.content, emb, chunk.sha);
          }

          state->current++;
          if (state->progress)
            state->progress(state->current, state->total);
          emit indexingProgress(state->repoPath, state->current, state->total);

          processNext();
        });
  };

  processNext();
}

// ---------------------------------------------------------------------------
// Index all tracked repos
// ---------------------------------------------------------------------------
void CodebaseIndex::indexAllTracked(
    std::function<void(int, int)> progress,
    std::function<void(const QString &)> done) {

  RecentRepositories *recent = RecentRepositories::instance();
  QStringList repoPaths;
  for (int i = 0; i < recent->count(); ++i) {
    RecentRepository *repo = recent->repository(i);
    QString path = repo->gitpath();
    QDir gitDir(path);
    if (gitDir.dirName() == ".git")
      gitDir.cdUp();
    repoPaths.append(gitDir.absolutePath());
  }

  if (repoPaths.isEmpty()) {
    if (done)
      done({});
    return;
  }

  auto remaining = std::make_shared<QStringList>(repoPaths);
  std::function<void()> next;
  next = [this, remaining, progress, done, next]() {
    if (remaining->isEmpty()) {
      if (done)
        done({});
      return;
    }

    QString path = remaining->takeFirst();
    indexRepo(path, progress, [next](const QString &) { next(); });
  };

  next();
}

// ---------------------------------------------------------------------------
// Search for similar chunks
// ---------------------------------------------------------------------------
void CodebaseIndex::searchSimilar(
    const QString &query, int topK,
    std::function<void(QList<SearchResult>)> callback) {

  QString model = Settings::instance()
                      ->value(Setting::Id::AiEmbeddingModel)
                      .toString();
  if (model.isEmpty())
    model = QStringLiteral("nomic-embed-text");
  mEmbeddingClient->setModelName(model);

  mEmbeddingClient->embed(
      query, [this, topK, model, callback](QVector<float> emb,
                                            const QString &err) {
        if (!err.isEmpty() || emb.isEmpty()) {
          callback({});
          return;
        }
        searchSimilar(emb, topK, callback);
      });
}

void CodebaseIndex::searchSimilar(
    const QVector<float> &queryEmbedding, int topK,
    std::function<void(QList<SearchResult>)> callback) {

  if (!mDb.isOpen() || queryEmbedding.isEmpty()) {
    callback({});
    return;
  }

  QString model = Settings::instance()
                      ->value(Setting::Id::AiEmbeddingModel)
                      .toString();
  if (model.isEmpty())
    model = QStringLiteral("nomic-embed-text");

  QSqlQuery q(mDb);
  q.prepare("SELECT id, repo_path, file_path, start_line, end_line, "
            "content, embedding, file_sha "
            "FROM file_chunks WHERE embedding IS NOT NULL "
            "AND embedding_model = ?");
  q.addBindValue(model);
  if (!q.exec()) {
    callback({});
    return;
  }

  QList<SearchResult> results;

  while (q.next()) {
    QVector<float> chunkEmb = deserializeEmbedding(q.value(6).toByteArray());
    if (chunkEmb.isEmpty())
      continue;

    float sim = VectorMath::cosineSimilarity(queryEmbedding, chunkEmb);

    if (results.size() < topK || sim > results.last().similarity) {
      FileChunk chunk;
      chunk.id = q.value(0).toLongLong();
      chunk.repoPath = q.value(1).toString();
      chunk.filePath = q.value(2).toString();
      chunk.startLine = q.value(3).toInt();
      chunk.endLine = q.value(4).toInt();
      chunk.content = q.value(5).toString();
      chunk.lastCommitSha = q.value(7).toString();

      SearchResult sr{chunk, sim};

      // Insert sorted by similarity (descending)
      int insertAt = 0;
      for (int i = 0; i < results.size(); ++i) {
        if (sim > results[i].similarity) {
          insertAt = i;
          break;
        }
        insertAt = i + 1;
      }
      results.insert(insertAt, sr);

      if (results.size() > topK)
        results.removeLast();
    }
  }

  callback(results);
}

// ---------------------------------------------------------------------------
// Format search results as context for LLM prompt
// ---------------------------------------------------------------------------
QString CodebaseIndex::buildContext(const QList<SearchResult> &results) const {
  if (results.isEmpty())
    return {};

  QString context = QStringLiteral("=== Relevant codebase context ===\n\n");
  for (const auto &r : results) {
    context += QString("--- %1 (lines %2-%3, similarity: %4) ---\n")
                   .arg(r.chunk.filePath)
                   .arg(r.chunk.startLine)
                   .arg(r.chunk.endLine)
                   .arg(r.similarity, 0, 'f', 3);
    context += r.chunk.content + "\n\n";
  }
  return context;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
CodebaseIndex::IndexStats CodebaseIndex::stats() const {
  IndexStats s = {0, 0, 0};
  if (!mDb.isOpen())
    return s;

  QSqlQuery q(mDb);
  q.exec("SELECT COUNT(DISTINCT file_path) FROM file_chunks");
  if (q.next())
    s.totalFiles = q.value(0).toInt();

  q.exec("SELECT COUNT(*) FROM file_chunks");
  if (q.next())
    s.totalChunks = q.value(0).toInt();

  q.exec("SELECT COUNT(DISTINCT repo_path) FROM file_chunks");
  if (q.next())
    s.repoCount = q.value(0).toInt();

  return s;
}

bool CodebaseIndex::isIndexing() const { return mIndexing; }

void CodebaseIndex::clearRepo(const QString &repoPath) {
  if (!mDb.isOpen())
    return;
  QSqlQuery q(mDb);
  q.prepare("DELETE FROM file_chunks WHERE repo_path = ?");
  q.addBindValue(repoPath);
  q.exec();
}

void CodebaseIndex::clearAll() {
  if (!mDb.isOpen())
    return;
  QSqlQuery q(mDb);
  q.exec("DELETE FROM file_chunks");
}

// ---------------------------------------------------------------------------
// Embedding serialization (same format as KnowledgeBase)
// ---------------------------------------------------------------------------
QByteArray CodebaseIndex::serializeEmbedding(const QVector<float> &v) {
  if (v.isEmpty())
    return {};
  return QByteArray(reinterpret_cast<const char *>(v.constData()),
                    v.size() * sizeof(float));
}

QVector<float> CodebaseIndex::deserializeEmbedding(const QByteArray &blob) {
  if (blob.isEmpty())
    return {};
  int count = blob.size() / sizeof(float);
  QVector<float> v(count);
  memcpy(v.data(), blob.constData(), count * sizeof(float));
  return v;
}
