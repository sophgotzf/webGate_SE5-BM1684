/* Files 面板的 UI 级测试：点目录进、点文件开、改内容保存、新建文件/目录、越界提示，
 * 以及「只许浏览」这两件事：
 *   - 面板里没有删除功能（v1.3.4）：行里没按钮、脚本里不再发 remove、刷新后文件还在；
 *   - 浏览（列目录 / 打开文件）也被「工作区」+ 勾选框管着（v1.3.4 起每个 /api/fs 请求都带
 *     root，服务端 --fs-root 比「工作区」宽的时候也不能越界读）；
 *   - 「新建文件」这种写操作同样吃这份范围（相对路径带 .. 也拦得住），工作区里面照常能用。
 * 用法： node ui_test.js           （默认连 http://127.0.0.1:8095）
 *        LLM_WEB=http://127.0.0.1:8093 node ui_test.js
 *        FSTEST_DIR=/data/hello/tmp/xxx node ui_test.js
 */
const fs = require('fs');
const { buildDom, installGlobals, sleep, waitFor } = require('./domstub');

const BASE    = process.env.LLM_WEB   || 'http://127.0.0.1:8095';
const FSDIR   = process.env.FSTEST_DIR || '/data/hello/tmp/llm_web_tests';
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
  eval(js);                                  // 执行前端脚本
  await waitFor(() => byId['stat']._text.length > 0, 5000);   // 等 health 探测回来

  const rows = () => {
    const table = byId['fs-list'].children[0];
    if (!table) return [];
    const tbody = table.children[0];
    return tbody ? tbody.children : [];
  };
  const findRow = part => rows().find(tr => tr.children[0] && tr.children[0].textContent.includes(part));

  console.log('== 前端加载 ==');
  check('前端脚本执行没抛异常', true);
  check('Chat 默认勾选“文件操作”', byId['use-fs'].checked === true);
  check('工作区默认 /', byId['fs-root'].value === '/', byId['fs-root'].value);
  check('health 探测有结果', byId['stat']._text.length > 0, byId['stat']._text);

  console.log('== 沙箱范围勾选框 ==');
  const health = await (await realFetch(BASE + '/api/health')).json();
  const scopeTag = html.match(/<input[^>]*id="fs-inside"[^>]*>/);
  const scopeLab = html.match(/<label id="fs-scope"[^>]*>/);
  check('Files 面板有沙箱勾选框', !!byId['fs-inside']);
  check('勾选框默认是勾上的（= 只允许工作区内）', byId['fs-inside'].checked === true);
  check('勾选框是可手动勾选的开关（没有 disabled）',
        !!scopeTag && !/\bdisabled\b/.test(scopeTag[0]), scopeTag && scopeTag[0]);
  check('勾选框上写明了勾上/取消各是什么意思',
        !!scopeLab && /title="勾上[^"]*取消勾选[^"]*"/.test(scopeLab[0]), scopeLab && scopeLab[0]);
  check('勾选框文案说的是当前工作区根',
        byId['fs-scope-txt']._text === '只允许操作 ' + health.fs_root + ' 内的路径',
        byId['fs-scope-txt']._text);
  check('health 带 fs_root / fs_allow_outside',
        typeof health.fs_root === 'string' && health.fs_allow_outside === false, health);
  check('勾上/放开与服务端状态一致（本服务没带 --fs-allow-outside）',
        byId['fs-inside'].checked === !health.fs_allow_outside, health);

  console.log('== Files 面板 ==');
  fs.writeFileSync(FSDIR + '/ui_edit.txt', 'before\n');   // 先放个文件，空目录不会渲染表格
  fs.writeFileSync(FSDIR + '/ui_seed.txt', 'seed\n');
  byId['fs-path'].value = FSDIR;
  byId['tab-files'].click();
  await waitFor(() => rows().length > 0, 5000);
  check('切到 Files 面板', !byId['panel-files'].classList.contains('hidden'));
  check('目录列表渲染出表格', rows().length > 0, '行数=' + rows().length);
  check('状态栏显示条目数', /项/.test(byId['fs-msg']._text), byId['fs-msg']._text);

  console.log('== 打开文件 → 改内容 → 保存 ==');
  const row = findRow('ui_edit.txt');
  check('新文件出现在列表里', !!row);
  row.children[0].click();                    // 点文件名 = 打开
  await waitFor(() => !byId['fs-edit'].classList.contains('hidden'), 5000);
  check('编辑器打开', !byId['fs-edit'].classList.contains('hidden'));
  check('读到内容正确', byId['fs-text'].value === 'before\n', JSON.stringify(byId['fs-text'].value));
  byId['fs-text'].value = 'after from UI\n';
  byId['fs-charset'].value = 'utf-8';
  byId['fs-eol'].value = 'lf';
  byId['fs-save'].click();
  await waitFor(() => fs.readFileSync(FSDIR + '/ui_edit.txt', 'utf8') === 'after from UI\n', 5000);
  check('保存后磁盘内容已变', fs.readFileSync(FSDIR + '/ui_edit.txt', 'utf8') === 'after from UI\n');

  console.log('== 新建文件 / 新建目录 ==');
  global.__promptAnswer = 'ui_new.txt';
  byId['fs-newfile'].click();
  await waitFor(() => fs.existsSync(FSDIR + '/ui_new.txt'), 5000);
  check('新建文件真的落盘', fs.existsSync(FSDIR + '/ui_new.txt'));
  check('新建后自动打开编辑器', byId['fs-edit-name']._text.includes('ui_new.txt'), byId['fs-edit-name']._text);

  global.__promptAnswer = 'ui_newdir';
  byId['fs-newdir'].click();
  await waitFor(() => fs.existsSync(FSDIR + '/ui_newdir'), 5000);
  check('新建目录真的落盘', fs.existsSync(FSDIR + '/ui_newdir') && fs.statSync(FSDIR + '/ui_newdir').isDirectory());

  byId['fs-refresh'].click();
  await waitFor(() => findRow('ui_new.txt'), 5000);

  console.log('== file 配置区不再提供「删除」（v1.3.4）==');
  // 行里除了 4 个数据格，原来还挂着第 5 格（td.act + 「删除」按钮）
  const btnTexts = tr => tr.children.reduce((a, td) => a.concat((td.children || []).map(c => String(c.textContent))), []);
  check('每行只剩 4 格：名称/类型/大小/修改时间',
        rows().length > 0 && rows().every(tr => tr.children.length === 4),
        rows().map(tr => tr.children.length).join(','));
  check('没有「删除」按钮（行里一个按钮都没有）',
        !rows().some(tr => btnTexts(tr).length > 0), rows().map(btnTexts).join(' | '));
  check('表头也不带那个空列（只剩 4 个表头）',
        /<th>修改时间<\/th><\/tr><\/thead>/.test(js), '脚本里的表头还是 5 列');
  check('前端脚本里再也没有 remove 这条路（不会发出删除请求）',
        !/action\s*:\s*['"]remove['"]/.test(js), '脚本里还有 remove');
  byId['fs-refresh'].click();                 // 反复刷新，文件该在的还在
  await waitFor(() => findRow('ui_new.txt'), 5000);
  check('刷新后新建的文件还在（面板删不掉东西）', fs.existsSync(FSDIR + '/ui_new.txt'));
  check('目录也还在', fs.existsSync(FSDIR + '/ui_newdir'));

  console.log('== 沙箱范围：工作区 / 时可以直接越界 ==');
  byId['fs-root'].value = '/';                 // 聊天栏的“工作区”就是文件面板的沙箱根，默认 /
  byId['fs-path'].value = '/etc';
  byId['fs-go'].click();
  await waitFor(() => /项/.test(byId['fs-msg']._text), 5000);
  check('工作区 / 时能直接浏览 /etc（可以越界）',
        /项/.test(byId['fs-msg']._text) && rows().length > 0, byId['fs-msg']._text);

  console.log('== 把工作区收窄到 /data/hello 后，越界被拒 ==');
  byId['fs-root'].value = '/data/hello';
  byId['fs-path'].value = '/etc';
  byId['fs-go'].click();
  await waitFor(() => byId['fs-msg'].classList.contains('err'), 5000);
  check('越界目录给出错误提示', byId['fs-msg'].classList.contains('err'), byId['fs-msg']._text);
  check('错误里写清了范围', /只允许操作 \/data\/hello 内的路径/.test(byId['fs-msg']._text), byId['fs-msg']._text);
  check('越界后界面不崩', true);

  console.log('== 勾选框能手动勾：取消 = 放开，勾上 = 收紧（v1.3.3）==');
  const clickBox = want => { byId['fs-inside'].checked = want; byId['fs-inside'].dispatch('change'); };
  clickBox(false);                                  // 手动取消勾选
  await waitFor(() => !byId['fs-msg'].classList.contains('err') && /项/.test(byId['fs-msg']._text), 5000);
  check('取消勾选后同一条越界路径立刻能读',
        /项/.test(byId['fs-msg']._text), byId['fs-msg']._text);
  check('取消勾选后文案写「已放开越界限制」',
        /已放开越界限制/.test(byId['fs-scope-txt']._text), byId['fs-scope-txt']._text);
  check('取消勾选的状态存进 localStorage',
        JSON.parse(global.localStorage.getItem('llm_web_chat_conf_v2') || '{}').outside === true,
        global.localStorage.getItem('llm_web_chat_conf_v2'));

  // 浏览（打开文件）也吃同一套沙箱：放开时能读工作区外的文件
  const hostRow = findRow('hostname');
  check('放开时工作区外的文件出现在列表里（/etc/hostname）', !!hostRow,
        rows().map(tr => tr.children[0].textContent).join(','));
  if (hostRow){
    hostRow.children[0].click();                    // 点文件名 = 打开
    await waitFor(() => byId['fs-edit-name']._text.includes('/etc/hostname'), 5000);
  }
  check('放开时能打开并读到工作区外的文件',
        byId['fs-edit-name']._text.includes('/etc/hostname'), byId['fs-edit-name']._text);
  let hostReal = '';
  try { hostReal = fs.readFileSync('/etc/hostname', 'utf8'); } catch(e){}
  check('编辑器里就是 /etc/hostname 的内容',
        hostReal === '' || byId['fs-text'].value === hostReal,
        JSON.stringify(byId['fs-text'].value).slice(0, 60));

  clickBox(true);                                   // 再勾回来
  await waitFor(() => byId['fs-msg'].classList.contains('err'), 5000);
  check('重新勾上后同一条路径又被拒（越界）',
        byId['fs-msg'].classList.contains('err') &&
        /只允许操作 \/data\/hello 内的路径/.test(byId['fs-msg']._text), byId['fs-msg']._text);
  check('重新勾上后文案回到「只允许操作 /data/hello 内的路径」',
        byId['fs-scope-txt']._text === '只允许操作 /data/hello 内的路径', byId['fs-scope-txt']._text);

  console.log('== 只许浏览沙箱内的文件：收紧后连打开都被拒 ==');
  byId['fs-text'].value = 'sentinel-not-overwritten';
  if (hostRow) hostRow.children[0].click();         // 收紧状态下点开同一个越界文件
  await sleep(300);                                 // 等请求走完（被拒是同步回来的）
  check('收紧后打开越界文件被拒（浏览也被沙箱管着）',
        /越界/.test(byId['fs-msg']._text) &&
        /只允许操作 \/data\/hello 内的路径/.test(byId['fs-msg']._text), byId['fs-msg']._text);
  check('被拒时没把工作区外的内容塞进编辑器',
        byId['fs-text'].value === 'sentinel-not-overwritten',
        JSON.stringify(byId['fs-text'].value).slice(0, 60));
  check('列表还在（拒绝不影响界面）', rows().length > 0);

  console.log('== 「工作区」+ 勾选框管着所有操作（服务端 --fs-root 更宽也不行，v1.3.4）==');
  // 造两个都在服务端沙箱里、但只有一个在「工作区」里的路径：
  // 服务端 --fs-root 是 /data/hello（或默认 /）时，sibling.txt 都在服务端范围内，
  // 只有「工作区」收窄到 scope_root 才该被挡 —— 这样不管服务端怎么起都能验到。
  fs.mkdirSync(FSDIR + '/scope_root', { recursive: true });
  fs.writeFileSync(FSDIR + '/scope_root/in.txt', 'inside\n');
  fs.writeFileSync(FSDIR + '/sibling.txt', 'outside-scope\n');
  byId['fs-root'].value = FSDIR;                 // 工作区先放大到整个测试目录
  byId['fs-path'].value = FSDIR;
  byId['fs-go'].click();
  await waitFor(() => findRow('sibling.txt'), 5000);
  const sibRow = findRow('sibling.txt');         // 先抓住这一行（下一刷新列表就换了）
  check('工作区 = 测试目录时能看到 sibling.txt', !!sibRow);

  byId['fs-root'].value = FSDIR + '/scope_root'; // 工作区收窄到子目录（勾选框还是勾着的）
  byId['fs-path'].value = FSDIR + '/scope_root';
  byId['fs-go'].click();
  await waitFor(() => !!findRow('in.txt'), 5000);
  check('收窄后列的是子目录里的东西', !!findRow('in.txt'), byId['fs-msg']._text);
  check('子目录外面的 sibling.txt 不在列表里', !findRow('sibling.txt'));

  byId['fs-text'].value = 'sentinel2-not-overwritten';
  if (sibRow) sibRow.children[0].click();        // 点老列表里那一行 = 打开工作区外的文件
  await waitFor(() => byId['fs-msg'].classList.contains('err'), 5000);
  check('打开「工作区」外面的文件被拒（读也吃「工作区」这份范围）',
        /越界/.test(byId['fs-msg']._text) && byId['fs-msg']._text.includes('scope_root'),
        byId['fs-msg']._text);
  check('被拒时没把那个文件的内容读进编辑器',
        byId['fs-text'].value === 'sentinel2-not-overwritten',
        JSON.stringify(byId['fs-text'].value).slice(0, 60));

  clickBox(false);                               // 放开越界 → 同一个文件立刻能打开
  await waitFor(() => !byId['fs-msg'].classList.contains('err'), 5000);
  if (sibRow) sibRow.children[0].click();
  await waitFor(() => byId['fs-edit-name']._text.includes('sibling.txt'), 5000);
  check('取消勾选后同一个文件立刻能打开',
        byId['fs-edit-name']._text.includes('sibling.txt') && byId['fs-text'].value === 'outside-scope\n',
        byId['fs-edit-name']._text);
  clickBox(true);                                // 收拾回收紧状态
  await sleep(200);
  check('重新勾上后范围又回到「工作区」',
        /^只允许操作 .*scope_root 内的路径$/.test(byId['fs-scope-txt']._text),
        byId['fs-scope-txt']._text);

  console.log('== 「新建」也是一条能碰到工作区外面的路（写同样吃这份范围）==');
  // 新建对话框收的是相对「当前目录」的路径、可以带 ..，所以它跟"打开"一样得被管着：
  // 在工作区里点「新建文件」写 ../xxx，服务端按「工作区」判越界，磁盘上不该多出东西。
  byId['fs-path'].value = FSDIR + '/scope_root';
  byId['fs-go'].click();
  await waitFor(() => !!findRow('in.txt'), 5000);
  global.__promptAnswer = '../escaped_by_panel.txt';
  byId['fs-newfile'].click();
  await waitFor(() => byId['fs-msg'].classList.contains('err'), 5000);
  check('在工作区外面「新建文件」被拒（越界，写也吃这份范围）',
        /越界/.test(byId['fs-msg']._text) && byId['fs-msg']._text.includes('scope_root'),
        byId['fs-msg']._text);
  check('被拒后磁盘上没多出那个文件',
        !fs.existsSync(FSDIR + '/escaped_by_panel.txt'));
  global.__promptAnswer = 'new_inside.txt';
  byId['fs-newfile'].click();
  await waitFor(() => fs.existsSync(FSDIR + '/scope_root/new_inside.txt'), 5000);
  check('工作区里面「新建文件」照常可用（只拦外面的路，没有把新建禁掉）',
        fs.existsSync(FSDIR + '/scope_root/new_inside.txt'));

  console.log('\nFiles 面板结果: %d 通过 / %d 失败', pass, fail);
  // 收尾：本测试自己造的文件自己清（面板没有删除功能，这里用 node 清）
  for (const f of ['ui_edit.txt', 'ui_seed.txt', 'ui_new.txt', 'sibling.txt', 'escaped_by_panel.txt'])
    try { fs.rmSync(FSDIR + '/' + f, { force: true }); } catch(e){}
  try { fs.rmSync(FSDIR + '/ui_newdir', { recursive: true, force: true }); } catch(e){}
  try { fs.rmSync(FSDIR + '/scope_root', { recursive: true, force: true }); } catch(e){}
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error('harness 崩了:', e); process.exit(2); });
