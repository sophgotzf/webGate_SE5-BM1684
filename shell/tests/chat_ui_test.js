/* Chat + 工具调用的 UI 级测试：
 *   点“发送” → llm_web 去问（假）模型 → 模型要 fs_write → 真落盘 → 界面显示轨迹
 * 需要一个假模型端点： python3 mock_llm.py &   （默认 127.0.0.1:8099）
 * 用法： node chat_ui_test.js
 */
const fs = require('fs');
const { buildDom, installGlobals, sleep, waitFor, collect } = require('./domstub');

const BASE      = process.env.LLM_WEB    || 'http://127.0.0.1:8095';
const MOCK      = process.env.MOCK_URL   || 'http://127.0.0.1:8099/v1';
const MOCK_LOG  = process.env.MOCK_LOG   || '/tmp/llm_web_mock_requests.jsonl';
const FSDIR     = process.env.FSTEST_DIR || '/data/hello/tmp/llm_web_tests';
const TARGET    = FSDIR + '/from_chat.txt';
const realFetch = global.fetch;

let pass = 0, fail = 0;
const check = (n, c, e = '') => c ? (pass++, console.log('  PASS  ' + n))
                                  : (fail++, console.log('  FAIL  ' + n + '   ' + e));

(async () => {
  const html = await (await realFetch(BASE + '/')).text();
  const js   = html.match(/<script>([\s\S]*)<\/script>/)[1];
  const byId = buildDom(html);
  installGlobals(byId, BASE, realFetch);

  fs.mkdirSync(FSDIR, { recursive: true });
  try { fs.unlinkSync(TARGET); } catch (e) {}
  fs.writeFileSync(MOCK_LOG, '');

  eval(js);
  await waitFor(() => byId['stat']._text.length > 0, 5000);

  console.log('== Chat 配置区：「超时(s)」输入框 ==');
  const toEl = byId['llm-timeout'];
  check('配置区有「超时(s)」输入框', !!toEl);
  check('默认值 180（= 服务端 --chat-timeout 的默认）', toEl.value === '180', toEl.value);
  toEl.value = '9999';
  byId['save-conf'].click();
  check('超过上限就地夹到 300', toEl.value === '300', toEl.value);
  check('夹紧时状态栏说清楚', /夹到 300/.test(byId['stat']._text), byId['stat']._text);
  toEl.value = '1';
  byId['save-conf'].click();
  check('低于下限就地夹到 5', toEl.value === '5', toEl.value);
  toEl.value = 'abc';
  byId['save-conf'].click();
  check('填了非数字 → 回到默认 180', toEl.value === '180', toEl.value);
  toEl.value = '120';
  byId['save-conf'].click();
  check('配置存进 localStorage',
        JSON.parse(localStorage.getItem('llm_web_chat_conf_v2')).timeout === '120',
        localStorage.getItem('llm_web_chat_conf_v2'));

  console.log('== 填好假模型，点发送 ==');
  byId['model'].value = 'mock-1';
  byId['key'].value   = 'sk-test';
  byId['base'].value  = MOCK;
  byId['use-fs'].checked    = true;
  byId['use-shell'].checked = false;
  byId['fs-root'].value = '/data/hello';
  byId['rounds'].value  = '4';
  // 「超时(s)」：这轮故意用 120，验证它真的被带到服务端、并按生效值回显
  byId['llm-timeout'].value = '120';
  byId['chat-text'].value = '帮我建个文件';
  const toolsBlocks = () => collect(byId['chat-msgs'], e => e.classList && e.classList.contains('tools'));
  const mockLines = () => fs.readFileSync(MOCK_LOG, 'utf8').trim().split('\n').filter(Boolean).length;
  byId['chat-send'].click();
  await waitFor(() => mockLines() >= 2 && toolsBlocks().length > 0, 30000);   // 跑完两轮再断言

  console.log('== 发给模型的东西 ==');
  const reqs = fs.readFileSync(MOCK_LOG, 'utf8').trim().split('\n').filter(Boolean).map(JSON.parse);
  check('确实发起了 LLM 调用', reqs.length >= 2, '请求数=' + reqs.length);
  if (!reqs.length) { console.log('\n假模型没收到请求，先看 /tmp/llm_web_mock.log'); process.exit(2); }
  const sent = reqs[0].req;
  const names = (sent.tools || []).map(t => t.function.name);
  check('带上 fs_* 工具清单', names.includes('fs_write') && names.includes('fs_list'), names.join(','));
  check('未勾选时不带 run_shell', !names.includes('run_shell'), names.join(','));
  check('system 提示里说明了工作区', /工作区根目录：\/data\/hello/.test(sent.messages[0].content));
  // v1.3.6：默认系统提示词全项目统一成一句，这里逐字盯住它（改了就红）
  const EXPECT_SYS = 'you are a concise(SE5), helpful assistant. 用中文回答';
  check('system 第一段 = 统一的默认提示词（逐字）',
        sent.messages[0].content.startsWith(EXPECT_SYS),
        JSON.stringify(sent.messages[0].content.slice(0, 60)));
  check('全请求只有一条 system（默认提示词 + 工具说明拼在一起）',
        sent.messages.filter(m => m.role === 'system').length === 1,
        'system 条数=' + sent.messages.filter(m => m.role === 'system').length);
  check('user 消息原样送达', sent.messages.some(m => m.role === 'user' && m.content === '帮我建个文件'));
  const second = reqs[reqs.length - 1].req;
  check('第 2 轮带回了 tool 结果', second.messages.some(m => m.role === 'tool' && /"ok":true/.test(m.content)));

  console.log('== 落盘 + 界面 ==');
  check('文件被真正创建（需求核心）', fs.existsSync(TARGET));
  const msgs = byId['chat-msgs'].children;
  check('消息气泡: 用户 + 模型', msgs.length === 2, '条数=' + msgs.length);
  check('模型气泡 class 正确', msgs[1].classList.contains('assistant'));
  const blocks = toolsBlocks();
  check('界面渲染出工具轨迹块', blocks.length === 1, '块数=' + blocks.length);
  if (blocks.length) {
    const summaries = blocks[0].children.map(d => d.children[0].textContent);
    check('轨迹里有 ✓ fs_write', summaries.some(s => s.includes('fs_write') && s.startsWith('✓')), summaries.join(' | '));
    check('轨迹里有 fs_list', summaries.some(s => s.includes('fs_list')), summaries.join(' | '));
    check('轨迹里能看到工具返回 JSON', /"ok":true/.test(blocks[0].children[0].children[1].textContent));
  }
  check('状态栏显示工具次数', /工具/.test(byId['stat']._text), byId['stat']._text);
  check('状态栏回显本轮预算 = 输入框里的 120s',
        /预算 120s/.test(byId['stat']._text), byId['stat']._text);

  console.log('\nChat UI 结果: %d 通过 / %d 失败', pass, fail);
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error('harness 崩了:', e); process.exit(2); });
