Changelog
=========

TL;DR
-----

This fork adds a full AI-powered development assistant to Gittyup: code review
with intelligent caching, commit message generation, per-hunk explanations, an
interactive chat panel, codebase-aware context injection (RAG), background repo
analysis, pattern-matching auto-fix recipes, and SSH repository cloning. Backed
by [Ollama](https://ollama.com) (local/LAN) or Anthropic Claude API.

Unreleased
----------

### AI Code Review

- Review staged or unstaged diffs via the **Review Code** button. Findings are
  displayed with severity indicators (CRITICAL / HIGH / MEDIUM / LOW), file
  references, and suggested fixes.
- Right-click any commit in the history list to review that specific diff.
- 3-step caching pipeline to avoid redundant API calls:
  1. Exact diff hash lookup (~0 ms)
  2. Semantic similarity search via embeddings (~100 ms)
  3. Full LLM review — only when no cache match is found
- Browse all past reviews for a repository via **Review History**.

### Commit Message Generation

- **Generate Message** button sends the current diff to the LLM and populates
  the commit message field using conventional commits format.

### Per-Hunk Explanations

- **Explain** button on each diff hunk header. The LLM response is rendered
  inline below the diff with basic markdown formatting.

### Chat Panel

- Interactive chat sidebar docked in the repo view. Toggle with
  `Ctrl+Shift+C`, the **Chat** button, or the View menu.

### Knowledge Base

- SQLite-backed local cache (`knowledge.db`) stores structured review findings
  with categories, severities, code patterns, fix templates, and embedding
  vectors. WAL mode enabled for crash safety.
- Finding parser extracts structured data from AI responses (category, severity,
  file, line, language, code pattern, fix suggestion). Falls back to heuristic
  parsing when the model doesn't follow the structured format.
- Embedding client calls Ollama `/api/embeddings` (default model:
  `nomic-embed-text`) for vector computation. Cosine similarity search via a
  header-only vector math implementation with no external dependencies.
- Cache hit rate tracking with statistics in Settings.

### Codebase RAG Index

- Scans all tracked repositories, collects source files (30+ extensions),
  chunks them into ~50-line overlapping segments, embeds each chunk, and stores
  vectors in a separate SQLite database (`codebase_index.db`).
- When a review is triggered, the top-5 most similar chunks are injected as
  context in the LLM prompt for deeper codebase understanding.
- Incremental indexing — only changed files (by git blob SHA) are re-indexed.
- Settings UI with **Index All Repos Now**, **Clear Index**, and live stats.

### Background Repo Analysis

- Continuously monitors recently-opened repositories for HEAD changes and
  automatically runs deep analysis with structured findings and FIND/REPLACE
  fix sections.
- Configurable interval (5–60 minutes) and enable/disable toggle in Settings.

### Fix Recipes

- Pattern-matching auto-fix without LLM calls. FIND/REPLACE pairs with file
  glob patterns are extracted from review responses and stored in the knowledge
  base. Known patterns can be applied directly on future diffs.

### SSH Repository Access

- **Open SSH Repository** action in the sidebar. Dialog with SSH URL input,
  connection testing, key selection, and background cloning.
- SSH repos displayed with a terminal icon to distinguish them from local
  repositories.

### Settings — AI Panel

- Full configuration panel in Preferences:
  - Provider selection (Anthropic Claude / Ollama)
  - API key, server URL, and model fields per provider
  - Knowledge Base controls (enable, similarity threshold, embedding model,
    stats, clear)
  - Continuous Analysis controls (enable, interval, analyze now, progress)
  - Codebase Index controls (stats, index now, clear)
  - Scrollable layout to accommodate all sections

### UI Integration

- **Generate Message**, **Review Code**, **Review History**, and **Chat**
  buttons added to the commit editor toolbar.
- View menu toggle **Show/Hide Chat** with `Ctrl+Shift+C` shortcut.
- Repo view splitter updated from 2-pane to 3-pane (detail + chat + log)
  with animated show/hide transitions.

### Build System

- Added `Sql` to `QT_MODULES` in root CMakeLists for SQLite support.
- New `src/ai/` library (8 source files, links to `conf`, `Qt6::Core`,
  `Qt6::Network`, `Qt6::Sql`).
- New UI sources: `ChatPanel`, `CodeReviewDialog`, `SpinnerButton`,
  `SshDialog`.
