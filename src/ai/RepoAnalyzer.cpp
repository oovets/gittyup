#include "RepoAnalyzer.h"
#include "AiService.h"
#include "FindingParser.h"
#include "KnowledgeBase.h"
#include "conf/RecentRepositories.h"
#include "conf/RecentRepository.h"
#include "conf/Settings.h"
#include "git/Commit.h"
#include "git/Diff.h"
#include "git/Id.h"
#include "git/Reference.h"
#include "git/Repository.h"
#include "git/Tree.h"

#include <QDir>

static const int kAnalysisIntervalMs = 10 * 60 * 1000; // 10 minutes

RepoAnalyzer *RepoAnalyzer::instance() {
  static RepoAnalyzer sInstance;
  return &sInstance;
}

RepoAnalyzer::RepoAnalyzer(QObject *parent) : QObject(parent) {
  mProgress = {0, 0, {}, false};

  connect(&mTimer, &QTimer::timeout, this, &RepoAnalyzer::onTimerTick);
  mTimer.setInterval(kAnalysisIntervalMs);
}

bool RepoAnalyzer::isRunning() const { return mRunning; }

bool RepoAnalyzer::isEnabled() const { return mEnabled; }

void RepoAnalyzer::setEnabled(bool enabled) {
  mEnabled = enabled;
  if (enabled) {
    mTimer.start();
    analyzeChanged();
  } else {
    mTimer.stop();
  }
}

RepoAnalyzer::Progress RepoAnalyzer::currentProgress() const {
  return mProgress;
}

// ---------------------------------------------------------------------------
// Gather HEAD sha for a repo path
// ---------------------------------------------------------------------------
QString RepoAnalyzer::headSha(const QString &repoPath) const {
  // libgit2 instead of a `git rev-parse` subprocess: in-process and fast, so
  // the periodic background tick never blocks the UI thread.
  git::Repository repo = git::Repository::open(repoPath);
  if (!repo.isValid())
    return {};
  git::Reference head = repo.head();
  git::Commit commit = head.isValid() ? head.target() : git::Commit();
  return commit.isValid() ? commit.id().toString() : QString();
}

// ---------------------------------------------------------------------------
// Get recent diff (uncommitted changes, else the last commit) via libgit2
// ---------------------------------------------------------------------------
QByteArray RepoAnalyzer::recentDiff(const QString &repoPath) const {
  git::Repository repo = git::Repository::open(repoPath);
  if (!repo.isValid())
    return {};

  git::Reference head = repo.head();
  git::Commit headCommit = head.isValid() ? head.target() : git::Commit();

  // Uncommitted changes (staged + unstaged) relative to HEAD.
  QByteArray out;
  if (headCommit.isValid()) {
    git::Diff staged = repo.diffTreeToIndex(headCommit.tree());
    if (staged.isValid())
      out += staged.print();
  }
  git::Diff unstaged = repo.diffIndexToWorkdir();
  if (unstaged.isValid())
    out += unstaged.print();
  if (!out.trimmed().isEmpty())
    return out.left(32000);

  // Fall back to the last commit's diff (HEAD~1 -> HEAD).
  if (headCommit.isValid()) {
    QList<git::Commit> parents = headCommit.parents();
    if (!parents.isEmpty()) {
      git::Diff d = headCommit.diff(parents.first());
      if (d.isValid())
        return d.print().left(32000);
    }
  }

  return {};
}

// ---------------------------------------------------------------------------
// Status of all tracked repos
// ---------------------------------------------------------------------------
QList<RepoAnalyzer::RepoStatus> RepoAnalyzer::trackedRepos() const {
  QList<RepoStatus> result;
  RecentRepositories *recent = RecentRepositories::instance();

  for (int i = 0; i < recent->count(); ++i) {
    RecentRepository *repo = recent->repository(i);
    QString path = repo->gitpath();

    // Resolve to working directory
    QDir gitDir(path);
    if (gitDir.dirName() == ".git")
      gitDir.cdUp();
    QString workDir = gitDir.absolutePath();

    RepoStatus status;
    status.path = workDir;
    status.currentSha = headSha(workDir);

    // Check if we've already analyzed this SHA
    KnowledgeBase *kb = KnowledgeBase::instance();
    QString cached = kb->findExactReview(status.currentSha.toUtf8());
    status.lastAnalyzedSha = cached.isEmpty() ? QString() : status.currentSha;
    status.needsAnalysis = cached.isEmpty();
    status.findingCount = 0;
    status.fixRecipeCount = 0;

    result.append(status);
  }

  return result;
}

// ---------------------------------------------------------------------------
// Analyze a single repo
// ---------------------------------------------------------------------------
void RepoAnalyzer::analyzeRepo(const QString &repoPath) {
  if (!mQueue.contains(repoPath))
    mQueue.append(repoPath);

  if (!mRunning)
    processQueue();
}

// ---------------------------------------------------------------------------
// Analyze all tracked repos
// ---------------------------------------------------------------------------
void RepoAnalyzer::analyzeAllTracked() {
  RecentRepositories *recent = RecentRepositories::instance();
  for (int i = 0; i < recent->count(); ++i) {
    RecentRepository *repo = recent->repository(i);
    QString path = repo->gitpath();
    QDir gitDir(path);
    if (gitDir.dirName() == ".git")
      gitDir.cdUp();
    QString workDir = gitDir.absolutePath();

    if (!mQueue.contains(workDir))
      mQueue.append(workDir);
  }

  if (!mRunning)
    processQueue();
}

// ---------------------------------------------------------------------------
// Analyze only repos that changed since last analysis
// ---------------------------------------------------------------------------
void RepoAnalyzer::analyzeChanged() {
  if (!KnowledgeBase::instance()->isEnabled())
    return;

  QList<RepoStatus> repos = trackedRepos();
  for (const auto &r : repos) {
    if (r.needsAnalysis && !r.currentSha.isEmpty())
      analyzeRepo(r.path);
  }
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------
void RepoAnalyzer::onTimerTick() {
  if (!mEnabled || mRunning)
    return;
  analyzeChanged();
}

// ---------------------------------------------------------------------------
// Process the queue
// ---------------------------------------------------------------------------
void RepoAnalyzer::processQueue() {
  if (mQueue.isEmpty()) {
    mRunning = false;
    mProgress.running = false;
    emit progressChanged();
    emit allAnalysesComplete();
    return;
  }

  mRunning = true;
  mProgress.totalRepos = mQueue.size();
  mProgress.completedRepos = 0;
  mProgress.running = true;
  emit progressChanged();

  analyzeNext();
}

void RepoAnalyzer::analyzeNext() {
  if (mQueue.isEmpty()) {
    processQueue(); // will emit allAnalysesComplete
    return;
  }

  QString repoPath = mQueue.takeFirst();
  mProgress.currentRepo = QDir(repoPath).dirName();
  emit progressChanged();
  emit analysisStarted(repoPath);

  QByteArray diff = recentDiff(repoPath);
  if (diff.isEmpty()) {
    emit analysisFinished(repoPath, 0);
    mProgress.completedRepos++;
    analyzeNext();
    return;
  }

  // Check if we already have this exact diff
  KnowledgeBase *kb = KnowledgeBase::instance();
  if (!kb->findExactReview(diff).isEmpty()) {
    emit analysisFinished(repoPath, 0);
    mProgress.completedRepos++;
    analyzeNext();
    return;
  }

  // Deep analysis prompt with fix recipe extraction
  QString prompt = QStringLiteral(
      "Perform a deep code review of this git diff. Analyze thoroughly for:\n"
      "- Bugs and logic errors\n"
      "- Security vulnerabilities\n"
      "- Memory safety issues\n"
      "- Performance problems\n"
      "- Error handling gaps\n\n"
      "For EACH issue found, output this EXACT format:\n\n"
      "ISSUE:\n"
      "SEVERITY: <CRITICAL|HIGH|MEDIUM|LOW>\n"
      "FILE: <path>\n"
      "LINE: <number>\n"
      "CATEGORY: <null-deref|buffer-overflow|memory-leak|use-after-free|"
      "race-condition|injection|xss|auth-bypass|type-error|logic-error|"
      "resource-leak|uninitialized|bounds-check|error-handling|style|"
      "performance|other>\n"
      "LANGUAGE: <C++|Python|JavaScript|...>\n"
      "PROBLEM: <concise description>\n"
      "FIX: <what to do>\n"
      "CODE_PATTERN: <generalized pattern without specific variable names>\n"
      "FIND:\n"
      "<exact lines from the file to replace>\n"
      "REPLACE:\n"
      "<corrected lines>\n"
      "---\n\n"
      "The FIND section must be the exact text from the source.\n"
      "The REPLACE section must be the corrected version.\n"
      "If no issues are found, say so clearly.\n\n") +
      QString::fromUtf8(diff);

  AiService::instance()->chat(prompt, 4096,
      [this, repoPath, diff](const QString &text, const QString &error) {
        if (!error.isEmpty()) {
          emit analysisError(repoPath, error);
          mProgress.completedRepos++;
          analyzeNext();
          return;
        }
        onReviewComplete(repoPath, diff, text);
        mProgress.completedRepos++;
        analyzeNext();
      });
}

// ---------------------------------------------------------------------------
// Handle completed review - parse findings AND fix recipes
// ---------------------------------------------------------------------------
void RepoAnalyzer::onReviewComplete(const QString &repoPath,
                                    const QByteArray &diff,
                                    const QString &response) {
  KnowledgeBase *kb = KnowledgeBase::instance();

  QList<KnowledgeBase::Finding> findings = FindingParser::parse(response);
  QList<qint64> findingIds = kb->storeReview(diff, repoPath, response, findings);

  QList<FindingParser::FixBlock> fixBlocks =
      FindingParser::parseFixBlocks(response);

  // Match fix blocks to stored findings by index correspondence
  // (ISSUE blocks and findings are parsed in the same order)
  for (int i = 0; i < fixBlocks.size(); ++i) {
    qint64 fid = (i < findingIds.size()) ? findingIds[i] : -1;

    // Fallback: use first valid finding ID
    if (fid < 0) {
      for (qint64 id : findingIds) {
        if (id >= 0) {
          fid = id;
          break;
        }
      }
    }

    if (fid >= 0) {
      const auto &block = fixBlocks[i];
      kb->storeFixRecipe(fid, block.find, block.replace,
                         block.file.isEmpty() ? "*" : block.file);
    }
  }

  emit analysisFinished(repoPath, findings.size());
}
