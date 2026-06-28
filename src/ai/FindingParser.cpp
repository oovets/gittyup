#include "FindingParser.h"

#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// ---------------------------------------------------------------------------
// Parse structured ISSUE: blocks from AI review text
// ---------------------------------------------------------------------------
QList<KnowledgeBase::Finding> FindingParser::parse(const QString &reviewText) {
  QList<KnowledgeBase::Finding> findings;

  // Try structured format first (ISSUE: blocks separated by ---)
  static const QRegularExpression issueSep(R"((?:^|\n)ISSUE:\s*\n)");
  QStringList blocks = reviewText.split(issueSep, Qt::SkipEmptyParts);

  bool hasStructured = false;

  for (const QString &block : blocks) {
    KnowledgeBase::Finding f;
    bool hasSeverity = false;

    for (const QString &rawLine : block.split('\n')) {
      QString line = rawLine.trimmed();

      if (line.startsWith("SEVERITY:")) {
        f.severity = line.mid(9).trimmed();
        hasSeverity = true;
      } else if (line.startsWith("FILE:")) {
        // stored in description context
      } else if (line.startsWith("CATEGORY:")) {
        f.category = line.mid(9).trimmed().toLower();
      } else if (line.startsWith("LANGUAGE:")) {
        f.language = line.mid(9).trimmed();
      } else if (line.startsWith("PROBLEM:")) {
        f.description = line.mid(8).trimmed();
      } else if (line.startsWith("FIX:")) {
        f.fixTemplate = line.mid(4).trimmed();
      } else if (line.startsWith("CODE_PATTERN:")) {
        // Use code pattern as a secondary description for matching
        QString pattern = line.mid(13).trimmed();
        if (!pattern.isEmpty() && !f.description.isEmpty())
          f.description += " | " + pattern;
        else if (!pattern.isEmpty())
          f.description = pattern;
      }
    }

    if (hasSeverity && !f.description.isEmpty()) {
      hasStructured = true;
      findings.append(f);
    }
  }

  if (hasStructured)
    return findings;

  // Fallback: heuristic parsing for unstructured review text
  static const QRegularExpression severityRe(
      R"((?:^|\n)\s*(?:\d+[\.\)]\s*)?(?:\*{0,2})(CRITICAL|HIGH|MEDIUM|LOW)(?:\*{0,2})\s*[:\-–]?\s*)",
      QRegularExpression::CaseInsensitiveOption);

  auto it = severityRe.globalMatch(reviewText);
  QList<int> positions;
  QStringList severities;

  while (it.hasNext()) {
    auto match = it.next();
    positions.append(match.capturedStart());
    severities.append(match.captured(1).toUpper());
  }

  for (int i = 0; i < positions.size(); ++i) {
    int start = positions[i];
    int end = (i + 1 < positions.size()) ? positions[i + 1] : reviewText.size();
    QString chunk = reviewText.mid(start, end - start).trimmed();

    KnowledgeBase::Finding f;
    f.severity = severities[i];

    // Remove the severity prefix to get the description
    auto sMatch = severityRe.match(chunk);
    if (sMatch.hasMatch())
      chunk = chunk.mid(sMatch.capturedEnd()).trimmed();

    // Take first meaningful line(s) as description
    QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
    if (!lines.isEmpty())
      f.description = lines.first().trimmed();

    if (!f.description.isEmpty())
      findings.append(f);
  }

  return findings;
}

// ---------------------------------------------------------------------------
// Extract FIND/REPLACE fix blocks from AI response
// ---------------------------------------------------------------------------
QList<FindingParser::FixBlock> FindingParser::parseFixBlocks(
    const QString &reviewText) {
  QList<FixBlock> blocks;

  static const QRegularExpression issueSep(R"((?:^|\n)ISSUE:\s*\n)");
  QStringList issueBlocks = reviewText.split(issueSep, Qt::SkipEmptyParts);

  for (const QString &block : issueBlocks) {
    FixBlock fb;

    static const QRegularExpression fileRe(R"(FILE:\s*(.+))");
    auto fileMatch = fileRe.match(block);
    if (fileMatch.hasMatch())
      fb.file = fileMatch.captured(1).trimmed();

    int findIdx = block.indexOf("FIND:");
    int replaceIdx = block.indexOf("REPLACE:");
    if (findIdx < 0 || replaceIdx < 0 || replaceIdx <= findIdx)
      continue;

    QString findText = block.mid(findIdx + 5, replaceIdx - findIdx - 5).trimmed();
    QString replaceText;

    int endIdx = block.indexOf("---", replaceIdx);
    if (endIdx > replaceIdx)
      replaceText = block.mid(replaceIdx + 8, endIdx - replaceIdx - 8).trimmed();
    else
      replaceText = block.mid(replaceIdx + 8).trimmed();

    if (!findText.isEmpty() && !replaceText.isEmpty()) {
      fb.find = findText;
      fb.replace = replaceText;
      blocks.append(fb);
    }
  }

  return blocks;
}

// ---------------------------------------------------------------------------
// Generate a follow-up prompt for categorization
// ---------------------------------------------------------------------------
QString FindingParser::buildCategorizationPrompt(
    const QList<KnowledgeBase::Finding> &findings) {

  QString prompt = QStringLiteral(
      "Categorize each of the following code review findings. "
      "For each finding, output a JSON object on a single line with these fields:\n"
      "  index (0-based), category (one of: null-deref, buffer-overflow, "
      "memory-leak, use-after-free, race-condition, injection, xss, "
      "auth-bypass, type-error, logic-error, resource-leak, uninitialized, "
      "bounds-check, error-handling, style, performance, other), "
      "language (e.g. C++, Python), code_pattern (a generalized pattern "
      "without specific variable names)\n\n"
      "Output ONLY a JSON array. No prose.\n\n"
      "Findings:\n");

  for (int i = 0; i < findings.size(); ++i) {
    prompt += QString("[%1] %2: %3\n")
                  .arg(i)
                  .arg(findings[i].severity, findings[i].description);
  }

  return prompt;
}

// ---------------------------------------------------------------------------
// Apply categorization response to findings
// ---------------------------------------------------------------------------
QList<KnowledgeBase::Finding> FindingParser::applyCategorization(
    const QList<KnowledgeBase::Finding> &findings,
    const QString &categorizationResponse) {

  QList<KnowledgeBase::Finding> result = findings;

  // Extract JSON array from the response (it might have surrounding text)
  int arrStart = categorizationResponse.indexOf('[');
  int arrEnd = categorizationResponse.lastIndexOf(']');
  if (arrStart < 0 || arrEnd <= arrStart)
    return result;

  QByteArray jsonBytes =
      categorizationResponse.mid(arrStart, arrEnd - arrStart + 1).toUtf8();
  QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
  if (!doc.isArray())
    return result;

  QJsonArray arr = doc.array();
  for (const auto &val : arr) {
    QJsonObject obj = val.toObject();
    int idx = obj["index"].toInt(-1);
    if (idx < 0 || idx >= result.size())
      continue;

    QString cat = obj["category"].toString();
    if (!cat.isEmpty())
      result[idx].category = cat;

    QString lang = obj["language"].toString();
    if (!lang.isEmpty())
      result[idx].language = lang;

    QString pattern = obj["code_pattern"].toString();
    if (!pattern.isEmpty() && !result[idx].description.isEmpty())
      result[idx].description += " | " + pattern;
  }

  return result;
}

// ---------------------------------------------------------------------------
// Compose a review response from cached findings
// ---------------------------------------------------------------------------
QString FindingParser::composeReviewFromFindings(
    const QList<KnowledgeBase::Finding> &findings) {
  if (findings.isEmpty())
    return QStringLiteral("No issues found.");

  QString text;
  for (int i = 0; i < findings.size(); ++i) {
    const auto &f = findings[i];
    text += f.severity + ": " + f.description + "\n";
    if (!f.fixTemplate.isEmpty())
      text += "  Suggested fix: " + f.fixTemplate + "\n";
    text += "\n";
  }
  return text.trimmed();
}
