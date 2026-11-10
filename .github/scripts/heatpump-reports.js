// Handles issues from the "Add my heat pump model" and "Share my topic values" forms.
// Called from .github/workflows/heatpump-reports.yml through actions/github-script.
//
// - Topic values (from either form) are stored as one text file per issue in the
//   branch "model-reports" (reports/<model bytes>/issue-<number>.txt).
// - "Add my heat pump model": if the model bytes are already in HeatPumpType.md, the
//   issue is closed with a comment. Otherwise a pull request adds a row to
//   HeatPumpType.md and closes the issue when it is merged.
// - "Share my topic values": the issue is closed after the values are stored.
//
// The issue text is untrusted. It is only parsed here and never passed to a shell.

const MODEL_LABEL = 'heat pump model';
const VALUES_LABEL = 'topic values';
const REPORTS_BRANCH = 'model-reports';
const TYPE_FILE = 'HeatPumpType.md';

// Splits the markdown that GitHub creates from an issue form into { heading: value }.
function parseForm(body) {
  const out = {};
  const parts = ('\n' + (body || '').replace(/\r/g, '')).split(/\n### /);
  for (const part of parts.slice(1)) {
    const nl = part.indexOf('\n');
    const head = (nl < 0 ? part : part.slice(0, nl)).trim();
    let val = nl < 0 ? '' : part.slice(nl + 1).trim();
    val = val.replace(/^```[a-z]*\n?/, '').replace(/\n?```$/, '').trim();
    if (val === '_No response_') val = '';
    out[head] = val;
  }
  return out;
}

// "12 d7 0c ..." -> "12 D7 0C ...", or null if it is not ten hex bytes.
function normBytes(s) {
  const hex = String(s || '').replace(/[^0-9a-fA-F]/g, '').toUpperCase();
  if (hex.length !== 20) return null;
  if (/^0+$/.test(hex)) return null;
  return hex.match(/../g).join(' ');
}

// A short single-line cell for a markdown table.
function cell(s, max) {
  return String(s || '').replace(/[|`\r\n<>]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, max || 60);
}

// Rows of the model table: { line, nr, bytes, cols }.
function tableRows(md) {
  const rows = [];
  md.split('\n').forEach((l, i) => {
    const c = l.split('|');
    if (c.length < 4 || !/^\s*\d+\s*$/.test(c[1])) return;
    const b = normBytes(c[2]);
    if (b) rows.push({ line: i, nr: parseInt(c[1], 10), bytes: b, cols: c.map(x => x.trim()) });
  });
  return rows;
}

// Only lines like TOP12=34, XTOP0=..., OPT3=... are kept.
function cleanValues(s) {
  return String(s || '').split('\n').map(l => l.trim())
    .filter(l => /^(X?TOP|OPT)\d{1,3}=[^\r\n]{0,60}$/.test(l)).slice(0, 400);
}

function kindOf(issue, f) {
  const labels = (issue.labels || []).map(l => (typeof l === 'string' ? l : l.name));
  if (labels.includes(MODEL_LABEL) || 'Power supply' in f) return 'model';
  if (labels.includes(VALUES_LABEL) || 'Topic values' in f) return 'values';
  return null;
}

function buildRow(nr, bytes, f) {
  return '|' + nr + ' | ' + bytes + ' | ' + cell(f['Indoor unit']) + ' | ' + cell(f['Outdoor unit']) +
    ' | ' + (cell(f['Kit name']) || '-') + ' | ' + (cell(f['Power in kW'], 10) || '?') +
    ' | ' + (cell(f['Power supply'], 10).replace('Not known', '?') || '?') +
    ' | ' + (cell(f['Type']) || '?') + ' |';
}

function insertRow(md, row) {
  const rows = tableRows(md);
  const lines = md.split('\n');
  lines.splice(rows[rows.length - 1].line + 1, 0, row);
  return lines.join('\n');
}

function reportText(issue, bytes, f, values) {
  return [
    '# Topic values from issue #' + issue.number,
    'model_bytes: ' + bytes,
    'indoor_unit: ' + cell(f['Indoor unit']),
    'outdoor_unit: ' + cell(f['Outdoor unit']),
    'series: ' + cell(f['Series'], 20),
    'version: ' + cell(f['HeishaMon firmware version'], 40),
    'board: ' + cell(f['Board'], 40),
    'date: ' + issue.created_at,
    '',
    ...values,
    '',
  ].join('\n');
}

async function storeReport(github, owner, repo, def, path, text) {
  const enc = Buffer.from(text, 'utf8').toString('base64');
  try {
    await github.rest.repos.getBranch({ owner, repo, branch: REPORTS_BRANCH });
  } catch (e) {
    if (e.status !== 404) throw e;
    // The reports branch holds only the reports, so it starts without history.
    const readme = 'Topic values shared through the "Share my topic values" form, one file per issue.\n';
    const blob = await github.rest.git.createBlob({ owner, repo, content: readme, encoding: 'utf-8' });
    const tree = await github.rest.git.createTree({ owner, repo,
      tree: [{ path: 'README.md', mode: '100644', type: 'blob', sha: blob.data.sha }] });
    const commit = await github.rest.git.createCommit({ owner, repo,
      message: 'Start the model-reports branch', tree: tree.data.sha, parents: [] });
    await github.rest.git.createRef({ owner, repo, ref: 'refs/heads/' + REPORTS_BRANCH, sha: commit.data.sha });
  }
  let sha;
  try {
    sha = (await github.rest.repos.getContent({ owner, repo, path, ref: REPORTS_BRANCH })).data.sha;
  } catch (e) {
    if (e.status !== 404) throw e;
  }
  await github.rest.repos.createOrUpdateFileContents({ owner, repo, path, branch: REPORTS_BRANCH,
    message: 'Add ' + path, content: enc, sha });
}

module.exports = async ({ github, context }) => {
  const issue = context.payload.issue;
  const { owner, repo } = context.repo;
  const f = parseForm(issue.body);
  const kind = kindOf(issue, f);
  if (!kind) return 'not a form issue';

  const say = body => github.rest.issues.createComment({ owner, repo, issue_number: issue.number, body });
  const close = reason => github.rest.issues.update({ owner, repo, issue_number: issue.number,
    state: 'closed', state_reason: reason || 'completed' });

  const bytes = normBytes(f['Model bytes (TOP92)']);
  if (!bytes) {
    await say('The model bytes could not be read. They should be ten hex bytes like `12 D7 0C 98 14 35 94 0D 83 10`. Please edit the issue; a maintainer will have a look.');
    return 'bad model bytes';
  }

  const def = (await github.rest.repos.get({ owner, repo })).data.default_branch;
  const typeFile = (await github.rest.repos.getContent({ owner, repo, path: TYPE_FILE, ref: def })).data;
  const md = Buffer.from(typeFile.content, 'base64').toString('utf8');
  const rows = tableRows(md);
  const known = rows.find(r => r.bytes === bytes);
  const notes = [];

  const values = cleanValues(f['Topic values (optional)'] || f['Topic values']);
  if (values.length) {
    const path = 'reports/' + bytes.replace(/ /g, '') + '/issue-' + issue.number + '.txt';
    await storeReport(github, owner, repo, def, path, reportText(issue, bytes, f, values));
    notes.push('Thanks, the ' + values.length + ' topic values are stored in [`' + path + '`](https://github.com/' + owner + '/' + repo + '/blob/' + REPORTS_BRANCH + '/' + path + ').');
  }

  if (kind === 'values') {
    if (!values.length) notes.push('No topic values were found in this issue.');
    await say(notes.join('\n\n'));
    await close('completed');
    return 'values';
  }

  if (known) {
    notes.unshift('This model is already listed in ' + TYPE_FILE + ' as number ' + known.nr + ': ' +
      known.cols[3] + ' / ' + known.cols[4] + '. Nothing to add.');
    await say(notes.join('\n\n'));
    await close('completed');
    return 'known';
  }

  if (!cell(f['Indoor unit']) || !cell(f['Outdoor unit'])) {
    notes.unshift('The indoor or the outdoor unit is missing. Please edit the issue and add both from the type plates; a maintainer will have a look.');
    await say(notes.join('\n\n'));
    return 'missing units';
  }

  const key = bytes.replace(/ /g, '');
  const open = await github.paginate(github.rest.pulls.list, { owner, repo, state: 'open', per_page: 100 });
  const dup = open.find(p => p.head && p.head.ref && p.head.ref.startsWith('model/' + key));
  if (dup) {
    notes.unshift('These model bytes are already proposed in #' + dup.number + '.');
    await say(notes.join('\n\n'));
    await close('not_planned');
    return 'duplicate';
  }

  const nr = Math.max(...rows.map(r => r.nr)) + 1;
  const row = buildRow(nr, bytes, f);
  const branch = 'model/' + key + '-issue' + issue.number;
  const head = (await github.rest.git.getRef({ owner, repo, ref: 'heads/' + def })).data.object.sha;
  await github.rest.git.createRef({ owner, repo, ref: 'refs/heads/' + branch, sha: head });
  await github.rest.repos.createOrUpdateFileContents({ owner, repo, path: TYPE_FILE, branch,
    message: 'Add heat pump model ' + bytes + ' (#' + issue.number + ')',
    content: Buffer.from(insertRow(md, row), 'utf8').toString('base64'), sha: typeFile.sha });
  const pr = await github.rest.pulls.create({ owner, repo, base: def, head: branch,
    title: 'Add heat pump model ' + cell(f['Indoor unit']) + ' / ' + cell(f['Outdoor unit']),
    body: 'Closes #' + issue.number + '\n\nAdded automatically from the "Add my heat pump model" form. Please check the row against the type plates before merging:\n\n```\n' + row + '\n```\n' });
  notes.unshift('Thanks, this is a new model. #' + pr.data.number + ' adds it to ' + TYPE_FILE + '; this issue is closed when that is merged.');
  await say(notes.join('\n\n'));
  return 'new model';
};

module.exports.parseForm = parseForm;
module.exports.normBytes = normBytes;
module.exports.tableRows = tableRows;
module.exports.buildRow = buildRow;
module.exports.insertRow = insertRow;
module.exports.cleanValues = cleanValues;
