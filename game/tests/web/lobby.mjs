// 大厅「按房间号加入 / 邀请链接」回归：纯函数 + Lobby 原型方法（不开浏览器）
// 覆盖：房间号归一化、链接脱敏、复制反馈走 toast、自动消费邀请链接。

const {
  Lobby, ROOM_CODE_RE, parseRoomCode, buildInviteUrl,
} = await import('../../web/js/lobby.js');

let pass = 0, fail = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a === e) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}\n       got ${a}\n       exp ${e}`); }
};
const ok = (name, cond) => check(name, !!cond, true);

const CODE = 'r-0fs9tx1ayb3ep';      // PROTOCOL §4.2：r- + 13 位小写 36 进制

// ---------------------------------------------------------------- 号码解析
check('纯房间号', parseRoomCode(CODE), CODE);
check('空白 + 大写归一', parseRoomCode('  R-0FS9TX1AYB3EP \n'), CODE);
check('?room= 片段', parseRoomCode(`?room=${CODE}&mode=riichi`), CODE);
check('完整邀请链接', parseRoomCode(`https://mj.example.com/?name=abc&room=${CODE}#top`), CODE);
check('编码过的 room 参数', parseRoomCode(`https://mj.example.com/?room=${encodeURIComponent(CODE)}`), CODE);
check('链接但无 room 参数', parseRoomCode('https://mj.example.com/?mode=riichi'), null);
check('空输入', parseRoomCode(''), null);
check('null 输入', parseRoomCode(null), null);
check('短号（测试用 r-0001）被拒', parseRoomCode('r-0001'), null);
check('14 位被拒', parseRoomCode(`${CODE}q`), null);
check('非法字符被拒', parseRoomCode('r-0fs9tx1ayb3e!'), null);
check('不带 ?/& 的裸片段被拒', parseRoomCode(`room=${CODE}`), null);
ok('导出正则无 g 标志（可重复 test）', ROOM_CODE_RE.test(CODE) && ROOM_CODE_RE.test(CODE));

// ---------------------------------------------------------------- 邀请链接
const href = `http://127.0.0.1:8080/?name=小明&user=alice&pass=secret&register=1&mode=riichi&length=hanchan`;
const invite = new URL(buildInviteUrl(CODE, href, 'riichi', 'hanchan'));
check('邀请链接带上 room', invite.searchParams.get('room'), CODE);
check('邀请链接保留日麻玩法', invite.searchParams.get('mode'), 'riichi');
check('邀请链接保留半庄', invite.searchParams.get('length'), 'hanchan');
check('不带昵称', invite.searchParams.get('name'), null);
check('不带账号', invite.searchParams.get('user'), null);
check('不带密码', invite.searchParams.get('pass'), null);
check('不带注册标记', invite.searchParams.get('register'), null);
check('保留站点地址', `${invite.origin}${invite.pathname}`, 'http://127.0.0.1:8080/');
const classic = new URL(buildInviteUrl(CODE, href, 'classic', 'tonpu'));
check('经典玩法不带 length', classic.searchParams.get('length'), null);
check('经典玩法写 classic', classic.searchParams.get('mode'), 'classic');
check('链接能被解析回房间号', parseRoomCode(buildInviteUrl(CODE, href, 'riichi', 'hanchan')), CODE);
ok('邀请链接去掉原 hash', !buildInviteUrl(CODE, `${href}#top`, 'classic').includes('#'));

// ---------------------------------------------------------------- Lobby 行为
// 极简 DOM / client stub：只用原型方法，不跑构造函数（那里要真 document）
function fakeLobby({ loginVisible = false, joinResult = true } = {}) {
  const events = [];
  const lobby = Object.create(Lobby.prototype);
  lobby.busy = false;
  lobby.visible = true;
  lobby.mode = 'classic';
  lobby.length = 'tonpu';
  lobby.inviteRoom = '';
  lobby.roomId = CODE;
  lobby.mySeat = 0;
  lobby.roomReady = false;
  lobby.handlers = {};
  lobby.app = { on: (name, fn) => { lobby.handlers[name] = fn; } };
  lobby.client = {
    config: { room: '' },
    joinRoom: (id) => { events.push(['join', id]); return joinResult; },
  };
  lobby.el = {
    joinInput: { value: '', disabled: false, focus() { events.push(['focus']); } },
    viewLogin: { hidden: !loginVisible },
    msg: { textContent: '', classList: { toggle() {} } },
    roomCode: { textContent: CODE },
    lobbyToast: { hidden: true, className: '', textContent: '' },
    roomSeats: { innerHTML: '' },
    roomTip: { textContent: '' },
    viewRoom: { hidden: false },
  };
  lobby.ui = { toast: (text, kind) => events.push(['toast', text, kind]) };
  return { lobby, events };
}

// 反馈通道：房间页写卡片内提示条（全局 toast 会被弹层盖住），登录页写 #lobby-msg
{
  const { lobby } = fakeLobby();
  lobby.notify('房间号已复制', true);
  check('房间页成功提示可见', lobby.el.lobbyToast.hidden, false);
  check('成功提示样式 good', lobby.el.lobbyToast.className, 'lobby-toast good');
  check('成功提示文案', lobby.el.lobbyToast.textContent, '房间号已复制');
  lobby.notify('房间不存在或已关闭');
  check('错误提示样式 error', lobby.el.lobbyToast.className, 'lobby-toast error');
  check('错误提示文案', lobby.el.lobbyToast.textContent, '房间不存在或已关闭');
}
{
  const { lobby, events } = fakeLobby();
  lobby.el.lobbyToast = null;
  lobby.notify('兜底提示');
  check('无卡片提示条时退回全局 toast', events[0], ['toast', '兜底提示', 'error']);
}
{
  const { lobby } = fakeLobby({ loginVisible: true });
  lobby.notify('请输入昵称');
  check('登录页提示仍写 #lobby-msg', lobby.el.msg.textContent, '请输入昵称');
  check('登录页不弹卡片提示条', lobby.el.lobbyToast.hidden, true);
}

// 按房间号加入
{
  const { lobby, events } = fakeLobby();
  lobby.el.joinInput.value = '  ';
  lobby.joinByCode();
  check('非法号码不发 join_room', events.filter((e) => e[0] === 'join'), []);
  ok('非法号码聚焦输入框', events.some((e) => e[0] === 'focus'));
  check('非法号码给出提示', lobby.el.lobbyToast.className, 'lobby-toast error');
  check('非法号码不锁按钮', lobby.busy, false);
}
{
  const { lobby, events } = fakeLobby();
  lobby.el.joinInput.value = ' R-0FS9TX1AYB3EP ';
  lobby.joinByCode();
  check('合法号码发 join_room', events.find((e) => e[0] === 'join'), ['join', CODE]);
  check('输入框回填规范化号码', lobby.el.joinInput.value, CODE);
  check('加入中给出 good 提示', lobby.el.lobbyToast.className, 'lobby-toast good');
  check('加入中按钮置灰', lobby.el.joinInput.disabled, true);
}
{
  const { lobby, events } = fakeLobby();
  lobby.joinByCode(`https://mj.example.com/?room=${CODE}&mode=riichi`);
  check('粘贴邀请链接也能加入', events.find((e) => e[0] === 'join'), ['join', CODE]);
}
{
  const { lobby, events } = fakeLobby();
  lobby.busy = true;
  lobby.joinByCode(CODE);
  check('busy 时不重复发帧', events, []);
}
{
  const { lobby, events } = fakeLobby({ joinResult: false });
  lobby.joinByCode(CODE);
  check('发送失败时 busy 复位', lobby.busy, false);
  check('发送失败时输入框恢复', lobby.el.joinInput.disabled, false);
  check('发送失败给出提示', lobby.el.lobbyToast.className, 'lobby-toast error');
}

// 邀请链接落地
{
  const { lobby, events } = fakeLobby();
  lobby.inviteRoom = CODE;
  lobby.client.config.room = CODE;
  lobby.consumeInvite();
  check('自动发 join_room', events.find((e) => e[0] === 'join'), ['join', CODE]);
  check('清掉 config.room（防「创建房间」误加入）', lobby.client.config.room, '');
  check('邀请只消费一次', lobby.inviteRoom, '');
  check('回填输入框', lobby.el.joinInput.value, CODE);
  const before = events.length;
  lobby.consumeInvite();
  check('第二次不再发帧', events.length, before);
}
{
  const { lobby, events } = fakeLobby();
  lobby.consumeInvite();
  check('无邀请链接时什么都不做', events, []);
}

// room_update（PROTOCOL §5.17）：服务端广播的座位快照要刷新等待页
const ALONE = [
  { seat: 0, name: 'abc', is_bot: false },
  { seat: 1, name: 'AI-1', is_bot: true },
  { seat: 2, name: 'AI-2', is_bot: true },
  { seat: 3, name: 'AI-3', is_bot: true },
];
const TWO = [
  { seat: 0, name: 'abc', is_bot: false },
  { seat: 1, name: 'lixueming', is_bot: false },
  { seat: 2, name: 'AI-2', is_bot: true },
  { seat: 3, name: 'AI-3', is_bot: true },
];
{
  const { lobby } = fakeLobby();
  lobby.renderRoomSeats(ALONE);
  ok('初始等待页显示 AI-1', lobby.el.roomSeats.innerHTML.includes('AI-1'));
  check('初始提示等待加入', lobby.el.roomTip.textContent, '等待其他玩家加入…');
  lobby.renderRoomSeats(TWO);
  ok('好友入座后显示昵称', lobby.el.roomSeats.innerHTML.includes('lixueming'));
  ok('好友入座后 AI-1 消失', !lobby.el.roomSeats.innerHTML.includes('AI-1'));
  ok('好友座位标为玩家', lobby.el.roomSeats.innerHTML.includes('>玩家<'));
  check('好友入座后提示 2 人', lobby.el.roomTip.textContent, '2 人已入座，可以开始了');
}
{
  const { lobby } = fakeLobby();
  Lobby.prototype.bindClient.call(lobby);
  check('bindClient 注册了 room_update', typeof lobby.handlers.room_update, 'function');
  lobby.handlers.room_update({ roomId: CODE, seats: TWO });
  ok('广播到达后等待页出现好友', lobby.el.roomSeats.innerHTML.includes('lixueming'));
  check('广播到达后人数提示更新', lobby.el.roomTip.textContent, '2 人已入座，可以开始了');
  lobby.el.roomSeats.innerHTML = '';
  lobby.handlers.room_update({ roomId: 'r-0000000000000', seats: [{ seat: 0, name: '别房', is_bot: false }] });
  check('别的房间的广播被忽略', lobby.el.roomSeats.innerHTML, '');
}
{
  const { lobby } = fakeLobby();
  lobby.roomReady = true;
  lobby.el.roomTip.textContent = '发牌中…';
  lobby.renderRoomSeats(TWO);
  check('已点开始后不被迟到广播覆盖', lobby.el.roomTip.textContent, '发牌中…');
}

// 复制按钮
{
  const { lobby, events } = fakeLobby();
  lobby.copyText = (text, message) => events.push(['copy', text, message]);
  lobby.copyRoomCode();
  check('复制房间号是纯号码', events[0], ['copy', CODE, `房间号已复制：${CODE}`]);
}
{
  const { lobby, events } = fakeLobby();
  lobby.copyText = (text, message) => events.push(['copy', text, message]);
  globalThis.location = { href, search: `?name=小明&mode=classic` };
  lobby.mode = 'riichi';
  lobby.length = 'hanchan';
  lobby.copyInviteLink();
  const copied = events[0][1];
  check('复制邀请链接可解析回房间号', parseRoomCode(copied), CODE);
  ok('邀请链接不含原昵称', !copied.includes('name='));
  ok('邀请链接带日麻玩法', copied.includes('mode=riichi'));
  delete globalThis.location;
}
{
  const { lobby, events } = fakeLobby();
  lobby.copyText = (...args) => events.push(['copy', ...args]);
  globalThis.location = { href: 'file:///Users/me/game/web/index.html', search: '' };
  lobby.copyInviteLink();
  check('file:// 页面不生成无效邀请链接', events.filter((e) => e[0] === 'copy'), []);
  ok('file:// 页面提示改用房间号', lobby.el.lobbyToast.textContent.includes(CODE));
  delete globalThis.location;
}
{
  const { lobby, events } = fakeLobby();
  lobby.el.roomCode.textContent = '—';
  lobby.copyText = (...args) => events.push(['copy', ...args]);
  lobby.copyRoomCode();
  lobby.copyInviteLink();
  check('还没房间号时不复制', events, []);
}

console.log(`\n[lobby] ${pass} pass, ${fail} fail`);
// notify 的提示条带 2.6s 自动隐藏 timer，测完直接退出不等它
process.exit(fail ? 1 : 0);
