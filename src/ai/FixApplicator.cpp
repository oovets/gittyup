#include "FixApplicator.h"
#include "EmbeddingClient.h"
#include "VectorMath.h"
#include "conf/Settings.h"
#include "conf/Setting.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QSqlQuery>
#include <QTextStream>

FixApplicator *FixApplicator::instance() {
  static FixApplicator sInstance;
  return &sInstance;
}

FixApplicator::FixApplicator(QObject *parent) : QObject(parent) {}

// ---------------------------------------------------------------------------
// Load fix recipes from the knowledge base
// ---------------------------------------------------------------------------
QList<KnowledgeBase::FixRecipe>
FixApplicator::loadRecipesForFinding(qint64 findingId) {
  QList<KnowledgeBase::FixRecipe> recipes;
  KnowledgeBase *kb = KnowledgeBase::instance();

  QSqlDatabase db =
      QSqlDatabase::database("knowledge", /* open= */ false);
  if (!db.isOpen())
    return recipes;

  QSqlQuery q(db);
  q.prepare("SELECT id, finding_id, search_pattern, replace_pattern, "
            "file_pattern_glob FROM fix_recipes WHERE finding_id = ?");
  q.addBindValue(findingId);
  if (!q.exec())
    return recipes;

  while (q.next()) {
    KnowledgeBase::FixRecipe r;
    r.id = q.value(0).toLongLong();
    r.findingId = q.value(1).toLongLong();
    r.searchPattern = q.value(2).toString();
    r.replacePattern = q.value(3).toString();
    r.filePatternGlob = q.value(4).toString();
    recipes.append(r);
  }

  return recipes;
}

QList<KnowledgeBase::FixRecipe> FixApplicator::loadAllRecipes() {
  QList<KnowledgeBase::FixRecipe> recipes;

  QSqlDatabase db =
      QSqlDatabase::database("knowledge", /* open= */ false);
  if (!db.isOpen())
    return recipes;

  QSqlQuery q(db);
  q.exec("SELECT id, finding_id, search_pattern, replace_pattern, "
         "file_pattern_glob FROM fix_recipes");

  while (q.next()) {
    KnowledgeBase::FixRecipe r;
    r.id = q.value(0).toLongLong();
    r.findingId = q.value(1).toLongLong();
    r.searchPattern = q.value(2).toString();
    r.replacePattern = q.value(3).toString();
    r.filePatternGlob = q.value(4).toString();
    recipes.append(r);
  }

  return recipes;
}

// ---------------------------------------------------------------------------
// Find applicable recipes by scanning diff hunks against stored patterns
// ---------------------------------------------------------------------------
QList<FixApplicator::MatchedRecipe>
FixApplicator::findApplicableRecipes(const QByteArray &diff,
                                     const QString &repoPath) {
  QList<MatchedRecipe> matched;
  QString diffStr = QString::fromUtf8(diff);

  QList<KnowledgeBase::FixRecipe> recipes = loadAllRecipes();

  for (const auto &recipe : recipes) {
    if (recipe.searchPattern.isEmpty())
      continue;

    // Direct string match: does the diff contain the search pattern?
    if (diffStr.contains(recipe.searchPattern)) {
      MatchedRecipe mr;
      mr.recipe = recipe;
      mr.similarity = 1.0;
      matched.append(mr);
    }
  }

  return matched;
}

// ---------------------------------------------------------------------------
// Apply recipes to files in a repo
// ---------------------------------------------------------------------------
QList<FixApplicator::AppliedFix>
FixApplicator::applyRecipes(const QList<MatchedRecipe> &recipes,
                            const QString &repoPath, bool dryRun) {
  QList<AppliedFix> results;

  for (const auto &mr : recipes) {
    const auto &recipe = mr.recipe;

    // Find files matching the glob pattern
    QStringList matchingFiles;
    QDir repoDir(repoPath);

    if (recipe.filePatternGlob == "*" || recipe.filePatternGlob.isEmpty()) {
      // Search all source files
      QDirIterator it(repoPath,
                      {"*.cpp", "*.h", "*.py", "*.js", "*.ts", "*.c"},
                      QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext())
        matchingFiles.append(it.next());
    } else {
      QDirIterator it(repoPath, {recipe.filePatternGlob}, QDir::Files,
                      QDirIterator::Subdirectories);
      while (it.hasNext())
        matchingFiles.append(it.next());
    }

    for (const QString &filePath : matchingFiles) {
      QFile file(filePath);
      if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        continue;

      QString content = QTextStream(&file).readAll();
      file.close();

      if (!content.contains(recipe.searchPattern))
        continue;

      AppliedFix fix;
      fix.filePath = filePath;
      fix.findText = recipe.searchPattern;
      fix.replaceText = recipe.replacePattern;
      fix.findingId = recipe.findingId;

      if (dryRun) {
        fix.success = true;
        fix.error = "dry-run";
      } else {
        content.replace(recipe.searchPattern, recipe.replacePattern);

        if (file.open(QIODevice::WriteOnly | QIODevice::Text |
                      QIODevice::Truncate)) {
          QTextStream out(&file);
          out << content;
          file.close();
          fix.success = true;
        } else {
          fix.success = false;
          fix.error = file.errorString();
        }
      }

      results.append(fix);
      emit fixApplied(fix);
    }
  }

  return results;
}

// ---------------------------------------------------------------------------
// Full auto-fix pipeline: scan repo diff, match recipes, apply
// ---------------------------------------------------------------------------
void FixApplicator::autoFix(const QString &repoPath) {
  QProcess git;
  git.setWorkingDirectory(repoPath);
  git.start("git", {"diff", "HEAD"});
  if (!git.waitForFinished(10000) || git.exitCode() != 0) {
    emit autoFixComplete(repoPath, 0);
    return;
  }

  QByteArray diff = git.readAllStandardOutput();
  if (diff.trimmed().isEmpty()) {
    emit autoFixComplete(repoPath, 0);
    return;
  }

  QList<MatchedRecipe> recipes = findApplicableRecipes(diff, repoPath);
  QList<AppliedFix> applied = applyRecipes(recipes, repoPath, false);

  int successCount = 0;
  for (const auto &fix : applied) {
    if (fix.success)
      ++successCount;
  }

  emit autoFixComplete(repoPath, successCount);
}
