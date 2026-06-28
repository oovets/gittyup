#include "AiReviewStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

AiReviewStore *AiReviewStore::instance() {
  static AiReviewStore sInstance;
  return &sInstance;
}

AiReviewStore::AiReviewStore(QObject *parent) : QObject(parent) {}

QString AiReviewStore::storageDir() const {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         "/ai-reviews";
}

void AiReviewStore::load() {
  if (mLoaded)
    return;
  mLoaded = true;

  QFile f(storageDir() + "/reviews.json");
  if (!f.open(QIODevice::ReadOnly))
    return;

  QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
  for (const QJsonValue &v : arr)
    mEntries << entryFromJson(v.toObject());
}

void AiReviewStore::persist() {
  QDir().mkpath(storageDir());
  QJsonArray arr;
  for (const Entry &e : mEntries)
    arr << entryToJson(e);

  QFile f(storageDir() + "/reviews.json");
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    f.write(QJsonDocument(arr).toJson());
}

QString AiReviewStore::save(const QString &repoPath, const QString &headSha,
                             const QString &provider, const QString &model,
                             const QByteArray &diff,
                             const QString &reviewText) {
  load();

  Entry e;
  e.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  e.timestamp = QDateTime::currentDateTime();
  e.provider = provider;
  e.model = model;
  e.repoPath = repoPath;
  e.headSha = headSha;
  e.reviewText = reviewText;

  // Store diff separately
  QDir().mkpath(storageDir());
  QFile df(storageDir() + "/" + e.id + ".diff");
  if (df.open(QIODevice::WriteOnly | QIODevice::Truncate))
    df.write(diff);

  mEntries.prepend(e);
  persist();
  return e.id;
}

void AiReviewStore::addFixes(const QString &id, const QList<FixRecord> &fixes) {
  load();
  for (Entry &e : mEntries) {
    if (e.id == id) {
      e.fixes << fixes;
      persist();
      return;
    }
  }
}

QList<AiReviewStore::Entry> AiReviewStore::listForRepo(const QString &repoPath) const {
  const_cast<AiReviewStore *>(this)->load();
  QList<Entry> result;
  for (const Entry &e : mEntries)
    if (e.repoPath == repoPath)
      result << e;
  return result; // already newest-first from prepend order
}

QByteArray AiReviewStore::diff(const QString &id) const {
  QFile f(storageDir() + "/" + id + ".diff");
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return f.readAll();
}

AiReviewStore::Entry AiReviewStore::entryFromJson(const QJsonObject &o) const {
  Entry e;
  e.id = o["id"].toString();
  e.timestamp = QDateTime::fromString(o["timestamp"].toString(), Qt::ISODate);
  e.provider = o["provider"].toString();
  e.model = o["model"].toString();
  e.repoPath = o["repoPath"].toString();
  e.headSha = o["headSha"].toString();
  e.reviewText = o["reviewText"].toString();

  for (const QJsonValue &v : o["fixes"].toArray()) {
    QJsonObject fo = v.toObject();
    FixRecord r;
    r.file = fo["file"].toString();
    r.success = fo["success"].toBool();
    r.message = fo["message"].toString();
    e.fixes << r;
  }
  return e;
}

QJsonObject AiReviewStore::entryToJson(const Entry &e) const {
  QJsonArray fixes;
  for (const FixRecord &r : e.fixes) {
    fixes << QJsonObject{
        {"file", r.file}, {"success", r.success}, {"message", r.message}};
  }
  return QJsonObject{{"id", e.id},
                     {"timestamp", e.timestamp.toString(Qt::ISODate)},
                     {"provider", e.provider},
                     {"model", e.model},
                     {"repoPath", e.repoPath},
                     {"headSha", e.headSha},
                     {"reviewText", e.reviewText},
                     {"fixes", fixes}};
}
