#ifndef SSHDIALOG_H
#define SSHDIALOG_H

#include <QDialog>
#include <QThread>
#include <libssh2.h>
#include <libssh2_sftp.h>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// ---------------------------------------------------------------------------
// SshEntry — data for one remote filesystem entry
// ---------------------------------------------------------------------------
struct SshEntry {
  QString name;
  bool isGitRepo = false;
  bool hasSubdirs = false;
};

// ---------------------------------------------------------------------------
// SshWorker — SFTP operations on a background thread
// ---------------------------------------------------------------------------
class SshWorker : public QObject {
  Q_OBJECT
public:
  explicit SshWorker(QObject *parent = nullptr);
  ~SshWorker() override;

public slots:
  void connectToServer(const QString &host, quint16 port, const QString &user,
                       const QString &keyPath);
  void listDir(const QString &path);
  void cleanup();

signals:
  void connected(const QString &homeDir);
  void error(const QString &msg);
  void dirListed(const QString &path, QList<SshEntry> entries);

private:
  bool statPath(const QString &path);
  bool authWithAgent(const QString &user);
  bool authWithKey(const QString &user, const QString &keyPath);

  int mSock = -1;
  LIBSSH2_SESSION *mSession = nullptr;
  LIBSSH2_SFTP *mSftp = nullptr;
};

// ---------------------------------------------------------------------------
// SshDialog — browse a remote SSH server and open a git repo
// ---------------------------------------------------------------------------
class SshDialog : public QDialog {
  Q_OBJECT
public:
  explicit SshDialog(QWidget *parent = nullptr);
  ~SshDialog() override;

  // Set after accept() — used by caller to clone/open
  QString sshUrl() const;    // ssh://user@host:port/path
  QString localPath() const; // local clone destination

signals:
  void doConnect(const QString &host, quint16 port, const QString &user,
                 const QString &keyPath);
  void doListDir(const QString &path);

private slots:
  void onConnectClicked();
  void onConnected(const QString &homeDir);
  void onError(const QString &msg);
  void onDirListed(const QString &path, QList<SshEntry> entries);
  void onItemExpanded(QTreeWidgetItem *item);
  void onSelectionChanged();
  void onOpenClicked();

private:
  void saveSettings();
  void loadSettings();
  void setStatus(const QString &msg, bool isError = false);

  QTreeWidgetItem *addEntry(QTreeWidgetItem *parent, const SshEntry &e,
                            const QString &parentPath);

  QLineEdit *mHostEdit;
  QLineEdit *mUserEdit;
  QLineEdit *mPortEdit;
  QLineEdit *mKeyEdit;
  QPushButton *mConnectBtn;
  QTreeWidget *mTree;
  QPushButton *mOpenBtn;
  QLabel *mStatusLabel;

  QString mHost;
  QString mUser;
  quint16 mPort = 22;
  QString mSelectedPath;

  QThread *mThread;
  SshWorker *mWorker;
};

#endif // SSHDIALOG_H
