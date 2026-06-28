#ifndef AIREVIEWSTORE_H
#define AIREVIEWSTORE_H

#include <QDateTime>
#include <QObject>

class AiReviewStore : public QObject {
  Q_OBJECT
public:
  static AiReviewStore *instance();

  struct FixRecord {
    QString file;
    bool success = false;
    QString message;
  };

  struct Entry {
    QString id;
    QDateTime timestamp;
    QString provider;
    QString model;
    QString repoPath;
    QString headSha;
    QString reviewText;
    QList<FixRecord> fixes;
  };

  // Save a new review. Returns assigned ID.
  QString save(const QString &repoPath, const QString &headSha,
               const QString &provider, const QString &model,
               const QByteArray &diff, const QString &reviewText);

  // Append fix records to an existing entry.
  void addFixes(const QString &id, const QList<FixRecord> &fixes);

  // All entries for a repo, newest first.
  QList<Entry> listForRepo(const QString &repoPath) const;

  // The stored diff for a given entry ID (for re-running).
  QByteArray diff(const QString &id) const;

private:
  explicit AiReviewStore(QObject *parent = nullptr);
  QString storageDir() const;
  void load();
  void persist();
  Entry entryFromJson(const QJsonObject &obj) const;
  QJsonObject entryToJson(const Entry &e) const;

  QList<Entry> mEntries;
  bool mLoaded = false;
};

#endif // AIREVIEWSTORE_H
