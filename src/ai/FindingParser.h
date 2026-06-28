#ifndef FINDINGPARSER_H
#define FINDINGPARSER_H

#include "KnowledgeBase.h"
#include <QList>
#include <QString>

class FindingParser {
public:
  struct FixBlock {
    QString file;
    QString find;
    QString replace;
  };

  static QList<KnowledgeBase::Finding> parse(const QString &reviewText);

  static QList<FixBlock> parseFixBlocks(const QString &reviewText);

  static QString buildCategorizationPrompt(
      const QList<KnowledgeBase::Finding> &findings);

  static QList<KnowledgeBase::Finding> applyCategorization(
      const QList<KnowledgeBase::Finding> &findings,
      const QString &categorizationResponse);

  static QString composeReviewFromFindings(
      const QList<KnowledgeBase::Finding> &findings);
};

#endif // FINDINGPARSER_H
