#include "CodeReviewDialog.h"
#include "ai/AiService.h"
#include "app/Application.h"
#include "app/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTextBrowser>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// Markdown-aware HTML renderer for AI review output
// ---------------------------------------------------------------------------

// Detect severity keyword anywhere in a string
static QString detectSeverity(const QString &s) {
  static QRegularExpression re(R"(\b(CRITICAL|HIGH|MEDIUM|LOW)\b)");
  QRegularExpressionMatch m = re.match(s);
  return m.hasMatch() ? m.captured(1) : QString();
}

// Strip markdown decoration from a string (for header text)
static QString stripMd(const QString &s) {
  QString r = s;
  r.remove(QRegularExpression(R"(\*{1,3})"));
  r.remove(QRegularExpression(R"(_{1,3})"));
  r.remove(QRegularExpression(R"(^#+\s*)"));
  return r.trimmed();
}

// Process inline markdown → HTML
static QString mdInline(const QString &raw, const QString &codeColor,
                         const QString &codeBg) {
  // Escape HTML first, then re-apply markdown sequences
  // We work with the raw string and escape per-segment to avoid double-escaping
  QString out;
  // Simple state machine for `code`, **bold**, *italic*
  int i = 0;
  int len = raw.length();
  while (i < len) {
    // Inline code: `...`
    if (raw[i] == '`') {
      int end = raw.indexOf('`', i + 1);
      if (end > i) {
        QString code = raw.mid(i + 1, end - i - 1).toHtmlEscaped();
        out += QString("<code style='color:%1; background:%2; padding:1px 4px;"
                       " border-radius:2px; font-family:monospace;'>%3</code>")
                   .arg(codeColor, codeBg, code);
        i = end + 1;
        continue;
      }
    }
    // Bold+italic: ***
    if (i + 2 < len && raw[i] == '*' && raw[i+1] == '*' && raw[i+2] == '*') {
      int end = raw.indexOf("***", i + 3);
      if (end > i) {
        out += "<strong><em>" + mdInline(raw.mid(i + 3, end - i - 3), codeColor, codeBg) + "</em></strong>";
        i = end + 3; continue;
      }
    }
    // Bold: **
    if (i + 1 < len && raw[i] == '*' && raw[i+1] == '*') {
      int end = raw.indexOf("**", i + 2);
      if (end > i) {
        out += "<strong>" + mdInline(raw.mid(i + 2, end - i - 2), codeColor, codeBg) + "</strong>";
        i = end + 2; continue;
      }
    }
    // Italic: *
    if (raw[i] == '*') {
      int end = raw.indexOf('*', i + 1);
      if (end > i) {
        out += "<em>" + mdInline(raw.mid(i + 1, end - i - 1), codeColor, codeBg) + "</em>";
        i = end + 1; continue;
      }
    }
    // Bold: __
    if (i + 1 < len && raw[i] == '_' && raw[i+1] == '_') {
      int end = raw.indexOf("__", i + 2);
      if (end > i) {
        out += "<strong>" + mdInline(raw.mid(i + 2, end - i - 2), codeColor, codeBg) + "</strong>";
        i = end + 2; continue;
      }
    }
    // Normal char
    out += QString(raw[i]).toHtmlEscaped();
    ++i;
  }
  return out;
}

// Render the stored diff bytes as HTML using diff-view colors
static QString renderDiffSection(const QByteArray &rawDiff) {
  if (rawDiff.isEmpty()) return QString();

  auto *theme = Application::theme();
  QString addBg   = theme->diff(Theme::Diff::Addition).name();
  QString delBg   = theme->diff(Theme::Diff::Deletion).name();
  QString plusCol = theme->diff(Theme::Diff::Plus).name();
  QString minCol  = theme->diff(Theme::Diff::Minus).name();

  QPalette pal2;
  bool dark    = pal2.color(QPalette::Text).lightnessF() > pal2.color(QPalette::Base).lightnessF();
  QString hdrBg = dark ? "#252535" : "#EAEAF4";
  QString hdrFg = dark ? "#8090B8" : "#505080";
  QString fileBg = dark ? "#1C1C2E" : "#F0F0F8";
  QString fileFg = dark ? "#B0B8D8" : "#303060";

  QString html = QString("<pre style='font-family:monospace; font-size:11px;"
                         " margin:0; padding:0; line-height:1.35;'>");

  for (const QString &line : QString::fromUtf8(rawDiff).split('\n')) {
    if (line.isEmpty()) continue;
    if (line.startsWith("diff --git") || line.startsWith("index ") ||
        line.startsWith("--- ") || line.startsWith("+++ ")) {
      // File header lines
      html += QString("<div style='background:%1; color:%2; padding:1px 8px;'>%3</div>")
                  .arg(fileBg, fileFg, line.toHtmlEscaped());
    } else if (line.startsWith("@@")) {
      html += QString("<div style='background:%1; color:%2; padding:2px 8px; font-weight:600;'>%3</div>")
                  .arg(hdrBg, hdrFg, line.toHtmlEscaped());
    } else if (line.startsWith('+')) {
      html += QString("<div style='background:%1; padding:0 8px;'>"
                      "<span style='color:%2;'>+</span>%3</div>")
                  .arg(addBg, plusCol, line.mid(1).toHtmlEscaped());
    } else if (line.startsWith('-')) {
      html += QString("<div style='background:%1; padding:0 8px;'>"
                      "<span style='color:%2;'>-</span>%3</div>")
                  .arg(delBg, minCol, line.mid(1).toHtmlEscaped());
    } else {
      html += QString("<div style='padding:0 8px;'> %1</div>").arg(line.mid(1).toHtmlEscaped());
    }
  }
  html += "</pre>";
  return html;
}

static QString renderReviewHtml(const QString &text, const QByteArray &diff = {}) {
  auto *theme = Application::theme();

  // Use same formula as Theme.cpp for dark-mode detection
  QPalette pal;
  bool dark = pal.color(QPalette::Text).lightnessF() > pal.color(QPalette::Base).lightnessF();

  // Derive colors from Theme (same source as diff section)
  QString addBg  = theme->diff(Theme::Diff::Addition).name();
  QString delBg  = theme->diff(Theme::Diff::Deletion).name();
  QString plusFg = theme->diff(Theme::Diff::Plus).name();
  QString minFg  = theme->diff(Theme::Diff::Minus).name();
  QString warnFg = theme->diff(Theme::Diff::Warning).name();
  QString noteFg = theme->diff(Theme::Diff::Note).name();

  struct Sev { QString border, bg, badge; };
  QMap<QString, Sev> sevMap;
  if (dark) {
    sevMap["CRITICAL"] = {minFg,  "#2A1010", minFg};
    sevMap["HIGH"]     = {warnFg, "#221400", warnFg};
    sevMap["MEDIUM"]   = {"#A09030", "#202000", "#D0B840"};
    sevMap["LOW"]      = {plusFg,  "#0E1A10", plusFg};
  } else {
    sevMap["CRITICAL"] = {minFg,  delBg,  minFg};
    sevMap["HIGH"]     = {warnFg, "#FFF0D8", warnFg};
    sevMap["MEDIUM"]   = {"#907800", "#FFFAD0", "#706000"};
    sevMap["LOW"]      = {plusFg,  addBg,  plusFg};
  }

  QString bgDoc    = pal.color(QPalette::Base).name();
  QString fgDoc    = pal.color(QPalette::Text).name();
  QString mutedCol = dark ? "#808080" : "#606060";
  QString codeCol  = dark ? "#C0D8F0" : "#1E5B9B";
  QString codeBg   = dark ? "#1A2535" : "#EEF3F8";
  QString divCol   = dark ? "#303040" : "#D0D0D8";
  QString labelCol = dark ? "#6080A0" : "#406080";
  QString fixBg    = dark ? "#0E1A10" : addBg;

  // ── Parser — handles all known AI output formats ─────────────────────────
  struct Section {
    QString severity;
    QString title;
    QStringList lines;
  };

  static QRegularExpression headRe(R"(^(#{1,4})\s*(.*))");
  static QRegularExpression numberedRe(R"(^\d+[\.\)]\s+(.*))");
  static QRegularExpression severityLineRe(R"(^[Ss]everity\s*:\s*(CRITICAL|HIGH|MEDIUM|LOW)\b)");
  static QRegularExpression titleSevRe(R"(^[^\-\*\>].{0,100}\b(CRITICAL|HIGH|MEDIUM|LOW)\b.{0,40}$)");

  QList<Section> sections;
  Section cur;

  auto cleanTitle = [](QString t) -> QString {
    t = stripMd(t);
    t.remove(QRegularExpression(R"(\s*\(\s*(CRITICAL|HIGH|MEDIUM|LOW)\s*\)\s*)"));
    t.remove(QRegularExpression(R"(\b(CRITICAL|HIGH|MEDIUM|LOW)\b[\s:–—\-]*)"));
    t.remove(QRegularExpression(R"([\(\)\-:]+$)"));
    return t.trimmed();
  };

  auto flush = [&] {
    if (!cur.severity.isEmpty() || !cur.title.isEmpty() ||
        !cur.lines.join("").trimmed().isEmpty())
      sections << cur;
    cur = Section{};
  };

  for (const QString &line : text.split('\n')) {
    QString trimmed = line.trimmed();

    // Numbered item: "1. Title" or "1) Title" → new section
    QRegularExpressionMatch nm = numberedRe.match(trimmed);
    if (nm.hasMatch()) {
      flush();
      cur.title = cleanTitle(nm.captured(1));
      cur.severity = detectSeverity(cur.title);
      if (!cur.severity.isEmpty()) {
        cur.title.remove(QRegularExpression(R"(\b(CRITICAL|HIGH|MEDIUM|LOW)\b[\s:–—\-]*)"));
        cur.title = cur.title.trimmed();
      }
      continue;
    }

    // "Severity: CRITICAL" on its own line → sets severity for current section
    QRegularExpressionMatch sm = severityLineRe.match(trimmed);
    if (sm.hasMatch()) {
      cur.severity = sm.captured(1);
      continue; // don't add this line to body
    }

    // Markdown heading
    QRegularExpressionMatch hm = headRe.match(line);
    if (hm.hasMatch()) {
      flush();
      cur.title    = cleanTitle(hm.captured(2));
      cur.severity = detectSeverity(hm.captured(2));
      if (!cur.severity.isEmpty())
        cur.title.remove(QRegularExpression(R"(\b(CRITICAL|HIGH|MEDIUM|LOW)\b[\s:–—\-]*)"));
      cur.title = cur.title.trimmed();
      continue;
    }

    // Horizontal rule → section break
    if (trimmed == "---" || trimmed == "***") { flush(); continue; }

    // Plain title line with severity anywhere (e.g. "Security Vulnerability (HIGH)")
    if (!trimmed.isEmpty() && !trimmed.startsWith('-') && !trimmed.startsWith('*') &&
        !trimmed.startsWith('>') && !trimmed.startsWith(' ') &&
        titleSevRe.match(trimmed).hasMatch() && cur.lines.isEmpty()) {
      flush();
      cur.severity = detectSeverity(trimmed);
      cur.title    = cleanTitle(trimmed);
      continue;
    }

    cur.lines << line;
  }
  flush();

  // ── Body renderer ─────────────────────────────────────────────────────────
  // Known label prefixes that get visual treatment
  static QRegularExpression labelRe(R"(^(File|Problem|Fix|Suggested fix|Suggestion|Recommendation|Note)\s*:(.*))",
                                     QRegularExpression::CaseInsensitiveOption);

  auto renderBody = [&](const QStringList &lines) -> QString {
    QString out;
    bool inList = false, inCode = false;

    auto closeList = [&] { if (inList) { out += "</ul>"; inList = false; } };

    for (const QString &raw : lines) {
      if (raw.trimmed().startsWith("```")) {
        closeList();
        if (!inCode) {
          out += QString("<pre style='background:%1; border-left:3px solid %2;"
                         " margin:4px 0; padding:4px 10px; border-radius:0 3px 3px 0;"
                         " font-family:monospace; font-size:12px; line-height:1.35;'>")
                     .arg(codeBg, codeCol);
          inCode = true;
        } else { out += "</pre>"; inCode = false; }
        continue;
      }
      if (inCode) { out += raw.toHtmlEscaped() + "\n"; continue; }

      QString t = raw.trimmed();
      if (t.isEmpty()) { closeList(); out += "<div style='height:4px;'></div>"; continue; }

      if (t.startsWith("- ") || t.startsWith("* ")) {
        if (!inList) { out += "<ul style='margin:3px 0; padding-left:18px;'>"; inList = true; }
        out += "<li style='margin:1px 0; line-height:1.5;'>" +
               mdInline(t.mid(2), codeCol, codeBg) + "</li>";
        continue;
      }
      closeList();

      // "File: ...", "Problem: ...", "Fix: ..." get label styling
      QRegularExpressionMatch lm = labelRe.match(t);
      if (lm.hasMatch()) {
        QString label = lm.captured(1);
        QString rest  = lm.captured(2).trimmed();
        bool isFix = label.compare("Fix", Qt::CaseInsensitive) == 0 ||
                     label.startsWith("Suggest", Qt::CaseInsensitive) ||
                     label.startsWith("Recommend", Qt::CaseInsensitive);
        QString bg = isFix ? fixBg : QString();
        QString row = QString("<div style='padding:2px 0%1;'>")
                          .arg(bg.isEmpty() ? "" : "; background:" + bg + "; margin:2px -4px; padding:2px 4px; border-radius:2px");
        row += QString("<span style='font-weight:600; color:%1;'>%2:</span> ")
                   .arg(labelCol, label.toHtmlEscaped());
        row += mdInline(rest, codeCol, codeBg);
        row += "</div>";
        out += row;
        continue;
      }

      out += "<p style='margin:2px 0; line-height:1.5;'>" +
             mdInline(t, codeCol, codeBg) + "</p>";
    }
    closeList();
    if (inCode) out += "</pre>";
    return out;
  };

  // ── Build HTML ────────────────────────────────────────────────────────────
  QString html = QString(
      "<html><body style='background:%1; color:%2;"
      " font-family:system-ui,sans-serif; font-size:13px; margin:0; padding:0;'>")
      .arg(bgDoc, fgDoc);

  for (const Section &sec : sections) {
    if (sec.severity.isEmpty()) {
      if (!sec.title.isEmpty())
        html += QString("<p style='margin:8px 14px 3px; font-weight:600; color:%1;'>%2</p>")
                    .arg(fgDoc, sec.title.toHtmlEscaped());
      html += "<div style='margin:0 14px;'>" + renderBody(sec.lines) + "</div>";
      continue;
    }

    const Sev &s = sevMap.contains(sec.severity) ? sevMap[sec.severity] : sevMap["LOW"];

    html += QString("<div style='margin:8px 0 0; border-left:4px solid %1; background:%2;'>")
                .arg(s.border, s.bg);
    html += "<div style='padding:6px 12px 2px;'>";
    html += QString("<span style='font-weight:700; font-size:11px; letter-spacing:0.8px; color:%1;'>%2</span>")
                .arg(s.badge, sec.severity);
    if (!sec.title.isEmpty())
      html += QString(" <span style='font-weight:500; color:%1;'>&mdash; %2</span>")
                  .arg(fgDoc, sec.title.toHtmlEscaped());
    html += "</div>";
    if (!sec.lines.isEmpty())
      html += "<div style='padding:2px 12px 8px;'>" + renderBody(sec.lines) + "</div>";
    html += "</div>";
  }

  // ── Diff section ──────────────────────────────────────────────────────────
  if (!diff.isEmpty()) {
    html += QString("<div style='height:1px; background:%1; margin:8px 0 0;'></div>").arg(divCol);
    html += QString("<div style='padding:6px 12px 3px; font-weight:600; font-size:11px;"
                    " letter-spacing:0.6px; color:%1;'>DIFF</div>").arg(mutedCol);
    html += renderDiffSection(diff);
  }

  html += "</body></html>";
  return html;
}


static QString renderFixLogHtml(const QList<AiReviewStore::FixRecord> &fixes) {
  QPalette pal3;
  bool dark = pal3.color(QPalette::Text).lightnessF() > pal3.color(QPalette::Base).lightnessF();
  auto *theme = Application::theme();
  QString okColor  = theme->diff(Theme::Diff::Plus).name();
  QString errColor = theme->diff(Theme::Diff::Minus).name();
  QString bgColor  = qApp->palette().base().color().name();
  QString textColor = qApp->palette().text().color().name();

  QString html = QString("<html><body style='background:%1; color:%2; "
                         "font-family:monospace; font-size:12px; margin:4px;'>")
                     .arg(bgColor, textColor);

  for (const AiReviewStore::FixRecord &r : fixes) {
    QString col = r.success ? okColor : errColor;
    QString mark = r.success ? "+" : "-";
    html += QString("<div style='color:%1;'>[%2] %3 &mdash; %4</div>")
                .arg(col, mark, r.file.toHtmlEscaped(), r.message.toHtmlEscaped());
  }

  html += "</body></html>";
  return html;
}

void CodeReviewDialog::initUi() {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Code Review"));
  resize(720, 560);

  mHistoryBox = new QComboBox(this);
  mHistoryBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(mHistoryBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &CodeReviewDialog::onHistoryChanged);

  mReviewText = new QTextBrowser(this);
  mReviewText->setOpenLinks(false);

  mFixLog = new QTextBrowser(this);
  mFixLog->setMaximumHeight(100);
  mFixLog->setVisible(false);

  mStatus = new QLabel(this);
  mStatus->setWordWrap(true);

  mFixBtn = new QPushButton(tr("Fix Issues"), this);
  mFixBtn->setToolTip(tr("Ask AI to generate and apply fixes for the issues above"));
  connect(mFixBtn, &QPushButton::clicked, this, &CodeReviewDialog::onFixIssues);

  mRerunBtn = new QPushButton(tr("Re-run"), this);
  mRerunBtn->setToolTip(tr("Re-run code review on the stored diff"));
  connect(mRerunBtn, &QPushButton::clicked, this, &CodeReviewDialog::onRerun);

  QPushButton *closeBtn = new QPushButton(tr("Close"), this);
  connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

  QHBoxLayout *btnRow = new QHBoxLayout;
  btnRow->addWidget(mStatus, 1);
  btnRow->addWidget(mRerunBtn);
  btnRow->addWidget(mFixBtn);
  btnRow->addWidget(closeBtn);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addWidget(mHistoryBox);
  layout->addWidget(mReviewText, 1);
  layout->addWidget(mFixLog);
  layout->addLayout(btnRow);
}

// New review — save to store then show
CodeReviewDialog::CodeReviewDialog(const QString &reviewText,
                                   const QByteArray &diff,
                                   const QString &headSha,
                                   const git::Repository &repo,
                                   QWidget *parent)
    : QDialog(parent), mRepo(repo), mCurrentDiff(diff) {
  initUi();

  AiService::Config cfg = AiService::instance()->currentConfig();
  mCurrentId = AiReviewStore::instance()->save(
      mRepo.workdir().path(), headSha, cfg.provider, cfg.model, diff,
      reviewText);

  loadHistory();
  mReviewText->setHtml(renderReviewHtml(reviewText, diff));
}

// History-only — open existing reviews without running a new one
CodeReviewDialog::CodeReviewDialog(const git::Repository &repo, QWidget *parent)
    : QDialog(parent), mRepo(repo) {
  initUi();
  loadHistory();

  if (!mHistory.isEmpty()) {
    // Show the most recent entry
    const AiReviewStore::Entry &latest = mHistory.first();
    mCurrentId = latest.id;
    mCurrentDiff = AiReviewStore::instance()->diff(latest.id);
    mReviewText->setHtml(renderReviewHtml(latest.reviewText, mCurrentDiff));
    updateFixLog(latest.fixes);
  } else {
    mReviewText->setPlaceholderText(
        tr("No reviews yet for this repository. Use 'Review Code' to run one."));
    mFixBtn->setEnabled(false);
    mRerunBtn->setEnabled(false);
  }
}

// ---------------------------------------------------------------------------
// History support
// ---------------------------------------------------------------------------
void CodeReviewDialog::loadHistory() {
  mHistory = AiReviewStore::instance()->listForRepo(mRepo.workdir().path());

  mHistoryBox->blockSignals(true);
  mHistoryBox->clear();
  for (const auto &e : mHistory) {
    QString label =
        e.timestamp.toString("yyyy-MM-dd hh:mm") + " — " + e.model;
    mHistoryBox->addItem(label, e.id);
  }
  // Select the current entry
  for (int i = 0; i < mHistory.size(); ++i) {
    if (mHistory[i].id == mCurrentId) {
      mHistoryBox->setCurrentIndex(i);
      break;
    }
  }
  mHistoryBox->blockSignals(false);
}

void CodeReviewDialog::onHistoryChanged(int index) {
  if (index < 0 || index >= mHistory.size())
    return;
  const auto &entry = mHistory[index];
  QByteArray diff = AiReviewStore::instance()->diff(entry.id);
  showEntry(entry, diff);
}

void CodeReviewDialog::showEntry(const AiReviewStore::Entry &entry,
                                 const QByteArray &diff) {
  mCurrentId = entry.id;
  mCurrentDiff = diff;
  mReviewText->setHtml(renderReviewHtml(entry.reviewText, diff));
  updateFixLog(entry.fixes);
}

void CodeReviewDialog::updateFixLog(
    const QList<AiReviewStore::FixRecord> &fixes) {
  if (fixes.isEmpty()) {
    mFixLog->setVisible(false);
    return;
  }
  mFixLog->setVisible(true);
  mFixLog->setHtml(renderFixLogHtml(fixes));
}

// ---------------------------------------------------------------------------
// Re-run review on the stored diff
// ---------------------------------------------------------------------------
void CodeReviewDialog::onRerun() {
  if (mCurrentDiff.isEmpty()) {
    mStatus->setText(tr("No diff available to re-run."));
    return;
  }

  mRerunBtn->setEnabled(false);
  mRerunBtn->setText(tr("Reviewing…"));

  QString prompt =
      QStringLiteral(
          "Review the following git diff for bugs, security vulnerabilities, "
          "and code quality issues.\n"
          "For each issue found:\n"
          "- State severity: CRITICAL / HIGH / MEDIUM / LOW\n"
          "- Identify the file and approximate line\n"
          "- Explain the problem concisely\n"
          "- Suggest a fix\n\n"
          "If no issues are found, say so clearly.\n\n") +
      QString::fromUtf8(mCurrentDiff);

  AiService::instance()->chat(prompt, 1024,
      [this](const QString &text, const QString &error) {
    mRerunBtn->setEnabled(true);
    mRerunBtn->setText(tr("Re-run"));

    if (!error.isEmpty()) {
      mStatus->setText(tr("Request failed: %1").arg(error));
      return;
    }

    AiService::Config cfg = AiService::instance()->currentConfig();
    mCurrentId = AiReviewStore::instance()->save(
        mRepo.workdir().path(), QString(), cfg.provider, cfg.model,
        mCurrentDiff, text);

    mReviewText->setHtml(renderReviewHtml(text, mCurrentDiff));
    loadHistory();
  });
}

// ---------------------------------------------------------------------------
// Parse AI fix response into FILE/FIND/REPLACE blocks
// ---------------------------------------------------------------------------
QList<CodeReviewDialog::Fix>
CodeReviewDialog::parseFixes(const QString &response) const {
  QList<Fix> fixes;

  QStringList blocks = response.split(QRegularExpression(R"(\n---+\n?)"),
                                      Qt::SkipEmptyParts);
  for (QString block : blocks) {
    block = block.trimmed();
    if (block.isEmpty())
      continue;

    Fix fix;
    QStringList lines = block.split('\n');
    QString section;
    QStringList findLines, replaceLines;

    for (const QString &line : lines) {
      if (line.startsWith("FILE:")) {
        fix.file = line.mid(5).trimmed();
      } else if (line.trimmed() == "FIND:") {
        section = "find";
      } else if (line.trimmed() == "REPLACE:") {
        section = "replace";
      } else {
        if (section == "find")
          findLines << line;
        else if (section == "replace")
          replaceLines << line;
      }
    }

    fix.find = findLines.join('\n');
    fix.replace = replaceLines.join('\n');

    if (!fix.file.isEmpty() && !fix.find.isEmpty())
      fixes.append(fix);
  }
  return fixes;
}

// ---------------------------------------------------------------------------
// Apply fixes to actual files in the working directory
// ---------------------------------------------------------------------------
void CodeReviewDialog::applyFixes(const QList<Fix> &fixes) {
  if (fixes.isEmpty()) {
    mStatus->setText(tr("No actionable fixes found in AI response."));
    mFixBtn->setEnabled(true);
    mFixBtn->setText(tr("Fix Issues"));
    return;
  }

  QString workdir = mRepo.workdir().path();
  int applied = 0, failed = 0;
  QStringList failedFiles;
  QList<AiReviewStore::FixRecord> records;

  for (const Fix &fix : fixes) {
    QString rel = fix.file;
    if (rel.startsWith("a/") || rel.startsWith("b/"))
      rel = rel.mid(2);

    QString fullPath = workdir + "/" + rel;
    QFile file(fullPath);

    AiReviewStore::FixRecord rec;
    rec.file = rel;

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      rec.message = "not found";
      failedFiles << rel + " (not found)";
      ++failed;
      records << rec;
      continue;
    }
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    if (!content.contains(fix.find)) {
      rec.message = "text not found";
      failedFiles << rel + " (text not found)";
      ++failed;
      records << rec;
      continue;
    }

    content.replace(fix.find, fix.replace);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate |
                   QIODevice::Text)) {
      rec.message = "write failed";
      failedFiles << rel + " (write failed)";
      ++failed;
      records << rec;
      continue;
    }
    file.write(content.toUtf8());
    file.close();
    rec.success = true;
    rec.message = "applied";
    records << rec;
    ++applied;
  }

  // Persist fix records
  AiReviewStore::instance()->addFixes(mCurrentId, records);
  updateFixLog(records);

  QString summary = tr("Applied %1 fix(es).").arg(applied);
  if (failed > 0)
    summary += "\n" +
               tr("Failed to apply %1 fix(es): %2")
                   .arg(failed)
                   .arg(failedFiles.join(", "));
  mStatus->setText(summary);
  mFixBtn->setEnabled(true);
  mFixBtn->setText(tr("Fix Issues"));

  if (applied > 0) {
    QMessageBox::information(
        this, tr("Fixes Applied"),
        tr("%1 fix(es) applied. Check the diff view for changes.")
            .arg(applied));
  }
}

// ---------------------------------------------------------------------------
// Main "Fix Issues" action
// ---------------------------------------------------------------------------
void CodeReviewDialog::onFixIssues() {
  mFixBtn->setEnabled(false);
  mFixBtn->setText(tr("Fixing…"));
  mStatus->setText(tr("Sending diff to AI for fixes…"));

  QString review = mReviewText->toPlainText();
  QString diff = QString::fromUtf8(mCurrentDiff);

  QString prompt =
      QStringLiteral("Output ONLY fix blocks in the exact format below. "
                     "No explanations, no markdown, no prose — just the blocks.\n\n"
                     "FORMAT (repeat for each fix):\n"
                     "FILE: relative/path/to/file.cpp\n"
                     "FIND:\n"
                     "exact original lines to replace\n"
                     "REPLACE:\n"
                     "corrected lines\n"
                     "---\n\n"
                     "Issues to fix (from diff review):\n\n") +
      review +
      QStringLiteral("\n\nOriginal diff:\n\n") + diff +
      QStringLiteral("\n\nRemember: output ONLY the FILE/FIND/REPLACE/--- blocks. "
                     "The FIND text must match exactly what is in the source file.");

  AiService::instance()->chat(prompt, 4096,
      [this](const QString &text, const QString &error) {
        mFixBtn->setEnabled(true);
        mFixBtn->setText(tr("Fix Issues"));

        if (!error.isEmpty()) {
          mStatus->setText(tr("Request failed: %1").arg(error));
          return;
        }

        QList<Fix> fixes = parseFixes(text);
        if (fixes.isEmpty()) {
          // Show first 120 chars of AI response to help diagnose format issues
          QString preview = text.left(120).replace('\n', ' ');
          mStatus->setText(tr("No fixes parsed. AI said: \"%1\"").arg(preview));
          return;
        }

        // ── Fix selection dialog ──────────────────────────────────────────
        auto *theme = Application::theme();
        QPalette selPal;
        bool dark  = selPal.color(QPalette::Text).lightnessF() > selPal.color(QPalette::Base).lightnessF();
        QString addBg  = theme->diff(Theme::Diff::Addition).name();
        QString delBg  = theme->diff(Theme::Diff::Deletion).name();
        QString plusCol = theme->diff(Theme::Diff::Plus).name();
        QString minCol  = theme->diff(Theme::Diff::Minus).name();
        QString docBg  = selPal.color(QPalette::Base).name();
        QString docFg  = selPal.color(QPalette::Text).name();
        QString hdrFg  = dark ? "#8090B8" : "#505080";

        QDialog selDlg(this);
        selDlg.setWindowTitle(tr("Select Fixes to Apply"));
        selDlg.resize(700, 500);

        // One checkbox + mini-diff per fix
        QWidget *container = new QWidget;
        QVBoxLayout *vbox  = new QVBoxLayout(container);
        vbox->setSpacing(10);
        vbox->setContentsMargins(8, 8, 8, 8);

        QList<QCheckBox *> boxes;
        for (int i = 0; i < fixes.size(); ++i) {
          const Fix &f = fixes[i];

          // Checkbox with file label
          QCheckBox *cb = new QCheckBox(
              QString("[%1] %2").arg(i + 1).arg(f.file), container);
          cb->setChecked(true);
          QFont font = cb->font();
          font.setBold(true);
          cb->setFont(font);
          boxes << cb;

          // Mini diff preview
          QString diffHtml = QString(
              "<html><body style='background:%1; color:%2;"
              " font-family:monospace; font-size:11px; margin:0; padding:0;'>")
              .arg(docBg, docFg);
          diffHtml += QString("<pre style='margin:0; padding:0; line-height:1.3;'>");
          for (const QString &line : f.find.split('\n')) {
            diffHtml += QString("<div style='background:%1; padding:0 6px;'>"
                                "<span style='color:%2;'>-</span>%3</div>")
                            .arg(delBg, minCol, line.toHtmlEscaped());
          }
          for (const QString &line : f.replace.split('\n')) {
            diffHtml += QString("<div style='background:%1; padding:0 6px;'>"
                                "<span style='color:%2;'>+</span>%3</div>")
                            .arg(addBg, plusCol, line.toHtmlEscaped());
          }
          diffHtml += "</pre></body></html>";

          QTextBrowser *preview = new QTextBrowser(container);
          preview->setOpenLinks(false);
          preview->setFrameShape(QFrame::NoFrame);
          preview->setHtml(diffHtml);
          int lineCount = f.find.count('\n') + f.replace.count('\n') + 2;
          preview->setFixedHeight(qMin(lineCount * 16 + 8, 160));

          vbox->addWidget(cb);
          vbox->addWidget(preview);
        }
        vbox->addStretch();

        QScrollArea *scroll = new QScrollArea(&selDlg);
        scroll->setWidget(container);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);

        QPushButton *cancelBtn = new QPushButton(tr("Cancel"), &selDlg);
        QPushButton *applyBtn  = new QPushButton(tr("Apply Selected"), &selDlg);
        applyBtn->setDefault(true);

        QHBoxLayout *btnRow = new QHBoxLayout;
        btnRow->setContentsMargins(8, 4, 8, 8);
        btnRow->addStretch();
        btnRow->addWidget(cancelBtn);
        btnRow->addWidget(applyBtn);

        QVBoxLayout *dlgLayout = new QVBoxLayout(&selDlg);
        dlgLayout->setContentsMargins(0, 0, 0, 0);
        dlgLayout->setSpacing(0);
        dlgLayout->addWidget(scroll, 1);
        dlgLayout->addLayout(btnRow);

        connect(cancelBtn, &QPushButton::clicked, &selDlg, &QDialog::reject);
        connect(applyBtn, &QPushButton::clicked, &selDlg, [&] {
          selDlg.accept();
        });

        if (selDlg.exec() == QDialog::Accepted) {
          QList<Fix> selected;
          for (int i = 0; i < boxes.size(); ++i)
            if (boxes[i]->isChecked())
              selected << fixes[i];
          applyFixes(selected);
        }
      });
}
