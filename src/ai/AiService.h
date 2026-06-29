#ifndef AISERVICE_H
#define AISERVICE_H

#include <QObject>
#include <QNetworkAccessManager>
#include <functional>

class AiService : public QObject {
  Q_OBJECT

public:
  static AiService *instance();

  struct Config {
    QString provider;
    QString model;
    QString apiKey;
    QString baseUrl;
  };

  Config currentConfig() const;

  using Callback = std::function<void(const QString &text, const QString &error)>;
  using StreamCallback = std::function<void(const QString &chunk)>;
  using FinishCallback = std::function<void(const QString &fullText, const QString &error)>;

  void chat(const QString &prompt, int maxTokens, Callback callback);
  QNetworkReply *chatStreaming(const QString &prompt, int maxTokens,
                               StreamCallback onChunk, FinishCallback onDone);

  static QString reviewPrompt(const QByteArray &diff,
                              const QString &commitSha = {},
                              const QString &commitMsg = {},
                              const QString &codebaseContext = {});
  static constexpr int ReviewMaxTokens = 4096;

private:
  explicit AiService(QObject *parent = nullptr);

  QNetworkAccessManager mNet;
};

#endif // AISERVICE_H
