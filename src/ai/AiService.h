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

  void chat(const QString &prompt, int maxTokens, Callback callback);

private:
  explicit AiService(QObject *parent = nullptr);

  QNetworkAccessManager mNet;
};

#endif // AISERVICE_H
