#ifndef FIXAPPLICATOR_H
#define FIXAPPLICATOR_H

#include "KnowledgeBase.h"
#include <QObject>
#include <QStringList>

class FixApplicator : public QObject {
  Q_OBJECT

public:
  static FixApplicator *instance();

  struct AppliedFix {
    QString filePath;
    QString findText;
    QString replaceText;
    qint64 findingId;
    bool success;
    QString error;
  };

  struct MatchedRecipe {
    KnowledgeBase::FixRecipe recipe;
    KnowledgeBase::Finding finding;
    double similarity;
  };

  QList<MatchedRecipe> findApplicableRecipes(
      const QByteArray &diff, const QString &repoPath);

  QList<AppliedFix> applyRecipes(const QList<MatchedRecipe> &recipes,
                                 const QString &repoPath, bool dryRun = true);

  void autoFix(const QString &repoPath);

signals:
  void fixApplied(const AppliedFix &fix);
  void autoFixComplete(const QString &repoPath, int fixCount);

private:
  explicit FixApplicator(QObject *parent = nullptr);

  QList<KnowledgeBase::FixRecipe> loadRecipesForFinding(qint64 findingId);
  QList<KnowledgeBase::FixRecipe> loadAllRecipes();
};

#endif // FIXAPPLICATOR_H
