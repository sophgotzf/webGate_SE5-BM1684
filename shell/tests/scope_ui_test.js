/* 只验「沙箱范围」那个勾选框：对着一个 llm_web 把前端脚本跑起来，看勾选框画成什么样、
 * 手动勾上 / 取消勾选是不是立刻改变沙箱范围。
 *
 * 用法： LLM_WEB=http://127.0.0.1:8101 FS_ALLOW=0 node scope_ui_test.js
 *        LLM_WEB=http://127.0.0.1:8103 FS_ALLOW=1 node scope_ui_test.js
 *
 *   FS_ALLOW=0  期望：勾上是"勾着的"，文案 = 只允许操作 <fs_root> 内的路径
 *   FS_ALLOW=1  期望：勾选框是空的，文案带"已放开越界限制"
 *
 * v1.3.3 起它是**真开关**（不再是只读展示），所以两种启动方式下都要验：
 *   勾选框没有 disabled（点得动）→ 取消勾选 = 立刻放开越界 → 重新勾上 = 立刻收紧，
 *   并且状态存进浏览器 localStorage（outside 字段）。
 */
const { buildDom, installGlobals, waitFor } = require('./domstub');

const BASE  = process.env.LLM_WEB || 'http://127.0.0.1:8095';
const ALLOW = process.env.FS_ALLOW === '1';
const NARROW = '/data/hello';                 // 把「工作区」收窄到这里，/etc 才算越界
const realFetch = global.fetch;

let pass = 0, fail = 0;
const check = (n, c, e = '') => c ? (pass++, console.log('  PASS  ' + n))
                                  : (fail++, console.log('  FAIL  ' + n + '   ' + e));

(async () => {
  const health = await (await realFetch(BASE + '/api/health')).json();
  const html   = await (await realFetch(BASE + '/')).text();
  const js     = html.match(/<script>([\s\S]*)<\/script>/)[1];
  const byId   = buildDom(html);
  installGlobals(byId, BASE, realFetch);

  eval(js);                                                  // 执行前端脚本
  await waitFor(() => byId['stat']._text.length > 0, 5000);   // 等 health 探测回来

  const tag  = html.match(/<input[^>]*id="fs-inside"[^>]*>/);
  const box  = byId['fs-inside'];
  const txt  = () => byId['fs-scope-txt']._text;
  const hot  = () => byId['fs-scope'].classList.contains('open');
  const saved = () => JSON.parse(global.localStorage.getItem('llm_web_chat_conf_v2') || '{}').outside;

  // 真 DOM 里点勾选框 = 翻转 checked + 派发 change，这里照做
  const clickBox = want => { box.checked = want; box.dispatch('change'); };
  // 用 Files 面板里的路径框打开一个路径，返回状态栏文案
  async function openPath(p) {
    byId['fs-path'].value = p;
    byId['fs-go'].click();
    await waitFor(() => /项$/.test(byId['fs-msg']._text) || byId['fs-msg'].classList.contains('err'), 5000);
    return byId['fs-msg']._text;
  }

  console.log('== 初始状态 ==');
  check('health 的 fs_allow_outside 与预期一致', health.fs_allow_outside === ALLOW, health);
  check('勾选框存在', !!box);
  check('勾选框可以手动勾选（没有 disabled）', !!tag && !/\bdisabled\b/.test(tag[0]), tag && tag[0]);

  if (ALLOW) {
    check('放开时勾选框是空的', box.checked === false, box.checked);
    check('放开时文案写"已放开越界限制"',
          /已放开越界限制/.test(txt()), txt());
    check('放开时整块高亮（.open）', hot(), byId['fs-scope'].className);
  } else {
    check('收紧时勾选框是勾上的', box.checked === true, box.checked);
    check('收紧时文案 = 只允许操作 ' + health.fs_root + ' 内的路径',
          txt() === '只允许操作 ' + health.fs_root + ' 内的路径', txt());
    check('收紧时不高亮', !hot(), byId['fs-scope'].className);
  }

  // ---- 手动勾选：两个方向都得真的换范围 ----
  byId['fs-root'].value = NARROW;          // 「工作区」收窄，越界判断才有意义

  console.log('== 取消勾选 = 立刻放开越界 ==');
  clickBox(false);
  await waitFor(() => /已放开越界限制/.test(txt()), 3000);
  check('取消勾选后文案变成「已放开越界限制（勾上可重新收紧）」',
        txt() === '已放开越界限制（勾上可重新收紧）', txt());
  check('取消勾选后整块高亮（.open）', hot(), byId['fs-scope'].className);
  check('取消勾选后状态存进 localStorage（outside:true）', saved() === true, String(saved()));
  let m = await openPath('/etc');
  check('取消勾选后工作区外面（/etc）立刻能读',
        byId['fs-msg'].className === 'ok' && /项$/.test(m), m);

  console.log('== 重新勾上 = 立刻收紧 ==');
  clickBox(true);
  await waitFor(() => byId['fs-msg'].classList.contains('err'), 5000);
  m = byId['fs-msg']._text;
  check('重新勾上后同一条路径立刻被拒（越界）', /越界/.test(m), m);
  check('重新勾上后文案 = 只允许操作 ' + NARROW + ' 内的路径',
        txt() === '只允许操作 ' + NARROW + ' 内的路径', txt());
  check('重新勾上后不高亮', !hot(), byId['fs-scope'].className);
  check('重新勾上后 localStorage 跟着变（outside:false）', saved() === false, String(saved()));
  check('勾选框本身就是勾着的', box.checked === true, box.checked);

  console.log('\n沙箱勾选框结果: %d 通过 / %d 失败', pass, fail);
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error('harness 崩了:', e); process.exit(2); });
