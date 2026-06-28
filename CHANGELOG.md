# Changelog

## TL;DR

Gittyup gains a full AI-powered development assistant: code review with caching, commit message generation, per-hunk explanations, an interactive chat panel, a background repo analyzer that continuously scans for issues, a codebase RAG embedding pipeline that injects relevant context into reviews, pattern-matching auto-fix recipes, SSH repository cloning, and a comprehensive settings UI. Backed by Ollama (local/LAN) or Anthropic Claude API.

---

## Unreleased

### New: AI Code Review System

- **AI-powered code review** via the "Review Code" button in the commit editor. Sends the current staged/unstaged diff to the configured LLM and displays findings in a dedicated dialog with severity indicators (CRITICAL/HIGH/MEDIUM/LOW), file references, and suggested fixes. (`src/ui/CommitEditor.cpp`, `src/ui/CodeReviewDialog.cpp`)
- **Commit-specific review** via right-click context menu on any commit in the history list. Reviews the selected commit's diff with full codebase context. (`src/ui/CommitList.cpp`)
- **3-step matching pipeline** reduces redundant API calls:
  1. Exact diff hash lookup (~0ms) — SHA-256 match against cached reviews.
  2. Semantic similarity search (~100ms) — Ollama embeddings + cosine similarity against stored findings.
  3. Full AI review — only if no cache match; results are parsed, embedded, and stored for future reuse.
- **Review history browser** via the "Review History" button. Browse, search, and re-read all past reviews for the current repository. (`src/ui/CodeReviewDialog.cpp`)

### New: AI Commit Message Generation

- **"Generate Message" button** in the commit editor. Sends the current diff to the LLM and populates the commit message field with a conventional-commits-formatted message. (`src/ui/CommitEditor.cpp`)

### New: Per-Hunk AI Explanations

- **"Explain" button** on each diff hunk header. Sends the hunk to the LLM and renders the explanation inline below the diff with markdown formatting (bold, italic, inline code). (`src/ui/DiffView/HunkWidget.cpp`)

### New: AI Chat Panel

- **Interactive chat panel** docked as a sidebar in the repo view. Ask questions about the repository's code, diffs, branches, or anything else. Toggle via Ctrl+Shift+C, the "Chat" button, or View menu. (`src/ui/ChatPanel.cpp`, `src/ui/RepoView.cpp`, `src/ui/MenuBar.cpp`)

### New: Knowledge Base (Local Cache)

- **SQLite-backed knowledge base** (`knowledge.db`) stores structured review findings with categories, severities, code patterns, and fix templates. Embeddings stored as raw float BLOBs for fast cosine similarity search. WAL mode enabled. (`src/ai/KnowledgeBase.cpp`)
- **Finding parser** extracts structured findings from AI review responses including category, severity, file, line, language, code pattern, problem description, and fix suggestion. Supports both structured ISSUE block format and heuristic fallback parsing. (`src/ai/FindingParser.cpp`)
- **Embedding client** calls Ollama `/api/embeddings` endpoint (default model: `nomic-embed-text`) for vector computation. (`src/ai/EmbeddingClient.cpp`)
- **Vector math** header-only cosine similarity implementation with no external dependencies. (`src/ai/VectorMath.h`)
- **Cache hit rate tracking** with statistics displayed in the Settings UI.

### New: Codebase RAG Embedding Pipeline

- **CodebaseIndex** scans all tracked repositories, collects source files (C++, Python, JS, TS, Rust, Go, Java, and 20+ other extensions), chunks them into ~50-line overlapping segments, embeds each chunk via Ollama, and stores the vectors in a separate SQLite database (`codebase_index.db`). (`src/ai/CodebaseIndex.cpp`)
- **Automatic context injection** — when a code review is triggered, the system searches the embedding index for the top-5 most similar code chunks to the diff and prepends them as context in the LLM prompt. This gives the AI deeper understanding of the surrounding codebase. (`src/ui/CommitEditor.cpp`, `src/ui/CommitList.cpp`)
- **Incremental indexing** — files are identified by git blob SHA; only changed files are re-indexed. Old chunks are automatically removed before re-embedding.
- **Settings UI** with "Index All Repos Now" button, "Clear Index" button, and live stats (repos/files/chunks). (`src/dialogs/SettingsDialog.cpp`)

### New: Continuous Repo Analysis

- **Background repo analyzer** (singleton) continuously monitors all recently-opened repositories for changes. When a repo's HEAD changes, it automatically runs a deep analysis requesting structured ISSUE blocks with FIND/REPLACE fix sections. (`src/ai/RepoAnalyzer.cpp`)
- **Configurable interval** (5-60 minutes) and enable/disable toggle in Settings.
- **Progress reporting** with live status updates in the Settings UI.

### New: Fix Recipe System

- **Pattern-matching auto-fix** without LLM calls. Fix recipes (FIND/REPLACE pairs with file glob patterns) are extracted from AI review responses and stored in the knowledge base. When a new diff contains a known pattern, the fix can be applied directly. (`src/ai/FixApplicator.cpp`)
- **Fix recipe count** displayed in Settings UI.

### New: SSH Repository Access

- **"Open SSH Repository" action** in the sidebar plus menu. Opens a dialog for SSH URL input with connection testing, key selection, and background cloning. Supports standard SSH key authentication. (`src/ui/SshDialog.cpp`, `src/ui/SideBar.cpp`)
- **SSH repos** displayed with a terminal icon in the sidebar to distinguish them from local repositories.

### New: AI Service Layer

- **Centralized AI HTTP client** (`AiService` singleton) replaces duplicated HTTP boilerplate across multiple call sites. Supports both Anthropic Claude API and Ollama providers with automatic provider selection based on settings. (`src/ai/AiService.cpp`)
- **Review store** provides legacy compatibility for review persistence. (`src/ai/AiReviewStore.cpp`)

### New: Settings — AI Panel

- **Full AI configuration panel** in Preferences with:
  - Provider selection (Anthropic Claude API / Ollama local+LAN)
  - Anthropic API key and model fields
  - Ollama server URL and model fields
  - Knowledge Base: enable/disable, similarity threshold slider (0.50-1.00), embedding model name, stats display, clear button
  - Continuous Repo Analysis: enable/disable, interval slider, "Analyze All Repos Now" button, live progress, fix recipe count
  - Codebase Index (RAG): stats display, "Index All Repos Now" button, "Clear Index" button
  - Scrollable panel layout to accommodate all sections
- **10 new settings keys** added to `Setting.h`/`Setting.cpp`: `AiApiKey`, `AiModel`, `AiProvider`, `AiOllamaUrl`, `AiOllamaModel`, `AiKnowledgeBaseEnabled`, `AiKnowledgeBaseSimilarityThreshold`, `AiEmbeddingModel`, `AiAutoAnalyzeEnabled`, `AiAutoAnalyzeInterval`.

### New: UI Integration

- **Toolbar buttons** — "Generate Message", "Review Code", "Review History", and "Chat" buttons added to the commit editor button row. (`src/ui/CommitEditor.cpp`)
- **View menu** — "Show Chat" / "Hide Chat" toggle with Ctrl+Shift+C shortcut. (`src/ui/MenuBar.cpp`)
- **Splitter layout** — RepoView splitter updated from 2-pane (detail + log) to 3-pane (detail + chat + log) with animated show/hide transitions. (`src/ui/RepoView.cpp`)

### Build System

- Added `Sql` to `QT_MODULES` in root `CMakeLists.txt` for SQLite support.
- Added `src/ai/` subdirectory with its own `CMakeLists.txt` (8 source files, links to `conf`, `Qt6::Core`, `Qt6::Network`, `Qt6::Sql`).
- Added `ChatPanel.cpp`, `CodeReviewDialog.cpp`, `SshDialog.cpp` to `src/ui/CMakeLists.txt`.
- Added `SshDialog.cpp` to `src/dialogs/CMakeLists.txt`.
- Added `ai` to `target_link_libraries` for the `ui` target.

### Stats

- **18 new source files** created in `src/ai/` (2,466 lines)
- **6 new source files** created in `src/ui/` and `src/dialogs/` (1,924 lines)
- **20 existing files** modified (+1,083 lines)
- **Total: ~5,473 new lines of code**
