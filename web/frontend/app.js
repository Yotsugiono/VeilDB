const API_BASE = window.API_BASE || "";

let DOC_ID_MIN = 1;
let DOC_ID_MAX = 10000;

let currentRole = "user";
let sessionId = null;
let edbId = null;
let occupiedDocIds = new Set();
let cachedDatabaseItems = [];
let auditLogs = [];
let auditDatabaseFilter = "all";

const $ = (id) => document.getElementById(id);

function setButtonBusy(button, busy, busyText) {
  if (!button) return;
  if (!button.dataset.defaultLabel) {
    button.dataset.defaultLabel = button.textContent;
  }
  button.classList.toggle("is-loading", busy);
  button.textContent = busy ? busyText : button.dataset.defaultLabel;
}

function applyDocCatalog(catalog) {
  if (!catalog || !Array.isArray(catalog.occupied)) return;
  occupiedDocIds = new Set(catalog.occupied);
  if (Number.isInteger(catalog.doc_id_min)) DOC_ID_MIN = catalog.doc_id_min;
  if (Number.isInteger(catalog.doc_id_max)) DOC_ID_MAX = catalog.doc_id_max;

  const input = $("doc-id");
  if (input) {
    input.min = String(DOC_ID_MIN);
    input.max = String(DOC_ID_MAX);
  }
}

function markDocOccupied(id) {
  occupiedDocIds.add(id);
}

function parseDocId() {
  const raw = $("doc-id").value.trim();
  if (!raw) {
    throw new Error("请输入文档 ID");
  }
  const id = Number.parseInt(raw, 10);
  if (!Number.isInteger(id) || id < DOC_ID_MIN || id > DOC_ID_MAX) {
    throw new Error(`文档 ID 范围为 ${DOC_ID_MIN}-${DOC_ID_MAX}`);
  }
  return id;
}

function parseBatchRange() {
  const startRaw = $("batch-start-id").value.trim();
  const endRaw = $("batch-end-id").value.trim();
  const start = Number.parseInt(startRaw, 10);
  const end = Number.parseInt(endRaw, 10);

  if (!Number.isInteger(start) || !Number.isInteger(end)) {
    throw new Error("请输入有效的批量插入范围。");
  }
  if (start < DOC_ID_MIN || end > DOC_ID_MAX || start > end) {
    throw new Error(`批量插入范围必须在 ${DOC_ID_MIN}-${DOC_ID_MAX} 内，且起始值不能大于结束值。`);
  }
  return { start, end };
}

async function api(path, options = {}) {
  const res = await fetch(`${API_BASE}${path}`, {
    headers: {
      "Content-Type": "application/json",
      "X-EncDB-Role": currentRole,
      ...options.headers,
    },
    ...options,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    const detail = data.detail || res.statusText;
    throw new Error(typeof detail === "string" ? detail : JSON.stringify(detail));
  }
  return data;
}

function formatSessionConnected(info) {
  const docs = info.doc_catalog?.occupied?.length ?? occupiedDocIds.size;
  return `已连接数据库 #${info.edb_id ?? "-"}，当前记录文档 ${docs} 篇。`;
}

function updateHeroSummary() {
  $("hero-edb-summary").textContent = edbId ? `#${edbId}` : "未连接";
  $("hero-doc-summary").textContent = sessionId
    ? `${occupiedDocIds.size} 篇`
    : "等待连接";
}

function setSessionMeta(text) {
  $("session-meta").textContent = text;
}

function setDocStatus(text) {
  $("doc-status").textContent = text;
}

function setQueryMeta(text) {
  $("query-meta").textContent = text;
}

function setQueryChips(items) {
  const host = $("query-kpis");
  host.innerHTML = items
    .map((item) => {
      const accentClass = item.accent ? " is-accent" : "";
      return `<div class="query-chip${accentClass}"><span>${escapeHtml(item.label)}</span><strong>${escapeHtml(item.value)}</strong></div>`;
    })
    .join("");
}

function setHealthStatus(ok, text) {
  const dot = $("health-dot");
  dot.classList.toggle("online", ok);
  dot.classList.toggle("offline", !ok);
  $("health-text").textContent = text;
}

function formatAuditTime(value) {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "-";
  return date.toLocaleString("zh-CN", { hour12: false });
}

function getAuditDatabaseIds() {
  const ids = new Set();
  cachedDatabaseItems.forEach((item) => {
    if (Number.isInteger(item.edb_id)) ids.add(item.edb_id);
  });
  auditLogs.forEach((entry) => {
    if (Number.isInteger(entry.edb_id)) ids.add(entry.edb_id);
  });
  return [...ids].sort((a, b) => a - b);
}

function renderAuditDatabaseFilter() {
  const select = $("audit-edb-filter");
  if (!select) return;

  const ids = getAuditDatabaseIds();
  if (auditDatabaseFilter !== "all" && !ids.includes(Number(auditDatabaseFilter))) {
    auditDatabaseFilter = "all";
  }

  select.innerHTML = [
    '<option value="all">全部数据库</option>',
    ...ids.map((id) => `<option value="${id}">数据库 #${id}</option>`),
  ].join("");
  select.value = auditDatabaseFilter;
}

async function loadAuditLogs() {
  const list = $("audit-log-list");
  const summary = $("audit-summary");
  const query = auditDatabaseFilter === "all"
    ? "?limit=500"
    : `?edb_id=${encodeURIComponent(auditDatabaseFilter)}&limit=500`;

  try {
    const data = await api(`/api/audit/logs${query}`);
    auditLogs = Array.isArray(data.logs) ? data.logs : [];
    renderAuditDatabaseFilter();
    renderAuditLogList();
  } catch (error) {
    auditLogs = [];
    renderAuditDatabaseFilter();
    if (summary) {
      summary.textContent = "审计加载失败";
    }
    if (list) {
      list.innerHTML = `<p class="database-empty">加载审计记录失败：${escapeHtml(error.message)}</p>`;
    }
  }
}

function renderAuditLogList() {
  const list = $("audit-log-list");
  const summary = $("audit-summary");
  if (!list || !summary) return;

  const visibleLogs = auditLogs.filter((entry) => (
    auditDatabaseFilter === "all" || String(entry.edb_id) === String(auditDatabaseFilter)
  ));

  summary.textContent = visibleLogs.length
    ? `共 ${visibleLogs.length} 条记录`
    : "暂无审计记录";

  if (!visibleLogs.length) {
    list.innerHTML = '<p class="database-empty">暂无使用记录。创建、连接、插入或查询数据库后会显示在这里。</p>';
    return;
  }

  list.innerHTML = visibleLogs
    .slice(0, 120)
    .map((entry) => {
      const statusText = entry.status === "success" ? "成功" : "失败";
      const statusClass = entry.status === "success" ? "success" : "error";
      const dbText = Number.isInteger(entry.edb_id) ? `数据库 #${entry.edb_id}` : "未绑定数据库";
      const duration = Number.isFinite(entry.latency_ms) ? ` · ${entry.latency_ms} ms` : "";
      return `
        <article class="audit-log-item">
          <div class="audit-log-main">
            <span class="audit-status ${statusClass}">${statusText}</span>
            <div>
              <strong>${escapeHtml(entry.action)}</strong>
              <p>${escapeHtml(entry.target || dbText)}${duration}</p>
            </div>
          </div>
          <div class="audit-log-meta">
            <span>${escapeHtml(dbText)}</span>
            <span>${escapeHtml(entry.role === "admin" ? "管理者" : entry.role === "user" ? "使用者" : "未知角色")}</span>
            <span>${escapeHtml(formatAuditTime(entry.time))}</span>
          </div>
          ${entry.detail ? `<p class="audit-log-detail">${escapeHtml(entry.detail)}</p>` : ""}
        </article>
      `;
    })
    .join("");
}

function updateAvailableDatabasesSummary(items = cachedDatabaseItems) {
  const summary = $("available-databases-summary");
  if (!summary) return;

  if (!items.length) {
    summary.textContent = "可连接数据库：暂无已保存数据库。";
    return;
  }

  const ids = items.map((item) => `#${item.edb_id}`);
  summary.textContent = `可连接数据库：${ids.join("、")}。`;
}

function applyRole(role) {
  currentRole = role;
  document.body.dataset.role = role;
  $("role-user").classList.toggle("active", role === "user");
  $("role-admin").classList.toggle("active", role === "admin");
  $("current-role-tag").textContent = role === "admin" ? "管理者" : "使用者";
  $("role-title").textContent = role === "admin" ? "管理者工作台" : "使用者工作台";
  $("role-description").textContent = role === "admin"
    ? "连接或创建数据库，插入与查询文档，并查看数据库状态。"
    : "连接已有加密数据库，完成文档插入和加密查询。";

  if (role === "admin") {
    loadDatabaseList(false);
    loadAuditLogs();
  } else {
    updateAvailableDatabasesSummary();
  }
}

function setSessionUI(info) {
  edbId = info.edb_id;
  $("btn-shutdown").hidden = false;
  $("btn-shutdown").disabled = false;
  $("btn-init").disabled = true;
  $("btn-resume").disabled = true;
  $("resume-edb-id").disabled = true;
  $("doc-id").disabled = false;
  $("btn-insert").disabled = false;
  $("batch-start-id").disabled = false;
  $("batch-end-id").disabled = false;
  $("btn-batch-insert").disabled = false;
  $("btn-query").disabled = false;
  setSessionMeta(formatSessionConnected(info));
  updateHeroSummary();
}

function resetSessionUI() {
  sessionId = null;
  edbId = null;
  occupiedDocIds = new Set();
  $("btn-shutdown").hidden = true;
  $("btn-shutdown").disabled = true;
  $("btn-init").disabled = false;
  $("btn-resume").disabled = false;
  $("resume-edb-id").disabled = false;
  $("doc-id").disabled = true;
  $("btn-insert").disabled = true;
  $("batch-start-id").disabled = true;
  $("batch-end-id").disabled = true;
  $("btn-batch-insert").disabled = true;
  $("btn-query").disabled = true;
  closeInsertMenu();
  setSessionMeta("等待连接数据库。");
  setDocStatus("连接数据库后即可执行文档插入。");
  setQueryMeta("输入查询语句后执行，结果会在下方展示。");
  setQueryChips([{ label: "状态", value: "等待查询" }]);
  renderEmptyState(
    "还没有查询结果",
    "连接数据库后执行 SELECT 查询，结果会在这里呈现。"
  );
  updateHeroSummary();
}

function closeInsertMenu() {
  $("insert-menu").classList.add("hidden");
}

function toggleInsertMenu() {
  $("insert-menu").classList.toggle("hidden");
}

function renderEmptyState(title, description) {
  $("results").innerHTML = `
    <div class="empty-state">
      <div>
        <strong>${escapeHtml(title)}</strong>
        <p>${escapeHtml(description)}</p>
      </div>
    </div>
  `;
}

function renderResults(data) {
  const chips = [
    { label: "模式", value: data.result_mode === "aggregate" ? "聚合" : "文档" },
    {
      label: "数量",
      value: data.result_mode === "aggregate"
        ? `${data.match_count ?? 0} 条匹配`
        : `${data.doc_count} 条结果`,
      accent: true,
    },
    { label: "耗时", value: `${data.latency_ms} ms` },
  ];

  if (data.result_mode === "aggregate" && data.aggregate_op) {
    chips.push({ label: "聚合", value: data.aggregate_op });
  }

  setQueryChips(chips);

  if (data.result_mode === "aggregate") {
    setQueryMeta(`${data.aggregate_op} 查询完成，共 ${data.match_count ?? 0} 条匹配，耗时 ${data.latency_ms} ms。`);
  } else {
    setQueryMeta(`查询完成，共返回 ${data.doc_count} 条结果，耗时 ${data.latency_ms} ms。`);
  }

  if (!data.hits.length) {
    renderEmptyState("查询完成但没有命中", "可以尝试调整关键词或更换逻辑运算组合。");
    return;
  }

  $("results").innerHTML = data.hits
    .map((hit) => {
      if (hit.is_aggregate) {
        return `
          <article class="hit aggregate">
            <header>
              <div class="hit-title">
                <span class="hit-badge">Aggregate</span>
                <div>
                  <strong>聚合结果</strong>
                  <span>EncDB 返回的计算结果</span>
                </div>
              </div>
            </header>
            <pre class="agg">${escapeHtml(hit.preview)}</pre>
          </article>
        `;
      }

      return `
        <article class="hit">
          <header>
            <div class="hit-title">
              <span class="hit-badge">Doc</span>
              <div>
                <strong>文档 #${hit.doc_id}</strong>
                <span>命中文档预览</span>
              </div>
            </div>
          </header>
          <pre>${escapeHtml(hit.preview)}</pre>
        </article>
      `;
    })
    .join("");
}

function normalizeDatabaseItems(data) {
  if (Array.isArray(data.databases)) {
    return data.databases;
  }
  return (data.edb_ids || []).map((id) => ({
    edb_id: id,
    doc_count: null,
    revision: null,
    has_catalog: false,
  }));
}

function renderDatabaseList(items) {
  const host = $("database-list");

  if (!items.length) {
    host.innerHTML = '<p class="database-empty">当前没有已保存数据库。管理者创建并保存数据库后会显示在这里。</p>';
    return;
  }

  host.innerHTML = items
    .map((item) => {
      const id = item.edb_id;
      const docCount = Number.isInteger(item.doc_count) ? `${item.doc_count} 篇文档` : "文档数未知";
      const catalog = item.has_catalog ? `catalog r${item.revision ?? 0}` : "未发现 catalog";
      return `
        <button type="button" class="database-item" data-edb-id="${id}">
          <span class="database-item-main">
            <strong>数据库 #${id}</strong>
            <em>${docCount}</em>
          </span>
          <span class="database-item-meta">${catalog} · 点击后填入连接编号</span>
        </button>
      `;
    })
    .join("");

  host.querySelectorAll(".database-item").forEach((button) => {
    button.addEventListener("click", () => {
      $("resume-edb-id").value = button.dataset.edbId;
      setSessionMeta(`已选择数据库 #${button.dataset.edbId}，点击“连接已有库”即可打开。`);
    });
  });
}

async function loadDatabaseList(updateStatus = true) {
  const listButton = $("btn-list-databases");
  const refreshButton = $("btn-refresh-databases");

  setButtonBusy(listButton, true, "加载中");
  setButtonBusy(refreshButton, true, "刷新中");

  try {
    const data = await api("/api/databases");
    const items = normalizeDatabaseItems(data);
    cachedDatabaseItems = items;
    renderDatabaseList(items);
    updateAvailableDatabasesSummary(items);
    await loadAuditLogs();
    if (updateStatus) {
      setSessionMeta(
        items.length
          ? `已加载 ${items.length} 个可连接数据库。`
          : "已加载数据库列表，当前没有可连接数据库。"
      );
    }
  } catch (error) {
    $("database-list").innerHTML = `<p class="database-empty">加载数据库状态失败：${escapeHtml(error.message)}</p>`;
    if (updateStatus) {
      setSessionMeta(`加载数据库状态失败：${error.message}`);
    }
  } finally {
    setButtonBusy(listButton, false);
    setButtonBusy(refreshButton, false);
  }
}

async function postUploadForm(form, replace) {
  const path = replace ? "/api/replace" : "/api/upload";
  const res = await fetch(`${API_BASE}${path}`, {
    method: "POST",
    headers: { "X-EncDB-Role": currentRole },
    body: form,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    const detail = data.detail || res.statusText;
    throw new Error(typeof detail === "string" ? detail : JSON.stringify(detail));
  }
  return data;
}

async function insertEnronDoc(docId) {
  return api("/api/insert", {
    method: "POST",
    body: JSON.stringify({ session_id: sessionId, doc_id: String(docId) }),
  });
}

async function deleteDatabaseById(targetEdbId) {
  return api(`/api/databases/${encodeURIComponent(targetEdbId)}`, {
    method: "DELETE",
  });
}

$("role-user").addEventListener("click", () => applyRole("user"));
$("role-admin").addEventListener("click", () => applyRole("admin"));

$("btn-init").addEventListener("click", async () => {
  if (currentRole !== "admin") return;

  const initButton = $("btn-init");
  $("btn-resume").disabled = true;
  initButton.disabled = true;
  setButtonBusy(initButton, true, "创建中");
  setSessionMeta("正在创建新数据库并建立会话...");

  try {
    const data = await api("/api/session/init", {
      method: "POST",
      body: JSON.stringify({}),
    });
    sessionId = data.session_id;
    occupiedDocIds = new Set();
    if (data.doc_catalog) {
      applyDocCatalog(data.doc_catalog);
    }
    setSessionUI(data);
    await loadDatabaseList(false);
  } catch (error) {
    setSessionMeta(`创建失败：${error.message}`);
    await loadAuditLogs();
    $("btn-resume").disabled = false;
    initButton.disabled = false;
  } finally {
    setButtonBusy(initButton, false);
  }
});

$("btn-resume").addEventListener("click", async () => {
  const raw = $("resume-edb-id").value.trim();
  if (!raw) {
    setSessionMeta("请输入要连接的数据库编号。");
    return;
  }
  const resumeId = Number.parseInt(raw, 10);
  if (!Number.isInteger(resumeId) || resumeId <= 0) {
    setSessionMeta("数据库编号无效。");
    return;
  }

  const resumeButton = $("btn-resume");
  $("btn-init").disabled = true;
  resumeButton.disabled = true;
  setButtonBusy(resumeButton, true, "连接中");
  setSessionMeta(`正在连接数据库 #${resumeId}...`);

  try {
    const data = await api("/api/session/init", {
      method: "POST",
      body: JSON.stringify({ edb_id: resumeId }),
    });
    sessionId = data.session_id;
    occupiedDocIds = new Set();
    if (data.doc_catalog) {
      applyDocCatalog(data.doc_catalog);
    }
    setSessionUI(data);
    await loadAuditLogs();
  } catch (error) {
    setSessionMeta(`连接失败：${error.message}`);
    await loadAuditLogs();
    $("btn-init").disabled = false;
    resumeButton.disabled = false;
  } finally {
    setButtonBusy(resumeButton, false);
  }
});

$("btn-list-databases").addEventListener("click", async () => {
  await loadDatabaseList();
});

$("btn-refresh-databases").addEventListener("click", async () => {
  await loadDatabaseList();
});

$("btn-delete-database").addEventListener("click", async () => {
  if (currentRole !== "admin") return;

  const raw = $("resume-edb-id").value.trim();
  if (!raw) {
    setSessionMeta("请输入要删除的数据库编号。");
    return;
  }

  const targetEdbId = Number.parseInt(raw, 10);
  if (!Number.isInteger(targetEdbId) || targetEdbId <= 0) {
    setSessionMeta("数据库编号无效。");
    return;
  }

  if (edbId === targetEdbId) {
    setSessionMeta(`数据库 #${targetEdbId} 正在当前会话中使用，请先保存并断开。`);
    return;
  }

  if (!window.confirm(`确认永久删除数据库 #${targetEdbId} 吗？此操作会删除持久化文件。`)) {
    return;
  }

  const deleteButton = $("btn-delete-database");
  deleteButton.disabled = true;
  setButtonBusy(deleteButton, true, "删除中");
  setSessionMeta(`正在删除数据库 #${targetEdbId}...`);

  try {
    const data = await deleteDatabaseById(targetEdbId);
    if (data.success) {
      if ($("resume-edb-id").value.trim() === String(targetEdbId)) {
        $("resume-edb-id").value = "";
      }
      if (String(auditDatabaseFilter) === String(targetEdbId)) {
        auditDatabaseFilter = "all";
      }
      setSessionMeta(`数据库 #${targetEdbId} 已删除。`);
      await loadDatabaseList(false);
    } else {
      setSessionMeta(data.message || `删除数据库 #${targetEdbId} 失败。`);
      await loadAuditLogs();
    }
  } catch (error) {
    setSessionMeta(`删除失败：${error.message}`);
    await loadAuditLogs();
  } finally {
    setButtonBusy(deleteButton, false);
    deleteButton.disabled = false;
  }
});

$("audit-edb-filter").addEventListener("change", async () => {
  auditDatabaseFilter = $("audit-edb-filter").value;
  await loadAuditLogs();
});

$("btn-refresh-audit").addEventListener("click", async () => {
  await loadAuditLogs();
});

$("btn-shutdown").addEventListener("click", async () => {
  if (!sessionId) return;
  if (!window.confirm(`保存数据库 #${edbId ?? "?"} 并断开当前会话？`)) {
    return;
  }

  const shutdownButton = $("btn-shutdown");
  const closingEdbId = edbId;
  shutdownButton.disabled = true;
  setButtonBusy(shutdownButton, true, "保存中");
  setSessionMeta("正在持久化数据库并关闭会话...");

  try {
    const data = await api("/api/session/shutdown", {
      method: "POST",
      body: JSON.stringify({ session_id: sessionId }),
    });
    if (data.success) {
      setSessionMeta(`数据库 #${data.edb_id} 已保存，耗时 ${data.latency_ms} ms。`);
      resetSessionUI();
      if (currentRole === "admin") {
        await loadDatabaseList(false);
      }
    } else {
      setSessionMeta(`保存失败：${data.message}`);
      await loadAuditLogs();
      shutdownButton.disabled = false;
    }
  } catch (error) {
    setSessionMeta(`保存失败：${error.message}`);
    await loadAuditLogs();
    shutdownButton.disabled = false;
  } finally {
    setButtonBusy(shutdownButton, false);
  }
});

$("btn-insert").addEventListener("click", () => {
  if (!sessionId) return;
  try {
    parseDocId();
    toggleInsertMenu();
  } catch (error) {
    setDocStatus(error.message);
    closeInsertMenu();
  }
});

$("btn-insert-enron").addEventListener("click", async () => {
  if (!sessionId) return;
  closeInsertMenu();

  let docId;
  try {
    docId = parseDocId();
  } catch (error) {
    setDocStatus(error.message);
    return;
  }

  if (occupiedDocIds.has(docId) && !window.confirm(`#${docId} 已存在，是否用 Enron 示例覆盖？`)) {
    return;
  }

  const insertButton = $("btn-insert");
  insertButton.disabled = true;
  setButtonBusy(insertButton, true, "插入中");
  setDocStatus(`正在插入 Enron 示例到文档 #${docId}...`);

  try {
    const data = await insertEnronDoc(docId);
    if (data.success) {
      markDocOccupied(data.doc_id);
      updateHeroSummary();
      setSessionMeta(formatSessionConnected({ edb_id: edbId, doc_catalog: { occupied: [...occupiedDocIds] } }));
      await loadAuditLogs();
      setDocStatus(`文档 #${data.doc_id} 插入成功，耗时 ${data.latency_ms} ms。`);
    } else {
      await loadAuditLogs();
      setDocStatus(data.message);
    }
  } catch (error) {
    setDocStatus(`插入失败：${error.message}`);
    await loadAuditLogs();
  } finally {
    setButtonBusy(insertButton, false);
    insertButton.disabled = false;
  }
});

$("btn-batch-insert").addEventListener("click", async () => {
  if (!sessionId || currentRole !== "admin") return;
  closeInsertMenu();

  let range;
  try {
    range = parseBatchRange();
  } catch (error) {
    setDocStatus(error.message);
    return;
  }

  const ids = [];
  for (let docId = range.start; docId <= range.end; docId += 1) {
    ids.push(docId);
  }

  const occupied = ids.filter((docId) => occupiedDocIds.has(docId));
  if (occupied.length) {
    const ok = window.confirm(
      `范围内已有 ${occupied.length} 个文档 ID 被占用：${occupied.map((id) => `#${id}`).join("、")}。是否继续并覆盖这些样例？`
    );
    if (!ok) return;
  }

  const batchButton = $("btn-batch-insert");
  const insertButton = $("btn-insert");
  batchButton.disabled = true;
  insertButton.disabled = true;
  setButtonBusy(batchButton, true, "批量插入中");

  let successCount = 0;
  const failures = [];

  try {
    for (const docId of ids) {
      setDocStatus(`正在批量插入 Enron 示例：#${docId}（${successCount}/${ids.length} 已完成）...`);
      try {
        const data = await insertEnronDoc(docId);
        if (data.success) {
          successCount += 1;
          markDocOccupied(data.doc_id);
          updateHeroSummary();
          continue;
        }
        failures.push(`#${docId}: ${data.message || "插入失败"}`);
      } catch (error) {
        failures.push(`#${docId}: ${error.message}`);
      }
    }

    setSessionMeta(formatSessionConnected({ edb_id: edbId, doc_catalog: { occupied: [...occupiedDocIds] } }));
    await loadAuditLogs();
    if (failures.length) {
      setDocStatus(`批量插入完成：成功 ${successCount} 个，失败 ${failures.length} 个。${failures.slice(0, 3).join("；")}`);
    } else {
      setDocStatus(`批量插入完成：已成功插入 #${range.start}-#${range.end}，共 ${successCount} 个样例。`);
    }
  } finally {
    setButtonBusy(batchButton, false);
    batchButton.disabled = false;
    insertButton.disabled = false;
  }
});

async function uploadSelectedFile(file) {
  let docId;
  try {
    docId = parseDocId();
  } catch (error) {
    setDocStatus(error.message);
    return;
  }

  const replace = occupiedDocIds.has(docId);
  if (replace && !window.confirm(`#${docId} 已存在，是否替换当前文档？`)) {
    return;
  }

  const form = new FormData();
  form.append("session_id", sessionId);
  form.append("file", file);
  form.append("doc_id", String(docId));

  const insertButton = $("btn-insert");
  insertButton.disabled = true;
  setButtonBusy(insertButton, true, replace ? "替换中" : "上传中");
  setDocStatus(replace ? `正在替换文档 #${docId}...` : `正在上传文档 #${docId}...`);

  try {
    const data = await postUploadForm(form, replace);
    if (data.success) {
      markDocOccupied(data.doc_id);
      updateHeroSummary();
      setSessionMeta(formatSessionConnected({ edb_id: edbId, doc_catalog: { occupied: [...occupiedDocIds] } }));
      await loadAuditLogs();
      setDocStatus(`文档 #${data.doc_id} 已${data.replaced ? "替换" : "上传"}，耗时 ${data.latency_ms} ms。`);
    } else {
      await loadAuditLogs();
      setDocStatus(data.message);
    }
  } catch (error) {
    setDocStatus(`上传失败：${error.message}`);
    await loadAuditLogs();
  } finally {
    setButtonBusy(insertButton, false);
    insertButton.disabled = false;
  }
}

$("btn-insert-upload").addEventListener("click", () => {
  if (!sessionId) return;
  closeInsertMenu();

  try {
    parseDocId();
  } catch (error) {
    setDocStatus(error.message);
    return;
  }

  const picker = document.createElement("input");
  picker.type = "file";
  picker.addEventListener("change", () => {
    const file = picker.files?.[0];
    if (file) {
      uploadSelectedFile(file);
    }
    picker.remove();
  });
  picker.addEventListener("cancel", () => picker.remove());
  document.body.appendChild(picker);
  picker.click();
});

document.addEventListener("click", (event) => {
  const wrap = document.querySelector(".insert-wrap");
  if (wrap && !wrap.contains(event.target)) {
    closeInsertMenu();
  }
});

$("btn-query").addEventListener("click", async () => {
  if (!sessionId) return;
  closeInsertMenu();

  const sql = $("sql-input").value.trim();
  if (!sql) {
    setQueryMeta("请输入查询语句。");
    return;
  }

  const queryButton = $("btn-query");
  queryButton.disabled = true;
  setButtonBusy(queryButton, true, "查询中");
  setQueryMeta("正在执行查询，请稍候...");
  setQueryChips([
    { label: "状态", value: "执行中", accent: true },
    { label: "语句", value: sql.length > 30 ? `${sql.slice(0, 30)}...` : sql },
  ]);
  renderEmptyState("查询正在执行", "网关已将请求发送给 EncDB Server，结果返回后会自动刷新。");

  try {
    const data = await api("/api/query", {
      method: "POST",
      body: JSON.stringify({ session_id: sessionId, sql }),
    });
    await loadAuditLogs();
    renderResults(data);
  } catch (error) {
    setQueryMeta(`查询失败：${error.message}`);
    await loadAuditLogs();
    setQueryChips([
      { label: "状态", value: "失败" },
      { label: "错误", value: error.message, accent: true },
    ]);
    renderEmptyState("查询失败", "请检查数据库状态、查询语句格式或网关连接。");
  } finally {
    setButtonBusy(queryButton, false);
    queryButton.disabled = false;
  }
});

function escapeHtml(text) {
  return String(text)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

async function checkHealth() {
  try {
    await api("/api/health");
    setHealthStatus(true, "网关在线");
  } catch {
    setHealthStatus(false, "网关离线");
  }
}

resetSessionUI();
applyRole("user");
loadAuditLogs();
loadDatabaseList(false);
checkHealth();
setInterval(checkHealth, 15000);
