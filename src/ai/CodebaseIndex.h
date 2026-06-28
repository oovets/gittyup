#ifndef CODEBASEINDEX_H
#define CODEBASEINDEX_H

#include <QObject>
#include <QSqlDatabase>
#include <QStringList>
#include <QVector>
#include <functional>

class EmbeddingClient;

class CodebaseIndex : public QObject {
  Q_OBJECT

public:
  static CodebaseIndex *instance();

  struct FileChunk {
    qint64 id = -1;
    QString repoPath;
    QString filePath;
    int startLine;
    int endLine;
    QString content;
    QVector<float> embedding;
    QString lastCommitSha;
  };

  struct SearchResult {
    FileChunk chunk;
    float similarity;
  };

  struct IndexStats {
    int totalFiles;
    int totalChunks;
    int repoCount;
  };

  void indexRepo(const QString &repoPath,
                 std::function<void(int indexed, int total)> progress = {},
                 std::function<void(const QString &error)> done = {});

  void indexAllTracked(std::function<void(int indexed, int total)> progress = {},
                       std::function<void(const QString &error)> done = {});

  void searchSimilar(const QString &query, int topK,
                     std::function<void(QList<SearchResult>)> callback);

  void searchSimilar(const QVector<float> &queryEmbedding, int topK,
                     std::function<void(QList<SearchResult>)> callback);

  QString buildContext(const QList<SearchResult> &results) const;

  IndexStats stats() const;
  void clearRepo(const QString &repoPath);
  void clearAll();

  bool isIndexing() const;

signals:
  void indexingStarted(const QString &repoPath);
  void indexingProgress(const QString &repoPath, int current, int total);
  void indexingFinished(const QString &repoPath, int chunksIndexed);
  void indexingError(const QString &repoPath, const QString &error);

private:
  explicit CodebaseIndex(QObject *parent = nullptr);
  void initDatabase();

  struct FileEntry {
    QString relativePath;
    QString absolutePath;
    QString sha;
  };

  QList<FileEntry> collectSourceFiles(const QString &repoPath) const;
  QStringList chunkFile(const QString &content, int maxLines = 60) const;
  bool isFileIndexed(const QString &repoPath, const QString &filePath,
                     const QString &sha) const;
  void storeChunk(const QString &repoPath, const QString &filePath,
                  int startLine, int endLine, const QString &content,
                  const QVector<float> &embedding, const QString &sha);

  static QByteArray serializeEmbedding(const QVector<float> &v);
  static QVector<float> deserializeEmbedding(const QByteArray &blob);

  QSqlDatabase mDb;
  EmbeddingClient *mEmbeddingClient;
  bool mIndexing = false;
};

#endif // CODEBASEINDEX_H
