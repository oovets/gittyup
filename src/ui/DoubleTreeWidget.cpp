//
//          Copyright (c) 2020
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Martin Marmsoler
//

#include "ContextMenuButton.h"
#include "DoubleTreeWidget.h"
#include "BlameEditor.h"
#include "DiffTreeModel.h"
#include "FileContextMenu.h"
#include "StatePushButton.h"
#include "TreeProxy.h"
#include "TreeView.h"
#include "Debug.h"
#include "conf/Settings.h"
#include "DiffView/DiffView.h"
#include "git/Index.h"
#include "git/Config.h"

#include "RepoView.h"
#include "ai/AiService.h"
#include "ai/TaskDispatcher.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QTextBrowser>
#include <QCheckBox>
#include <QRegularExpression>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QScrollBar>
#include <qnamespace.h>
#include <qtreeview.h>

namespace {

const QString kSplitterKey = QString("diffsplitter");
const QString kExpandAll = QString(QObject::tr("Expand all"));
const QString kCollapseAll = QString(QObject::tr("Collapse all"));
const QString kStagedFiles = QString(QObject::tr("Staged Files"));
const QString kUnstagedFiles = QString(QObject::tr("Unstaged Files"));
const QString kCommitedFiles = QString(QObject::tr("Committed Files"));
const QString kAllFiles = QString(QObject::tr("Workdir Files"));

class SegmentedButton : public QWidget {
public:
  SegmentedButton(QWidget *parent = nullptr) : QWidget(parent) {
    mLayout = new QHBoxLayout(this);
    mLayout->setContentsMargins(0, 0, 0, 0);
    mLayout->setSpacing(0);
  }

  void addButton(QAbstractButton *button, const QString &text = QString(),
                 bool checkable = false) {
    button->setToolTip(text);
    button->setCheckable(checkable);

    mLayout->addWidget(button);
    mButtons.addButton(button, mButtons.buttons().size());
  }

  const QButtonGroup *buttonGroup() const { return &mButtons; }

private:
  QHBoxLayout *mLayout;
  QButtonGroup mButtons;
};

} // namespace

QAction *DoubleTreeWidget::setupAppearanceAction(const char *name,
                                                 Setting::Id id,
                                                 bool defaultValue) {
  QAction *action = new QAction(tr(name));
  action->setCheckable(true);
  action->setChecked(Settings::instance()->value(id, defaultValue).toBool());
  connect(action, &QAction::triggered, this, [this, id](bool checked) {
    Settings::instance()->setValue(id, checked);
    mSelectedFile.filename =
        ""; // When switching view, it is not possible to restore
    RepoView::parentView(this)->refresh();
  });
  return action;
}

DoubleTreeWidget::DoubleTreeWidget(const git::Repository &repo, QWidget *parent)
    : ContentWidget(parent) {
  // first column
  // top (Buttons to switch between Blame editor and DiffView)
  SegmentedButton *segmentedButton = new SegmentedButton(this);
  QPushButton *blameView = new QPushButton(tr("Blame"), this);
  segmentedButton->addButton(blameView, tr("Show Blame Editor"), true);
  QPushButton *diffView = new QPushButton(tr("Diff"), this);
  segmentedButton->addButton(diffView, tr("Show Diff View"), true);
  QPushButton *reviewView = new QPushButton(tr("Review"), this);
  segmentedButton->addButton(reviewView, tr("Show AI Code Review"), true);

  // Context button.
  ContextMenuButton *contextButton = new ContextMenuButton(this);
  QMenu *contextMenu = new QMenu(this);
  contextButton->setMenu(contextMenu);

  QAction *singleTree = setupAppearanceAction(
      "Single View", Setting::Id::ShowChangedFilesInSingleView);
  QAction *listView =
      setupAppearanceAction("List View", Setting::Id::ShowChangedFilesAsList);
  QAction *multiColumn = setupAppearanceAction(
      "Multi Column", Setting::Id::ShowChangedFilesMultiColumn, true);
  RepoView::parentView(this)->refresh(); // apply read settings

  QAction *hideUntrackedFiles = setupAppearanceAction(
      "Hide Untracked Files", Setting::Id::HideUntracked, false);

  contextMenu->addAction(singleTree);
  contextMenu->addAction(listView);
  contextMenu->addAction(multiColumn);
  contextMenu->addAction(hideUntrackedFiles);
  QHBoxLayout *buttonLayout = new QHBoxLayout();
  buttonLayout->addStretch();
  buttonLayout->addWidget(segmentedButton);
  buttonLayout->addStretch();
  buttonLayout->addWidget(contextButton);

  // bottom (Stacked widget with Blame editor and DiffView)
  QVBoxLayout *fileViewLayout = new QVBoxLayout();
  mFileView = new QStackedWidget(this);
  mEditor = new BlameEditor(repo, this);
  mDiffView = new DiffView(repo, this);
  mFileView->addWidget(mEditor);
  mFileView->addWidget(mDiffView);

  mReviewContainer = new QWidget(this);
  QVBoxLayout *reviewLayout = new QVBoxLayout(mReviewContainer);
  reviewLayout->setContentsMargins(0, 0, 0, 0);
  reviewLayout->setSpacing(0);

  mReviewPanel = new QTextBrowser(mReviewContainer);
  mReviewPanel->setOpenExternalLinks(true);
  mReviewPanel->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  mReviewPanel->setLineWrapMode(QTextBrowser::WidgetWidth);

  mFixBtn = new QPushButton(tr("Fix Issues"), mReviewContainer);
  mFixBtn->setMinimumHeight(32);
  mFixBtn->setStyleSheet(
      "QPushButton { background:#1565c0; color:#fff; border:none; "
      "font-weight:bold; font-size:13px; padding:6px; }"
      "QPushButton:hover { background:#1976d2; }"
      "QPushButton:disabled { background:#555; color:#999; }");
  mFixBtn->setVisible(false);
  connect(mFixBtn, &QPushButton::clicked, this, [this] {
    if (mLastReviewDiff.isEmpty()) return;
    mFixBtn->setEnabled(false);
    mFixBtn->setText(tr("Fixing..."));

    QString review = mLastReviewText;
    QString diff = QString::fromUtf8(mLastReviewDiff);
    QString prompt =
        QStringLiteral(
            "You are a code fixer. Output ONLY fix blocks.\n\n"
            "CRITICAL RULES:\n"
            "1. Each fix is ONE block. ONE block per change.\n"
            "2. If two changes are in different files, use TWO blocks.\n"
            "3. If two changes are in the same file but different locations, "
            "use TWO blocks.\n"
            "4. FIND text must be EXACT copy from the source file.\n"
            "5. Get file paths from the diff headers (--- a/path or +++ b/path).\n"
            "6. No markdown. No code fences. No explanations. ONLY blocks.\n\n"
            "FORMAT (repeat for each fix):\n\n"
            "FILE: src/ui/SomeFile.cpp\n"
            "FIND:\n"
            "exact lines from source\n"
            "REPLACE:\n"
            "corrected lines\n"
            "---\n\n"
            "Issues found in review:\n\n") +
        review + "\n\nDiff for context:\n\n" + diff;

    // Show live streaming area
    mReviewPanel->append(
        "<div style='margin-top:12px; border-left:3px solid #546e7a; "
        "padding:6px 10px;'>"
        "<b style='color:#90caf9;'>Generating fixes...</b></div>");

    QPointer<DoubleTreeWidget> self = this;
    TaskDispatcher::instance()->submitStreaming(
        TaskDispatcher::TaskType::Chat, prompt,
        [self](const QString &chunk) {
          if (!self) return;
          QMetaObject::invokeMethod(self, [self, chunk] {
            QTextCursor cursor = self->mReviewPanel->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(chunk);
            self->mReviewPanel->verticalScrollBar()->setValue(
                self->mReviewPanel->verticalScrollBar()->maximum());
          });
        },
        [self](const QString &text, const QString &error) {
          if (!self) return;
          QMetaObject::invokeMethod(self, [self, text, error] {
            QString doc = self->mReviewPanel->toHtml();
            int streamStart = doc.lastIndexOf("Generating fixes...");
            if (streamStart >= 0) {
              QTextCursor findCur = self->mReviewPanel->document()->find(
                  "Generating fixes...");
              if (!findCur.isNull()) {
                findCur.movePosition(QTextCursor::StartOfBlock);
                findCur.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                findCur.removeSelectedText();
              }
            }

            if (!error.isEmpty()) {
              self->mFixBtn->setEnabled(true);
              self->mFixBtn->setText(QObject::tr("Fix Issues"));
              self->mReviewPanel->append(
                  "<p style='color:#e57373;'>Fix request failed: " +
                  error.toHtmlEscaped() + "</p>");
              return;
            }
            self->applyFixBlocks(text);
          });
        },
        TaskDispatcher::Priority::High, 4096);
  });

  mHideFixedCb = new QCheckBox(tr("Hide fixed"), mReviewContainer);
  mHideFixedCb->setVisible(false);
  connect(mHideFixedCb, &QCheckBox::toggled, this, [this] {
    showReview(mLastReviewText, mLastReviewDiff);
  });

  mStopReviewBtn = new QPushButton(tr("Stop"), mReviewContainer);
  mStopReviewBtn->setMinimumHeight(32);
  mStopReviewBtn->setVisible(false);
  connect(mStopReviewBtn, &QPushButton::clicked, this, [this] {
    if (mCancelReview)
      mCancelReview();
  });

  QHBoxLayout *reviewBtnRow = new QHBoxLayout;
  reviewBtnRow->setContentsMargins(0, 0, 0, 0);
  reviewBtnRow->addWidget(mHideFixedCb);
  reviewBtnRow->addStretch();
  reviewBtnRow->addWidget(mStopReviewBtn);
  reviewBtnRow->addWidget(mFixBtn);

  reviewLayout->addWidget(mReviewPanel, 1);
  reviewLayout->addLayout(reviewBtnRow);

  mFileView->addWidget(mReviewContainer);

  fileViewLayout->addLayout(buttonLayout);
  fileViewLayout->addWidget(mFileView);
  mFileView->setCurrentIndex(DoubleTreeWidget::Diff);
  mFileView->show();
  QWidget *fileView = new QWidget(this);
  fileView->setLayout(fileViewLayout);

  auto *repoView = qobject_cast<RepoView *>(parent->parent());

  // second column
  // staged files
  QVBoxLayout *vBoxLayout = new QVBoxLayout();
  stagedFiles = new TreeView(this, "Staged");
  stagedFiles->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
  stagedFiles->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(stagedFiles, &QWidget::customContextMenuRequested,
          [this, repoView](const QPoint &pos) {
            showFileContextMenu(pos, repoView, stagedFiles, true);
          });

  mDiffTreeModel = new DiffTreeModel(repo, this);
  mDiffView->setModel(mDiffTreeModel);
  Q_ASSERT(repoView);
  connect(mDiffTreeModel, &DiffTreeModel::updateSubmodules,
          [repoView](const QList<git::Submodule> &submodules, bool recursive,
                     bool init, bool force_checkout) {
            repoView->updateSubmodules(submodules, recursive, init,
                                       force_checkout);
          });

  stagedFiles->setModel(new TreeProxy(true, mDiffTreeModel, this));
  connect(stagedFiles, &QAbstractItemView::doubleClicked,
          [this, repoView](const QModelIndex &index) {
            openExternalDiffTool(index, repoView, true);
          });

  QHBoxLayout *hBoxLayout = new QHBoxLayout();
  QLabel *label = new QLabel(kStagedFiles);
  hBoxLayout->addWidget(label);
  hBoxLayout->addStretch();
  collapseButtonStagedFiles =
      new StatePushButton(kCollapseAll, kExpandAll, this);
  hBoxLayout->addWidget(collapseButtonStagedFiles);

  vBoxLayout->addLayout(hBoxLayout);
  vBoxLayout->addWidget(stagedFiles);
  mStagedWidget = new QWidget();
  mStagedWidget->setLayout(vBoxLayout);

  // unstaged files
  vBoxLayout = new QVBoxLayout();
  unstagedFiles = new TreeView(this, "Unstaged");
  unstagedFiles->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
  unstagedFiles->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(unstagedFiles, &QWidget::customContextMenuRequested,
          [this, repoView](const QPoint &pos) {
            showFileContextMenu(pos, repoView, unstagedFiles, false);
          });

  unstagedFiles->setModel(new TreeProxy(false, mDiffTreeModel, this));
  connect(unstagedFiles, &QAbstractItemView::doubleClicked,
          [this, repoView](const QModelIndex &index) {
            openExternalDiffTool(index, repoView, false);
          });

  hBoxLayout = new QHBoxLayout();
  mUnstagedCommitedFiles = new QLabel(kUnstagedFiles);
  hBoxLayout->addWidget(mUnstagedCommitedFiles);
  hBoxLayout->addStretch();
  collapseButtonUnstagedFiles =
      new StatePushButton(kCollapseAll, kExpandAll, this);
  hBoxLayout->addWidget(collapseButtonUnstagedFiles);

  vBoxLayout->addLayout(hBoxLayout);
  vBoxLayout->addWidget(unstagedFiles);
  QWidget *unstagedWidget = new QWidget();
  unstagedWidget->setLayout(vBoxLayout);

  // splitter between the staged and unstaged section
  QSplitter *treeViewSplitter = new QSplitter(Qt::Vertical, this);
  treeViewSplitter->setHandleWidth(10);
  treeViewSplitter->addWidget(mStagedWidget);
  treeViewSplitter->addWidget(unstagedWidget);
  treeViewSplitter->setStretchFactor(0, 0);
  treeViewSplitter->setStretchFactor(1, 1);

  // splitter between editor/diffview and TreeViews
  QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setHandleWidth(1);
  splitter->addWidget(fileView);
  splitter->addWidget(treeViewSplitter);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 1);
  // prevent that diffview will be collapsed
  // The problem is that the diffview is between two splitters
  // and if the diffview is collapsed only the splitter of the
  // commitlist is visible and is is not possible to get the
  // diffview visible again.
  splitter->setCollapsible(0, false);
  splitter->setCollapsible(1, false);
  connect(splitter, &QSplitter::splitterMoved, this, [splitter] {
    QSettings().setValue(kSplitterKey, splitter->saveState());
  });

  // Restore splitter state.
  splitter->restoreState(QSettings().value(kSplitterKey).toByteArray());

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(splitter);

  setLayout(layout);

  const QButtonGroup *viewGroup = segmentedButton->buttonGroup();
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
  connect(
      viewGroup, QOverload<int>::of(&QButtonGroup::idClicked), this,
      [this](int id) {
        mFileView->setCurrentIndex(id);
        // Change selection mode.
        if (id == Blame) {
          stagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
        } else {
          stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
        }
      });
#else
  connect(
      viewGroup, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
      [this, viewGroup](QAbstractButton *button) {
        mFileView->setCurrentIndex(viewGroup->id(button));
        // Change selection mode.
        if (viewGroup->id(button) == Blame) {
          stagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
        } else {
          stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
        }
      });
#endif

  connect(mDiffTreeModel, &DiffTreeModel::checkStateChanged, this,
          &DoubleTreeWidget::treeModelStateChanged);

  connect(stagedFiles, &TreeView::filesSelected, this,
          &DoubleTreeWidget::filesSelected);
  connect(stagedFiles, &TreeView::collapseCountChanged, this,
          &DoubleTreeWidget::collapseCountChanged);

  connect(unstagedFiles, &TreeView::filesSelected, this,
          &DoubleTreeWidget::filesSelected);
  connect(unstagedFiles, &TreeView::collapseCountChanged, this,
          &DoubleTreeWidget::collapseCountChanged);

  connect(collapseButtonStagedFiles, &StatePushButton::clicked, this,
          &DoubleTreeWidget::toggleCollapseStagedFiles);
  connect(collapseButtonUnstagedFiles, &StatePushButton::clicked, this,
          &DoubleTreeWidget::toggleCollapseUnstagedFiles);

  connect(repo.notifier(), &git::RepositoryNotifier::indexChanged, this,
          [this](const QStringList &paths) { mDiffTreeModel->refresh(paths); });

  RepoView *view = RepoView::parentView(this);
  connect(mEditor, &BlameEditor::linkActivated, view, &RepoView::visitLink);
}

QModelIndex DoubleTreeWidget::selectedIndex() const {
  TreeProxy *proxy = static_cast<TreeProxy *>(stagedFiles->model());
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    return proxy->mapToSource(indexes.first());
  }

  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  if (!indexes.isEmpty()) {
    return proxy->mapToSource(indexes.first());
  }
  return QModelIndex();
}

static void addNodeToMenu(const git::Index &index, QStringList &files,
                          const Node *node, bool staged, bool statusDiff) {
  Debug("DoubleTreeWidgetr addNodeToMenu()" << node->name());

  if (node->hasChildren()) {
    for (auto child : node->children()) {
      addNodeToMenu(index, files, child, staged, statusDiff);
    }

  } else {
    auto path = node->path(true);

    auto stageState = index.isStaged(path);

    if ((staged && stageState != git::Index::Unstaged) ||
        (!staged && stageState != git::Index::Staged) || !statusDiff) {
      files.append(path);
    }
  }
}

void DoubleTreeWidget::showFileContextMenu(const QPoint &pos, RepoView *view,
                                           QTreeView *tree, bool staged) {
  QStringList files;
  QModelIndexList indexes = tree->selectionModel()->selectedIndexes();
  const auto diff = view->diff();
  if (!diff.isValid())
    return;

  const bool statusDiff = diff.isStatusDiff();
  foreach (const QModelIndex &index, indexes) {
    auto node = index.data(Qt::UserRole).value<Node *>();

    addNodeToMenu(view->repo().index(), files, node, staged, statusDiff);
  }

  if (files.isEmpty())
    return;

  auto menu = new FileContextMenu(view, files, git::Index(), tree);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  menu->popup(tree->mapToGlobal(pos));
}

void DoubleTreeWidget::openExternalDiffTool(const QModelIndex &index,
                                            RepoView *view, bool staged) {
  const auto diff = view->diff();
  if (!diff.isValid())
    return;

  const bool statusDiff = diff.isStatusDiff();
  QStringList files;
  auto node = index.data(Qt::UserRole).value<Node *>();
  addNodeToMenu(view->repo().index(), files, node, staged, statusDiff);
  if (files.isEmpty())
    return;

  FileContextMenu fileMenu(view, files, git::Index(), nullptr);
  auto doubleClickAction = fileMenu.doubleClickAction();
  if (doubleClickAction)
    doubleClickAction->trigger();
}

QList<QModelIndex> DoubleTreeWidget::selectedIndices() const {
  QList<QModelIndex> list;

  TreeProxy *proxy = static_cast<TreeProxy *>(stagedFiles->model());
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  for (auto index : indexes)
    list.append(proxy->mapToSource(index));

  proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  for (auto index : indexes)
    list.append(proxy->mapToSource(index));

  return list;
}

QString DoubleTreeWidget::selectedFile() const {
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    return indexes.first().data(Qt::DisplayRole).toString();
  }

  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    return indexes.first().data(Qt::DisplayRole).toString();
  }
  return "";
}

/*!
 * \brief DoubleTreeWidget::setDiff
 * \param diff
 * \param file
 * \param pathspec
 */
void DoubleTreeWidget::setDiff(const git::Diff &diff, const QString &file,
                               const QString &pathspec) {
  Q_UNUSED(file)
  Q_UNUSED(pathspec)

  mSetDiffCounter++;

  DebugRefresh("time: " << QDateTime::currentDateTime()
                        << "Counter: " << mSetDiffCounter);

  mDiff = diff;

  // Remember selection.
  storeSelection();

  // Reset model.
  // because of this, the content in the view is shown.
  TreeProxy *proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  DiffTreeModel *model = static_cast<DiffTreeModel *>(proxy->sourceModel());
  model->setDiff(diff);

  // Single tree & list view.
  bool singleTree =
      Settings::instance()
          ->value(Setting::Id::ShowChangedFilesInSingleView, false)
          .toBool();
  bool listView = Settings::instance()
                      ->value(Setting::Id::ShowChangedFilesAsList, false)
                      .toBool();
  const bool multiColumn =
      Settings::instance()
          ->value(Setting::Id::ShowChangedFilesMultiColumn, true)
          .toBool();

  // Widget modifications.
  model->enableListView(listView);
  model->setMultiColumn(multiColumn);
  stagedFiles->setRootIsDecorated(!listView);
  unstagedFiles->setRootIsDecorated(!listView);
  // mUnstagedCommitedFiles->setVisible(!singleTree);
  collapseButtonStagedFiles->setVisible(!listView);
  collapseButtonUnstagedFiles->setVisible(!listView);

  unstagedFiles->updateView(); // Must be before expandAll/collapseAll is done,
                               // otherwise the collapse counter is wrong
  stagedFiles->updateView();

  // If statusDiff, there exist no staged/unstaged, but only
  // the commited files must be shown
  if (!diff.isValid() || diff.isStatusDiff()) {
    mUnstagedCommitedFiles->setText(singleTree ? kAllFiles : kUnstagedFiles);
    if (diff.isValid() && diff.count() < fileCountExpansionThreshold)
      stagedFiles->expandAll();
    else
      stagedFiles->collapseAll();

    proxy->enableFilter(!singleTree);
    mStagedWidget->setVisible(!singleTree);
  } else {
    mUnstagedCommitedFiles->setText(kCommitedFiles);
    mStagedWidget->setVisible(false);
  }

  // do not expand if to many files exist, it takes really long
  // So do it only when there are less than 100
  if (diff.isValid() && diff.count() < fileCountExpansionThreshold)
    unstagedFiles->expandAll();
  else
    unstagedFiles->collapseAll();

  // Clear editor.
  mEditor->clear();

  mDiffView->setDiff(diff);

  // Restore selection.
  if (diff.isValid())
    loadSelection();

  DebugRefresh("finished, time: " << QDateTime::currentDateTime()
                                  << "Counter: " << mSetDiffCounter);
}

void DoubleTreeWidget::find() { mEditor->find(); }

void DoubleTreeWidget::findNext() { mEditor->findNext(); }

void DoubleTreeWidget::findPrevious() { mEditor->findPrevious(); }

void DoubleTreeWidget::cancelBackgroundTasks() { mEditor->cancelBlame(); }

void DoubleTreeWidget::storeSelection() {
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    mSelectedFile.filename = indexes.first().data(Qt::EditRole).toString();
    mSelectedFile.stagedModel = true;
    return;
  }

  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    mSelectedFile.filename = indexes.first().data(Qt::EditRole).toString();
    mSelectedFile.stagedModel = false;
    return;
  }
  mSelectedFile.filename = "";
}

void DoubleTreeWidget::loadSelection() {
  QModelIndex index;
  Qt::CheckState state;

  if (mSelectedFile.filename != "") {
    index = mDiffTreeModel->index(mSelectedFile.filename);

    if (!index.isValid()) {
      // If index is anymore valid, because of removed file,
      // select the parent if possible
      auto list = mSelectedFile.filename.split(
          QStringLiteral("/")); // TODO: check also on windows
      list.removeLast();
      while (!index.isValid() && !list.isEmpty()) {
        const QString s = list.join(QStringLiteral("/"));
        index = mDiffTreeModel->index(s);
        list.removeLast();
      }
    }
    state = static_cast<Qt::CheckState>(
        mDiffTreeModel->data(index, Qt::CheckStateRole).toInt());
  }

  if (!index.isValid() ||
      (mSelectedFile.stagedModel && state != Qt::CheckState::Checked) ||
      (!mSelectedFile.stagedModel && state != Qt::CheckState::Unchecked)) {
    mSelectedFile.filename = "";
    if (mDiffTreeModel->rowCount() > 0) {
      index = mDiffTreeModel->index(0, 0);
      git::Index::StagedState s = static_cast<git::Index::StagedState>(
          mDiffTreeModel->data(index, Qt::CheckStateRole).toInt());
      mSelectedFile.stagedModel = (s == git::Index::StagedState::Staged);
    }
  }

  mIgnoreSelectionChange = true;
  if (mSelectedFile.stagedModel) {
    TreeProxy *proxy = static_cast<TreeProxy *>(stagedFiles->model());
    index = proxy->mapFromSource(index);
    stagedFiles->selectionModel()->setCurrentIndex(index,
                                                   QItemSelectionModel::Select);
  } else {
    TreeProxy *proxy = static_cast<TreeProxy *>(unstagedFiles->model());
    index = proxy->mapFromSource(index);
    unstagedFiles->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::Select);
  }
  mIgnoreSelectionChange = false;
}

void DoubleTreeWidget::treeModelStateChanged(const QModelIndex &index,
                                             int checkState) {
  Q_UNUSED(index)
  Q_UNUSED(checkState)

  // clear editor and disable diffView when no item is selected
  QModelIndexList stagedSelections =
      stagedFiles->selectionModel()->selectedIndexes();
  if (stagedSelections.count())
    return;

  QModelIndexList unstagedSelections =
      unstagedFiles->selectionModel()->selectedIndexes();
  if (unstagedSelections.count())
    return;

  mDiffView->enable(false);
  mEditor->clear();
}

void DoubleTreeWidget::collapseCountChanged(int count) {
  TreeView *view = static_cast<TreeView *>(QObject::sender());

  if (view == stagedFiles)
    collapseButtonStagedFiles->setState(count == 0);
  else
    collapseButtonUnstagedFiles->setState(count == 0);
}

void DoubleTreeWidget::filesSelected(const QModelIndexList &indexes) {
  if (indexes.isEmpty())
    return;

  QObject *obj = QObject::sender();
  if (obj) {
    TreeView *treeview = static_cast<TreeView *>(obj);
    if (treeview == stagedFiles) {
      unstagedFiles->deselectAll();
    } else if (treeview == unstagedFiles) {
      stagedFiles->deselectAll();
    }
  }
  loadEditorContent(indexes);
}

void DoubleTreeWidget::loadEditorContent(const QModelIndexList &indexes) {
  QString name;
  git::Blob blob;
  git::Commit commit;

  if (indexes.count() == 1) {
    RepoView *view = RepoView::parentView(this);
    name = indexes.first().data(Qt::EditRole).toString();
    QList<git::Commit> commits = view->commits();
    commit = !commits.isEmpty() ? commits.first() : git::Commit();
    int idx = mDiff.isValid() ? mDiff.indexOf(name) : -1;
    blob = idx < 0 ? git::Blob()
                   : view->repo().lookupBlob(mDiff.id(idx, git::Diff::NewFile));
  }

  mEditor->load(name, blob, std::move(commit));

  mDiffView->enable(true);
  mDiffView->updateFiles();
}

void DoubleTreeWidget::toggleCollapseStagedFiles() {
  if (collapseButtonStagedFiles->toggleState())
    stagedFiles->expandAll();
  else
    stagedFiles->collapseAll();
}

void DoubleTreeWidget::toggleCollapseUnstagedFiles() {
  if (collapseButtonUnstagedFiles->toggleState())
    unstagedFiles->expandAll();
  else
    unstagedFiles->collapseAll();
}

static QString severityColor(const QString &sev) {
  QString s = sev.toUpper().trimmed();
  if (s == "CRITICAL") return "#d32f2f";
  if (s == "HIGH")     return "#e65100";
  if (s == "MEDIUM")   return "#f9a825";
  if (s == "LOW")      return "#2e7d32";
  return "#546e7a";
}

static QString inlineFmt(const QString &text) {
  QString s = text.toHtmlEscaped();
  static QRegularExpression codeRe("`([^`]+)`");
  s.replace(codeRe, "<code>\\1</code>");
  static QRegularExpression boldRe("\\*\\*([^*]+)\\*\\*");
  s.replace(boldRe, "\\1");
  return s;
}

static QString issueKey(const QString &file, const QString &title) {
  return file.toLower().trimmed() + "|" + title.toLower().trimmed();
}

static QString formatReviewHtml(const QString &raw, const QWidget *w,
                                const QSet<QString> &fixedKeys = {},
                                bool hideFixed = false) {
  QFont baseFont = w->font();
#ifdef Q_OS_MAC
  baseFont.setPointSize(13);
#endif
  int basePt = baseFont.pointSize();
  int smallPt = basePt - 1;
  QString fg = w->palette().color(QPalette::Text).name();
  QString dimFg = w->palette().color(QPalette::PlaceholderText).name();
  QString bgAlt = w->palette().color(QPalette::AlternateBase).name();

  QString css = QStringLiteral(
      "<style>"
      "body { font-family: '%1'; font-size: %2pt; color: %3; margin:0; padding:8px; word-wrap:break-word; }"
      "code { background:%4; padding:1px 3px; }"
      ".dim { color:%5; }"
      ".issue { border-left:3px solid; padding:6px 0 10px 10px; margin:12px 0; overflow-wrap:break-word; }"
      ".badge { color:#fff; padding:2px 8px; font-weight:bold; font-size:%6pt; border-radius:3px; }"
      ".title { font-weight:bold; margin-left:6px; }"
      ".issue-num { color:%5; font-size:%6pt; margin-right:4px; }"
      ".detail { color:%5; margin-top:4px; line-height:1.4; }"
      ".file-ref { color:#64b5f6; margin-top:4px; }"
      ".fix { color:#81c784; margin-top:6px; }"
      ".fix-label { font-weight:bold; color:#81c784; }"
      "pre { background:%4; padding:4px 6px; font-size:%6pt; margin:4px 0 0 0; }"
      ".separator { height:1px; background:%4; margin:8px 0; }"
      "</style>")
      .arg(baseFont.family())
      .arg(basePt)
      .arg(fg, bgAlt, dimFg)
      .arg(smallPt);

  static QRegularExpression sevRe(
      "\\b(CRITICAL|HIGH|MEDIUM|LOW)\\b",
      QRegularExpression::CaseInsensitiveOption);

  bool hasIssues = sevRe.match(raw).hasMatch();

  if (!hasIssues) {
    QString clean = raw;
    clean.remove(QRegularExpression("^#+.*$", QRegularExpression::MultilineOption));
    clean = clean.trimmed();
    return css +
        QStringLiteral(
            "<body>"
            "<span class='badge' style='background:#2e7d32;'>PASS</span> "
            "<span style='color:#a5d6a7;'>No issues found</span>"
            "<p class='dim'>%1</p>"
            "</body>")
            .arg(inlineFmt(clean));
  }

  struct Issue {
    QString severity, title, file, problem, fix;
  };

  QList<Issue> issues;
  Issue cur;
  bool parsing = false;
  bool inCode = false;
  QString codeAccum;

  for (const QString &line : raw.split('\n')) {
    QString t = line.trimmed();

    if (t.startsWith("```")) {
      inCode = !inCode;
      if (!inCode && !codeAccum.isEmpty()) {
        cur.fix += "\n" + codeAccum;
        codeAccum.clear();
      }
      continue;
    }
    if (inCode) { codeAccum += t + "\n"; continue; }

    auto sm = sevRe.match(t);
    if (sm.hasMatch()) {
      if (!cur.severity.isEmpty()) issues.append(cur);
      cur = Issue();
      cur.severity = sm.captured(1).toUpper();
      QString rest = t;
      rest.remove(QRegularExpression("^#+\\s*"));
      rest.remove(QRegularExpression("\\*+"));
      rest.remove(QRegularExpression("\\(?" + cur.severity + "\\)?",
                                     QRegularExpression::CaseInsensitiveOption));
      rest.remove(QRegularExpression("^[:\\-\\s]+"));
      rest.remove(QRegularExpression("^\\d+\\.\\s*"));
      rest.remove(QRegularExpression("(?:severity|Severity)[:\\s]*",
                                     QRegularExpression::CaseInsensitiveOption));
      cur.title = rest.trimmed();
      parsing = true;
      continue;
    }

    if (!parsing) continue;

    // Skip markdown separators and sub-headers that leak from AI
    if (QRegularExpression("^-{3,}$").match(t).hasMatch()) continue;
    if (QRegularExpression("^#{2,}\\s").match(t).hasMatch()) continue;

    static QRegularExpression fileRe(
        "^[\\*\\-]*\\s*\\*{0,2}(?:File|Location)[:\\s]*\\*{0,2}\\s*(.+)",
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression probRe(
        "^[\\*\\-]*\\s*\\*{0,2}(?:Problem|Issue|Description)[:\\s]*\\*{0,2}\\s*(.+)",
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression fixRe(
        "^[\\*\\-]*\\s*\\*{0,2}(?:Fix|Suggestion|Recommended|Solution)[:\\s]*\\*{0,2}\\s*(.+)",
        QRegularExpression::CaseInsensitiveOption);

    auto fm = fileRe.match(t);
    if (fm.hasMatch()) { cur.file = fm.captured(1).trimmed(); cur.file.remove('`'); cur.file.remove(QRegularExpression("^[:\\s]+")); continue; }
    auto pm = probRe.match(t);
    if (pm.hasMatch()) { QString p = pm.captured(1).trimmed(); p.remove(QRegularExpression("^[:\\s]+")); cur.problem = p; continue; }
    auto xm = fixRe.match(t);
    if (xm.hasMatch()) { QString x = xm.captured(1).trimmed(); x.remove(QRegularExpression("^[:\\s]+")); cur.fix = x; continue; }

    if (!t.isEmpty()) {
      if (cur.problem.isEmpty() && cur.title.isEmpty()) cur.title = t;
      else if (cur.problem.isEmpty()) cur.problem = t;
      else cur.problem += " " + t;
    }
  }
  if (!cur.severity.isEmpty()) issues.append(cur);

  // Clean trailing markdown artifacts from all fields
  static QRegularExpression trailingMd("\\s*-{3,}.*$");
  for (Issue &i : issues) {
    i.title.remove(trailingMd);
    i.problem.remove(trailingMd);
    i.fix.remove(trailingMd);
    i.title = i.title.trimmed();
    i.problem = i.problem.trimmed();
    i.fix = i.fix.trimmed();
  }

  // Filter out non-issues: summary lines the AI emits that happen to
  // contain a severity keyword but have no file, no problem, and no fix.
  issues.erase(std::remove_if(issues.begin(), issues.end(),
      [](const Issue &i) {
        bool hasSubstance = !i.file.isEmpty() || !i.fix.isEmpty();
        if (hasSubstance) return false;

        // Title-only "issues" that are really summary text
        QString t = i.title.toLower() + " " + i.problem.toLower();
        if (t.contains("no ") && (t.contains("found") || t.contains("identified")
            || t.contains("detected") || t.contains("vulnerabilit")))
          return true;
        if (t.contains("unlikely to cause") || t.contains("overall")
            || t.contains("summary") || t.contains("code quality issues were"))
          return true;

        // No problem description and title looks like a preamble
        if (i.problem.isEmpty() && i.fix.isEmpty()
            && (i.title.contains("issues") || i.title.contains("were identified")
                || i.title.contains("were noted") || i.title.contains("were found")))
          return true;

        return false;
      }), issues.end());

  std::sort(issues.begin(), issues.end(),
            [](const Issue &a, const Issue &b) {
              auto ord = [](const QString &s) {
                if (s == "CRITICAL") return 0;
                if (s == "HIGH") return 1;
                if (s == "MEDIUM") return 2;
                return 3;
              };
              return ord(a.severity) < ord(b.severity);
            });

  // Count by severity
  int crit = 0, high = 0, med = 0, low = 0;
  for (const Issue &i : issues) {
    if (i.severity == "CRITICAL") ++crit;
    else if (i.severity == "HIGH") ++high;
    else if (i.severity == "MEDIUM") ++med;
    else ++low;
  }

  QString html = css + "<body>";

  // Summary header
  if (issues.isEmpty()) {
    html += "<b style='color:#81c784;'>No issues found</b><br>"
            "<span class='dim'>The code looks clean.</span>";
    html += "</body>";
    return html;
  }

  html += QStringLiteral("<b>Found %1 issue%2</b><br>")
              .arg(issues.size())
              .arg(issues.size() == 1 ? "" : "s");

  QStringList counts;
  if (crit > 0) counts << QStringLiteral("<span style='color:#d32f2f;'>%1 Critical</span>").arg(crit);
  if (high > 0) counts << QStringLiteral("<span style='color:#e65100;'>%1 High</span>").arg(high);
  if (med > 0)  counts << QStringLiteral("<span style='color:#f9a825;'>%1 Medium</span>").arg(med);
  if (low > 0)  counts << QStringLiteral("<span style='color:#2e7d32;'>%1 Low</span>").arg(low);
  html += "<span class='dim'>" + counts.join(" &middot; ") + "</span>";

  html += "<div class='separator'></div>";

  // Each issue
  int shown = 0;
  for (int i = 0; i < issues.size(); ++i) {
    const Issue &issue = issues[i];
    QString key = issueKey(issue.file, issue.title);
    bool fixed = fixedKeys.contains(key);

    if (fixed && hideFixed)
      continue;

    ++shown;
    QString color = severityColor(issue.severity);

    if (fixed)
      html += QStringLiteral(
          "<div class='issue' style='border-color:%1; opacity:0.45;'>").arg(color);
    else
      html += QStringLiteral(
          "<div class='issue' style='border-color:%1;'>").arg(color);

    // Header: number + badge + title
    html += QStringLiteral(
                "<span class='issue-num'>#%1</span>"
                "<span class='badge' style='background:%2;'>%3</span>")
                .arg(i + 1)
                .arg(color, issue.severity);
    if (fixed)
      html += QStringLiteral(
          "<span class='title' style='text-decoration:line-through;'>%1</span>"
          " <span style='color:#81c784;'>&#x2714; fixed</span>")
          .arg(inlineFmt(issue.title));
    else
      html += QStringLiteral("<span class='title'>%1</span>")
          .arg(inlineFmt(issue.title));

    // File reference
    if (!issue.file.isEmpty()) {
      QString f = issue.file;
      f.remove(QRegularExpression("^[:\\s]+"));
      html += "<div class='file-ref'>" + f.toHtmlEscaped() + "</div>";
    }

    if (!fixed) {
      // Problem description
      if (!issue.problem.isEmpty()) {
        QString p = issue.problem;
        p.remove(QRegularExpression("^[:\\s]+"));
        html += "<div class='detail'>" + inlineFmt(p) + "</div>";
      }

      // Fix suggestion
      if (!issue.fix.isEmpty()) {
        QString fx = issue.fix;
        fx.remove(QRegularExpression("^[:\\s]+"));
        if (fx.contains('\n'))
          html += "<div class='fix'><pre>" + fx.toHtmlEscaped() + "</pre></div>";
        else
          html += "<div class='fix'><span class='fix-label'>Fix: </span>"
                  + inlineFmt(fx) + "</div>";
      }
    }

    html += "</div>";
  }

  html += "</body>";
  return html;
}

void DoubleTreeWidget::showReview(const QString &text, const QByteArray &diff) {
  if (text != mLastReviewText) {
    mFixedIssueKeys.clear();
    mHideFixedCb->setChecked(false);
  }
  mLastReviewText = text;
  mLastReviewDiff = diff;
  mCancelReview = nullptr;
  mStopReviewBtn->setVisible(false);
  bool hide = mHideFixedCb->isChecked();
  mReviewPanel->setHtml(formatReviewHtml(text, this, mFixedIssueKeys, hide));
  mFixBtn->setVisible(!diff.isEmpty() && !mFixedIssueKeys.contains("__all__"));
  mHideFixedCb->setVisible(!mFixedIssueKeys.isEmpty());
  mFileView->setCurrentIndex(Review);
}

void DoubleTreeWidget::beginStreamingReview(std::function<void()> onCancel) {
  mCancelReview = std::move(onCancel);
  mStopReviewBtn->setEnabled(true);
  mStopReviewBtn->setVisible(true);
  mFixBtn->setVisible(false);
  mHideFixedCb->setVisible(false);
  mReviewPanel->setHtml(formatReviewHtml(tr("Reviewing…"), this, mFixedIssueKeys,
                                         false));
  mFileView->setCurrentIndex(Review);
}

void DoubleTreeWidget::streamReviewChunk(const QString &partialText) {
  mReviewPanel->setHtml(
      formatReviewHtml(partialText, this, mFixedIssueKeys, false));
  // Keep the newest content in view as it streams in.
  mReviewPanel->verticalScrollBar()->setValue(
      mReviewPanel->verticalScrollBar()->maximum());
}

void DoubleTreeWidget::applyFixBlocks(const QString &aiResponse) {
  struct Fix { QString file, find, replace; };
  QList<Fix> fixes;

  QStringList blocks = aiResponse.split(QRegularExpression(R"(\n---+\n?)"),
                                        Qt::SkipEmptyParts);

  for (const QString &raw : blocks) {
    QString block = raw.trimmed();
    if (block.isEmpty()) continue;
    Fix f;
    QString section;
    QStringList findLines, replaceLines;
    for (const QString &line : block.split('\n')) {
      if (line.startsWith("FILE:")) {
        f.file = line.mid(5).trimmed();
      } else if (line.trimmed() == "FIND:") {
        section = "find";
      } else if (line.trimmed() == "REPLACE:") {
        section = "replace";
      } else if (section == "find") {
        findLines << line;
      } else if (section == "replace") {
        replaceLines << line;
      }
    }
    f.find = findLines.join('\n');
    f.replace = replaceLines.join('\n');
    if (!f.file.isEmpty() && !f.find.isEmpty())
      fixes.append(f);
  }

  if (fixes.isEmpty()) {
    mReviewPanel->append(
        "<div style='margin-top:12px; padding:10px; "
        "border-left:3px solid #ffb74d;'>"
        "<b style='color:#ffb74d;'>Could not parse fix blocks</b>"
        "<p style='color:#aaa; font-size:11px; margin-top:6px;'>"
        "The AI response did not contain valid FILE/FIND/REPLACE blocks.<br>"
        "First 300 chars:</p>"
        "<pre style='background:#1e1e1e; padding:6px; font-size:10px; "
        "color:#888; margin-top:4px;'>" +
        aiResponse.left(300).toHtmlEscaped() + "</pre></div>");
    return;
  }

  RepoView *view = RepoView::parentView(this);
  if (!view) return;
  QString workdir = view->repo().workdir().path();

  struct Result {
    QString file;
    bool ok;
    QString error;
    QString find, replace;
  };
  QList<Result> results;

  // Helper: find file by name if exact path fails
  auto resolveFile = [&](const QString &rel) -> QString {
    QString exact = workdir + "/" + rel;
    if (QFile::exists(exact)) return exact;

    // Try common prefix variations
    QString basename = QFileInfo(rel).fileName();
    QDirIterator it(workdir, {basename}, QDir::Files,
                    QDirIterator::Subdirectories);
    QString best;
    while (it.hasNext()) {
      QString candidate = it.next();
      // Prefer match that shares the most path components
      if (candidate.endsWith(rel)) return candidate;
      if (best.isEmpty()) best = candidate;
    }
    return best.isEmpty() ? exact : best;
  };

  for (const Fix &fix : fixes) {
    Result r;
    r.file = fix.file;
    if (r.file.startsWith("a/") || r.file.startsWith("b/"))
      r.file = r.file.mid(2);
    r.find = fix.find;
    r.replace = fix.replace;

    QString fullPath = resolveFile(r.file);
    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      r.ok = false;
      r.error = tr("file not found");
      results.append(r);
      continue;
    }
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    if (fix.find.trimmed() == fix.replace.trimmed()) {
      r.ok = false;
      r.error = tr("no-op (find and replace are identical)");
      results.append(r);
      continue;
    }

    if (!content.contains(fix.find)) {
      r.ok = false;
      r.error = tr("search text not found in file");
      results.append(r);
      continue;
    }

    content.replace(fix.find, fix.replace);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
      r.ok = false;
      r.error = tr("could not write file");
      results.append(r);
      continue;
    }
    file.write(content.toUtf8());
    file.close();
    r.ok = true;
    results.append(r);
  }

  int applied = 0, failed = 0;
  for (const Result &r : results) {
    if (r.ok) ++applied;
    else ++failed;
  }

  // Build styled output — inherit review panel font
  QString html;
  html += "<div class='separator'></div>";

  // Summary line
  QString sumColor = failed == 0 ? "#81c784" : (applied > 0 ? "#e2c08d" : "#e57373");
  html += QStringLiteral("<b style='color:%1;'>").arg(sumColor);
  if (applied > 0 && failed == 0)
    html += QStringLiteral("All %1 fix(es) applied successfully").arg(applied);
  else if (applied > 0)
    html += QStringLiteral("%1 applied, %2 failed").arg(applied).arg(failed);
  else
    html += QStringLiteral("All %1 fix(es) failed").arg(failed);
  html += "</b>";

  // Per-file results
  for (const Result &r : results) {
    QString borderColor = r.ok ? "#81c784" : "#e57373";
    QString icon = r.ok ? "&#x2714;" : "&#x2718;";
    QString iconColor = r.ok ? "#81c784" : "#e57373";

    html += QStringLiteral(
        "<div class='issue' style='border-color:%1;'>").arg(borderColor);

    // File header
    html += QStringLiteral(
        "<span style='color:%1;'>%2</span> "
        "<span class='file-ref' style='font-weight:bold;'>%3</span>")
        .arg(iconColor, icon, r.file.toHtmlEscaped());
    if (!r.ok)
      html += " <span style='color:#e57373;'>&mdash; " +
              r.error.toHtmlEscaped() + "</span>";

    // Diff
    html += "<pre>";
    for (const QString &line : r.find.split('\n'))
      html += "<span style='color:#e57373;'>- " +
              line.toHtmlEscaped() + "</span>\n";
    for (const QString &line : r.replace.split('\n'))
      html += "<span style='color:#81c784;'>+ " +
              line.toHtmlEscaped() + "</span>\n";
    html += "</pre></div>";
  }

  mReviewPanel->append(html);

  // Track which issues were fixed by matching fix file to review issues
  for (const Result &r : results) {
    if (!r.ok) continue;
    // Match against all issues that reference this file
    QString fixBase = QFileInfo(r.file).fileName().toLower();
    // Parse review to find matching issues
    static QRegularExpression sevRe(
        "\\b(CRITICAL|HIGH|MEDIUM|LOW)\\b",
        QRegularExpression::CaseInsensitiveOption);
    QString currentTitle;
    QString currentFile;
    for (const QString &line : mLastReviewText.split('\n')) {
      QString t = line.trimmed();
      auto sm = sevRe.match(t);
      if (sm.hasMatch()) {
        if (!currentTitle.isEmpty() && !currentFile.isEmpty()) {
          QString base = QFileInfo(currentFile).fileName().toLower();
          if (base == fixBase)
            mFixedIssueKeys.insert(issueKey(currentFile, currentTitle));
        }
        currentFile.clear();
        QString rest = t;
        rest.remove(QRegularExpression("^#+\\s*"));
        rest.remove(QRegularExpression("\\*+"));
        rest.remove(QRegularExpression("\\(?" + sm.captured(1).toUpper() + "\\)?",
                                       QRegularExpression::CaseInsensitiveOption));
        rest.remove(QRegularExpression("^[:\\-\\s]+"));
        rest.remove(QRegularExpression("^\\d+\\.\\s*"));
        rest.remove(QRegularExpression("(?:severity|Severity)[:\\s]*",
                                       QRegularExpression::CaseInsensitiveOption));
        currentTitle = rest.trimmed();
        continue;
      }
      static QRegularExpression fileRe(
          "^[\\*\\-]*\\s*\\*{0,2}(?:File|Location)[:\\s]*\\*{0,2}\\s*(.+)",
          QRegularExpression::CaseInsensitiveOption);
      auto fm = fileRe.match(t);
      if (fm.hasMatch()) {
        currentFile = fm.captured(1).trimmed();
        currentFile.remove('`');
        currentFile.remove(QRegularExpression("^[:\\s]+"));
      }
    }
    if (!currentTitle.isEmpty() && !currentFile.isEmpty()) {
      QString base = QFileInfo(currentFile).fileName().toLower();
      if (base == fixBase)
        mFixedIssueKeys.insert(issueKey(currentFile, currentTitle));
    }
  }

  // Hide button if all applied
  if (failed == 0) {
    mFixBtn->setVisible(false);
    mFixedIssueKeys.insert("__all__");
  } else {
    mFixBtn->setEnabled(true);
  }

  mHideFixedCb->setVisible(!mFixedIssueKeys.isEmpty());

  if (applied > 0) view->refresh();
}

void DoubleTreeWidget::startReviewSpinner() {
  mReviewPanel->setHtml(
      "<div style='font-family: -apple-system, sans-serif; padding:24px; "
      "color:#aaa; font-size:14px;'>"
      "&#x23F3; Reviewing code&hellip;</div>");
  mFileView->setCurrentIndex(Review);
}

void DoubleTreeWidget::stopReviewSpinner() {
}
