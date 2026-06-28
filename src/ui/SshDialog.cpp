#include "SshDialog.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr int kMaxEntries = 500;
static const QString kPlaceholder = QStringLiteral("__loading__");
static const QString kSettingsGroup = QStringLiteral("ssh/servers/last");

// ---------------------------------------------------------------------------
// SshWorker
// ---------------------------------------------------------------------------

SshWorker::SshWorker(QObject *parent) : QObject(parent) {}

SshWorker::~SshWorker() { cleanup(); }

void SshWorker::cleanup() {
  if (mSftp) {
    libssh2_sftp_shutdown(mSftp);
    mSftp = nullptr;
  }
  if (mSession) {
    libssh2_session_disconnect(mSession, "bye");
    libssh2_session_free(mSession);
    mSession = nullptr;
  }
  if (mSock >= 0) {
    ::close(mSock);
    mSock = -1;
  }
}

bool SshWorker::statPath(const QString &path) {
  if (!mSftp) return false;
  LIBSSH2_SFTP_ATTRIBUTES attrs{};
  return libssh2_sftp_stat(mSftp, path.toUtf8().constData(), &attrs) == 0;
}

bool SshWorker::authWithAgent(const QString &user) {
  LIBSSH2_AGENT *agent = libssh2_agent_init(mSession);
  if (!agent) return false;

  bool authed = false;
  if (libssh2_agent_connect(agent) == 0 &&
      libssh2_agent_list_identities(agent) == 0) {
    struct libssh2_agent_publickey *id = nullptr, *prev = nullptr;
    QByteArray u = user.toUtf8();
    while (libssh2_agent_get_identity(agent, &id, prev) == 0) {
      if (libssh2_agent_userauth(agent, u.constData(), id) == 0) {
        authed = true;
        break;
      }
      prev = id;
    }
    libssh2_agent_disconnect(agent);
  }
  libssh2_agent_free(agent);
  return authed;
}

bool SshWorker::authWithKey(const QString &user, const QString &keyPath) {
  QByteArray u = user.toUtf8();
  QByteArray pub = (keyPath + ".pub").toUtf8();
  QByteArray priv = keyPath.toUtf8();
  return libssh2_userauth_publickey_fromfile(mSession, u.constData(),
                                             pub.constData(), priv.constData(),
                                             nullptr) == 0;
}

void SshWorker::connectToServer(const QString &host, quint16 port,
                                const QString &user, const QString &keyPath) {
  cleanup();

  // Resolve & connect
  struct addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  QByteArray hostB = host.toUtf8();
  QByteArray portB = QByteArray::number(port);
  int rc = getaddrinfo(hostB.constData(), portB.constData(), &hints, &res);
  if (rc != 0) {
    emit error(QString("DNS lookup failed: %1").arg(gai_strerror(rc)));
    return;
  }

  mSock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (mSock < 0 || ::connect(mSock, res->ai_addr, res->ai_addrlen) != 0) {
    freeaddrinfo(res);
    emit error(QString("TCP connect failed: %1").arg(strerror(errno)));
    if (mSock >= 0) { ::close(mSock); mSock = -1; }
    return;
  }
  freeaddrinfo(res);

  // SSH handshake
  mSession = libssh2_session_init();
  if (libssh2_session_handshake(mSession, mSock) != 0) {
    char *msg;
    libssh2_session_last_error(mSession, &msg, nullptr, 0);
    emit error(QString("SSH handshake failed: %1").arg(msg));
    cleanup();
    return;
  }

  // Authenticate: agent first, then key file
  bool authed = authWithAgent(user);
  if (!authed && !keyPath.isEmpty())
    authed = authWithKey(user, keyPath);
  if (!authed) {
    emit error("Authentication failed — check SSH agent or key file.");
    cleanup();
    return;
  }

  // SFTP session
  mSftp = libssh2_sftp_init(mSession);
  if (!mSftp) {
    char *msg;
    libssh2_session_last_error(mSession, &msg, nullptr, 0);
    emit error(QString("SFTP init failed: %1").arg(msg));
    cleanup();
    return;
  }

  // Resolve home dir
  char home[512] = "/";
  libssh2_sftp_realpath(mSftp, ".", home, sizeof(home) - 1);

  emit connected(QString::fromUtf8(home));
}

void SshWorker::listDir(const QString &path) {
  if (!mSftp) { emit error("Not connected"); return; }

  QByteArray pathB = path.toUtf8();
  LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(mSftp, pathB.constData());
  if (!dir) {
    char *msg;
    libssh2_session_last_error(mSession, &msg, nullptr, 0);
    emit error(QString("Cannot open %1: %2").arg(path, msg));
    return;
  }

  struct Raw { QString name; bool isDir; };
  QList<Raw> items;
  char name[512];
  LIBSSH2_SFTP_ATTRIBUTES attrs{};

  while ((int)items.size() < kMaxEntries) {
    int n = libssh2_sftp_readdir_ex(dir, name, sizeof(name), nullptr, 0, &attrs);
    if (n <= 0) break;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

    bool isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                 S_ISDIR(attrs.permissions);
    items.append({QString::fromUtf8(name), isDir});
  }
  libssh2_sftp_closedir(dir);

  // Check if current dir is already a git repo (has .git)
  // and for each subdir check if it's a git repo + if it has further subdirs
  QList<SshEntry> entries;
  for (const Raw &r : items) {
    if (!r.isDir) continue;
    if (r.name.startsWith('.')) continue; // skip hidden dirs

    SshEntry e;
    e.name = r.name;

    QString full = path + "/" + r.name;
    e.isGitRepo = statPath(full + "/.git");

    // Check for subdirs (peek inside)
    QByteArray fullB = full.toUtf8();
    LIBSSH2_SFTP_HANDLE *sub = libssh2_sftp_opendir(mSftp, fullB.constData());
    if (sub) {
      char sname[256];
      LIBSSH2_SFTP_ATTRIBUTES sattrs{};
      while (libssh2_sftp_readdir_ex(sub, sname, sizeof(sname), nullptr, 0, &sattrs) > 0) {
        if (strcmp(sname, ".") == 0 || strcmp(sname, "..") == 0) continue;
        bool sd = (sattrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && S_ISDIR(sattrs.permissions);
        if (sd && sname[0] != '.') { e.hasSubdirs = true; break; }
        if (strcmp(sname, ".git") == 0) e.isGitRepo = true;
      }
      libssh2_sftp_closedir(sub);
    }

    entries.append(e);
  }

  // Sort: git repos first, then alpha
  std::sort(entries.begin(), entries.end(), [](const SshEntry &a, const SshEntry &b) {
    if (a.isGitRepo != b.isGitRepo) return a.isGitRepo > b.isGitRepo;
    return a.name < b.name;
  });

  emit dirListed(path, entries);
}

// ---------------------------------------------------------------------------
// SshDialog
// ---------------------------------------------------------------------------

SshDialog::SshDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Open SSH Repository"));
  setMinimumSize(600, 480);

  // ── Connection form ──
  mHostEdit = new QLineEdit(this);
  mHostEdit->setPlaceholderText("myserver.example.com");

  mUserEdit = new QLineEdit(this);
  mUserEdit->setPlaceholderText("stefan");

  mPortEdit = new QLineEdit("22", this);
  mPortEdit->setMaximumWidth(80);

  mKeyEdit = new QLineEdit(this);
  mKeyEdit->setPlaceholderText(QDir::homePath() + "/.ssh/id_rsa");

  QPushButton *browseKey = new QPushButton("…", this);
  browseKey->setMaximumWidth(32);
  connect(browseKey, &QPushButton::clicked, this, [this] {
    QString f = QFileDialog::getOpenFileName(this, tr("SSH Private Key"),
                                             QDir::homePath() + "/.ssh");
    if (!f.isEmpty()) mKeyEdit->setText(f);
  });

  QHBoxLayout *keyRow = new QHBoxLayout;
  keyRow->addWidget(mKeyEdit);
  keyRow->addWidget(browseKey);

  QFormLayout *form = new QFormLayout;
  form->addRow(tr("Host:"), mHostEdit);
  form->addRow(tr("User:"), mUserEdit);
  form->addRow(tr("Port:"), mPortEdit);
  form->addRow(tr("SSH Key (optional):"), keyRow);

  mConnectBtn = new QPushButton(tr("Connect"), this);
  mConnectBtn->setDefault(true);
  connect(mConnectBtn, &QPushButton::clicked, this, &SshDialog::onConnectClicked);

  QGroupBox *connBox = new QGroupBox(tr("Server"), this);
  QVBoxLayout *connLayout = new QVBoxLayout(connBox);
  connLayout->addLayout(form);
  connLayout->addWidget(mConnectBtn, 0, Qt::AlignRight);

  // ── File tree ──
  mTree = new QTreeWidget(this);
  mTree->setHeaderHidden(true);
  mTree->setSelectionMode(QAbstractItemView::SingleSelection);
  connect(mTree, &QTreeWidget::itemExpanded, this, &SshDialog::onItemExpanded);
  connect(mTree, &QTreeWidget::itemSelectionChanged, this, &SshDialog::onSelectionChanged);

  // ── Status ──
  mStatusLabel = new QLabel(tr("Enter server details and click Connect."), this);
  mStatusLabel->setWordWrap(true);

  // ── Buttons ──
  mOpenBtn = new QPushButton(tr("Open Repository"), this);
  mOpenBtn->setEnabled(false);
  connect(mOpenBtn, &QPushButton::clicked, this, &SshDialog::onOpenClicked);

  QPushButton *cancelBtn = new QPushButton(tr("Cancel"), this);
  connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

  QHBoxLayout *btnRow = new QHBoxLayout;
  btnRow->addWidget(mStatusLabel, 1);
  btnRow->addWidget(cancelBtn);
  btnRow->addWidget(mOpenBtn);

  // ── Main layout ──
  QVBoxLayout *main = new QVBoxLayout(this);
  main->addWidget(connBox);
  main->addWidget(mTree, 1);
  main->addLayout(btnRow);

  // ── Worker thread ──
  mThread = new QThread(this);
  mWorker = new SshWorker;
  mWorker->moveToThread(mThread);
  connect(mThread, &QThread::finished, mWorker, &QObject::deleteLater);
  connect(this, &SshDialog::doConnect, mWorker, &SshWorker::connectToServer,
          Qt::QueuedConnection);
  connect(this, &SshDialog::doListDir, mWorker, &SshWorker::listDir,
          Qt::QueuedConnection);
  connect(mWorker, &SshWorker::connected, this, &SshDialog::onConnected,
          Qt::QueuedConnection);
  connect(mWorker, &SshWorker::error, this, &SshDialog::onError,
          Qt::QueuedConnection);
  connect(mWorker, &SshWorker::dirListed, this, &SshDialog::onDirListed,
          Qt::QueuedConnection);
  mThread->start();

  loadSettings();
}

SshDialog::~SshDialog() {
  // Ask worker to clean up, then stop thread
  QMetaObject::invokeMethod(mWorker, "cleanup", Qt::QueuedConnection);
  mThread->quit();
  mThread->wait(3000);
}

void SshDialog::saveSettings() {
  QSettings s;
  s.beginGroup(kSettingsGroup);
  s.setValue("host", mHostEdit->text());
  s.setValue("user", mUserEdit->text());
  s.setValue("port", mPortEdit->text());
  s.setValue("key", mKeyEdit->text());
  s.endGroup();
}

void SshDialog::loadSettings() {
  QSettings s;
  s.beginGroup(kSettingsGroup);
  mHostEdit->setText(s.value("host").toString());
  mUserEdit->setText(s.value("user").toString());
  mPortEdit->setText(s.value("port", "22").toString());
  mKeyEdit->setText(s.value("key").toString());
  s.endGroup();
}

void SshDialog::setStatus(const QString &msg, bool isError) {
  mStatusLabel->setText(msg);
  mStatusLabel->setStyleSheet(isError ? "color: red" : "");
}

void SshDialog::onConnectClicked() {
  QString host = mHostEdit->text().trimmed();
  QString user = mUserEdit->text().trimmed();
  if (host.isEmpty() || user.isEmpty()) {
    setStatus(tr("Host and user are required."), true);
    return;
  }

  quint16 port = (quint16)mPortEdit->text().toUShort();
  if (port == 0) port = 22;

  mHost = host;
  mUser = user;
  mPort = port;

  saveSettings();
  mTree->clear();
  mConnectBtn->setEnabled(false);
  mOpenBtn->setEnabled(false);
  setStatus(tr("Connecting to %1@%2:%3…").arg(user, host).arg(port));

  emit doConnect(host, port, user, mKeyEdit->text().trimmed());
}

void SshDialog::onConnected(const QString &homeDir) {
  mConnectBtn->setEnabled(true);
  setStatus(tr("Connected. Browse to a git repository."));

  mTree->clear();

  // Seed tree with home dir
  QTreeWidgetItem *root = new QTreeWidgetItem(mTree);
  root->setText(0, homeDir);
  root->setData(0, Qt::UserRole, homeDir);
  root->setData(0, Qt::UserRole + 1, false); // not git

  // Placeholder so expand arrow shows
  QTreeWidgetItem *ph = new QTreeWidgetItem(root);
  ph->setText(0, kPlaceholder);

  mTree->expandItem(root);
}

void SshDialog::onError(const QString &msg) {
  mConnectBtn->setEnabled(true);
  setStatus(msg, true);
}

QTreeWidgetItem *SshDialog::addEntry(QTreeWidgetItem *parent, const SshEntry &e,
                                     const QString &parentPath) {
  QString full = parentPath + "/" + e.name;
  QTreeWidgetItem *item = new QTreeWidgetItem(parent);
  item->setData(0, Qt::UserRole, full);
  item->setData(0, Qt::UserRole + 1, e.isGitRepo);

  if (e.isGitRepo) {
    item->setText(0, "⬡ " + e.name);
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
    item->setToolTip(0, tr("Git repository: %1").arg(full));
  } else {
    item->setText(0, e.name);
  }

  if (e.hasSubdirs || e.isGitRepo) {
    // Add placeholder so Qt shows the expand arrow
    QTreeWidgetItem *ph = new QTreeWidgetItem(item);
    ph->setText(0, kPlaceholder);
  }

  return item;
}

void SshDialog::onDirListed(const QString &path, QList<SshEntry> entries) {
  // Find item whose path matches
  QList<QTreeWidgetItem *> stack;
  for (int i = 0; i < mTree->topLevelItemCount(); ++i)
    stack.append(mTree->topLevelItem(i));

  QTreeWidgetItem *target = nullptr;
  while (!stack.isEmpty()) {
    QTreeWidgetItem *it = stack.takeFirst();
    if (it->data(0, Qt::UserRole).toString() == path) {
      target = it;
      break;
    }
    for (int i = 0; i < it->childCount(); ++i)
      stack.append(it->child(i));
  }

  if (!target) return;

  // Remove placeholder children
  while (target->childCount() > 0) delete target->takeChild(0);

  if (entries.isEmpty()) {
    setStatus(tr("No directories found in %1.").arg(path));
    return;
  }

  for (const SshEntry &e : entries)
    addEntry(target, e, path);

  setStatus(tr("Loaded %1 — click a repository to open it.").arg(path));
}

void SshDialog::onItemExpanded(QTreeWidgetItem *item) {
  if (!item) return;

  // Check if first child is the loading placeholder
  if (item->childCount() == 1 &&
      item->child(0)->text(0) == kPlaceholder) {
    QString path = item->data(0, Qt::UserRole).toString();
    setStatus(tr("Loading %1…").arg(path));
    emit doListDir(path);
  }
}

void SshDialog::onSelectionChanged() {
  QList<QTreeWidgetItem *> sel = mTree->selectedItems();
  if (sel.isEmpty()) {
    mSelectedPath.clear();
    mOpenBtn->setEnabled(false);
    return;
  }

  QTreeWidgetItem *item = sel.first();
  bool isGit = item->data(0, Qt::UserRole + 1).toBool();
  mSelectedPath = item->data(0, Qt::UserRole).toString();
  mOpenBtn->setEnabled(isGit);
}

void SshDialog::onOpenClicked() {
  if (mSelectedPath.isEmpty()) return;
  accept();
}

QString SshDialog::sshUrl() const {
  // ssh://user@host:port/path
  return QStringLiteral("ssh://%1@%2:%3%4")
      .arg(mUser, mHost)
      .arg(mPort)
      .arg(mSelectedPath);
}

QString SshDialog::localPath() const {
  QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                 + "/ssh-repos/" + mHost + "/" + QString::number(mPort);

  // Turn /home/stefan/project → base/home/stefan/project
  QString rel = mSelectedPath;
  if (rel.startsWith('/')) rel = rel.mid(1);

  return base + "/" + rel;
}
