#include "StatusBar.h"
#include "RepoView.h"
#include "ai/AiService.h"
#include "ai/TaskDispatcher.h"
#include "git/Repository.h"
#include "git/Branch.h"
#include "conf/Settings.h"
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QApplication>

namespace {
const int kStatusBarHeight = 24;
const QString kSeparator = QStringLiteral("  │  ");
} // namespace

StatusBar::StatusBar(QWidget *parent) : QFrame(parent) {
  setFixedHeight(kStatusBarHeight);
  setFrameShape(QFrame::NoFrame);

  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(8, 0, 8, 0);
  layout->setSpacing(0);

  auto makeLabel = [this](const QString &text = {}) {
    auto *label = new QLabel(text, this);
    label->setFont([label] {
      QFont f = label->font();
      f.setPointSize(10);
      return f;
    }());
    return label;
  };

  mBranchLabel = makeLabel();
  mTrackingLabel = makeLabel();
  mStateLabel = makeLabel();
  mDirtyLabel = makeLabel();

  layout->addWidget(mBranchLabel);
  layout->addWidget(mTrackingLabel);
  layout->addWidget(mStateLabel);
  layout->addWidget(mDirtyLabel);

  layout->addStretch();

  mQueueLabel = makeLabel();
  mTokenLabel = makeLabel();
  mCacheLabel = makeLabel();
  mAiModelLabel = makeLabel();
  mOllamaLabel = makeLabel();

  layout->addWidget(mQueueLabel);
  layout->addWidget(mTokenLabel);
  layout->addWidget(mCacheLabel);
  layout->addWidget(mAiModelLabel);
  layout->addWidget(mOllamaLabel);

  connect(TaskDispatcher::instance(), &TaskDispatcher::statsChanged, this,
          [this] {
            auto s = TaskDispatcher::instance()->stats();
            if (s.active > 0 || s.queued > 0)
              mQueueLabel->setText(
                  QStringLiteral("⏳ %1/%2").arg(s.active).arg(s.queued) +
                  kSeparator);
            else
              mQueueLabel->clear();

            qint64 total = s.totalTokensIn + s.totalTokensOut;
            if (total > 0) {
              QString tok = total > 1000
                                ? QStringLiteral("%1K").arg(total / 1000)
                                : QString::number(total);
              mTokenLabel->setText(tok + QStringLiteral(" tok") + kSeparator);
            }

            if (s.cacheHits > 0)
              mCacheLabel->setText(
                  QStringLiteral("⚡ %1 cached").arg(s.cacheHits) + kSeparator);
            else
              mCacheLabel->clear();
          });

  AiService::Config cfg = AiService::instance()->currentConfig();
  setAiStatus(cfg.model, cfg.provider);

  if (cfg.provider == QStringLiteral("ollama")) {
    mOllamaUrl = cfg.baseUrl;
    mOllamaTimer.setInterval(15000);
    connect(&mOllamaTimer, &QTimer::timeout, this, &StatusBar::checkOllamaStatus);
    mOllamaTimer.start();
    checkOllamaStatus();
  }

  connect(Settings::instance(), &Settings::settingsChanged, this, [this](bool) {
    AiService::Config cfg = AiService::instance()->currentConfig();
    setAiStatus(cfg.model, cfg.provider);
    if (cfg.provider == QStringLiteral("ollama")) {
      mOllamaUrl = cfg.baseUrl;
      if (!mOllamaTimer.isActive())
        mOllamaTimer.start();
      checkOllamaStatus();
    } else {
      mOllamaTimer.stop();
      mOllamaLabel->clear();
    }
  });
}

void StatusBar::updateRepo(RepoView *view) {
  if (!view) {
    mBranchLabel->clear();
    mTrackingLabel->clear();
    mStateLabel->clear();
    mDirtyLabel->clear();
    return;
  }

  git::Repository repo = view->repo();
  git::Reference head = repo.head();

  QString branchName = head.isValid() ? head.name() : repo.unbornHeadName();
  mBranchLabel->setText(QStringLiteral("⎇ ") + branchName);

  if (git::Branch branch = head) {
    if (git::Branch upstream = branch.upstream()) {
      int ahead = branch.difference(upstream);
      int behind = upstream.difference(branch);
      QStringList parts;
      if (ahead > 0)
        parts.append(QStringLiteral("↑%1").arg(ahead));
      if (behind > 0)
        parts.append(QStringLiteral("↓%1").arg(behind));
      QString tracking = parts.isEmpty() ? QStringLiteral("✓") : parts.join(" ");
      mTrackingLabel->setText(kSeparator + tracking);
    } else {
      mTrackingLabel->setText(kSeparator + QStringLiteral("no upstream"));
    }
  } else {
    mTrackingLabel->clear();
  }

  QString state;
  switch (repo.state()) {
    case GIT_REPOSITORY_STATE_MERGE:
      state = QStringLiteral("MERGING");
      break;
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
      state = QStringLiteral("REVERTING");
      break;
    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
      state = QStringLiteral("CHERRY-PICKING");
      break;
    case GIT_REPOSITORY_STATE_REBASE_MERGE:
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
    case GIT_REPOSITORY_STATE_REBASE:
      state = QStringLiteral("REBASING");
      break;
    default:
      break;
  }
  mStateLabel->setText(state.isEmpty() ? QString() : kSeparator + state);
}

void StatusBar::setAiStatus(const QString &model, const QString &provider) {
  QString icon = (provider == QStringLiteral("ollama"))
                     ? QStringLiteral("⚙")
                     : QStringLiteral("✨");
  mAiModelLabel->setText(icon + " " + model);
}

void StatusBar::checkOllamaStatus() {
  if (mOllamaUrl.isEmpty())
    return;

  auto *mgr = new QNetworkAccessManager(this);
  QNetworkRequest req(QUrl(mOllamaUrl + "/api/tags"));
  req.setTransferTimeout(3000);
  QNetworkReply *reply = mgr->get(req);

  connect(reply, &QNetworkReply::finished, this, [this, reply, mgr] {
    bool ok = (reply->error() == QNetworkReply::NoError);
    mOllamaLabel->setText(kSeparator + (ok ? QStringLiteral("✓ GPU online")
                                           : QStringLiteral("✗ GPU offline")));
    reply->deleteLater();
    mgr->deleteLater();
  });
}
