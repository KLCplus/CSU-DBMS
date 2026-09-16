// ------------------------------------------------------------------------------------------------
// 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
// ------------------------------------------------------------------------------------------------
//  app.js:41                  api
//  app.js:43                  post
//  app.js:45                  text
//  app.js:46                  num
//  app.js:48                  table
//  app.js:50                  showPage
//  app.js:52                  renderLineChart
//  app.js:54                  renderStatus
//  app.js:56                  databases
//  app.js:58                  tables
//  app.js:60                  schemaGraph
//  app.js:62                  loadTable
//  app.js:64                  loadStructure
//  app.js:66                  loadIndexes
//  app.js:68                  run
//  app.js:70                  recent
//  app.js:72                  refresh
//  app.js:77                  acHide
//  app.js:78                  acRender
//  app.js:79                  acAccept
//  app.js:80                  acAcceptGhost
//  app.js:81                  acShowGhost
//  app.js:82                  acRequest
//  app.js:83                  acMove
// ------------------------------------------------------------------------------------------------
/*
 * @file app.js
 * @brief CSUDB Web Console 前端脚本（单文件、无构建依赖）
 * @details 通过 fetch 调用本地 Web Gateway 的 /api/* 接口，维护登录会话令牌
 *  （session token，存入 localStorage 并在请求头 X-CSUDB-Session 中回传），
 *  渲染服务端状态、Buffer Pool、表结构与数据，并为 SQL 编辑器提供自动补全
 *  下拉框（确定性候选）与 AI ghost text 提示（Tab 接受）。
 * @note 核心原则：前端不直接连接数据库，所有请求都经 Gateway 转成 native 协议；
 *  补全/ghost 由 /api/complete 返回，前端只负责展示与插入文本。
 */
const $=id=>document.getElementById(id);let state={db:"sys",tables:[],status:null,frames:[],samples:[],history:[]};const API_BASE=(typeof window!=="undefined"&&window.CSUDB_API_BASE?String(window.CSUDB_API_BASE):"").replace(/\/+$/,"");let sessionToken="";try{sessionToken=localStorage.getItem("csudb_session_token")||""}catch(e){}
/* 统一 API 封装：拼接 API_BASE，注入会话令牌头，解析 JSON，响应失败时抛出错误。 */
async function api(path,opt={}){const headers={...(opt.headers||{})};if(sessionToken)headers["X-CSUDB-Session"]=sessionToken;const r=await fetch(API_BASE+path,{credentials:"same-origin",...opt,headers});const d=await r.json();if(!r.ok||d.success===false)throw Error(d.error?.message||d.message||"Request failed");return d}
/* POST 请求封装：设置 JSON 内容类型并序列化请求体，复用 api()。 */
function post(path,body){return api(path,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)})}
/* 安全写入元素文本：元素不存在时静默忽略，空值显示为破折号。 */
function text(id,v){const e=$(id);if(e)e.textContent=v??"—"}/* 把可能带单位的文本转成数字，剔除非数字字符，无法解析时返回 0。 */
function num(v){return Number(String(v??0).replace(/[^0-9.-]/g,""))||0}
/* 用列定义与行数据在指定容器内渲染一张纯文本表格。 */
function table(node,cols,rows){node.textContent="";const t=document.createElement("table"),h=document.createElement("thead"),tr=document.createElement("tr");cols.forEach(c=>{const x=document.createElement("th");x.textContent=c.name||c;tr.append(x)});h.append(tr);t.append(h);const b=document.createElement("tbody");(rows||[]).forEach(row=>{const r=document.createElement("tr");row.forEach(v=>{const c=document.createElement("td");c.textContent=v??"NULL";r.append(c)});b.append(r)});t.append(b);node.append(t)}
/* 切换激活页面：同步 .page 与导航按钮的 active 状态，并写入 location.hash。 */
function showPage(page){document.querySelectorAll(".page").forEach(x=>x.classList.toggle("active",x.id==="page-"+page));document.querySelectorAll("nav button").forEach(x=>x.classList.toggle("active",x.dataset.page===page));location.hash=page}
/* 用内联 SVG 绘制折线图：数据点不足时显示占位，否则归一化后生成网格与折线。 */
function renderLineChart(id,series){const node=$(id);if(!node)return;const all=series.flatMap(s=>s.values);if(all.length<2){node.innerHTML="<div class=chart-empty>Collecting metrics…</div>";return}const w=640,h=170,p=10,max=Math.max(1,...all),min=Math.min(0,...all),range=Math.max(1,max-min),points=values=>values.map((v,i)=>(p+i*(w-p*2)/Math.max(1,values.length-1))+","+(h-p-(v-min)*(h-p*2)/range)).join(" ");let grid="";for(let i=1;i<4;i++)grid+="<line x1=0 y1="+(i*h/4)+" x2="+w+" y2="+(i*h/4)+" />";node.innerHTML="<svg viewBox=\"0 0 "+w+" "+h+"\" preserveAspectRatio=\"none\"><g class=\"chart-grid-lines\">"+grid+"</g>"+series.map(s=>"<polyline points=\""+points(s.values)+"\" style=\"stroke:"+s.color+"\" />").join("")+"</svg>"}
/* 渲染 /api/status 响应：更新概览与 Buffer Pool 指标、堆叠条、帧映射与帧表，并采样计算速率曲线。 */
function renderStatus(d){const a=d.status.attributes||{},p=d.pages||{};state.status=a;state.frames=p.rows||[];text("v-version",a.version);text("v-requests",a.requests);text("v-hit-rate",a.hit_rate);text("v-io",(a.disk_reads||0)+" / "+(a.disk_writes||0));text("v-buffer",(a.buffer_pool_used||0)+" / "+(a.buffer_pool_frames||0));text("overview-endpoint",(typeof location!=="undefined"&&location.host)?location.host:(a.client_address||"127.0.0.1"));text("overview-database",state.db);text("buffer-used",a.buffer_pool_used);text("buffer-pinned",a.buffer_pool_pinned);text("buffer-dirty",a.buffer_pool_dirty);text("buffer-capacity",(a.buffer_pool_frames||0)+" frames");["capacity","used","pinned","dirty","reads","writes"].forEach(k=>text("i-"+k,a[k==="reads"?"disk_reads":k==="writes"?"disk_writes":"buffer_pool_"+k]||0));let cap=num(a.buffer_pool_frames),used=num(a.buffer_pool_used),pin=num(a.buffer_pool_pinned),dirty=num(a.buffer_pool_dirty);text("buffer-percent",(cap?Math.round(used*100/cap):0)+"%");$("buffer-stacked").innerHTML="<i class=used style=width:"+(cap?used*100/cap:0)+"%></i><i class=pinned style=width:"+(cap?pin*100/cap:0)+"%></i><i class=dirty style=width:"+(cap?dirty*100/cap:0)+"%></i>";$("server-details").innerHTML=["product","version","database","page_size","replacement_policy","io_backend"].map(k=>"<dt>"+k.replaceAll("_"," ")+"</dt><dd>"+(a[k]||"—")+"</dd>").join("");const sample={requests:num(a.requests),reads:num(a.disk_reads),writes:num(a.disk_writes)},prev=state.samples[state.samples.length-1];if(prev&&(sample.requests<prev.requests||sample.reads<prev.reads||sample.writes<prev.writes))state.samples=[];state.samples.push(sample);if(state.samples.length>40)state.samples.shift();const deltas=(key)=>state.samples.slice(1).map((s,i)=>Math.max(0,s[key]-state.samples[i][key]));const requests=deltas("requests"),reads=deltas("reads"),writes=deltas("writes");text("request-rate",requests.at(-1)||0);renderLineChart("activity-chart",[{values:requests,color:"var(--cyan)"}]);renderLineChart("io-chart",[{values:reads,color:"var(--blue)"},{values:writes,color:"var(--purple)"}]);let map=$("frame-map");map.textContent="";state.frames.forEach(r=>{let f=document.createElement("i");f.className="frame-cell "+(r[4]==="yes"?"dirty ":num(r[3])?"pinned ":"used");f.title="Frame "+r[0]+" · page "+r[2]+" · pins "+r[3];map.append(f)});table($("frame-table"),p.columns||[],state.frames)}
/* 拉取数据库列表填充下拉框；切换库时调用 /api/use 并刷新页面。 */
async function databases(){const d=await api("/api/databases"),s=$("database-select"),rows=d.rows||[];text("v-databases",rows.length);s.textContent="";rows.forEach(r=>{let o=document.createElement("option");o.value=o.textContent=r[0];o.selected=r[0]===state.db;s.append(o)});s.onchange=async()=>{await post("/api/use",{database:s.value});state.db=s.value;state.samples=[];text("sql-db",state.db);text("overview-database",state.db);await refresh()}}
/* 拉取当前库表名填充下拉框，重建 schema 图，并默认加载第一张表。 */
async function tables(){const d=await api("/api/tables");state.tables=(d.rows||[]).map(r=>r[0]);text("v-tables",state.tables.length);let s=$("table-select");s.textContent="";state.tables.forEach(n=>{let o=document.createElement("option");o.value=o.textContent=n;s.append(o)});schemaGraph();if(state.tables.length)loadTable();else $("data-content").innerHTML="<div class=empty>No tables in this database.</div>"}
/* 为每张表请求列信息并绘制 schema 节点，点击显示列结构；支持按名称过滤。 */
async function schemaGraph(){let box=$("schema-graph");box.textContent="";let filter=($("schema-search").value||"").toLowerCase();for(const name of state.tables.filter(x=>x.toLowerCase().includes(filter))){let d=await api("/api/schema?name="+encodeURIComponent(name)),n=document.createElement("article");n.className="schema-node";n.innerHTML="<h3>"+name+"</h3>"+(d.columns||[]).map(c=>"<p>"+c.name+" <span>"+c.type+"</span></p>").join("");n.onclick=()=>{text("schema-title",name);$("schema-columns").innerHTML="<table><tr><th>Column</th><th>Type</th><th>Length</th></tr>"+(d.columns||[]).map(c=>"<tr><td>"+c.name+"</td><td>"+c.type+"</td><td>"+c.length+"</td></tr>").join("")+"</table>"};box.append(n)}if(!box.children.length)box.innerHTML="<div class=empty>No tables in database &quot;"+state.db+"&quot;.</div>"}
/* 加载选中表的数据到数据区，并把编辑器内容重置为 SELECT *。 */
async function loadTable(){let n=$("table-select").value;if(!n)return;let d=await api("/api/table?name="+encodeURIComponent(n));table($("data-content"),d.columns||[],d.rows||[]);$("sql-editor").value="SELECT * FROM "+n+";"}
/* 加载选中表的列结构（列名/类型/长度）到数据区。 */
async function loadStructure(){let n=$("table-select").value;if(!n)return;let d=await api("/api/schema?name="+encodeURIComponent(n));table($("data-content"),["Column","Type","Length"],(d.columns||[]).map(c=>[c.name,c.type,c.length]));}
/* 索引信息占位：当前服务端 API 未提供索引元数据。 */
function loadIndexes(){$("data-content").innerHTML="<div class=empty>Index metadata is not available from the current server API.</div>"}
/* 执行编辑器中的 SQL：POST /api/query，渲染结果或影响行数，记录历史并刷新状态。 */
async function run(){let sql=$("sql-editor").value.trim();if(!sql)return;let started=performance.now();try{let d=await post("/api/query",{sql});$("sql-message").className="message";$("sql-message").textContent=d.message||"Query OK";if(d.columns?.length)table($("sql-result"),d.columns,d.rows);else $("sql-result").innerHTML="<div class=empty>"+(d.affected_rows??0)+" rows affected</div>";text("query-time",(performance.now()-started).toFixed(2)+" ms");state.history.unshift({sql,duration:(performance.now()-started).toFixed(2)});recent();await refresh()}catch(e){$("sql-message").className="message error";$("sql-message").textContent=e.message}}
/* 渲染最近 5 条查询历史，SQL 文本做 HTML 转义。 */
function recent(){let box=$("recent-queries");if(!state.history.length)return;box.innerHTML=state.history.slice(0,5).map(x=>"<p><code>"+x.sql.replace(/</g,"&lt;")+"</code><span>"+x.duration+" ms</span></p>").join("")}
/* 拉取状态并渲染，同时刷新数据库与表列表；失败且提示登录时弹出登录层。 */
async function refresh(){try{const d=await api("/api/status");renderStatus(d);$("login").style.display="none";await databases();await tables()}catch(e){if(e.message.includes("login"))$("login").style.display="grid"}}
/* 页面初始化：绑定导航、刷新、标签页、登录/登出、SQL 运行与快捷键、健康检查与定时状态轮询。 */
document.querySelectorAll("nav button").forEach(b=>b.onclick=()=>showPage(b.dataset.page));document.querySelectorAll("[data-refresh]").forEach(b=>b.onclick=refresh);document.querySelectorAll("[data-tab]").forEach(b=>b.onclick=()=>{document.querySelectorAll("[data-tab]").forEach(x=>x.classList.toggle("active",x===b));if(b.dataset.tab==="rows")loadTable();else if(b.dataset.tab==="structure")loadStructure();else loadIndexes()});$("login-form").onsubmit=async e=>{e.preventDefault();try{let d=await post("/api/connect",{password:$("password").value});$("password").value="";if(d.session_token){sessionToken=d.session_token;try{localStorage.setItem("csudb_session_token",sessionToken)}catch(e){}}state.db=d.database||"sys";await refresh()}catch(e){text("login-error",e.message)}};$("logout").onclick=async()=>{await post("/api/logout",{});sessionToken="";try{localStorage.removeItem("csudb_session_token")}catch(e){}$("login").style.display="grid"};$("run-sql").onclick=run;$("clear-sql").onclick=()=>{$("sql-editor").value=""};$("sql-editor").onkeydown=e=>{if((e.ctrlKey||e.metaKey)&&e.key==="Enter"){e.preventDefault();run()}};$("load-table").onclick=loadTable;$("schema-search").oninput=schemaGraph;window.addEventListener("hashchange",()=>showPage(location.hash.slice(1)||"overview"));api("/api/health").then(d=>{const h=(typeof location!=="undefined"&&location.host)?location.host:d.database_endpoint;text("endpoint",h);text("login-endpoint",h)});refresh();setInterval(()=>{if($("login").style.display!=="grid")api("/api/status").then(renderStatus).catch(()=>{})},2000);
/* SQL 自动补全与 AI ghost text：防抖请求 /api/complete，渲染候选下拉框与行内提示，处理键盘选择与接受。 */
let acItems=[],acIndex=0,acTimer=0,acGen=0,acGhost="",acGhostCursor=0;/* 隐藏补全下拉框并清空候选。 */
function acHide(){const b=$("sql-complete");if(b){b.style.display="none";b.textContent=""}acItems=[]}/* 渲染补全下拉框：每个候选显示文本、类型/AI 标记与说明，并绑定鼠标选择。 */
function acRender(){const b=$("sql-complete");if(!b)return;b.textContent="";acItems.forEach((it,i)=>{const d=document.createElement("div");d.className="completion-item"+(i===acIndex?" active":"")+(it.ai?" ai":"");const t=document.createElement("span");t.className="ci-text";t.textContent=it.text;const k=document.createElement("span");k.className="ci-kind";k.textContent=it.ai?"AI":it.kind;const det=document.createElement("span");det.className="ci-detail";det.textContent=it.detail||"";d.append(t,k,det);d.onmousedown=e=>{e.preventDefault();acAccept(i)};b.append(d)});b.style.display="block"}/* 接受第 i 个候选：用插入文本替换 [start,end) 区间，复位光标并关闭下拉框。 */
function acAccept(i){const it=acItems[i];if(!it)return;const t=$("sql-editor");const v=t.value;t.value=v.slice(0,it.start)+it.text+v.slice(it.end);const pos=it.start+it.text.length;t.selectionStart=t.selectionEnd=pos;acHide();t.focus()}/* 接受 ghost 文本：在记录的光标位置插入，复位光标并清除提示。 */
function acAcceptGhost(){if(!acGhost)return;const t=$("sql-editor"),v=t.value,c=acGhostCursor,g=acGhost;t.value=v.slice(0,c)+g+v.slice(c);const pos=c+g.length;t.selectionStart=t.selectionEnd=pos;acShowGhost("",0);t.focus()}/* 显示/清除 AI ghost 提示条，点击提示条也可接受。 */
function acShowGhost(g,cursor){acGhost=g||"";acGhostCursor=cursor;const el=$("sql-ghost");if(!el)return;if(!acGhost){el.style.display="none";el.textContent="";return}el.textContent="AI ⟶ "+acGhost+"   (Tab 接受)";el.style.display="block";el.onclick=acAcceptGhost}/* 防抖后请求 /api/complete：带请求序号丢弃过期响应，组装候选并把 ghost 置顶。 */
async function acRequest(){const t=$("sql-editor");if(!t)return;const sql=t.value,cursor=t.selectionStart||0;const gen=++acGen;try{const d=await post("/api/complete",{sql,cursor,want_model:true});if(gen!==acGen)return;acItems=(d.rows||[]).map(r=>({text:r[0],kind:r[2],detail:r[7],start:+r[4],end:+r[5]}));const g=(d.attributes&&d.attributes.ghost_text)||"";if(g)acItems.unshift({text:g,kind:"AI",detail:"model ghost",start:cursor,end:cursor,ai:true});if(acItems.length){acIndex=0;acRender()}else acHide();acShowGhost(g,cursor)}catch(e){acHide();acShowGhost("",0)}}/* 在候选中循环上下移动高亮项。 */
function acMove(dir){if(!acItems.length)return;acIndex=(acIndex+dir+acItems.length)%acItems.length;acRender()}/* 为 SQL 编辑器绑定输入防抖、Escape 关闭、方向键移动与 Tab/Enter 接受。 */
(function(){const t=$("sql-editor");if(!t)return;t.addEventListener("input",()=>{clearTimeout(acTimer);acTimer=setTimeout(acRequest,100)});t.addEventListener("keydown",e=>{const b=$("sql-complete"),open=b&&b.style.display==="block";if(e.key==="Escape"){acHide();acShowGhost("",0);return}if(!open){if(e.key==="Tab"){e.preventDefault();if(acGhost)acAcceptGhost();else acRequest()}return}if(e.key==="ArrowDown"){e.preventDefault();acMove(1)}else if(e.key==="ArrowUp"){e.preventDefault();acMove(-1)}else if((e.key==="Tab")||(e.key==="Enter"&&!e.ctrlKey&&!e.metaKey)){e.preventDefault();acAccept(acIndex)}})})();
