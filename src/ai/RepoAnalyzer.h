#ifndef REPOANALYZER_H
#define REPOANALYZER_H

#include <QObject>
#include <QStringList>
#include <QTimer>

class RepoAnalyzer : public QObject {
  Q_OBJECT

public:
  static RepoAnalyzer *instance();

  struct RepoStatus {
    QString path;
    QString lastAnalyzedSha;
    QString currentSha;
    bool needsAnalysis;
    int findingCount;
    int fixRecipeCount;
  };

  bool isRunning() const;
  void setEnabled(bool enabled);
  bool isEnabled() const;

  void analyzeRepo(const QString &repoPath);
  void analyzeAllTracked();
  void analyzeChanged();

  QList<RepoStatus> trackedRepos() const;

  struct Progress {
    int totalRepos;
    int completedRepos;
    QString currentRepo;
    bool running;
  };

  Progress currentProgress() const;

signals:
  void analysisStarted(const QString &repoPath);
  void analysisFinished(const QString &repoPath, int findingCount);
  void analysisError(const QString &repoPath, const QString &error);
  void allAnalysesComplete();
  void progressChanged();

private slots:
  void onTimerTick();

private:
  explicit RepoAnalyzer(QObject *parent = nullptr);

  void processQueue();
  void analyzeNext();
  void onReviewComplete(const QString &repoPath, const QByteArray &diff,
                        const QString &response);
  QString headSha(const QString &repoPath) const;
  QByteArray recentDiff(const QString &repoPath) const;

  QTimer mTimer;
  QStringList mQueue;
  bool mRunning = false;
  bool mEnabled = false;

  Progress mProgress;
};

#endif // REPOANALYZER_H
